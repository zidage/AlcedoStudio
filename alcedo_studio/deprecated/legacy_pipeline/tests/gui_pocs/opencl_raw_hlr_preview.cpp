//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QSize>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>

#include <libraw/libraw.h>

#include "decoders/dng_default_crop.hpp"
#include "decoders/libraw_unpack_guard.hpp"
#include "decoders/processor/raw_processor.hpp"
#include "image/image_buffer.hpp"
#include "image/metadata_extractor.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"
#include "opencl/opencl_runtime.hpp"

using namespace alcedo;

namespace {

constexpr double kPreviewGamma    = 2.2;
constexpr double kPreviewExposure = 0.25;
constexpr int    kPreviewMaxW     = 860;
constexpr int    kPreviewMaxH     = 620;

struct TimingResult {
  cv::Mat     image;
  double      elapsed_ms = 0.0;
  std::string backend_label;
  bool        success = false;
  std::string error_msg;
};

static QImage CvMatToPreviewQImage(const cv::Mat& mat) {
  cv::Mat rgb32f;
  if (mat.type() == CV_32FC4) {
    cv::cvtColor(mat, rgb32f, cv::COLOR_RGBA2RGB);
  } else if (mat.type() == CV_32FC3) {
    rgb32f = mat;
  } else if (mat.type() == CV_32FC1) {
    cv::cvtColor(mat, rgb32f, cv::COLOR_GRAY2RGB);
  } else {
    throw std::runtime_error("Unsupported cv::Mat type for QImage conversion");
  }

  cv::Mat display32f;
  cv::max(rgb32f, 0.0f, display32f);
  display32f *= kPreviewExposure;
  cv::min(display32f, 1.0f, display32f);
  cv::pow(display32f, 1.0 / kPreviewGamma, display32f);

  cv::Mat rgb8;
  display32f.convertTo(rgb8, CV_8UC3, 255.0);
  return QImage(rgb8.data, rgb8.cols, rgb8.rows, static_cast<int>(rgb8.step), QImage::Format_RGB888)
      .copy();
}

static auto ReadFileToBuffer(const std::filesystem::path& path) -> std::vector<uint8_t> {
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

static TimingResult ProcessRawWithBackend(const std::filesystem::path& path,
                                          RawGpuBackend                backend) {
  TimingResult result;
  result.backend_label = (backend == RawGpuBackend::OpenCL) ? "OpenCL" : "CUDA";

  try {
    auto buffer = ReadFileToBuffer(path);
    if (buffer.empty()) {
      result.error_msg = "Failed to read file: " + path.string();
      return result;
    }

    auto raw = std::make_unique<LibRaw>();
    if (raw->open_buffer(buffer.data(), buffer.size()) != LIBRAW_SUCCESS) {
      result.error_msg = "Failed to open RAW buffer";
      return result;
    }
    raw->imgdata.params.output_bps      = 16;
    raw->imgdata.rawparams.use_rawspeed = 1;

    if (libraw_guard::Unpack(*raw) != LIBRAW_SUCCESS) {
      result.error_msg = "Failed to unpack RAW";
      return result;
    }

    const auto dng_metadata = dng::ExtractMetadata(std::span(buffer.data(), buffer.size()));

    RawRuntimeColorContext ctx{};
    MetadataExtractor::PopulateRuntimeContextFromOpenLibRaw(*raw, ctx);
    ctx.dng_warp_rectilinear_present_ = dng_metadata.warp_rectilinear.has_value();

    RawParams params{};
    params.gpu_backend_            = backend;
    params.highlights_reconstruct_ = true;
    params.decode_res_             = DecodeRes::FULL;

    RawProcessor processor{params, raw->imgdata.rawdata, *raw, ctx,
                           dng_metadata.default_crop.data(), dng_metadata.warp_rectilinear};

    const auto start = std::chrono::steady_clock::now();
    ImageBuffer output = processor.Process();
    const auto end   = std::chrono::steady_clock::now();

    result.elapsed_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();

    output.SyncToCPU();
    result.image  = output.GetCPUData().clone();
    result.success = true;

    raw->recycle();
  } catch (const std::exception& e) {
    result.error_msg = e.what();
  }

  return result;
}

static auto DefaultVisualInputs() -> std::vector<std::filesystem::path> {
  return {
      std::string(TEST_IMG_PATH) + "/raw/edge_case/_DSC4071.ARW",
  };
}

static QWidget* CreateStagePreview(QWidget* parent, const QString& title, const cv::Mat& mat) {
  const QImage qimg    = CvMatToPreviewQImage(mat);
  const QImage preview =
      qimg.scaled(QSize(kPreviewMaxW, kPreviewMaxH), Qt::KeepAspectRatio, Qt::SmoothTransformation);

  auto* wrapper = new QWidget(parent);
  auto* layout  = new QVBoxLayout(wrapper);
  layout->setSpacing(6);
  layout->setContentsMargins(0, 0, 0, 0);

  auto* title_label = new QLabel(title, wrapper);
  title_label->setStyleSheet("font-weight: bold; font-size: 13px;");
  title_label->setAlignment(Qt::AlignCenter);

  auto* details = new QLabel(
      QString("%1x%2, gamma %3").arg(mat.cols).arg(mat.rows).arg(kPreviewGamma, 0, 'f', 1),
      wrapper);
  details->setStyleSheet("color: #555; font-size: 12px;");
  details->setAlignment(Qt::AlignCenter);

  auto* label = new QLabel(wrapper);
  label->setPixmap(QPixmap::fromImage(preview));
  label->setFixedSize(preview.size());
  label->setStyleSheet("border: 1px solid #303030; background: #111;");

  layout->addWidget(title_label);
  layout->addWidget(details);
  layout->addWidget(label);
  return wrapper;
}

}  // namespace

static int RunOpenClRawHlrPreview(int argc, char* argv[]) {
  QApplication app(argc, argv);

  // Initialize OpenCL
  if (!OpenClContext::Instance().TryInitialize()) {
    std::cerr << "OpenCL initialization failed: "
              << OpenClContext::Instance().LastInitializationError() << "\n";
    return 1;
  }
  PrepareOpenClRuntime();
  (void)OpenClProgramLibrary::Instance().GetProgram("raw_processor_core");
  (void)OpenClProgramLibrary::Instance().GetProgram("raw_processor_debayer_rcd");
  (void)OpenClProgramLibrary::Instance().GetProgram("raw_processor_highlight");

  std::vector<std::filesystem::path> images;
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      images.emplace_back(argv[i]);
    }
  } else {
    images = DefaultVisualInputs();
  }

  QWidget window;
  window.setWindowTitle("OpenCL / CUDA RAW + HLR Visual Comparison");
  auto* layout = new QVBoxLayout(&window);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(16);

  for (const auto& path : images) {
    if (!std::filesystem::exists(path)) {
      std::cerr << "Image not found: " << path.string() << "\n";
      continue;
    }

    auto* image_wrapper = new QWidget(&window);
    auto* vlayout       = new QVBoxLayout(image_wrapper);
    vlayout->setSpacing(8);

    auto* title = new QLabel(QString::fromStdString(path.filename().string()), image_wrapper);
    title->setStyleSheet("font-weight: bold; font-size: 14px;");
    title->setAlignment(Qt::AlignCenter);
    vlayout->addWidget(title);

    auto* hlayout = new QHBoxLayout();
    hlayout->setSpacing(12);

    // OpenCL path
    TimingResult opencl_result = ProcessRawWithBackend(path, RawGpuBackend::OpenCL);
    if (opencl_result.success) {
      std::cout << "[OpenCL] " << path.filename().string() << " " << opencl_result.image.cols
                << "x" << opencl_result.image.rows << " in " << opencl_result.elapsed_ms << " ms\n";
      auto* preview = CreateStagePreview(
          image_wrapper,
          QString("OpenCL + HLR\n%1 ms").arg(opencl_result.elapsed_ms, 0, 'f', 2),
          opencl_result.image);
      hlayout->addWidget(preview);
    } else {
      std::cerr << "[OpenCL] Failed: " << opencl_result.error_msg << "\n";
      auto* err_label = new QLabel(QString("OpenCL Error: %1").arg(QString::fromStdString(opencl_result.error_msg)), image_wrapper);
      hlayout->addWidget(err_label);
    }

    // CUDA path
#ifdef HAVE_CUDA
    TimingResult cuda_result = ProcessRawWithBackend(path, RawGpuBackend::CUDA);
    if (cuda_result.success) {
      std::cout << "[CUDA]   " << path.filename().string() << " " << cuda_result.image.cols << "x"
                << cuda_result.image.rows << " in " << cuda_result.elapsed_ms << " ms\n";
      auto* preview = CreateStagePreview(
          image_wrapper,
          QString("CUDA + HLR\n%1 ms").arg(cuda_result.elapsed_ms, 0, 'f', 2),
          cuda_result.image);
      hlayout->addWidget(preview);
    } else {
      std::cerr << "[CUDA]   Failed: " << cuda_result.error_msg << "\n";
      auto* err_label = new QLabel(QString("CUDA Error: %1").arg(QString::fromStdString(cuda_result.error_msg)), image_wrapper);
      hlayout->addWidget(err_label);
    }
#else
    auto* na_label = new QLabel("CUDA not enabled in this build.", image_wrapper);
    hlayout->addWidget(na_label);
#endif

    vlayout->addLayout(hlayout);
    layout->addWidget(image_wrapper);
  }

  if (layout->count() == 0) {
    std::cerr << "No RAW images were available for visual preview.\n";
    return 1;
  }

  window.adjustSize();
  window.show();
  return app.exec();
}

int main(int argc, char* argv[]) {
  try {
    return RunOpenClRawHlrPreview(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "OpenCL/CUDA RAW HLR visual test failed: " << e.what() << "\n";
    return 1;
  }
}
