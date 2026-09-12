//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/diagnostics/render_e2e_timing.hpp"

#include "utils/diagnostics/preview_performance.hpp"

#include <string_view>

namespace alcedo::diag {
namespace {

auto RoleFromLabel(const std::string_view role) -> PreviewFrameRole {
  if (role == "QualityBase") {
    return PreviewFrameRole::QualityBase;
  }
  if (role == "DetailPatch") {
    return PreviewFrameRole::DetailPatch;
  }
  return PreviewFrameRole::InteractivePrimary;
}

auto QualityFromLabel(const std::string_view quality) -> PreviewQuality {
  if (quality == "Quality") {
    return PreviewQuality::Quality;
  }
  if (quality == "Detail") {
    return PreviewQuality::Detail;
  }
  return PreviewQuality::Interactive;
}

auto OutcomeFromLabel(const std::string_view outcome) -> PreviewTerminalOutcome {
  if (outcome == "replaced" || outcome == "superseded-metal-import") {
    return PreviewTerminalOutcome::Coalesced;
  }
  if (outcome == "cancelled") {
    return PreviewTerminalOutcome::Cancelled;
  }
  if (outcome == "failed" || outcome == "qrhi-import-failed" || outcome == "sink-not-mapped" ||
      outcome == "sink-no-queue") {
    return PreviewTerminalOutcome::Failed;
  }
  if (outcome == "stale-request-id") {
    return PreviewTerminalOutcome::Stale;
  }
  return PreviewTerminalOutcome::Dropped;
}

auto HasUserInput(const std::string_view reason) -> bool {
  return reason == "InteractiveAdjustment" || reason == "SettledAdjustment" ||
         reason == "SettledMaskEdit";
}

}  // namespace

void NoteRenderE2eSubmit(const std::uint64_t request_id, const std::string_view reason,
                         const std::string_view quality, const std::string_view role) {
  PreviewPerformance::NoteSubmit(request_id, RoleFromLabel(role), QualityFromLabel(quality), reason,
                                 HasUserInput(reason));
}

void NoteRenderE2eScheduled(const std::uint64_t request_id) {
  PreviewPerformance::NoteScheduled(request_id);
}

void NoteRenderE2eProducerReady(const std::uint64_t request_id) {
  PreviewPerformance::NoteProducerReady(request_id);
}

void NoteRenderE2ePresentWake(const std::uint64_t request_id) {
  PreviewPerformance::NotePresentWake(request_id);
}

void NoteRenderE2eGuiUpdate() { PreviewPerformance::NoteGuiUpdate(); }

void NoteRenderE2eRenderEnter() { PreviewPerformance::NoteRenderEnter(); }

void NoteRenderE2eConsumeBegin(const std::uint64_t request_id) {
  PreviewPerformance::NoteConsumeBegin(request_id);
}

void NoteRenderE2eDisplayed(const std::uint64_t request_id) {
  PreviewPerformance::NoteDisplayed(request_id);
}

void NoteRenderE2eTerminal(const std::uint64_t request_id, const std::string_view outcome) {
  PreviewPerformance::NoteTerminal(request_id, OutcomeFromLabel(outcome), outcome);
}

}  // namespace alcedo::diag
