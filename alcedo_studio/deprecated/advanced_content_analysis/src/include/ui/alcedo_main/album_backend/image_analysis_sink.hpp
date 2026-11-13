//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "app/image_analysis_service.hpp"  // alcedo::ImageAnalysisItemResult

namespace alcedo::ui {

/// Phase 7a — the host-state mutation seam for `ImageAnalysisController`.
///
/// The controller stays decoupled from the module host (Phase 6d invariant) and
/// `ImageAnalysisService` stays storage-agnostic (Phase 5d/6d tests unchanged). This
/// narrow interface, injected into the controller's constructor, owns every host-side
/// side effect of a finished remote-analysis job: persisting understanding / rating
/// reasons, writing the EXIF star, flushing the batched star writes, and refreshing the
/// album search view. Tests pass a fake that records calls so "no upsert on failure /
/// cancel" is a one-liner assertion.
///
/// Persistence fires at job end (in the finished callback): describe/analyze
/// understanding rows are submitted as one batch, while score rows still apply rating
/// side effects per analyzed item; `kError`/`kCanceled` items are skipped entirely. After
/// the loop, a score job calls `FlushPendingStarRatings` and a describe/analyze job calls
/// `NotifySearchDocumentChanged`. A cancelled or failed call therefore produces ZERO sink
/// calls — no active annotation can be left behind.
class IImageAnalysisSink {
 public:
  virtual ~IImageAnalysisSink()                                              = default;

  /// Describe: persist the understanding (caption/tags/scene/confidence + provider/model/
  /// prompt-profile/rendition identity) as an active-for-search row. Returns false if the
  /// storage layer rejected the row (e.g. orphan file_id); the controller does not treat
  /// this as a job failure, it only surfaces results in QML state.
  virtual bool   PersistUnderstanding(const ImageAnalysisItemResult& result) = 0;

  /// Describe/analyze batch path. The default preserves existing fake/test behavior by
  /// delegating to `PersistUnderstanding`; the production sink overrides it so a finished
  /// job writes all descriptions in one storage transaction and refreshes the FTS index
  /// once.
  virtual size_t PersistUnderstandings(const std::vector<ImageAnalysisItemResult>& results) {
    size_t persisted = 0;
    for (const auto& result : results) {
      if (PersistUnderstanding(result)) {
        ++persisted;
      }
    }
    return persisted;
  }

  /// Score: persist the rating *reasons* only (rationale + identity), with `rating = 0`
  /// as a sentinel. Does NOT write the EXIF star — that is `ApplyStarRating`'s job.
  virtual bool PersistRatingReasons(const ImageAnalysisItemResult& result)       = 0;

  /// Score: write the model's 1..5 value into the EXIF/metadata `Rating` column in-memory
  /// (`Write_NoSync` + view-state patch + thumbnail-model update) — the light half of the
  /// star-rating path. No `SyncWithStorage`/`SaveProject`/`Package` here; the batched
  /// flush is `FlushPendingStarRatings`.
  virtual bool ApplyStarRating(uint32_t elementId, uint32_t imageId, int rating) = 0;

  /// Score job end: one `SyncWithStorage` (flushes all MODIFIED image rows in a single
  /// transaction) + `RefreshStats` (re-runs the rating-bucket GROUP BY so star-filter
  /// stats are correct). No `SaveProject`/`Package` in 7a — the `.alcd` packaged snapshot
  /// is left stale until the next normal save/close; the live DB is authoritative.
  virtual void FlushPendingStarRatings()                                         = 0;

  /// Describe/analyze job end: re-run the active search so newly-persisted captions/tags
  /// match. The storage layer refreshes the derived FTS index during persistence when the
  /// DuckDB fts extension is available; this refreshes the current thumbnail view.
  virtual void NotifySearchDocumentChanged()                                     = 0;

  /// Phase 2 (Step 4) — project DB write barrier integration. While the barrier
  /// is held (export is in flight), the production sink queues writes instead of
  /// committing them. These three hooks let `ImageAnalysisController::Finish`
  /// defer a background task's `FinishTask` until the queued writes actually
  /// commit, so the task's interaction locks stay held across the export-barrier
  /// gap (preventing a user from editing a just-analyzed image's rating in the
  /// window before the queued AI rating would overwrite it). All default to
  /// no-op/false so test fakes (which never queue) are unaffected.
  virtual bool HasPendingWrites() const                 { return false; }
  virtual void SetOnDrainComplete(std::function<void()> /*cb*/) {}
  virtual void FlushPendingWrites()                     {}
};

}  // namespace alcedo::ui
