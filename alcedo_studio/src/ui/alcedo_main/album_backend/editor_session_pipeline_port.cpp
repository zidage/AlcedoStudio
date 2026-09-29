//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

#include <QtGlobal>
#include <exception>
#include <utility>

namespace alcedo::ui {

void EditorSessionPipelinePort::SetServices(EditorSessionPipelineMappers services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
}

auto EditorSessionPipelinePort::AcquireLease(sl_element_id_t element_id, std::string* error)
    -> std::optional<EditorImageLease> {
  std::function<alcedo::EditorHistoryLease(sl_element_id_t)> acquire;
  std::shared_ptr<alcedo::PipelineMgmtService>               service;
  {
    std::scoped_lock lock(mutex_);
    if (leases_.contains(element_id)) {
      if (error) *error = "The editor already holds image " + std::to_string(element_id);
      return std::nullopt;
    }
    acquire = services_.acquire_editor_lease;
    if (services_.pipeline_service) {
      service = services_.pipeline_service();
    }
  }
  if (!acquire && !service) {
    if (error) *error = "Pipeline service is unavailable";
    return std::nullopt;
  }
  bool service_lease_taken = false;
  try {
    auto lease          = acquire ? acquire(element_id) : service->AcquireEditorLease(element_id);
    service_lease_taken = !acquire;
    EditorImageLease held{
        .graph_ = std::make_shared<alcedo::CommitGraph>(std::move(lease.graph_)),
        .root_  = std::move(lease.root_),
        .document_ =
            std::make_shared<alcedo::EditorWorkingDocument>(element_id, std::move(lease.document_)),
    };
    std::scoped_lock lock(mutex_);
    leases_[element_id] = held.document_;
    return held;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
  } catch (...) {
    if (error) *error = "Unknown editor lease failure";
  }
  // A failure after the service granted the lease (building the working document) returns it.
  if (service_lease_taken) {
    ReturnLeaseToService(*service, element_id);
  }
  return std::nullopt;
}

void EditorSessionPipelinePort::ReleaseLease(sl_element_id_t element_id) {
  std::shared_ptr<alcedo::PipelineMgmtService> service;
  {
    std::scoped_lock lock(mutex_);
    if (leases_.erase(element_id) == 0) {
      return;
    }
    if (!services_.acquire_editor_lease && services_.pipeline_service) {
      service = services_.pipeline_service();
    }
  }
  if (service) {
    ReturnLeaseToService(*service, element_id);
  }
}

void EditorSessionPipelinePort::ReturnLeaseToService(alcedo::PipelineMgmtService& service,
                                                     sl_element_id_t              element_id) {
  // The lease ends inside ReleaseEditorLease before its compatibility write of the element
  // pipeline JSON. A failed write leaves the history in storage intact and must not escape a
  // release (a destructor path returns leases), so it is reported here.
  try {
    service.ReleaseEditorLease(element_id);
  } catch (const std::exception& ex) {
    qWarning("Editor lease of image %llu: element pipeline JSON was not written: %s",
             static_cast<unsigned long long>(element_id), ex.what());
  }
}

auto EditorSessionPipelinePort::CurrentPreview(sl_element_id_t element_id) const
    -> std::shared_ptr<const alcedo::PipelineGraphSnapshot> {
  std::scoped_lock lock(mutex_);
  const auto       it = leases_.find(element_id);
  return it == leases_.end() ? nullptr : it->second->CurrentPreview();
}

auto EditorSessionPipelinePort::PipelineMapper() const
    -> std::shared_ptr<alcedo::PipelineMgmtService> {
  std::scoped_lock lock(mutex_);
  return services_.pipeline_service ? services_.pipeline_service() : nullptr;
}

}  // namespace alcedo::ui
