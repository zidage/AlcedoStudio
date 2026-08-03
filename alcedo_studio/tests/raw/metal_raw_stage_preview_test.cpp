//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <opencv2/imgproc.hpp>
#include <vector>

#include "edit/operators/operator_registeration.hpp"
#include "edit/operators/raw/raw_decode_op.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/pipeline/pipeline_stage.hpp"
#include "image/image_buffer.hpp"
#include "type/type.hpp"

namespace alcedo {
namespace {

auto ReadFileToBuffer(const std::filesystem::path& path) -> std::vector<uint8_t> {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return {};
  }

  file.seekg(0, std::ios::end);
  const std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);
  if (size <= 0) {
    return {};
  }

  std::vector<uint8_t> buffer(static_cast<size_t>(size));
  if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
    return {};
  }
  return buffer;
}

auto CiRawFixturePath() -> std::filesystem::path {
  const auto collect_first = [](const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) {
      return std::filesystem::path{};
    }

    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (entry.is_regular_file()) {
        const auto ext = entry.path().extension().string();
        if (ext == ".ARW" || ext == ".arw" || ext == ".DNG" || ext == ".dng" ||
            ext == ".NEF" || ext == ".nef" || ext == ".RW2" || ext == ".rw2" ||
            ext == ".CR2" || ext == ".cr2" || ext == ".ORF" || ext == ".orf") {
          paths.push_back(entry.path());
        }
      }
    }
    std::sort(paths.begin(), paths.end());
    return paths.empty() ? std::filesystem::path{} : paths.front();
  };

  if (auto path = collect_first(std::filesystem::path(TEST_IMG_PATH) / "ci_rawfiles");
      !path.empty()) {
    return path;
  }
  return collect_first(std::filesystem::path("/Users/zidage/Photos"));
}

}  // namespace

TEST(MetalRawStagePreview, DecodeStillLifeWithRawStageOnly) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  const auto raw_path = CiRawFixturePath();
  if (raw_path.empty()) {
    GTEST_SKIP() << "CI RAW fixtures missing under TEST_IMG_PATH/ci_rawfiles and /Users/zidage/Photos";
  }
  ASSERT_TRUE(std::filesystem::exists(raw_path)) << raw_path.string();

  auto raw_bytes = ReadFileToBuffer(raw_path);
  ASSERT_FALSE(raw_bytes.empty());

  RegisterAllOperators();

  OperatorParams global_params;
  PipelineStage  raw_stage(PipelineStageName::Image_Loading, false, false);

  // The decode backend is a runtime property, not a param: push it directly
  // into the op, exactly like CPUPipelineExecutor::ApplyRuntimeRawDecodeBackend
  // does for pipeline-managed decodes.
  nlohmann::json decode_params                   = pipeline_defaults::MakeDefaultRawDecodeParams();
  decode_params["raw"]["backend"]                = "alcedo";
  decode_params["raw"]["highlights_reconstruct"] = false;
  decode_params["raw"]["decode_res"]             = static_cast<int>(DecodeRes::FULL);
  raw_stage.SetOperator(OperatorType::RAW_DECODE, decode_params, global_params);
  auto raw_entry = raw_stage.GetOperator(OperatorType::RAW_DECODE);
  ASSERT_TRUE(raw_entry.has_value());
  ASSERT_NE(raw_entry.value()->op_, nullptr);
  auto* raw_op = dynamic_cast<RawDecodeOp*>(raw_entry.value()->op_.get());
  ASSERT_NE(raw_op, nullptr);
  raw_op->SetRuntimeGpuBackend(GpuBackendKind::Metal);

  auto input = std::make_shared<ImageBuffer>(std::move(raw_bytes));
  raw_stage.SetInputImage(input);

  auto output = raw_stage.ApplyStage(global_params);
  ASSERT_NE(output, nullptr);
  EXPECT_TRUE(output->gpu_data_valid_);
  EXPECT_EQ(output->GetGPUType(), CV_32FC4);

  ASSERT_NO_THROW(output->SyncToCPU());
  const cv::Mat& raw_cpu = output->GetCPUData();
  ASSERT_FALSE(raw_cpu.empty());
  EXPECT_EQ(raw_cpu.type(), CV_32FC4);
  EXPECT_EQ(raw_cpu.channels(), 4);
#endif
}

}  // namespace alcedo
