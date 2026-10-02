//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_comparison_service.hpp"

#include <utility>

namespace alcedo {

EditorComparisonService::EditorComparisonService(Dependencies dependencies)
    : dependencies_(std::move(dependencies)) {}

EditorComparisonService::~EditorComparisonService() {
  if (activity_ && activity_->job_id != 0 && dependencies_.images) {
    dependencies_.images->CancelImages(activity_->job_id);
  }
}

void EditorComparisonService::Open(std::uint64_t operation_id, ImageTarget target,
                                   std::shared_ptr<const PipelineGraphSnapshot> captured_current,
                                   EditorComparisonKind                         kind) {
  Activity activity;
  activity.operation_id     = operation_id;
  activity.target           = target;
  activity.captured_current = std::move(captured_current);
  activity.kind             = kind;
  activity_                 = std::move(activity);
  RenderSelectedPair();
}

auto EditorComparisonService::SelectSources(EditorComparisonKind kind, const EditorComparisonSource& a,
                                            const EditorComparisonSource& b, std::string* error)
    -> bool {
  if (!activity_) {
    if (error != nullptr) *error = "No comparison is open";
    return false;
  }
  if (Rendering()) {
    if (error != nullptr) *error = "Wait until the comparison images are rendered";
    return false;
  }
  const bool sources_changed = !(activity_->a == a) || !(activity_->b == b);
  activity_->kind            = kind;
  activity_->a               = a;
  activity_->b               = b;
  if (sources_changed) {
    RenderSelectedPair();
    return true;
  }
  // A kind-only change keeps the pair and its images.
  std::scoped_lock lock(publish_mutex_);
  published_.kind = kind;
  return true;
}

auto EditorComparisonService::Retry(std::string* error) -> bool {
  if (!activity_) {
    if (error != nullptr) *error = "No comparison is open";
    return false;
  }
  if (Rendering()) {
    if (error != nullptr) *error = "Wait until the comparison images are rendered";
    return false;
  }
  RenderSelectedPair();
  return true;
}

auto EditorComparisonService::Close() -> bool {
  if (!activity_) {
    return false;
  }
  const auto job_id = activity_->job_id;
  activity_.reset();
  if (job_id != 0 && dependencies_.images) {
    dependencies_.images->CancelImages(job_id);
  }
  Publish(EditorComparisonStatus::Inactive, 0, {});
  return true;
}

void EditorComparisonService::HandleImagesFinished(std::uint64_t           job_id,
                                                   EditorImageRenderResult result) {
  if (!activity_ || job_id == 0 || activity_->job_id != job_id) {
    // The comparison closed, or its selection changed, after this job was accepted.
    return;
  }
  activity_->job_id = 0;
  if (result.status == EditorImageRenderStatus::Completed && result.images.size() == 2) {
    Publish(EditorComparisonStatus::Ready, job_id, {}, std::move(result.images));
    return;
  }
  std::string reason = result.message;
  if (reason.empty()) {
    reason = result.status == EditorImageRenderStatus::Cancelled
                 ? "Rendering the comparison images was cancelled"
                 : "Rendering the comparison images failed";
  }
  Publish(EditorComparisonStatus::Failed, 0, std::move(reason));
}

auto EditorComparisonService::state() const -> EditorComparisonState {
  std::scoped_lock lock(publish_mutex_);
  return published_;
}

auto EditorComparisonService::TakeImages(std::uint64_t pair_id) -> std::vector<RenderedPipelineImage> {
  std::scoped_lock lock(publish_mutex_);
  if (pair_id == 0 || published_.pair_id != pair_id ||
      published_.status != EditorComparisonStatus::Ready) {
    return {};
  }
  return std::exchange(ready_images_, {});
}

void EditorComparisonService::RenderSelectedPair() {
  auto& activity = *activity_;
  if (dependencies_.history == nullptr) {
    Publish(EditorComparisonStatus::Failed, 0, "The image history is not available");
    return;
  }
  EditorComparisonInputPair pair;
  std::string               error;
  if (!dependencies_.history->BuildComparisonInputs(activity.target.guard,
                                                    activity.captured_current, activity.a,
                                                    activity.b, &pair, &error)) {
    Publish(EditorComparisonStatus::Failed, 0,
            error.empty() ? "The comparison images could not be prepared" : std::move(error));
    return;
  }
  if (!dependencies_.images) {
    Publish(EditorComparisonStatus::Failed, 0, "Comparison rendering is not available");
    return;
  }

  EditorImageRenderRequest request;
  request.element_id            = activity.target.element_id;
  request.image_id              = activity.target.image_id;
  request.image_load_request_id = activity.target.image_load_request;
  request.snapshots             = {pair.a.snapshot, pair.b.snapshot};
  // The completion runs on the render worker; it only moves the result to the owner thread,
  // where HandleImagesFinished compares the job id with the selected pair. The worker can finish
  // before ScheduleImages returns here, so the job id is read on the owner thread, after this
  // function stored it.
  auto job_cell                 = std::make_shared<std::uint64_t>(0);
  auto post                     = dependencies_.post_to_owner;
  const auto job_id             = dependencies_.images->ScheduleImages(
      std::move(request),
      [this, post, job_cell](EditorImageRenderResult result) {
        if (!post) {
          return;
        }
        post([this, job_cell, result = std::move(result)]() mutable {
          HandleImagesFinished(*job_cell, std::move(result));
        });
      },
      &error);
  if (job_id == 0) {
    Publish(EditorComparisonStatus::Failed, 0,
            error.empty() ? "The comparison images could not be scheduled" : std::move(error));
    return;
  }
  *job_cell       = job_id;
  activity.job_id = job_id;
  // A new selection clears the previous pair from display until both new images exist.
  Publish(EditorComparisonStatus::Rendering, job_id, {});
}

void EditorComparisonService::Publish(EditorComparisonStatus status, std::uint64_t pair_id,
                                      std::string error, std::vector<RenderedPipelineImage> images) {
  EditorComparisonState next;
  next.status  = status;
  next.pair_id = pair_id;
  next.error   = std::move(error);
  if (activity_) {
    next.kind         = activity_->kind;
    next.a            = activity_->a;
    next.b            = activity_->b;
    next.operation_id = activity_->operation_id;
  }
  std::vector<RenderedPipelineImage> released;
  {
    std::scoped_lock lock(publish_mutex_);
    published_ = std::move(next);
    released   = std::exchange(ready_images_, std::move(images));
  }
  // Pixels of a replaced or closed pair are released outside the lock.
}

auto EditorComparisonService::Rendering() const -> bool {
  return activity_ && activity_->job_id != 0;
}

}  // namespace alcedo
