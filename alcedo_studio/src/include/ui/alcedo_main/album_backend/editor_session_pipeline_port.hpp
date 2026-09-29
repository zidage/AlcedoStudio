//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "app/editor_session_ports.hpp"
#include "app/editor_working_document.hpp"
#include "app/pipeline_root_state.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"

namespace alcedo::ui {

/// Dependencies for the editor lease port. The port owns the table of held leases; these
/// callbacks only resolve application services.
struct EditorSessionPipelineMappers {
  /// Resolve the service that owns the image histories and the editor lease table.
  std::function<std::shared_ptr<alcedo::PipelineMgmtService>()> pipeline_service;
  /// Take the editor lease of one Sleeve element. When empty, the lease is taken from
  /// pipeline_service. Harnesses without storage install an in-memory history here.
  std::function<alcedo::EditorHistoryLease(sl_element_id_t)>    acquire_editor_lease;
};

/// History, root, and working document of one image while the editor holds its lease.
struct EditorImageLease {
  /// The only CommitGraph of the image; the Mini-Git working history appends to it.
  std::shared_ptr<alcedo::CommitGraph>           graph_;
  /// Decoded immutable root; the start of every replay.
  std::shared_ptr<const alcedo::LoadedRootState> root_;
  /// Working document and its preview publication.
  std::shared_ptr<alcedo::EditorWorkingDocument> document_;
};

/**
 * @brief Holds the editor leases of one editor session and exposes their preview snapshots.
 *
 * This port is the only place that takes and returns an editor lease. The history working state
 * takes it once per open image (@ref AcquireLease) and returns it when the image is released
 * (@ref ReleaseLease). Readers such as the render dispatch and the GUI read
 * @ref CurrentPreview and never load: an image without a held lease has no preview, so a stale
 * request fails instead of loading the image from storage.
 */
class EditorSessionPipelinePort final : public alcedo::IEditorPipelinePort {
 public:
  /// Replace the service callbacks used by later lease acquisitions.
  void SetServices(EditorSessionPipelineMappers services);

  /**
   * @brief Take the editor lease of @p element_id and bind its working document.
   * @return The held lease, or nullopt with @p error set when the lease is already held by this
   *         port, the service is unavailable, or the service refuses the lease.
   */
  auto AcquireLease(sl_element_id_t element_id, std::string* error)
      -> std::optional<EditorImageLease>;

  /// Return the lease of @p element_id to the service. No effect when this port does not hold it.
  void               ReleaseLease(sl_element_id_t element_id);

  [[nodiscard]] auto CurrentPreview(sl_element_id_t element_id) const
      -> std::shared_ptr<const alcedo::PipelineGraphSnapshot> override;

  /// Resolve the application pipeline service for history persistence and publication.
  [[nodiscard]] auto PipelineMapper() const -> std::shared_ptr<alcedo::PipelineMgmtService>;

 private:
  /// Return the service lease of @p element_id; reports a failed element JSON write.
  static void                  ReturnLeaseToService(alcedo::PipelineMgmtService& service,
                                                    sl_element_id_t              element_id);

  EditorSessionPipelineMappers services_{};
  mutable std::mutex           mutex_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<alcedo::EditorWorkingDocument>> leases_;
};

}  // namespace alcedo::ui
