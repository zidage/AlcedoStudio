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
struct EditorSessionPipelineServices {
  /// Resolve the service that owns the image-scoped pipeline guard.
  std::function<std::shared_ptr<alcedo::PipelineMgmtService>()>          pipeline_service;
  /// Load the serialized editor pipeline for one Sleeve element.
  std::function<std::shared_ptr<alcedo::PipelineGuard>(sl_element_id_t)> load_editor_pipeline_guard;
};

/// Owns the pipeline guards used by one editor session. Acquire is deliberately
/// lightweight; EnsureLoaded is the explicit first-frame operation that may
/// resolve a real pipeline from the project service.
class EditorSessionPipelinePort final : public alcedo::IEditorPipelinePort {
 public:
  /// Replace the service callbacks used by later guard acquisitions.
  void SetServices(EditorSessionPipelineServices services);

  /// Acquire the lightweight session handle for an image.
  auto Acquire(sl_element_id_t element_id, std::string* error)
      -> alcedo::EditorPipelineGuardHandle override;
  /// Release the image-scoped guard owned by this port.
  void               Release(const alcedo::EditorPipelineGuardHandle& guard) override;

  /// Return the currently loaded guard without creating a new one.
  [[nodiscard]] auto CurrentGuard(sl_element_id_t element_id) const
      -> std::shared_ptr<alcedo::PipelineGuard>;
  /// Resolve and cache the real pipeline guard used by history and rendering.
  auto EnsureLoaded(sl_element_id_t element_id, std::string* error)
      -> std::shared_ptr<alcedo::PipelineGuard>;

  /// Switch the loaded editor pipeline to another Version via root + first-parent
  /// rebuild. Fail closed: the prior Version and pipeline remain published.
  auto CheckoutVersion(sl_element_id_t element_id, const alcedo::Hash128& version_id,
                       std::string* error) -> bool;

 private:
  EditorSessionPipelineServices                                               services_{};
  mutable std::mutex                                                          mutex_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<alcedo::PipelineGuard>> guards_;
};

}  // namespace alcedo::ui
