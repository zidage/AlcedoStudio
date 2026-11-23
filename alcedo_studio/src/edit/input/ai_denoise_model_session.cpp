//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/input/ai_denoise_model_session.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <fstream>
#include <iterator>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "decoders/processor/nn/denoise_errors.hpp"
#include "edit/input/ai_denoise_input.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "utils/hash/sha256.hpp"
#include "utils/logging/log.hpp"

namespace alcedo {
namespace {

auto PathText(const std::filesystem::path& path) -> std::string {
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

auto Seconds(std::chrono::steady_clock::duration time) -> double {
  return std::chrono::duration<double>(time).count();
}

}  // namespace

DenoiseModelSession::DenoiseModelSession(std::unique_ptr<AiDenoisePlatform> platform,
                                         nn::UtNet2TileLoopOptions          options)
    : platform_(std::move(platform)), options_(options) {}

DenoiseModelSession::~DenoiseModelSession() = default;

void DenoiseModelSession::SetGraphLocator(GraphLocator locator) {
  std::scoped_lock lock(locator_mutex_);
  graph_locator_ = std::move(locator);
}

auto DenoiseModelSession::GraphPath() const -> std::filesystem::path {
  GraphLocator locator;
  {
    std::scoped_lock lock(locator_mutex_);
    locator = graph_locator_;
  }
  AiDenoiseGraphLookup lookup;
  if (locator) {
    lookup = locator();
  } else {
    lookup.problem = "the application sets no model location";
  }
  if (lookup.graph.empty()) {
    throw AiDenoiseError(AiDenoiseErrorKind::ModelNotInstalled,
                         "AI Denoise: the model is not installed: " + lookup.problem);
  }
  return lookup.graph;
}

auto DenoiseModelSession::InstalledModelVersion() -> std::string {
  const auto      path = GraphPath();
  std::error_code error;
  const bool      file = std::filesystem::is_regular_file(path, error);
  if (!file) {
    throw AiDenoiseError(
        AiDenoiseErrorKind::ModelNotInstalled,
        "AI Denoise: the model is not installed: " + PathText(path) + " does not exist");
  }
  const auto size       = std::filesystem::file_size(path, error);
  const auto write_time = std::filesystem::last_write_time(path, error);
  if (error) {
    throw AiDenoiseError(AiDenoiseErrorKind::RunFailed,
                         "AI Denoise: " + PathText(path) + ": " + error.message());
  }
  std::scoped_lock lock(version_mutex_);
  if (installed_ && installed_->path == path && installed_->size == size &&
      installed_->write_time == write_time) {
    return installed_->version;
  }
  std::ifstream     stream(path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());
  if (!stream.good() && !stream.eof()) {
    throw AiDenoiseError(AiDenoiseErrorKind::RunFailed,
                         "AI Denoise: " + PathText(path) + " cannot be read");
  }
  auto version = ComputeSha256Hex(std::as_bytes(std::span(bytes.data(), bytes.size())));
  installed_   = InstalledGraph{path, size, write_time, version};
  return version;
}

auto DenoiseModelSession::Denoise(const PreparedRawInput& cfa, std::string_view model_version,
                                  const std::array<double, 9>& xyz_to_camera,
                                  const TileProgress&          on_tile) -> PreparedRawInput {
  std::scoped_lock lock(run_mutex_);
  if (!platform_) {
    throw AiDenoiseError(AiDenoiseErrorKind::RunFailed,
                         "AI Denoise: this platform has no forward code for the model");
  }
  std::string device;
  try {
    device = platform_->DeviceKey();
  } catch (const std::exception& error) {
    throw AiDenoiseError(AiDenoiseErrorKind::RunFailed, std::string("AI Denoise: ") + error.what());
  }
  if (const auto slow = slow_devices_.find(device); slow != slow_devices_.end()) {
    throw AiDenoiseError(AiDenoiseErrorKind::DeviceTooSlow, slow->second);
  }
  const auto            graph = GraphPath();

  // The input of the model; an input that the preparation rejects runs no model.
  AiDenoisePackedMosaic mosaic;
  std::array<double, 3> weights{};
  try {
    mosaic = PackBayerForAiDenoise(cfa);
    ApplyAiDenoiseInputCurve(mosaic, AiDenoiseInputCurveFor(mosaic.input_means));
    weights = AiDenoiseGainWeights(xyz_to_camera);
  } catch (const std::exception& error) {
    throw AiDenoiseError(AiDenoiseErrorKind::RunFailed, error.what());
  }

  if (session_ && (session_device_ != device || session_version_ != model_version)) {
    ReleaseLocked();
  }
  try {
    const auto started = std::chrono::steady_clock::now();
    if (!session_) {
      session_         = platform_->CreateSession(graph, model_version);
      session_device_  = device;
      session_version_ = std::string(model_version);
      ++stats_.sessions_created;
    }
    const auto              loaded = std::chrono::steady_clock::now();
    std::vector<float>      output(std::size_t{3} * mosaic.OutputWidth() * mosaic.OutputHeight());
    const nn::RgbPlanesView view{output, mosaic.OutputWidth(), mosaic.OutputHeight()};
    AiDenoiseOutputSums     sums(mosaic);
    const auto              record = nn::DenoiseBayerMosaic(
        *session_, mosaic.View(), view,
        [&](const nn::UtNet2TileResult& tile) {
          sums.AddKept(view, tile.kept);
          if (on_tile) {
            on_tile(tile.index + 1, tile.tile_count);
          }
        },
        {},
        [] { return std::chrono::steady_clock::now(); }, options_);
    ++stats_.denoise_runs;
    stats_.last_input_gain = mosaic.curve.gain;
    const double gain      = AiDenoiseGainFactor(weights, mosaic.input_means, sums.Means());
    mosaic.planes          = {};
    auto plane             = AssembleDenoisedPlane(view, mosaic, gain, cfa.linearization);
    output                 = {};
    auto denoised =
        RawInputLoader::FromDenoisedBayer(cfa, std::move(plane), model_version, xyz_to_camera);
    ALCEDO_LOG_INFO(Render) << "AI Denoise: " << mosaic.area_width << " x " << mosaic.area_height
                            << " px in " << record.tile_count << " tiles of " << record.tile_size
                            << ", session " << Seconds(loaded - started) << " s, tile loop "
                            << Seconds(record.loop_time) << " s, total "
                            << Seconds(std::chrono::steady_clock::now() - started)
                            << " s, input gain " << mosaic.curve.gain << " ("
                            << std::log2(mosaic.curve.gain) << " EV), gain match " << gain;
    return denoised;
  } catch (const nn::DenoiseDeviceTooSlowError& error) {
    slow_devices_[device] = error.what();
    ReleaseLocked();
    ALCEDO_LOG_WARN(Render) << error.what() << " (" << device << ")";
    throw AiDenoiseError(AiDenoiseErrorKind::DeviceTooSlow, error.what());
  } catch (const std::exception& error) {
    // A failed run can leave the device removed; the next run creates the session again.
    ReleaseLocked();
    ALCEDO_LOG_ERROR(Render) << "AI Denoise: " << error.what();
    throw AiDenoiseError(AiDenoiseErrorKind::RunFailed, error.what());
  }
}

void DenoiseModelSession::Release() {
  std::scoped_lock lock(run_mutex_);
  ReleaseLocked();
}

void DenoiseModelSession::ReleaseLocked() {
  session_.reset();
  session_device_.clear();
  session_version_.clear();
}

auto DenoiseModelSession::IsSessionLoaded() const -> bool {
  std::scoped_lock lock(run_mutex_);
  return session_ != nullptr;
}

auto DenoiseModelSession::Stats() const -> DenoiseModelSessionStats {
  std::scoped_lock lock(run_mutex_);
  return stats_;
}

auto DenoiseModelSession::IsPlatformAvailable() -> bool {
  std::call_once(platform_check_once_, [this] {
    if (!platform_) {
      ALCEDO_LOG_INFO(Render) << "AI Denoise: this platform has no forward code for the model";
      return;
    }
    try {
      const auto device = platform_->DeviceKey();
      platform_->CheckRuntime();
      platform_available_ = true;
      ALCEDO_LOG_INFO(Render) << "AI Denoise: the model can run on " << device;
    } catch (const std::exception& error) {
      ALCEDO_LOG_WARN(Render) << "AI Denoise: the model cannot run on this computer: "
                              << error.what();
    }
  });
  return platform_available_;
}

auto ProcessDenoiseModelSession() -> const std::shared_ptr<DenoiseModelSession>& {
  static const std::shared_ptr<DenoiseModelSession> session =
      std::make_shared<DenoiseModelSession>(CreateProductAiDenoisePlatform());
  return session;
}

}  // namespace alcedo
