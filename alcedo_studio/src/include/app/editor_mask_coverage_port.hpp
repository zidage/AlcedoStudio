//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "app/editor_session_request_ids.hpp"
#include "edit/runtime/mask_coverage_selection.hpp"
#include "type/type.hpp"

/**
 * @file
 * @brief Application seam for coverage-only presentation on the editor render executor (SM8.7).
 *
 * The editor session hands one coverage presentation at a time to this port. The implementation
 * runs it on the executor and worker of the editor frames, with the device of the photo frames,
 * and presents to the coverage channel of the bound viewport. Callers never see the executor, the
 * GPU backend, or the viewport.
 */
namespace alcedo {

/**
 * @brief One coverage presentation of the image the editor holds.
 *
 * @p selection is the candidate (the shared original R8 of the AI Mask edit session and its edge
 * values) or one committed AI Mask source. @p mask holds the invert and opacity of the Mask.
 * @p request_id is the session operation that asked for the presentation; the viewport keeps the
 * newest one.
 */
struct EditorMaskCoverageRequest {
  sl_element_id_t           element_id = 0;
  image_id_t                image_id   = 0;
  /// Open/switch identity of the image; the coverage frame carries it as its session epoch.
  ImageLoadRequestId        image_load_request_id{};
  std::uint64_t             request_id = 0;
  MaskCoverageSelection     selection{};
  MaskCoverageInvertOpacity mask{};
};

/// Terminal state of one coverage presentation job.
enum class EditorMaskCoverageJobStatus : std::uint8_t {
  /// The GPU work completed and the frame waits for the viewport; it replaces the displayed one.
  Presented,
  /// The viewport changed image session first; nothing was displayed.
  DiscardedForImageChange,
  /// Acquisition, edge work, the display write, or the transfer failed; the viewport keeps the
  /// coverage it displayed before. The message has the real error.
  Failed,
};

struct EditorMaskCoverageJobResult {
  EditorMaskCoverageJobStatus status = EditorMaskCoverageJobStatus::Failed;
  std::string                 message;
};

/// Completion of one coverage job. Runs once, on the editor render worker, after the executor is
/// released; the receiver moves the result to its own owner thread.
using EditorMaskCoverageJobCompletion = std::function<void(EditorMaskCoverageJobResult result)>;

/**
 * @brief Coverage-only presentation on the editor executor.
 *
 * Jobs and clears run on the editor worker in call order, between viewport frames, never during
 * one. The port renders no photo, compiles no photo DAG, and keeps every photo result.
 */
class IEditorMaskCoveragePort {
 public:
  virtual ~IEditorMaskCoveragePort() = default;

  /**
   * @brief Accept @p request and present it later on the editor worker.
   * @return Job id, or 0 with @p error set when the port cannot run it now: another coverage job is
   *         accepted, the requested image is not the bound editor image, no viewport is bound, an
   *         export holds the GPU, or the port is shutting down. A rejected request never calls
   *         @p on_complete.
   */
  virtual auto ScheduleMaskCoverage(EditorMaskCoverageRequest       request,
                                    EditorMaskCoverageJobCompletion on_complete, std::string* error)
      -> std::uint64_t             = 0;

  /**
   * @brief Hide the coverage of the bound viewport after every accepted job.
   *
   * A frame that an accepted job presents is not displayed after the clear. No effect without a
   * bound viewport.
   */
  virtual void ClearMaskCoverage() = 0;
};

}  // namespace alcedo
