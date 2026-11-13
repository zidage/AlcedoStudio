//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
//  Local bisection tool: render one RAW through the Metal DAG develop path and
//  dump sensor_linear / display_output as PFM + PNG for visual diffing.
//
//  Usage: DagRawDump <raw_path> <demosaic_method> <out_dir>
//    demosaic_method: default | neural_engine | legacy

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "decoders/processor/raw_processor_pattern.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "image/image_buffer.hpp"
#include "image/metal_image.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/metal/metal_backend.hpp"
#include "edit/runtime/metal/metal_pass_encoder.hpp"

namespace {

auto ReadFile(const std::filesystem::path& path) -> std::vector<std::byte> {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    throw std::runtime_error("cannot open " + path.string());
  }
  const auto size = in.tellg();
  in.seekg(0);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
    throw std::runtime_error("cannot read " + path.string());
  }
  return bytes;
}

auto DownloadRgba(alcedo::MetalRenderDevice& device, const alcedo::GraphValueId& id,
                  const char* tag) -> cv::Mat {
  auto* lease = device.Workspace().Images().Find(id);
  if (lease == nullptr) {
    std::fprintf(stderr, "[dump] %s: image id not produced\n", tag);
    return {};
  }
  const auto& tex = lease->Texture();
  cv::Mat       out(static_cast<int>(tex.Height()), static_cast<int>(tex.Width()), CV_32FC4);
  device.Workspace().Device().DownloadTexture2D(
      tex,
      std::span<std::byte>(reinterpret_cast<std::byte*>(out.data),
                           static_cast<std::size_t>(out.total()) * out.elemSize()),
      device.CommandContext());
  return out;
}

void WriteOutputs(const std::filesystem::path& prefix, const cv::Mat& rgba) {
  if (rgba.empty()) {
    return;
  }
  // PFM keeps the raw float values for numeric diffing.
  {
    cv::Mat rgb;
    cv::cvtColor(rgba, rgb, cv::COLOR_RGBA2RGB);
    cv::Mat flipped;
    cv::flip(rgb, flipped, 0);  // PFM readers expect bottom-up rows
    std::ofstream out(prefix.string() + ".pfm", std::ios::binary);
    out << "PF\n" << flipped.cols << " " << flipped.rows << "\n-1.0\n";
    out.write(reinterpret_cast<const char*>(flipped.data),
              static_cast<std::streamsize>(flipped.total() * flipped.elemSize()));
  }
  // PNG with a simple sRGB-ish transfer for eyeballing.
  {
    cv::Mat clamped;
    cv::max(rgba, 0.0f, clamped);
    cv::min(clamped, 1.0f, clamped);
    cv::Mat gamma;
    cv::pow(clamped, 1.0 / 2.2, gamma);
    cv::Mat u8;
    gamma.convertTo(u8, CV_8UC4, 255.0);
    cv::Mat bgr;
    cv::cvtColor(u8, bgr, cv::COLOR_RGBA2BGRA);
    cv::imwrite(prefix.string() + ".png", bgr);
  }
  std::fprintf(stderr, "[dump] wrote %s.{pfm,png} %dx%d\n", prefix.c_str(), rgba.cols, rgba.rows);
}

auto PatternName(const alcedo::RawCfaPattern& p) -> const char* {
  switch (p.kind) {
    case alcedo::RawCfaKind::Bayer2x2:
      return "Bayer2x2";
    case alcedo::RawCfaKind::XTrans6x6:
      return "XTrans6x6";
    default:
      return "Other";
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <raw_path> <demosaic_method> <out_dir>\n", argv[0]);
    return 2;
  }
  const std::filesystem::path raw_path   = argv[1];
  const std::string           method     = argv[2];
  const std::filesystem::path out_dir    = argv[3];
  std::filesystem::create_directories(out_dir);
  const std::string stem = raw_path.stem().string() + "." + method;

  (void)alcedo::BindSystemDefaultMetalPresentationDevice();

  const auto encoded  = ReadFile(raw_path);
  const auto prepared = alcedo::RawInputLoader::LoadEncoded(encoded, alcedo::DecodeRes::FULL);

  std::fprintf(stderr,
               "[dump] input kind=%d pattern=%s host=%ux%u flip=%d margins=(%d,%d) size=(%d,%d)\n",
               static_cast<int>(prepared.input_kind), PatternName(prepared.cfa_pattern),
               prepared.host_extent.width, prepared.host_extent.height,
               prepared.sensor.orientation_flip, prepared.sensor.left_margin,
               prepared.sensor.top_margin, prepared.sensor.width, prepared.sensor.height);
  std::fprintf(stderr,
               "[dump] demosaic_crop=(%d,%d %dx%d) neural_crop=(%d,%d %dx%d) "
               "develop_extent=%ux%u neural_extent=%ux%u active=(%d,%d %dx%d)\n",
               prepared.demosaic_output_crop.x, prepared.demosaic_output_crop.y,
               prepared.demosaic_output_crop.width, prepared.demosaic_output_crop.height,
               prepared.neural_output_crop.x, prepared.neural_output_crop.y,
               prepared.neural_output_crop.width, prepared.neural_output_crop.height,
               prepared.develop_output_extent.width, prepared.develop_output_extent.height,
               prepared.neural_output_extent.width, prepared.neural_output_extent.height,
               prepared.sensor_active_area.x, prepared.sensor_active_area.y,
               prepared.sensor_active_area.width, prepared.sensor_active_area.height);
  std::fprintf(stderr, "[dump] cam_mul=(%.4f %.4f %.4f %.4f) apply_as_shot_wb=%d\n",
               prepared.linearization.cam_mul[0], prepared.linearization.cam_mul[1],
               prepared.linearization.cam_mul[2], prepared.linearization.cam_mul[3],
               prepared.linearization.apply_as_shot_wb);
  std::fprintf(stderr, "[dump] bayer rgb_fc: %d %d %d %d (origin color RgbColorAt(0,0)=%d)\n",
               prepared.cfa_pattern.bayer_pattern.rgb_fc[0],
               prepared.cfa_pattern.bayer_pattern.rgb_fc[1],
               prepared.cfa_pattern.bayer_pattern.rgb_fc[2],
               prepared.cfa_pattern.bayer_pattern.rgb_fc[3],
               alcedo::RgbColorAt(prepared.cfa_pattern, 0, 0));

  auto document = alcedo::CreateDefaultPipelineDocument();
  {
    auto* develop = document.Develop();
    auto  payload = develop->Params().Params();
    alcedo::BindDevelopCameraProfile(payload, prepared.color_context);
    if (!payload.camera_profile.color_matrices_valid) {
      std::fprintf(stderr,
                   "[dump] no camera matrices in color_context; using working-space profile\n");
      alcedo::BindRgbWorkingSpaceCameraProfile(payload);
    }
    payload.demosaic_method        = method;
    payload.highlights_reconstruct = true;
    develop->Params().ReplaceParams(std::move(payload));
  }

  const auto plan =
      alcedo::GraphCompiler::Compile(document, prepared.CompileSource(), alcedo::RenderRequest{});
  std::fprintf(stderr,
               "[dump] plan source: develop_extent=%ux%u full_ref=%ux%u neural_extent=%ux%u "
               "geometry decoded=%ux%u render=%ux%u resample=%d\n",
               plan.source.develop_output_extent.width, plan.source.develop_output_extent.height,
               plan.source.full_reference_extent.width, plan.source.full_reference_extent.height,
               plan.source.neural_output_extent.width, plan.source.neural_output_extent.height,
               plan.geometry.decoded_extent.width, plan.geometry.decoded_extent.height,
               plan.geometry.render_extent.width, plan.geometry.render_extent.height,
               static_cast<int>(plan.encode_geometry_resample));

  alcedo::MetalRenderDevice device;
  try {
    (void)device.Execute(plan, prepared, document, /*publish_on_success=*/true);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[dump] Execute threw: %s (intermediate outputs may still exist)\n",
                 e.what());
  }
  device.WaitIdle();

  WriteOutputs(out_dir / (stem + ".sensor_linear"),
               DownloadRgba(device, plan.sensor_linear_output, "sensor_linear"));
  WriteOutputs(out_dir / (stem + ".display"),
               DownloadRgba(device, plan.display_output, "display_output"));
  std::fprintf(stderr, "[dump] done\n");
  return 0;
}
