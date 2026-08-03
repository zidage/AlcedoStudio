//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <QApplication>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QSize>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/operators/geometry/lens_calib_op.hpp"
#include "edit/operators/geometry/resize_op.hpp"
#include "edit/operators/raw/raw_decode_op.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "image/image_buffer.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

using ProfileClock                              = std::chrono::steady_clock;

constexpr int    kFastPreviewMaxLongEdge        = 2560;
constexpr int    kDetailRoiPreviewMaxLongEdge   = 4096;
constexpr int    kQualityBasePreviewMaxLongEdge = 4096;
constexpr int    kPreviewMaxW                   = 520;
constexpr int    kPreviewMaxH                   = 360;
constexpr double kPreviewExposure               = 0.75;
constexpr double kPreviewGamma                  = 2.2;
constexpr double kDiffAmplification             = 80.0;
constexpr int    kDefaultAutoCloseMs            = 8000;

struct DecodedFrame {
  std::shared_ptr<ImageBuffer> buffer;
  OperatorParams               global_params;
  double                       decode_ms = 0.0;
};

struct RenderScenario {
  const char*               name;
  int                       region_x;
  int                       region_y;
  float                     scale_x;
  float                     scale_y;
  bool                      enable_roi;
  int                       maximum_edge;
  ResizeDownsampleAlgorithm downsample_algorithm;
};

struct RenderResult {
  cv::Mat cuda_image;
  cv::Mat opencl_image;
  cv::Mat diff_image;
  double  cuda_ms   = 0.0;
  double  opencl_ms = 0.0;
  float   max_diff  = 0.0f;
  double  mean_abs  = 0.0;
};

auto ElapsedMs(const ProfileClock::time_point start) -> double {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(ProfileClock::now() -
                                                                               start)
      .count();
}

void SynchronizeCuda() {
  const cudaError_t error = cudaDeviceSynchronize();
  if (error != cudaSuccess) {
    throw std::runtime_error(std::string("cudaDeviceSynchronize failed: ") +
                             cudaGetErrorString(error));
  }
}

void EnsureRuntime() {
  if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
    throw std::runtime_error("CUDA device is unavailable.");
  }
  cv::cuda::setDevice(0);
  if (!TryPrepareOpenClRuntime() && !OpenClContext::Instance().IsInitialized()) {
    throw std::runtime_error("OpenCL runtime is unavailable: " +
                             OpenClContext::Instance().LastInitializationError());
  }
}

auto ReadFileToBuffer(const std::filesystem::path& path) -> std::vector<std::uint8_t> {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Unable to open RAW fixture: " + path.string());
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  stream.seekg(0, std::ios::beg);

  std::vector<std::uint8_t> buffer(static_cast<size_t>(size));
  if (!buffer.empty()) {
    stream.read(reinterpret_cast<char*>(buffer.data()), size);
  }
  return buffer;
}

auto MakeRawDecodeParams() -> nlohmann::json {
  // The decode backend is a runtime property, never a param: it is pushed on
  // the op via SetRuntimeGpuBackend, like the pipeline executor does.
  nlohmann::json params;
  params["raw"] = {{"highlights_reconstruct", true},
                   {"use_camera_wb", true},
                   {"backend", "alcedo"},
                   {"decode_res", static_cast<int>(DecodeRes::FULL)}};
  return params;
}

auto DecodeRawFrame(const std::filesystem::path& raw_path, RawGpuBackend backend) -> DecodedFrame {
  auto        input = std::make_shared<ImageBuffer>(ReadFileToBuffer(raw_path));
  RawDecodeOp raw_decode_op(MakeRawDecodeParams());
  raw_decode_op.SetRuntimeGpuBackend(backend == RawGpuBackend::CUDA ? GpuBackendKind::CUDA
                                                                     : GpuBackendKind::OpenCL);

  const auto  start = ProfileClock::now();
  raw_decode_op.Apply(input);
  const double   decode_ms = ElapsedMs(start);

  OperatorParams global_params;
  raw_decode_op.SetGlobalParams(global_params);
  if (!input->gpu_data_valid_ || !global_params.raw_runtime_valid_) {
    throw std::runtime_error("RAW decode did not produce GPU frame and runtime metadata.");
  }
  return {.buffer = input, .global_params = global_params, .decode_ms = decode_ms};
}

auto MakeLensCalibParams() -> nlohmann::json {
  auto  params             = pipeline_defaults::MakeDefaultLensCalibParams();
  auto& lens               = params["lens_calib"];
  lens["enabled"]          = true;
  lens["apply_vignetting"] = true;
  lens["apply_distortion"] = true;
  lens["apply_tca"]        = true;
  lens["apply_crop"]       = false;
  lens["auto_scale"]       = true;
  lens["use_user_scale"]   = false;
  lens["lens_profile_db_path"] =
      (std::filesystem::path("alcedo_studio") / "src" / "config" / "lens_calib").string();
  return params;
}

auto MakeResizeParams(const RenderScenario& scenario, int reference_width, int reference_height)
    -> nlohmann::json {
  nlohmann::json params;
  params["resize"] = {
      {"enable_scale", true},
      {"maximum_edge", scenario.maximum_edge},
      {"enable_roi", scenario.enable_roi},
      {"downsample_algorithm", scenario.downsample_algorithm == ResizeDownsampleAlgorithm::Bilinear
                                   ? "bilinear"
                                   : "inter_area"},
      {"roi",
       {{"x", scenario.region_x},
        {"y", scenario.region_y},
        {"resize_factor_x", scenario.scale_x},
        {"resize_factor_y", scenario.scale_y},
        {"resize_factor", std::min(scenario.scale_x, scenario.scale_y)},
        {"reference_width", reference_width},
        {"reference_height", reference_height}}},
  };
  return params;
}

auto CopyGpuFrame(const ImageBuffer& src) -> std::shared_ptr<ImageBuffer> {
  auto copy = std::make_shared<ImageBuffer>();
  src.CopyGPUDataTo(*copy);
  return copy;
}

auto ApplyPipelineTail(const DecodedFrame& decoded, const RenderScenario& scenario,
                       GpuBackendKind backend) -> std::pair<cv::Mat, double> {
  auto           frame  = CopyGpuFrame(*decoded.buffer);
  OperatorParams global = decoded.global_params;

  LensCalibOp    lens_op(MakeLensCalibParams());
  lens_op.SetGlobalParams(global);
  if (!global.lens_calib_runtime_valid_) {
    throw std::runtime_error("Lens calibration runtime did not resolve.");
  }

  const auto start = ProfileClock::now();
  lens_op.ApplyGPU(frame);

  ResizeOp resize_op(MakeResizeParams(scenario, frame->GetGPUWidth(), frame->GetGPUHeight()));
  resize_op.ApplyGPU(frame);
  if (backend == GpuBackendKind::CUDA) {
    SynchronizeCuda();
  }
  const double elapsed_ms = ElapsedMs(start);

  frame->SyncToCPU();
  return {frame->GetCPUData().clone(), elapsed_ms};
}

auto BuildDiffImage(const cv::Mat& cuda_image, const cv::Mat& opencl_image, float& max_diff,
                    double& mean_abs) -> cv::Mat {
  cv::Mat diff;
  cv::absdiff(cuda_image, opencl_image, diff);

  max_diff          = 0.0f;
  double    sum_abs = 0.0;
  size_t    count   = 0;
  const int ch      = diff.channels();
  for (int y = 0; y < diff.rows; ++y) {
    const float* row = diff.ptr<float>(y);
    for (int x = 0; x < diff.cols * ch; ++x) {
      max_diff = std::max(max_diff, row[x]);
      sum_abs += row[x];
      ++count;
    }
  }
  mean_abs = count == 0 ? 0.0 : sum_abs / static_cast<double>(count);

  cv::Mat rgb_diff;
  if (diff.type() == CV_32FC4) {
    cv::cvtColor(diff, rgb_diff, cv::COLOR_RGBA2RGB);
  } else if (diff.type() == CV_32FC3) {
    rgb_diff = diff;
  } else {
    cv::cvtColor(diff, rgb_diff, cv::COLOR_GRAY2RGB);
  }
  rgb_diff *= kDiffAmplification;
  cv::min(rgb_diff, 1.0f, rgb_diff);
  return rgb_diff;
}

auto RunScenario(const DecodedFrame& cuda_decoded, const DecodedFrame& opencl_decoded,
                 const RenderScenario& scenario) -> RenderResult {
  auto [cuda_image, cuda_ms] = ApplyPipelineTail(cuda_decoded, scenario, GpuBackendKind::CUDA);
  auto [opencl_image, opencl_ms] =
      ApplyPipelineTail(opencl_decoded, scenario, GpuBackendKind::OpenCL);

  if (cuda_image.size() != opencl_image.size() || cuda_image.type() != opencl_image.type()) {
    throw std::runtime_error(std::string(scenario.name) +
                             " produced mismatched CUDA/OpenCL output shapes.");
  }

  RenderResult result;
  result.cuda_image   = std::move(cuda_image);
  result.opencl_image = std::move(opencl_image);
  result.cuda_ms      = cuda_ms;
  result.opencl_ms    = opencl_ms;
  result.diff_image =
      BuildDiffImage(result.cuda_image, result.opencl_image, result.max_diff, result.mean_abs);
  return result;
}

auto ToPreviewQImage(const cv::Mat& image, bool already_display_linear = false) -> QImage {
  cv::Mat rgb32f;
  if (image.type() == CV_32FC4) {
    cv::cvtColor(image, rgb32f, cv::COLOR_RGBA2RGB);
  } else if (image.type() == CV_32FC3) {
    rgb32f = image;
  } else if (image.type() == CV_32FC1) {
    cv::cvtColor(image, rgb32f, cv::COLOR_GRAY2RGB);
  } else {
    throw std::runtime_error("Unsupported image type for preview.");
  }

  cv::Mat display32f;
  cv::max(rgb32f, 0.0f, display32f);
  if (!already_display_linear) {
    display32f *= kPreviewExposure;
    cv::pow(display32f, 1.0 / kPreviewGamma, display32f);
  }
  cv::min(display32f, 1.0f, display32f);

  cv::Mat rgb8;
  display32f.convertTo(rgb8, CV_8UC3, 255.0);
  return QImage(rgb8.data, rgb8.cols, rgb8.rows, static_cast<int>(rgb8.step), QImage::Format_RGB888)
      .copy();
}

auto MakePreviewWidget(QWidget* parent, const QString& title, const QString& subtitle,
                       const cv::Mat& image, bool is_diff = false) -> QWidget* {
  const QImage qimage  = ToPreviewQImage(image, is_diff);
  const QImage preview = qimage.scaled(QSize(kPreviewMaxW, kPreviewMaxH), Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);

  auto*        wrapper = new QWidget(parent);
  auto*        layout  = new QVBoxLayout(wrapper);
  layout->setSpacing(5);
  layout->setContentsMargins(0, 0, 0, 0);

  auto* title_label = new QLabel(title, wrapper);
  title_label->setStyleSheet("font-weight: bold; font-size: 13px;");
  title_label->setAlignment(Qt::AlignCenter);

  auto* subtitle_label = new QLabel(subtitle, wrapper);
  subtitle_label->setStyleSheet("color: #555; font-size: 12px;");
  subtitle_label->setAlignment(Qt::AlignCenter);

  auto* image_label = new QLabel(wrapper);
  image_label->setPixmap(QPixmap::fromImage(preview));
  image_label->setFixedSize(preview.size());
  image_label->setStyleSheet("border: 1px solid #303030; background: #111;");

  layout->addWidget(title_label);
  layout->addWidget(subtitle_label);
  layout->addWidget(image_label);
  return wrapper;
}

auto ParseAutoCloseMs(int argc, char* argv[]) -> int {
  for (int i = 1; i < argc; ++i) {
    const std::string          arg    = argv[i];
    constexpr std::string_view prefix = "--auto-close-ms=";
    if (arg.rfind(prefix.data(), 0) == 0) {
      return std::max(0, std::stoi(arg.substr(prefix.size())));
    }
  }
  return kDefaultAutoCloseMs;
}

auto RawPathFromArgs(int argc, char* argv[]) -> std::filesystem::path {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--", 0) != 0) {
      return std::filesystem::path(arg);
    }
  }
  return std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" / "canon" / "r8" / "IMG_0365.CR3";
}

auto DefaultScenarios() -> std::vector<RenderScenario> {
  return {
      RenderScenario{.name                 = "FastPreviewRoiBilinear",
                     .region_x             = 720,
                     .region_y             = 420,
                     .scale_x              = 0.62f,
                     .scale_y              = 0.58f,
                     .enable_roi           = true,
                     .maximum_edge         = kFastPreviewMaxLongEdge,
                     .downsample_algorithm = ResizeDownsampleAlgorithm::Bilinear},
      RenderScenario{.name                 = "DetailRoiPreviewArea",
                     .region_x             = 1320,
                     .region_y             = 840,
                     .scale_x              = 0.42f,
                     .scale_y              = 0.38f,
                     .enable_roi           = true,
                     .maximum_edge         = kDetailRoiPreviewMaxLongEdge,
                     .downsample_algorithm = ResizeDownsampleAlgorithm::Area},
      RenderScenario{.name                 = "QualityBaseFullFrameArea",
                     .region_x             = 0,
                     .region_y             = 0,
                     .scale_x              = 1.0f,
                     .scale_y              = 1.0f,
                     .enable_roi           = false,
                     .maximum_edge         = kQualityBasePreviewMaxLongEdge,
                     .downsample_algorithm = ResizeDownsampleAlgorithm::Area},
  };
}

int RunPreview(int argc, char* argv[]) {
  QApplication app(argc, argv);
  EnsureRuntime();

  const int                   auto_close_ms = ParseAutoCloseMs(argc, argv);
  const std::filesystem::path raw_path      = RawPathFromArgs(argc, argv);
  if (!std::filesystem::exists(raw_path)) {
    throw std::runtime_error("RAW fixture not found: " + raw_path.string());
  }

  const DecodedFrame cuda_decoded   = DecodeRawFrame(raw_path, RawGpuBackend::CUDA);
  const DecodedFrame opencl_decoded = DecodeRawFrame(raw_path, RawGpuBackend::OpenCL);

  QWidget            window;
  window.setWindowTitle("CUDA / OpenCL RAW + Lens + Geometry Preview");
  auto* root = new QVBoxLayout(&window);
  root->setContentsMargins(14, 14, 14, 14);
  root->setSpacing(14);

  auto* title = new QLabel(QString("%1 | RAW CUDA %2 ms | RAW OpenCL %3 ms | auto-close %4 ms")
                               .arg(QString::fromStdString(raw_path.filename().string()))
                               .arg(cuda_decoded.decode_ms, 0, 'f', 1)
                               .arg(opencl_decoded.decode_ms, 0, 'f', 1)
                               .arg(auto_close_ms),
                           &window);
  title->setStyleSheet("font-weight: bold; font-size: 14px;");
  root->addWidget(title);

  for (const auto& scenario : DefaultScenarios()) {
    const RenderResult result = RunScenario(cuda_decoded, opencl_decoded, scenario);
    std::cout << "[" << scenario.name << "] CUDA " << result.cuda_ms << " ms | OpenCL "
              << result.opencl_ms << " ms | max_diff=" << result.max_diff
              << " mean_abs=" << result.mean_abs << "\n";

    auto* row        = new QWidget(&window);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(10);

    row_layout->addWidget(MakePreviewWidget(row, QString("%1 CUDA").arg(scenario.name),
                                            QString("%1x%2, %3 ms")
                                                .arg(result.cuda_image.cols)
                                                .arg(result.cuda_image.rows)
                                                .arg(result.cuda_ms, 0, 'f', 1),
                                            result.cuda_image));
    row_layout->addWidget(MakePreviewWidget(row, QString("%1 OpenCL").arg(scenario.name),
                                            QString("%1x%2, %3 ms")
                                                .arg(result.opencl_image.cols)
                                                .arg(result.opencl_image.rows)
                                                .arg(result.opencl_ms, 0, 'f', 1),
                                            result.opencl_image));
    row_layout->addWidget(MakePreviewWidget(
        row, QString("%1 Diff x%2").arg(scenario.name).arg(kDiffAmplification, 0, 'f', 0),
        QString("max %1, mean %2").arg(result.max_diff, 0, 'g', 4).arg(result.mean_abs, 0, 'g', 4),
        result.diff_image, true));
    root->addWidget(row);
  }

  window.adjustSize();
  window.show();

  QTimer::singleShot(auto_close_ms, &app, &QCoreApplication::quit);
  return app.exec();
}

}  // namespace
}  // namespace alcedo

int main(int argc, char* argv[]) {
  try {
    return alcedo::RunPreview(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "OpenCL/CUDA pipeline preview failed: " << e.what() << "\n";
    return 1;
  }
}
