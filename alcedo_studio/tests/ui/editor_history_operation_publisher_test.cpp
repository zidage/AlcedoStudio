//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// Phase 7A R4: EditorHistoryOperationPublisher unit tests — operation id
/// allocation, invokable start/terminal publication, async correlation, and
/// stale/duplicate completion rejection. Does not construct the full controller.

#include <gtest/gtest.h>

#include "app/editor_session_types.hpp"
#include "ui/alcedo_main/album_backend/editor_history_operation_publisher.hpp"

namespace alcedo::ui {
namespace {

auto MakeResult(EditorSessionResultKind kind, std::uint64_t task_id = 0,
                std::string message = {}) -> EditorSessionResult {
  EditorSessionResult result;
  result.kind              = kind;
  result.state             = EditorSessionState::Interactive;
  result.identity          = {42, 84, 1, 1, 1};
  result.task_id           = task_id;
  result.render_request_id = 0;
  result.message           = std::move(message);
  return result;
}

TEST(EditorHistoryOperationPublisherTest,
     AllocatesMonotonicOperationIdsAndPublishesRejectedTerminal) {
  EditorHistoryOperationPublisher publisher;
  const auto first  = publisher.AllocateOperationId();
  const auto second = publisher.AllocateOperationId();
  EXPECT_EQ(second, first + 1);

  const auto published = publisher.PublishRejected(
      first, QStringLiteral("checkoutVersion"), QStringLiteral("Invalid Version id"),
      QStringLiteral("not-a-hex"));
  EXPECT_TRUE(published.event.terminal);
  EXPECT_TRUE(published.failed);
  EXPECT_EQ(published.event.operation_id, first);
  EXPECT_EQ(published.event.action, "checkoutVersion");
  EXPECT_EQ(published.event.result_kind, EditorSessionResultKind::Rejected);
  EXPECT_EQ(published.map.value(QStringLiteral("operationId")).toULongLong(), first);
  EXPECT_EQ(published.map.value(QStringLiteral("selectedId")).toString(),
            QStringLiteral("not-a-hex"));
  EXPECT_FALSE(publisher.has_pending());
}

TEST(EditorHistoryOperationPublisherTest,
     SaveStartedKeepsPendingAndTerminalSaveFinishedReusesOperationId) {
  EditorHistoryOperationPublisher publisher;
  const auto op = publisher.AllocateOperationId();
  const auto started = publisher.PublishInvokableReturn(
      op, QStringLiteral("createRootVersion"),
      MakeResult(EditorSessionResultKind::SaveStarted, 77, "Save started"));
  EXPECT_FALSE(started.event.terminal);
  EXPECT_FALSE(started.failed);
  EXPECT_TRUE(publisher.has_pending());
  EXPECT_EQ(publisher.pending_operation_id(), op);
  EXPECT_EQ(started.map.value(QStringLiteral("kind")).toInt(),
            static_cast<int>(EditorSessionResultKind::SaveStarted));

  // Duplicate SaveStarted must not close the draft.
  EXPECT_FALSE(publisher
                   .CorrelateObservedResult(
                       MakeResult(EditorSessionResultKind::SaveStarted, 77, "Save started"))
                   .has_value());
  EXPECT_TRUE(publisher.has_pending());

  // Intermediate RenderRouted from RouteInitialRender carries no checkpoint
  // task id and must not clear the pending create/branch/checkout draft.
  EXPECT_FALSE(publisher
                   .CorrelateObservedResult(
                       MakeResult(EditorSessionResultKind::RenderRouted, 0, "Render routed"))
                   .has_value());
  EXPECT_TRUE(publisher.has_pending());

  const auto finished = publisher.CorrelateObservedResult(
      MakeResult(EditorSessionResultKind::SaveFinished, 77, "Editor session materialized"));
  ASSERT_TRUE(finished.has_value());
  EXPECT_TRUE(finished->event.terminal);
  EXPECT_FALSE(finished->failed);
  EXPECT_EQ(finished->event.operation_id, op);
  EXPECT_EQ(finished->event.action, "createRootVersion");
  EXPECT_EQ(finished->event.result_kind, EditorSessionResultKind::SaveFinished);
  EXPECT_EQ(finished->message.toStdString(), "Editor session materialized");
  EXPECT_FALSE(publisher.has_pending());
}

TEST(EditorHistoryOperationPublisherTest,
     StaleTaskIdCompletionIsIgnoredAndFailedKeepsFailureFlag) {
  EditorHistoryOperationPublisher publisher;
  const auto op = publisher.AllocateOperationId();
  (void)publisher.PublishInvokableReturn(
      op, QStringLiteral("branchFromCommit"),
      MakeResult(EditorSessionResultKind::SaveStarted, 11, "Save started"),
      QStringLiteral("abcdef0123456789fedcba9876543210"));

  EXPECT_FALSE(publisher
                   .CorrelateObservedResult(MakeResult(EditorSessionResultKind::SaveFinished, 99,
                                                       "stale ticket"))
                   .has_value())
      << "a different checkpoint task id must not close another draft";
  EXPECT_TRUE(publisher.has_pending());

  const auto failed = publisher.CorrelateObservedResult(
      MakeResult(EditorSessionResultKind::Failed, 11, "branch creation failed: disk full"));
  ASSERT_TRUE(failed.has_value());
  EXPECT_TRUE(failed->event.terminal);
  EXPECT_TRUE(failed->failed);
  EXPECT_EQ(failed->event.operation_id, op);
  EXPECT_EQ(failed->event.selected_id, "abcdef0123456789fedcba9876543210");
  EXPECT_EQ(failed->message.toStdString(), "branch creation failed: disk full");
  EXPECT_FALSE(publisher.has_pending());

  // Second terminal for the same op must be ignored (pending already cleared).
  EXPECT_FALSE(publisher
                   .CorrelateObservedResult(
                       MakeResult(EditorSessionResultKind::SaveFinished, 11, "late success"))
                   .has_value());
}

TEST(EditorHistoryOperationPublisherTest, SynchronousAcceptedIsImmediatelyTerminal) {
  EditorHistoryOperationPublisher publisher;
  const auto op = publisher.AllocateOperationId();
  const auto published = publisher.PublishInvokableReturn(
      op, QStringLiteral("undo"), MakeResult(EditorSessionResultKind::Accepted, 0, "Undone"));
  EXPECT_TRUE(published.event.terminal);
  EXPECT_FALSE(published.failed);
  EXPECT_FALSE(publisher.has_pending());
  EXPECT_EQ(published.map.value(QStringLiteral("action")).toString(), QStringLiteral("undo"));
}

}  // namespace
}  // namespace alcedo::ui
