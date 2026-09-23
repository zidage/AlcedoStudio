//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace alcedo::tests {

enum class EditorJournalCrashPoint {
  None,
  RecordHeader,
  RecordPayload,
  RecordChecksum,
  Flush,
  HeadMarker,
  Materialize,
  ThumbnailInvalidation,
  CompactionReplace,
  ImageSwitch,
};

[[nodiscard]] auto ToString(EditorJournalCrashPoint point) -> const char*;

struct EditorJournalFuzzConfig {
  std::uint64_t           seed        = 0x5f3759dfULL;
  std::size_t             steps       = 32;
  EditorJournalCrashPoint crash_point = EditorJournalCrashPoint::None;
  std::size_t             crash_step  = 0;
  std::filesystem::path   artifact_directory;
};

struct EditorJournalFuzzResult {
  bool                    passed         = false;
  std::uint64_t           seed           = 0;
  std::size_t             steps_executed = 0;
  EditorJournalCrashPoint crash_point    = EditorJournalCrashPoint::None;
  std::size_t             crash_step     = 0;
  std::string             operation_sequence;
  std::string             message;
  std::filesystem::path   artifact_path;
};

/// Deterministic WAL restart harness. It owns no production state and uses the
/// same writer, file port, decoder, and timeline simulator as the application.
/// A run is deliberately bounded so fixed seeds are suitable for presubmit;
/// callers can increase steps for scheduled CI.
[[nodiscard]] auto RunEditorJournalFuzz(const EditorJournalFuzzConfig& config)
    -> EditorJournalFuzzResult;

}  // namespace alcedo::tests
