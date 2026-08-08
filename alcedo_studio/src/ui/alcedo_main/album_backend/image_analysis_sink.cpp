//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/image_analysis_controller.hpp"

#include <utility>
#include <vector>

#include "ai/ai_description.hpp"
#include "ai/ai_rating.hpp"
#include "ui/alcedo_main/album_backend/image_controller.hpp"
#include "ui/alcedo_main/album_backend/project_db_write_barrier.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"

namespace alcedo::ui {

class AlbumImageAnalysisSink final : public IImageAnalysisSink {
 public:
  AlbumImageAnalysisSink(ProjectModule* project, ImageController* images, StatsEngine* stats,
                         ProjectDbWriteBarrier* barrier)
      : project_(project), images_(images), stats_(stats), queue_(*barrier) {}

  static constexpr const char* kDescribeTaskId = "describe";
  static constexpr const char* kScoreTaskId    = "rate";

  bool PersistUnderstanding(const ImageAnalysisItemResult& result) override {
    queue_.Submit([this, result] { DoPersistUnderstanding(result); });
    return true;
  }

  size_t PersistUnderstandings(const std::vector<ImageAnalysisItemResult>& results) override {
    const size_t n = results.size();
    queue_.Submit([this, results] { DoPersistUnderstandings(results); });
    return n;
  }

  bool PersistRatingReasons(const ImageAnalysisItemResult& result) override {
    queue_.Submit([this, result] { DoPersistRatingReasons(result); });
    return true;
  }

  bool ApplyStarRating(uint32_t elementId, uint32_t imageId, int rating) override {
    queue_.Submit(
        [this, elementId, imageId, rating] { DoApplyStarRating(elementId, imageId, rating); });
    return true;
  }

  void FlushPendingStarRatings() override { queue_.Submit([this] { DoFlushPendingStarRatings(); }); }

  void NotifySearchDocumentChanged() override { queue_.Submit([this] { DoNotifySearchDocumentChanged(); }); }

  bool HasPendingWrites() const override { return queue_.IsPending(); }
  void SetOnDrainComplete(std::function<void()> cb) override {
    queue_.SetOnDrainComplete(std::move(cb));
  }
  void FlushPendingWrites() override { queue_.Drain(); }

 private:
  void DoPersistUnderstanding(const ImageAnalysisItemResult& result) {
    if (!project_) {
      return;
    }
    auto project = project_->handler().project();
    if (!project) {
      return;
    }
    auto& ai = project->GetStorage()->GetAiStore();
    (void)ai.UpsertUnderstanding(MakeDescription(result));
  }

  void DoPersistUnderstandings(const std::vector<ImageAnalysisItemResult>& results) {
    if (!project_ || results.empty()) {
      return;
    }
    auto project = project_->handler().project();
    if (!project) {
      return;
    }
    std::vector<AiDescription> descriptions;
    descriptions.reserve(results.size());
    for (const auto& result : results) {
      descriptions.push_back(MakeDescription(result));
    }
    auto& ai = project->GetStorage()->GetAiStore();
    (void)ai.UpsertUnderstandings(descriptions);
  }

  void DoPersistRatingReasons(const ImageAnalysisItemResult& result) {
    if (!project_) {
      return;
    }
    auto project = project_->handler().project();
    if (!project) {
      return;
    }
    auto&    ai = project->GetStorage()->GetAiStore();
    AiRating r;
    r.file_id_           = result.item.element_id;
    r.task_id_           = kScoreTaskId;
    r.provider_id_       = result.rating.provider;
    r.model_id_          = result.rating.model_id;
    r.prompt_profile_id_ = result.rating.prompt_profile_id;
    r.rendition_kind_    = result.rating.rendition.kind;
    r.rating_            = 0;
    r.rubric_id_         = result.rating.rubric_id;
    r.rubric_version_    = result.rating.rubric_version;
    r.reasons_           = result.rating.reasons;
    r.active_            = true;
    (void)ai.UpsertRatingReasons(r);
  }

  void DoApplyStarRating(uint32_t elementId, uint32_t imageId, int rating) {
    if (images_) {
      images_->ApplyStarRatingLight(elementId, imageId, rating);
    }
  }

  void DoFlushPendingStarRatings() {
    if (images_) {
      images_->FlushPendingStarRatings();
    }
  }

  void DoNotifySearchDocumentChanged() {
    if (stats_) {
      stats_->RebuildThumbnailView();
    }
  }

  static auto MakeDescription(const ImageAnalysisItemResult& result) -> AiDescription {
    AiDescription d;
    d.file_id_           = result.item.element_id;
    d.task_id_           = kDescribeTaskId;
    d.provider_id_       = result.understanding.provider;
    d.model_id_          = result.understanding.model_id;
    d.prompt_profile_id_ = result.understanding.prompt_profile_id;
    d.rendition_kind_    = result.understanding.rendition.kind;
    d.caption_           = result.understanding.caption;
    d.scene_             = result.understanding.scene;
    d.confidence_        = result.understanding.confidence;
    d.active_            = true;
    d.SetTags(result.understanding.tags);
    return d;
  }

  ProjectModule*           project_ = nullptr;
  ImageController*         images_  = nullptr;
  StatsEngine*             stats_   = nullptr;
  AnalysisResultWriteQueue queue_;
};

std::shared_ptr<IImageAnalysisSink> MakeAlbumImageAnalysisSink(ProjectModule* project,
                                                               ImageController* images,
                                                               StatsEngine* stats,
                                                               ProjectDbWriteBarrier* barrier) {
  return std::make_shared<AlbumImageAnalysisSink>(project, images, stats, barrier);
}

}  // namespace alcedo::ui
