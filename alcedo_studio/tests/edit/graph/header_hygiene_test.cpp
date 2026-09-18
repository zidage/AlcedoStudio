//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace alcedo {

namespace {

auto FileContainsForbiddenToken(const std::filesystem::path& path) -> std::string {
  std::ifstream input(path);
  std::string   line;
  int           line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const char* tokens[] = {"image_buffer.hpp", "image_buffer.h", "cuda.h", "cuda_runtime",
                            "OpenCL",           "opencl.h",       "Metal/", "metal.h"};
    for (const char* token : tokens) {
      if (line.find(token) != std::string::npos) {
        return path.filename().string() + ":" + std::to_string(line_number) + " " + token;
      }
    }
  }
  return {};
}

void ScanDirectory(const std::filesystem::path& root, std::vector<std::string>* hits) {
  if (!std::filesystem::exists(root)) {
    hits->push_back("missing directory: " + root.string());
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (entry.path().extension() != ".hpp") {
      continue;
    }
    const auto hit = FileContainsForbiddenToken(entry.path());
    if (!hit.empty()) {
      hits->push_back(hit);
    }
  }
}

}  // namespace

TEST(GpuDagModelGraph, ModelAndGraphHeadersDoNotIncludeGpuOrImageBuffer) {
  std::vector<std::string> hits;
  ScanDirectory(std::filesystem::path{ALCEDO_MODEL_HEADER_ROOT}, &hits);
  ScanDirectory(std::filesystem::path{ALCEDO_GRAPH_HEADER_ROOT}, &hits);
  ScanDirectory(std::filesystem::path{ALCEDO_MASK_HEADER_ROOT}, &hits);
  EXPECT_TRUE(hits.empty()) << (hits.empty() ? "" : hits.front());
}

}  // namespace alcedo
