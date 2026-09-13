//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string_view>

namespace alcedo::diag {

/// Compatibility notes for preview timing. Implementation forwards to
/// PreviewPerformance. Process start turns Detail on.
/// Qt frame identity is attached only to the request imported in that render().

void NoteRenderE2eSubmit(std::uint64_t request_id, std::string_view reason,
                         std::string_view quality, std::string_view role);

/// Coordinator handed the request to the pipeline scheduler (left the pending
/// slot). Time from Submit to here is coalesce / single-flight queue wait.
void NoteRenderE2eScheduled(std::uint64_t request_id);

/// Producer finished GPU/host write and handed the frame to the present queue
/// (NotifyFrameReady / SubmitMetalFrame). Covers pipeline + pool wait after
/// schedule.
void NoteRenderE2eProducerReady(std::uint64_t request_id);

/// Viewport was asked to redraw after the frame became Ready
/// (requestPresentUpdate posted from the producer path).
void NoteRenderE2ePresentWake(std::uint64_t request_id);

/// GUI thread executed the coalesced update()/window->requestUpdate() for
/// pending Ready frames. No request id: stamps every sample that already has
/// present_wake and still lacks gui_update.
void NoteRenderE2eGuiUpdate();

/// Render-thread QQuickRhiItemRenderer::render() entry. Stamps every sample
/// that is Ready (has present_wake) and still lacks render_enter.
void NoteRenderE2eRenderEnter();

/// Render thread picked the Ready frame and is about to import it into QRhi.
void NoteRenderE2eConsumeBegin(std::uint64_t request_id);

/// Viewport renderer imported the frame for the next composition pass.
/// Prints the e2e line and drops the sample.
void NoteRenderE2eDisplayed(std::uint64_t request_id);

/// Request will never display (replaced, cancelled, failed, present drop).
void NoteRenderE2eTerminal(std::uint64_t request_id, std::string_view outcome);

}  // namespace alcedo::diag
