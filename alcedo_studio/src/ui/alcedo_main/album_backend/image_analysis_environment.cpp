//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/image_analysis_controller.hpp"

#include <cctype>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "app/ai_provider_profile.hpp"
#include "image/image.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"

namespace alcedo::ui {
namespace {

using namespace std::chrono_literals;

constexpr auto kImageAnalysisSidecarStartupTimeout = 60s;

auto TrimAscii(std::string value) -> std::string {
  auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

auto CleanContextValue(std::string value) -> std::string {
  value = TrimAscii(std::move(value));
  for (char& ch : value) {
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      ch = ' ';
    }
  }
  return value;
}

auto FormatOneDecimal(float value) -> std::string {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(1) << value;
  std::string out = ss.str();
  while (out.size() > 1 && out.back() == '0') {
    out.pop_back();
  }
  if (!out.empty() && out.back() == '.') {
    out.pop_back();
  }
  return out;
}

auto FormatShutterSpeed(std::pair<int, int> shutter) -> std::string {
  const auto [num, den] = shutter;
  if (num <= 0 || den <= 0) {
    return {};
  }
  if (den == 1) {
    return std::to_string(num) + "s";
  }
  return std::to_string(num) + "/" + std::to_string(den) + "s";
}

void AppendContextField(std::vector<std::string>* fields, std::string key, std::string value) {
  if (fields == nullptr) {
    return;
  }
  value = CleanContextValue(std::move(value));
  if (!value.empty()) {
    fields->push_back(std::move(key) + "=" + std::move(value));
  }
}

auto BuildCameraContext(const ExifDisplayMetaData& exif) -> std::string {
  std::vector<std::string> fields;
  fields.reserve(12);
  AppendContextField(&fields, "camera_make", exif.make_);
  AppendContextField(&fields, "camera_model", exif.model_);
  AppendContextField(&fields, "lens_make", exif.lens_make_);
  AppendContextField(&fields, "lens_model", exif.lens_);
  if (std::isfinite(exif.aperture_) && exif.aperture_ > 0.0f) {
    AppendContextField(&fields, "aperture", "f/" + FormatOneDecimal(exif.aperture_));
  }
  AppendContextField(&fields, "shutter_speed", FormatShutterSpeed(exif.shutter_speed_));
  if (exif.iso_ > 0) {
    AppendContextField(&fields, "iso", std::to_string(exif.iso_));
  }
  if (std::isfinite(exif.focal_) && exif.focal_ > 0.0f) {
    AppendContextField(&fields, "focal_length", FormatOneDecimal(exif.focal_) + "mm");
  }
  if (std::isfinite(exif.focal_35mm_) && exif.focal_35mm_ > 0.0f) {
    AppendContextField(&fields, "focal_length_35mm", FormatOneDecimal(exif.focal_35mm_) + "mm");
  }
  if (std::isfinite(exif.focus_distance_m_) && exif.focus_distance_m_ > 0.0f) {
    AppendContextField(&fields, "focus_distance", FormatOneDecimal(exif.focus_distance_m_) + "m");
  }
  if (exif.width_ > 0 && exif.height_ > 0) {
    AppendContextField(&fields, "image_size",
                       std::to_string(exif.width_) + "x" + std::to_string(exif.height_));
  }
  AppendContextField(&fields, "captured_at", exif.date_time_str_);
  if (exif.is_hdr_) {
    AppendContextField(&fields, "hdr", "true");
  }
  if (fields.empty()) {
    return {};
  }
  std::ostringstream ss;
  ss << "Camera/EXIF metadata for this image: ";
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) {
      ss << "; ";
    }
    ss << fields[i];
  }
  ss << ".";
  return ss.str();
}

}  // namespace

class AlbumImageAnalysisEnvironment final : public IImageAnalysisEnvironment {
 public:
  AlbumImageAnalysisEnvironment(ProjectModule* project, SemanticGenerationController* semantic,
                                alcedo::AiProviderProfileController* profiles,
                                std::shared_ptr<alcedo::ImageAnalysisInFlightGate> gate)
      : project_(project), semantic_(semantic), profiles_(profiles), gate_(std::move(gate)) {}

  auto ThumbnailProvider() -> std::shared_ptr<IImageAnalysisThumbnailProvider> override {
    if (thumbnail_provider_) {
      return thumbnail_provider_;
    }
    if (!project_) {
      return nullptr;
    }
    auto ts = project_->handler().thumbnail_service();
    if (!ts) {
      return nullptr;
    }
    thumbnail_provider_ = std::make_shared<ThumbnailServiceImageAnalysisProvider>(ts);
    return thumbnail_provider_;
  }

  auto AnalysisClient() -> std::shared_ptr<IImageAnalysisClient> override {
    if (!project_) {
      return nullptr;
    }
    auto project = project_->handler().project();
    if (!project) {
      return nullptr;
    }
    auto runtime = project->GetAiSidecarRuntimeService();
    if (!runtime) {
      return nullptr;
    }
    return std::make_shared<AiSidecarRuntimeImageAnalysisClient>(runtime);
  }

  auto CredentialStore() -> std::shared_ptr<IAiCredentialStore> override {
    return profiles_ ? profiles_->CredentialStore() : nullptr;
  }

  auto Gate() -> std::shared_ptr<ImageAnalysisInFlightGate> override { return gate_; }

  auto CameraContextForItem(const ImageAnalysisItem& item) -> std::string override {
    if (!project_ || item.image_id == 0) {
      return {};
    }
    auto project = project_->handler().project();
    if (!project) {
      return {};
    }
    auto image_pool = project->GetImagePoolService();
    if (!image_pool) {
      return {};
    }
    std::string context;
    image_pool->Read<void>(item.image_id, [&context](const std::shared_ptr<Image>& image) {
      if (!image || !image->has_exif_display_.load()) {
        return;
      }
      context = BuildCameraContext(image->exif_display_);
    });
    return context;
  }

  auto AcquireSidecarLease() -> std::shared_ptr<void> override {
    if (!project_) {
      return {};
    }
    auto project = project_->handler().project();
    if (!project) {
      return {};
    }
    auto runtime = project->GetAiSidecarRuntimeService();
    return runtime ? runtime->AcquireLease() : std::shared_ptr<void>{};
  }

  auto EnsureSidecarReady(bool provider_configs_dirty, std::string* error) -> bool override {
    (void)provider_configs_dirty;
    return EnsureSidecarReadyImpl(false, error);
  }

  auto EnsureSidecarReadyInteractive(bool provider_configs_dirty, std::string* error)
      -> bool override {
    (void)provider_configs_dirty;
    return EnsureSidecarReadyImpl(true, error);
  }

  void RequestSidecarStartCancel() override {
    if (!project_) {
      return;
    }
    auto project = project_->handler().project();
    if (!project) {
      return;
    }
    auto runtime = project->GetAiSidecarRuntimeService();
    if (runtime) {
      runtime->RequestCancelStart();
    }
  }

 private:
  auto EnsureSidecarReadyImpl(bool interactive, std::string* error) -> bool {
    if (!project_) {
      if (error) {
        *error = "no project is open";
      }
      return false;
    }
    auto project = project_->handler().project();
    if (!project) {
      if (error) {
        *error = "no project is open";
      }
      return false;
    }
    auto runtime = project->GetAiSidecarRuntimeService();
    if (!runtime) {
      if (error) {
        *error = "ai sidecar runtime service is unavailable";
      }
      return false;
    }
    if (runtime->Status().state == AiSidecarRuntimeState::kReady) {
      return true;
    }
    AiSidecarRuntimeOptions options =
        semantic_ ? semantic_->RuntimeOptionsForCurrentSidecarSnapshot(false)
                  : AiSidecarRuntimeOptions{};
    options.allow_download     = false;
    options.require_model_info = false;
    options.startup_timeout    = kImageAnalysisSidecarStartupTimeout;
    const bool started = interactive ? runtime->StartAndWaitInteractive(options)
                                     : runtime->StartAndWait(options);
    if (!started) {
      if (error) {
        *error = runtime->Status().message;
        if (error->empty()) {
          *error = "ai sidecar failed to start";
        }
      }
      return false;
    }
    return runtime->Status().state == AiSidecarRuntimeState::kReady;
  }

  ProjectModule*                                   project_  = nullptr;
  SemanticGenerationController*                    semantic_ = nullptr;
  alcedo::AiProviderProfileController*             profiles_ = nullptr;
  std::shared_ptr<alcedo::ImageAnalysisInFlightGate> gate_;
  std::shared_ptr<IImageAnalysisThumbnailProvider> thumbnail_provider_;
};

std::shared_ptr<IImageAnalysisEnvironment> MakeAlbumImageAnalysisEnvironment(
    ProjectModule* project, SemanticGenerationController* semantic,
    alcedo::AiProviderProfileController* profiles,
    std::shared_ptr<alcedo::ImageAnalysisInFlightGate> gate) {
  return std::make_shared<AlbumImageAnalysisEnvironment>(project, semantic, profiles,
                                                         std::move(gate));
}

}  // namespace alcedo::ui
