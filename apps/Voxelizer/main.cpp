/* Copyright (c) 2015-2017, EPFL/Blue Brain Project
 * All rights reserved. Do not distribute without permission.
 * Responsible Author: Cyrille Favreau <cyrille.favreau@epfl.ch>
 *                     Grigori Chevtchenko <grigori.chevtchenko@epfl.ch>
 *
 * This file is part of SIMDVoxelizer <https://github.com/favreau/SIMDVoxelizer>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 3.0 as published
 * by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iostream>
#include <limits>
#include <map>
#include <simdvoxelizer/Octree.h>
#include <simdvoxelizer/SIMDSparseVoxelizer_ispc.h>
#include <sstream>
#include <string.h>
#include <vector>

int span = 32;
float voxelSize = 2.f;
std::string inputFile;
std::string outputFile;

typedef std::map<uint64_t, OctreeNode> OctreeLevelMap;

inline uint32_t pow2roundup(uint32_t x) {
  --x;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  return x + 1;
}

void flattenChildren(const OctreeNode *node, uint32_t *offsetPerLevel,
                     uint32_t *flatOctreeIndex, float *flatOctreeData,
                     uint32_t level) {
  const std::vector<OctreeNode *> children = node->getChildren();

  if ((children.empty()) || (level == 0)) {
    flatOctreeData[offsetPerLevel[level] * 4u] = node->getCenter().x;
    flatOctreeData[offsetPerLevel[level] * 4u + 1] = node->getCenter().y;
    flatOctreeData[offsetPerLevel[level] * 4u + 2] = node->getCenter().z;
    flatOctreeData[offsetPerLevel[level] * 4u + 3] = node->getValue();

    offsetPerLevel[level] += 1u;
    return;
  }
  flatOctreeData[offsetPerLevel[level] * 4u] = node->getCenter().x;
  flatOctreeData[offsetPerLevel[level] * 4u + 1] = node->getCenter().y;
  flatOctreeData[offsetPerLevel[level] * 4u + 2] = node->getCenter().z;
  flatOctreeData[offsetPerLevel[level] * 4u + 3] = node->getValue();

  flatOctreeIndex[offsetPerLevel[level] * 2u] = offsetPerLevel[level - 1];
  flatOctreeIndex[offsetPerLevel[level] * 2u + 1] =
      offsetPerLevel[level - 1] + children.size() - 1u;
  offsetPerLevel[level] += 1u;

  for (const OctreeNode *child : children)
    flattenChildren(child, offsetPerLevel, flatOctreeIndex, flatOctreeData,
                    level - 1u);
}

void writeMHDHeader(const glm::vec3 &volumeDimensions,
                    const glm::vec3 &volumeOffset,
                    const glm::vec3 &volumeElementSpacing,
                    const std::string &volumeFilename) {
  const std::string mhdFilename = volumeFilename + ".mhd";
  std::ofstream mhdFile(mhdFilename, std::ofstream::out);
  if (mhdFile.is_open()) {
    mhdFile << "ObjectType = Image" << std::endl;
    mhdFile << "NDims = 3" << std::endl;
    mhdFile << "BinaryData = True" << std::endl;
    mhdFile << "BinaryDataByteOrderMSB = False" << std::endl;
    mhdFile << "CompressedData = False" << std::endl;
    mhdFile << "TransformMatrix = 1 0 0 0 1 0 0 0 1" << std::endl;
    mhdFile << "Offset = " << volumeOffset.x << " " << volumeOffset.y << " "
            << volumeOffset.z << std::endl;
    mhdFile << "CenterOfRotation = 0 0 0" << std::endl;
    mhdFile << "AnatomicalOrientation = RAI" << std::endl;
    mhdFile << "ElementSpacing = " << volumeElementSpacing.x << " "
            << volumeElementSpacing.y << " " << volumeElementSpacing.z
            << std::endl;
    mhdFile << "DimSize = " << volumeDimensions.x << " " << volumeDimensions.y
            << " " << volumeDimensions.z << std::endl;
    mhdFile << "ElementType = MET_UCHAR" << std::endl;
    mhdFile << "ElementDataFile = "
            << volumeFilename.substr(volumeFilename.find_last_of("\\/") + 1)
            << std::endl;
    mhdFile.close();
  }
}

int main(int argc, char *argv[]) {
  if (argc != 5) {
    std::cerr << "usage: SIMDVoxelizer <voxel_size> <span> "
              << "<input_file> <output_file>" << std::endl;
    exit(1);
  }

  voxelSize = atof(argv[1]);
  span = atoi(argv[2]);
  inputFile = argv[3];
  outputFile = argv[4];
  std::vector<float> events;
  std::ifstream file(inputFile.c_str(), std::ios::in | std::ios::binary);
  if (file.is_open()) {
    while (!file.eof()) {
      float v;
      file.read((char *)&v, sizeof(float));
      events.push_back(v);
    }
    file.close();
  }

  // Determine model bounding box
  glm::vec3 minAABB = {std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max()};
  glm::vec3 maxAABB = {-std::numeric_limits<float>::max(),
                       -std::numeric_limits<float>::max(),
                       -std::numeric_limits<float>::max()};
  for (uint64_t i = 0; i < events.size(); i += 5) {
    if (events[i + 4] != 0.f) {
      minAABB = {std::min(minAABB.x, events[i]),
                 std::min(minAABB.y, events[i + 1]),
                 std::min(minAABB.z, events[i + 2])};
      maxAABB = {std::max(maxAABB.x, events[i]),
                 std::max(maxAABB.y, events[i + 1]),
                 std::max(maxAABB.z, events[i + 2])};
    }
  }

  // Compute volume information
  const glm::vec3 sceneSize = maxAABB - minAABB;

  // Double AABB size
  glm::vec3 center = (minAABB + maxAABB) / 2.f;
  minAABB = center - sceneSize * 0.75f;
  maxAABB = center + sceneSize * 0.75f;
  const glm::vec3 volumeOffset = minAABB;

  // Build acceleration structure
  Octree morphoOctree(events, voxelSize, minAABB, maxAABB);
  uint64_t volumeSize = morphoOctree.getVolumeSize();
  glm::uvec3 volumeDim = morphoOctree.getVolumeDim();
  const glm::vec3 volumeElementSpacing = sceneSize / glm::vec3(volumeDim);

  std::cout << "--------------------------------------------" << std::endl;
  std::cout << "SIMDVoxelizer" << std::endl;
  std::cout << "--------------------------------------------" << std::endl;
  std::cout << "Element spacing   : " << voxelSize << std::endl;
  std::cout << "Span              : " << span << std::endl;
  std::cout << "Input file        : " << inputFile << std::endl;
  std::cout << "Output file       : " << outputFile << std::endl;
  std::cout << "Scene AABB        : [" << minAABB.x << "," << minAABB.y << ","
            << minAABB.z << "] [" << maxAABB.x << "," << maxAABB.y << ","
            << maxAABB.z << "]" << std::endl;
  std::cout << "Scene size        : [" << sceneSize.x << "," << sceneSize.y
            << "," << sceneSize.z << "]" << std::endl;
  std::cout << "Volume offset     : [" << volumeOffset.x << ","
            << volumeOffset.y << "," << volumeOffset.z << "]" << std::endl;
  std::cout << "Volume dimensions : [" << volumeDim.x << ", " << volumeDim.y
            << ", " << volumeDim.z << "] " << volumeSize << " bytes"
            << std::endl;
  std::cout << "--------------------------------------------" << std::endl;

  std::vector<float> volume(volumeSize, 0);
  const uint32_t zLenght = 8;
  for (uint32_t zOffset = 0; zOffset < volumeDim.z; zOffset += zLenght) {
    const size_t progress = float(zOffset) / float(volumeDim.z) * 100.f;
    std::cout << progress << "%\r";
    std::cout.flush();
    ispc::SIMDSparseVoxelizer_ispc(zOffset, zOffset + zLenght, span, voxelSize,
                                   morphoOctree.getOctreeSize(), volumeDim.x,
                                   volumeDim.y, volumeDim.z,
                                   morphoOctree.getFlatIndexes(),
                                   morphoOctree.getFlatData(), volume.data());
  }
  std::cout << std::endl;

  // Determine value range in volume
  float minValue = std::numeric_limits<float>::max();
  float maxValue = -std::numeric_limits<float>::max();
  for (size_t i = 0; i < volumeSize; ++i)
    if (volume[i] != 0.f) {
      minValue = std::min(minValue, volume[i]);
      maxValue = std::max(maxValue, volume[i]);
    }
  // Normalize volume
  float a = 255.f / (maxValue - minValue);
  std::cout << "Normalization [" << minValue << " - " << maxValue << "] " << a
            << std::endl;

  std::vector<char> volumeAsChar(volumeSize);
  for (size_t i = 0; i < volumeSize; ++i)
    volumeAsChar[i] = static_cast<char>((volume[i] - minValue) * a);

  std::cout << std::endl;
  std::cout << "--------------------------------------------" << std::endl;

  std::ofstream volumeFile(outputFile.c_str(),
                           std::ios::out | std::ios::binary);
  volumeFile.write((char *)volumeAsChar.data(), sizeof(char) * volumeSize);
  volumeFile.close();

  // Write MDH file
  writeMHDHeader(volumeDim, volumeOffset, volumeElementSpacing, outputFile);

  return 0;
}
