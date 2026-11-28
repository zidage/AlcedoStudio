//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "decoders/processor/nn/utnet2_tile_loop.hpp"
#include "edit/input/ai_denoise_error.hpp"
#include "edit/input/ai_denoise_platform.hpp"
#include "edit/input/prepared_raw_input.hpp"

namespace alcedo {

/// Where the FP16 graph of the installed AI Denoise model is (integration plan, DN12).
struct AiDenoiseGraphLookup {
  /// The graph file; empty when no usable model is installed.
  std::filesystem::path graph;
  /// Why there is no graph (English, part of the `ModelNotInstalled` error); empty with a graph.
  std::string           problem;
};

/// Counters of a @ref DenoiseModelSession, for the tests and the log.
struct DenoiseModelSessionStats {
  std::uint64_t denoise_runs     = 0;
  std::uint64_t sessions_created = 0;
  /// The gain of the input exposure curve of the last run (1: identity).
  double        last_input_gain  = 1.0;
};

/**
 * @brief The process-wide owner of the AI Denoise platform session (integration plan, 2.3, DN6).
 *
 * The editor render and the export call @ref Denoise one run at a time (a mutex). The platform
 * session is created at the first run and kept until @ref Release (the editor GPU release, the
 * editor close). A run error releases it, so that a removed device is not used again. A
 * slow-device result is kept for the device until the process ends; a later run on that device
 * throws at once and creates no session.
 *
 * Thread safety: every member function can be called from any thread.
 */
class DenoiseModelSession {
 public:
  /// Finds the graph of the installed model package. Called at each check of the installed
  /// model (each render with AI Denoise), from any thread.
  using GraphLocator = std::function<AiDenoiseGraphLookup()>;

  /**
   * @param platform Null on a platform without forward code: a run then throws `RunFailed`.
   * @param options Tile loop options; the product uses the defaults.
   */
  explicit DenoiseModelSession(std::unique_ptr<AiDenoisePlatform> platform,
                               nn::UtNet2TileLoopOptions          options = {});
  ~DenoiseModelSession();

  DenoiseModelSession(const DenoiseModelSession&)                                  = delete;
  auto               operator=(const DenoiseModelSession&) -> DenoiseModelSession& = delete;

  /// Sets the locator of the graph. The application finds the graph in the AI Denoise package of
  /// its AI model folder (`<folder>/alcedo-ai-denoise-v<N>/`, DN12).
  void               SetGraphLocator(GraphLocator locator);

  /**
   * @brief The installed model version: the SHA-256 of the graph file, 64 lowercase hexadecimal
   *        digits (D10). Read again when the size or the write time of the file changes.
   * @throws AiDenoiseError `ModelNotInstalled` when no locator is set or it finds no graph (with
   *         its reason), or the file is absent; `RunFailed` when the file cannot be read.
   */
  [[nodiscard]] auto InstalledModelVersion() -> std::string;

  /// Receives the tiles of a run: @p completed of @p total tiles finished (DN14). Called on the
  /// thread of @ref Denoise after each tile.
  using TileProgress = std::function<void(int completed, int total)>;

  /**
   * @brief Denoise the full-size Bayer input @p cfa with the installed model of version
   *        @p model_version and return the denoised input of DN5
   *        (`RawInputLoader::FromDenoisedBayer`).
   *
   * Runs the input preparation, the tile loop with the gain match sums, the gain match, and the
   * plane assembly. Waits while another run is in progress. @p xyz_to_camera is the camera matrix
   * of the render at D65 (ResolveDevelopXyzToCameraD65); the gain match weights come from it.
   * @p on_tile (it can be empty) gets tile k of n after each tile of the loop.
   * @throws AiDenoiseError of each kind; no partial result.
   */
  [[nodiscard]] auto Denoise(const PreparedRawInput& cfa, std::string_view model_version,
                             const std::array<double, 9>& xyz_to_camera,
                             const TileProgress&          on_tile = {}) -> PreparedRawInput;

  /// Ends the platform session and frees its device memory. Waits for a run in progress. The
  /// slow-device results stay.
  void               Release();

  /**
   * @brief True when this platform can run the model (D13): it has a platform, a device for the
   *        model, and its runtime libraries load. The editor shows the switch only then.
   *
   * The first call checks; later calls return its result (the libraries and the adapters of a
   * process do not change).
   */
  [[nodiscard]] auto IsPlatformAvailable() -> bool;

  [[nodiscard]] auto IsSessionLoaded() const -> bool;
  [[nodiscard]] auto Stats() const -> DenoiseModelSessionStats;

 private:
  void                               ReleaseLocked();

  std::unique_ptr<AiDenoisePlatform> platform_;
  nn::UtNet2TileLoopOptions          options_;
  std::once_flag                     platform_check_once_;
  bool                               platform_available_ = false;

  mutable std::mutex                 locator_mutex_;
  GraphLocator                       graph_locator_;

  struct InstalledGraph {
    std::filesystem::path           path;
    std::uintmax_t                  size = 0;
    std::filesystem::file_time_type write_time{};
    std::string                     version;
  };
  std::mutex                             version_mutex_;
  std::optional<InstalledGraph>          installed_;

  // The run state, under run_mutex_.
  mutable std::mutex                     run_mutex_;
  std::unique_ptr<nn::UtNet2TileSession> session_;
  std::string                            session_device_;
  std::string                            session_version_;
  std::map<std::string, std::string>     slow_devices_;
  DenoiseModelSessionStats               stats_{};

  [[nodiscard]] auto                     GraphPath() const -> std::filesystem::path;
};

/**
 * @brief The one @ref DenoiseModelSession of the process, with the platform of this build.
 *
 * The executors of the editor, the export, the thumbnails, and the semantic input images use it
 * (the last two never run the model: they decode reduced). The application sets its graph locator.
 *
 * The instance is a static of this (static) library, so each binary that links it has its own:
 * on Windows the EditPipeline DLL and the application. Code outside EditPipeline uses
 * PipelineExecutor::ProcessAiDenoiseSession(), which returns the instance of the executors.
 */
[[nodiscard]] auto ProcessDenoiseModelSession() -> const std::shared_ptr<DenoiseModelSession>&;

}  // namespace alcedo
