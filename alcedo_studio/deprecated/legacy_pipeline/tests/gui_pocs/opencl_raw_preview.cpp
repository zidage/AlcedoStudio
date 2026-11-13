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
#include <filesystem>
#include <iostream>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>

#include <libraw/libraw.h>

#include "decoders/processor/operators/gpu/opencl_debayer_rcd.hpp"
#include "decoders/processor/operators/gpu/opencl_to_linear_ref.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "image/opencl_image.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"
#include "opencl/opencl_runtime.hpp"

using namespace alcedo;

namespace {

constexpr double kPreviewGamma  = 2.2;
constexpr int    kPreviewMaxW   = 860;
constexpr int    kPreviewMaxH   = 620;

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
  cv::min(display32f, 1.0f, display32f);
  cv::pow(display32f, 1.0 / kPreviewGamma, display32f);

  cv::Mat rgb8;
  display32f.convertTo(rgb8, CV_8UC3, 255.0);
  return QImage(rgb8.data, rgb8.cols, rgb8.rows, static_cast<int>(rgb8.step), QImage::Format_RGB888)
      .copy();
}

static cv::Mat ProcessRawOpenCL(const std::filesystem::path& path) {
  auto raw_processor = std::make_unique<LibRaw>();
  if (raw_processor->open_file(path.string().c_str()) != LIBRAW_SUCCESS) {
    throw std::runtime_error("Failed to open RAW file: " + path.string());
  }
  if (raw_processor->unpack() != LIBRAW_SUCCESS) {
    throw std::runtime_error("Failed to unpack RAW file: " + path.string());
  }

  const libraw_rawdata_t& rawdata = raw_processor->imgdata.rawdata;
  const int               raw_w   = static_cast<int>(rawdata.sizes.raw_width);
  const int               raw_h   = static_cast<int>(rawdata.sizes.raw_height);
  const RawCfaPattern     cfa     = ReadLibRawCfaPattern(*raw_processor);
  if (cfa.kind != RawCfaKind::Bayer2x2) {
    throw std::runtime_error("OpenCL visual preview currently supports Bayer RAW only.");
  }

  cv::Mat raw_view(raw_h, raw_w, CV_16UC1, rawdata.raw_image);
  cv::Mat raw_cpu = raw_view.clone();

  opencl::OpenClImage cl_img;
  cl_img.Upload(raw_cpu);

  OpenCL::ToLinearRef(cl_img, *raw_processor, cfa);
  OpenCL::Bayer2x2ToRGB_RCD(cl_img, cfa.bayer_pattern);

  cv::Mat rcd_result;
  cl_img.Download(rcd_result);
  return rcd_result;
}

static auto DefaultVisualInputs() -> std::vector<std::pair<std::string, std::filesystem::path>> {
  const std::filesystem::path canon_r5 =
      std::string(TEST_IMG_PATH) + "/raw/camera/canon/r5/Canon-eos-r5-raw-00016.cr3";

  const std::filesystem::path leica_sl2 =
      std::string(TEST_IMG_PATH) + "/raw/camera/leica/sl2/L1010172.DNG";

  return {
      {"Canon R5 - OpenCL RAW", canon_r5},
      {"Leica SL2 - OpenCL RAW", leica_sl2},
  };
}

static QWidget* CreateStagePreview(QWidget* parent, const QString& title, const cv::Mat& mat) {
  const QImage qimg = CvMatToPreviewQImage(mat);
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

static int RunOpenClRawPreview(int argc, char* argv[]) {
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

  std::vector<std::pair<std::string, std::filesystem::path>> images;
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      const std::filesystem::path path = argv[i];
      images.emplace_back(path.filename().string() + " - OpenCL RAW", path);
    }
  } else {
    images = DefaultVisualInputs();
  }

  QWidget window;
  window.setWindowTitle("OpenCL RAW Visual Test");
  auto* layout = new QHBoxLayout(&window);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(16);

  for (const auto& [name, path] : images) {
    if (!std::filesystem::exists(path)) {
      std::cerr << "Image not found: " << path.string() << "\n";
      continue;
    }

    try {
      const cv::Mat rcd_result = ProcessRawOpenCL(path);
      std::cout << "[" << name << "] ToLinearRef + RCD " << rcd_result.cols << "x"
                << rcd_result.rows << "\n";

      auto* wrapper = new QWidget(&window);
      auto* vlayout = new QVBoxLayout(wrapper);
      vlayout->setSpacing(8);

      auto* title = new QLabel(QString::fromStdString(name), wrapper);
      title->setStyleSheet("font-weight: bold; font-size: 14px;");
      title->setAlignment(Qt::AlignCenter);

      vlayout->addWidget(title);
      vlayout->addWidget(CreateStagePreview(wrapper, "OpenCL ToLinearRef + RCD", rcd_result));
      layout->addWidget(wrapper);
    } catch (const std::exception& e) {
      std::cerr << "Failed to process " << name << ": " << e.what() << "\n";
      auto* error_label = new QLabel(QString("Error: %1").arg(e.what()), &window);
      layout->addWidget(error_label);
    }
  }

  if (layout->count() == 0) {
    std::cerr << "No RAW images were available for OpenCL visual preview.\n";
    return 1;
  }

  window.adjustSize();
  window.show();
  return app.exec();
}

int main(int argc, char* argv[]) {
  try {
    return RunOpenClRawPreview(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "OpenCL RAW visual test failed: " << e.what() << "\n";
    return 1;
  }
}
