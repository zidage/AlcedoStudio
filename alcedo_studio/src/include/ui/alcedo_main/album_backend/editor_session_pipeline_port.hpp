//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "app/editor_session_ports.hpp"
#include "type/hash_type.hpp"

namespace alcedo {
class PipelineMgmtService;
struct PipelineGuard;
}  // namespace alcedo

namespace alcedo::ui {

/// Dependencies for the image-scoped pipeline guard port. The port owns the
/// acquired guard map; these callbacks only resolve application services.
struct EditorSessionPipelineMappers {
  /// Resolve the service that owns the image-scoped pipeline guard.
  std::function<std::shared_ptr<alcedo::PipelineMgmtService>()>          pipeline_service;
  /// Take editor ownership of one Sleeve element's pipeline (history + live document).
  std::function<std::shared_ptr<alcedo::PipelineGuard>(sl_element_id_t)> load_editor_pipeline_guard;
};

/// Owns the pipeline guards used by one editor session. This port is the only
/// place that binds an image's history and live document for the editor:
/// EnsureLoaded (driven by the history working-state acquisition) takes editor
/// ownership once, and Release returns it. Readers such as the render worker
/// use CurrentGuard and never load; a request for an image this port does not
/// hold is stale and fails instead of rebinding the image from storage.
class EditorSessionPipelinePort final : public alcedo::IEditorPipelinePort {
 public:
  /// Replace the service callbacks used by later guard acquisitions.
  void SetServices(EditorSessionPipelineMappers services);

  /// Acquire the lightweight session handle for an image.
  auto Acquire(sl_element_id_t element_id, std::string* error)
      -> alcedo::EditorPipelineGuardHandle override;
  /// Release the image-scoped guard owned by this port.
  void               Release(const alcedo::EditorPipelineGuardHandle& guard) override;

  /// Return the currently loaded guard without creating a new one.
  [[nodiscard]] auto CurrentGuard(sl_element_id_t element_id) const
      -> std::shared_ptr<alcedo::PipelineGuard>;
  /// Live document for the loaded guard, or nullptr when the image is unloaded.
  [[nodiscard]] auto CurrentDocument(sl_element_id_t element_id) const
      -> const alcedo::PipelineDocument* override;
  /// Resolve the application pipeline service for history operations that
  /// rebuild the already-loaded editor guard.
  [[nodiscard]] auto PipelineMapper() const -> std::shared_ptr<alcedo::PipelineMgmtService>;
  /// Take editor ownership of the image's pipeline once and cache the guard until Release.
  /// Loads are serialized, so concurrent callers never bind the same image twice.
  auto               EnsureLoaded(sl_element_id_t element_id, std::string* error)
      -> std::shared_ptr<alcedo::PipelineGuard>;

  /// Switch the loaded editor pipeline to another Version via root + first-parent
  /// rebuild. Fail closed: the prior Version and pipeline remain published.
  auto CheckoutVersion(sl_element_id_t element_id, const alcedo::Hash128& version_id,
                       std::string* error) -> bool;

 private:
  EditorSessionPipelineMappers                                               services_{};
  mutable std::mutex                                                          mutex_;
  /// Serializes EnsureLoaded's check-then-load; never held by readers.
  std::mutex                                                                  load_mutex_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<alcedo::PipelineGuard>> guards_;
};

}  // namespace alcedo::ui
