//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/project_launch_coordinator.hpp"

#include <utility>

namespace alcedo::ui {

ProjectLaunchCoordinator::ProjectLaunchCoordinator(ProjectModule* project, QObject* parent)
    : QObject(parent), project_(project) {
  launch_timer_.setSingleShot(true);
  launch_timer_.setInterval(kLaunchRepaintDelayMs);
  connect(&launch_timer_, &QTimer::timeout, this, &ProjectLaunchCoordinator::RunPendingLaunch);
  connect(this, &ProjectLaunchCoordinator::LaunchStateChanged, this,
          &ProjectLaunchCoordinator::launchStateChanged);
  if (project_ != nullptr) {
    connect(project_, &ProjectModule::ProjectLoadStateChanged, this,
            &ProjectLaunchCoordinator::OnProjectLoadStateChanged);
    connect(project_, &ProjectModule::ProjectChanged, this,
            &ProjectLaunchCoordinator::OnProjectChanged);
  }
}

auto ProjectLaunchCoordinator::loading_overlay_visible() const -> bool {
  return launch_pending_ || (project_ != nullptr && project_->ProjectLoading() &&
                             project_->ProjectLoadEntryMode() == QStringLiteral("enter"));
}

auto ProjectLaunchCoordinator::launch_busy() const -> bool {
  return loading_overlay_visible() || pending_request_.has_value();
}

bool ProjectLaunchCoordinator::BeginPromptOpen() {
  return Begin(LaunchRequest{LaunchKind::kPromptOpen, {}, {}});
}

bool ProjectLaunchCoordinator::BeginPromptCreate() {
  return Begin(LaunchRequest{LaunchKind::kPromptCreate, {}, {}});
}

bool ProjectLaunchCoordinator::BeginOpen(const QString& projectUrlOrPath) {
  return Begin(LaunchRequest{LaunchKind::kOpen, projectUrlOrPath, {}});
}

bool ProjectLaunchCoordinator::BeginCreate(const QString& folderUrlOrPath,
                                           const QString& projectName) {
  return Begin(LaunchRequest{LaunchKind::kCreate, folderUrlOrPath, projectName});
}

auto ProjectLaunchCoordinator::Begin(LaunchRequest request) -> bool {
  if (project_ == nullptr || project_->AcceleratorPreparing() || pending_request_.has_value()) {
    return false;
  }
  restore_welcome_on_launch_failure_ = !project_->ProjectEntered();
  pending_request_                   = std::move(request);
  // The overlay shows in the open window before the welcome surface closes, so the empty
  // Library never shows between the two.
  StartPendingLaunch();
  SetLaunchState(true, true);
  return true;
}

void ProjectLaunchCoordinator::StartPendingLaunch() {
  if (!pending_request_.has_value() || launch_timer_.isActive()) {
    return;
  }
  SetLaunchState(true, welcome_dismissed_for_launch_);
  launch_timer_.start();
}

void ProjectLaunchCoordinator::RunPendingLaunch() {
  if (!pending_request_.has_value() || project_ == nullptr) {
    return;
  }
  const LaunchRequest request = std::move(*pending_request_);
  pending_request_.reset();
  bool started = false;
  switch (request.kind) {
    case LaunchKind::kPromptOpen:
      started = project_->PromptAndLoadProject();
      break;
    case LaunchKind::kPromptCreate:
      started = project_->PromptAndCreateProject();
      break;
    case LaunchKind::kOpen:
      started = project_->LoadProject(request.path);
      break;
    case LaunchKind::kCreate:
      started = project_->CreateProjectInFolderNamed(request.path, request.name);
      break;
  }
  const bool restore_welcome         = restore_welcome_on_launch_failure_;
  restore_welcome_on_launch_failure_ = false;
  if (!started && !project_->ProjectLoading()) {
    launch_pending_ = false;
    if (restore_welcome) {
      welcome_dismissed_for_launch_ = false;
    }
  }
  // The queued request is gone, so launchBusy changes even when the two flags stay.
  emit LaunchStateChanged();
  emit LaunchRequestFinished(started);
}

bool ProjectLaunchCoordinator::ContinueWelcomeProject() {
  if (project_ == nullptr) {
    return false;
  }
  const bool load_running = project_->ProjectLoading();
  if (!project_->EnterLoadedProject()) {
    return false;
  }
  if (load_running) {
    SetLaunchState(launch_pending_, true);
  }
  return true;
}

void ProjectLaunchCoordinator::OnProjectLoadStateChanged() {
  // The enter-mode load shows the overlay through the load state from here on.
  launch_pending_ = false;
  if (project_ == nullptr || !project_->ProjectLoading()) {
    welcome_dismissed_for_launch_ = false;
  }
  // The overlay also depends on the load state, so publish even when the two flags stay.
  emit LaunchStateChanged();
}

void ProjectLaunchCoordinator::OnProjectChanged() { SetLaunchState(false, false); }

void ProjectLaunchCoordinator::SetLaunchState(bool launch_pending,
                                              bool welcome_dismissed_for_launch) {
  if (launch_pending_ == launch_pending &&
      welcome_dismissed_for_launch_ == welcome_dismissed_for_launch) {
    return;
  }
  launch_pending_               = launch_pending;
  welcome_dismissed_for_launch_ = welcome_dismissed_for_launch;
  emit LaunchStateChanged();
}

}  // namespace alcedo::ui
