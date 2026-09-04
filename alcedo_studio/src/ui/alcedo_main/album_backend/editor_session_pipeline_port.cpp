//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

#include <utility>

#include "app/pipeline_service.hpp"
#include "edit/graph/pipeline_document.hpp"

namespace alcedo::ui {

void EditorSessionPipelinePort::SetServices(EditorSessionPipelineMappers services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
}

auto EditorSessionPipelinePort::Acquire(sl_element_id_t element_id, std::string* /*error*/)
    -> alcedo::EditorPipelineGuardHandle {
  // Open remains non-blocking; EnsureLoaded is the explicit first-frame load.
  return {element_id, true};
}

void EditorSessionPipelinePort::Release(const alcedo::EditorPipelineGuardHandle& guard) {
  if (!guard.valid) {
    return;
  }
  std::shared_ptr<alcedo::PipelineGuard>       loaded_guard;
  std::shared_ptr<alcedo::PipelineMgmtService> service;
  {
    std::scoped_lock lock(mutex_);
    auto             it = guards_.find(guard.element_id);
    if (it != guards_.end()) {
      loaded_guard = it->second;
      guards_.erase(it);
    }
    if (services_.pipeline_service) {
      service = services_.pipeline_service();
    }
  }
  if (service && loaded_guard) {
    service->ReleasePipelineUse(std::move(loaded_guard));
  }
}

auto EditorSessionPipelinePort::CurrentGuard(sl_element_id_t element_id) const
    -> std::shared_ptr<alcedo::PipelineGuard> {
  std::scoped_lock lock(mutex_);
  auto             it = guards_.find(element_id);
  return it == guards_.end() ? nullptr : it->second;
}

auto EditorSessionPipelinePort::CurrentDocument(sl_element_id_t element_id) const
    -> const alcedo::PipelineDocument* {
  auto guard = CurrentGuard(element_id);
  if (!guard || !guard->document_) {
    return nullptr;
  }
  return guard->document_.get();
}

auto EditorSessionPipelinePort::PipelineMapper() const
    -> std::shared_ptr<alcedo::PipelineMgmtService> {
  std::scoped_lock lock(mutex_);
  return services_.pipeline_service ? services_.pipeline_service() : nullptr;
}

auto EditorSessionPipelinePort::EnsureLoaded(sl_element_id_t element_id, std::string* error)
    -> std::shared_ptr<alcedo::PipelineGuard> {
  {
    std::scoped_lock lock(mutex_);
    auto             it = guards_.find(element_id);
    if (it != guards_.end()) {
      return it->second;
    }
  }

  std::function<std::shared_ptr<alcedo::PipelineGuard>(sl_element_id_t)> guard_loader;
  std::shared_ptr<alcedo::PipelineMgmtService>                           service;
  {
    std::scoped_lock lock(mutex_);
    guard_loader = services_.load_editor_pipeline_guard;
    if (services_.pipeline_service) {
      service = services_.pipeline_service();
    }
  }
  if (!guard_loader && !service) {
    if (error) *error = "Pipeline service is unavailable";
    return nullptr;
  }
  try {
    auto guard = guard_loader ? guard_loader(element_id) : service->LoadEditorPipeline(element_id);
    if (!guard || !guard->pipeline_) {
      if (error) *error = "Failed to load pipeline for editor session";
      return nullptr;
    }
    std::scoped_lock lock(mutex_);
    guards_[element_id] = guard;
    return guard;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
  } catch (...) {
    if (error) *error = "Unknown pipeline load failure";
  }
  return nullptr;
}

auto EditorSessionPipelinePort::CheckoutVersion(sl_element_id_t        element_id,
                                                const alcedo::Hash128& version_id,
                                                std::string*           error) -> bool {
  auto guard = EnsureLoaded(element_id, error);
  if (!guard) {
    return false;
  }
  std::shared_ptr<alcedo::PipelineMgmtService> service;
  {
    std::scoped_lock lock(mutex_);
    if (services_.pipeline_service) {
      service = services_.pipeline_service();
    }
  }
  if (!service) {
    if (error) *error = "Pipeline service is unavailable for Version checkout";
    return false;
  }
  return service->CheckoutVersion(guard, version_id, error);
}

}  // namespace alcedo::ui
