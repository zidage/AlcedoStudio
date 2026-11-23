//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "app/pipeline_service.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "type/type.hpp"

namespace alcedo {

/// The result of @ref WriteAiDenoiseSwitchToStoredHistory.
struct AiDenoiseHistoryWrite {
  /// False when the active Version head already had the requested switch: no commit was written.
  bool         changed            = false;
  /// Creation time of the active Version head after the write (UTC epoch nanoseconds).
  std::int64_t head_created_at_ns = 0;
  /// Display name of the active Version.
  std::string  version_name;
};

/**
 * @brief Write the AI Denoise switch of an image that the editor does not hold into its stored
 *        history: one `raw_decode` SetParameter commit on the active Version (integration plan,
 *        DN8).
 *
 * This is the change of the editor switch (DN7) for `Turn Off AI Denoise` of an export queue
 * item. It reads the history with
 * PipelineMgmtService::LoadHistorySnapshot, replays the active Version head, applies the
 * `raw_decode` write with @p denoise (on with its version, or off when empty), and writes the new
 * history with PipelineMgmtService::PersistHistory in one storage transaction. The Version list
 * does not change.
 *
 * Thread: any thread; storage reads and writes run on the calling thread, so call it off the UI
 * thread.
 * @param denoise The switch on with the model version, or std::nullopt for off.
 * @return `changed` false when the head already has @p denoise (nothing is written).
 * @throws std::runtime_error when the editor holds the image, the image has no history, the
 *         replay or the write fails, or another writer changed the stored history. The stored
 *         history is unchanged on every failure.
 */
auto WriteAiDenoiseSwitchToStoredHistory(PipelineMgmtService& pipelines, sl_element_id_t element_id,
                                         const std::optional<DevelopAiDenoise>& denoise)
    -> AiDenoiseHistoryWrite;

}  // namespace alcedo
