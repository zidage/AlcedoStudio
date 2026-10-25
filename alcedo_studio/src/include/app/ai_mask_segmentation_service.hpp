//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sidecar_client/dto/segmentation.hpp"
#include "sidecar_client/segmentation_client.hpp"

namespace alcedo {

/// Timeouts of the segmentation calls. A cold DirectML encoder load takes about 3 s; the Core ML
/// models can need longer on the first load after an operating system update.
struct AiMaskSegmentationTimeouts {
  std::chrono::milliseconds load{120000};
  std::chrono::milliseconds start{30000};
  std::chrono::milliseconds decode{10000};
  /// The first Subject request runs S3OD (about 0.2 s on DirectML, 0.5 s on Core ML; the first
  /// DirectML run of a process takes longer).
  std::chrono::milliseconds task{30000};
  std::chrono::milliseconds control{5000};
};

/**
 * @brief The model calls of an AI Mask edit session on the current segmentation session (SM5.2).
 *
 * The AI Mask edit session calls the runner for each step that needs a model output. Each call
 * blocks until the model answers. A failure is reported in the result status; the runner throws
 * no exception for a model failure.
 */
class IAiMaskSegmentationRunner {
 public:
  virtual ~IAiMaskSegmentationRunner()                                              = default;

  /// Decodes @p prompt on the current segmentation session.
  virtual auto Decode(const SegmentationPrompt& prompt) -> SegmentationDecodeResult = 0;
  /// Runs the task mask @p task. @p person_index is the People list index for kPerson and empty
  /// for the other tasks.
  virtual auto RunTask(SegmentationTask task, std::optional<uint32_t> person_index)
      -> SegmentationTaskResult = 0;
};

/**
 * @brief One activation of the AI Mask tool over the sidecar segmentation service (SM3.A to
 * SM3.C).
 *
 * The service holds a sidecar lease, so the sidecar process keeps running while the tool is
 * open, and one segmentation client with its own persistent channel. It owns the identity of the
 * current segmentation session; the sidecar owns the models and the image embedding. The caller
 * owns the selection and passes the full prompt to Decode().
 *
 * Close() ends the session, unloads the models (lab decision U6: the models unload when the user
 * goes back to adjusting), and then releases the lease. The destructor calls Close().
 *
 * Threading: not thread-safe. The owner calls every method from one thread. Each method blocks
 * until the sidecar answers or its timeout passes.
 */
class AiMaskSegmentationService final : public IAiMaskSegmentationRunner {
 public:
  /// @p sidecar_lease keeps the sidecar process running (AiSidecarRuntimeService::AcquireLease).
  /// @p client talks to that process.
  AiMaskSegmentationService(std::shared_ptr<void>                               sidecar_lease,
                            std::unique_ptr<sidecar_client::SegmentationClient> client,
                            AiMaskSegmentationTimeouts                          timeouts = {});
  ~AiMaskSegmentationService() override;

  AiMaskSegmentationService(const AiMaskSegmentationService&)                    = delete;
  auto operator=(const AiMaskSegmentationService&) -> AiMaskSegmentationService& = delete;

  /// Loads the model set from @p model_directory on @p device ("" for the platform default).
  auto LoadModels(const std::filesystem::path& model_directory, const std::string& device = {})
      -> SegmentationCallStatus;
  /// The load state of the sidecar: the model set identity, the directory, the device, and the
  /// sessions. Fails without a sidecar call when the service is closed.
  auto Status() -> SegmentationStatus;
  /// Ends the current session, then starts a session on @p image. A non-empty @p image_key
  /// names the image content (SM4 analysis render identity): the sidecar uses its cached
  /// embedding of that key, or caches the new one. When the end fails, no session starts and the
  /// end failure is returned.
  auto StartImage(const SegmentationRgb8Image& image, const std::string& image_key = {})
      -> SegmentationSessionResult;
  /// Ends the current session, then starts a session from the cached embedding of @p image_key
  /// without sending pixels. An uncached key returns `image_not_cached` and keeps no session; the
  /// caller then renders the image and calls StartImage() with the same key. An empty key fails
  /// without a sidecar call.
  auto               StartCachedImage(const std::string& image_key) -> SegmentationSessionResult;
  /// Decodes @p prompt on the current session. Fails without a sidecar call when no session
  /// exists.
  auto               Decode(const SegmentationPrompt& prompt) -> SegmentationDecodeResult override;
  /// Runs the task mask @p task on the current session (SM3.C, SM3.D). @p person_index is the
  /// index into the People list for kPerson; the other tasks take no index. Fails without a
  /// sidecar call when no session exists or the service is closed.
  auto               RunTask(SegmentationTask task, std::optional<uint32_t> person_index)
      -> SegmentationTaskResult override;
  /// Ends the current session. Succeeds without a sidecar call when no session exists.
  auto               EndImage() -> SegmentationCallStatus;
  /// Ends the session, unloads the models, and releases the lease. Later calls fail without a
  /// sidecar call. A second Close() succeeds without a call. Returns the first failure.
  auto               Close() -> SegmentationCallStatus;

  [[nodiscard]] auto HasSession() const -> bool { return !session_id_.empty(); }
  [[nodiscard]] auto IsClosed() const -> bool { return closed_; }

 private:
  [[nodiscard]] static auto LocalFailure(std::string message) -> SegmentationCallStatus;
  auto StartSession(const SegmentationRgb8Image& image, const std::string& image_key)
      -> SegmentationSessionResult;

  std::shared_ptr<void>     sidecar_lease_;
  std::unique_ptr<sidecar_client::SegmentationClient> client_;
  AiMaskSegmentationTimeouts                          timeouts_;
  std::string                                         session_id_;
  bool                                                closed_ = false;
};

}  // namespace alcedo
