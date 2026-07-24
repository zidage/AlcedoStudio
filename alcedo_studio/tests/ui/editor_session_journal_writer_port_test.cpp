//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_journal_writer_port.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace alcedo::ui {
namespace {

/// Verifies that constructing the port without a path resolver leaves it in a
/// usable state where operations that require a journal path return sensible
/// defaults instead of crashing.
TEST(EditorSessionJournalWriterPortTest, DefaultConstructionDoesNotCrash) {
  EditorSessionJournalWriterPort port;
  // Without path resolution, CommitJournal cannot find a writer and returns
  // a non-accepted outcome.
  std::string error;
  auto        outcome = port.CommitJournal(42, 1, &error);
  EXPECT_FALSE(outcome.accepted);
}

/// Verifies that providing a valid path resolver enables CommitJournal to
/// create a writer and return an accepted (durable) outcome.
TEST(EditorSessionJournalWriterPortTest, CommitJournalAfterPathResolutionDurables) {
  auto tmp = std::filesystem::temp_directory_path() / "jwp_test_commit.jrnl";
  std::filesystem::remove(tmp);

  EditorSessionJournalWriterPort::Services svc;
  svc.journal_path = [&](sl_element_id_t) { return tmp; };

  EditorSessionJournalWriterPort port(svc);
  std::string                   error;
  auto outcome = port.CommitJournal(42, 1, &error);
  EXPECT_TRUE(outcome.accepted) << error;

  std::filesystem::remove(tmp);
}

/// Verifies that DiscardUnflushed removes the writer entry and a subsequent
/// CommitJournal auto-recreates the writer and commits cleanly.
TEST(EditorSessionJournalWriterPortTest, DiscardRemovesWriterAndRecreateCommitsCleanly) {
  auto tmp = std::filesystem::temp_directory_path() / "jwp_test_discard.jrnl";
  std::filesystem::remove(tmp);

  EditorSessionJournalWriterPort::Services svc;
  svc.journal_path = [&](sl_element_id_t) { return tmp; };

  EditorSessionJournalWriterPort port(svc);

  // First commit creates the writer.
  {
    std::string error;
    auto        outcome = port.CommitJournal(42, 1, &error);
    EXPECT_TRUE(outcome.accepted) << error;
  }

  // Discard removes the writer entry.
  {
    std::string error;
    EXPECT_TRUE(port.DiscardUnflushed(42, &error)) << error;
  }

  // A second commit auto-recreates the writer and succeeds because no
  // records are pending.
  {
    std::string error;
    auto        outcome = port.CommitJournal(42, 2, &error);
    EXPECT_TRUE(outcome.accepted) << error;
  }

  std::filesystem::remove(tmp);
}

}  // namespace
}  // namespace alcedo::ui
