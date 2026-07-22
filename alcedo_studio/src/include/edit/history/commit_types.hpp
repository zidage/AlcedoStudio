//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "type/hash_type.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Content-addressed identity of one immutable edit or merge commit.
using commit_hash_t = Hash128;

/// Immutable identity of the image-specific root pipeline after import metadata resolution.
using root_id_t = Hash128;

/// Stable branch identity for a named Version ref. Independent of the current head commit.
using version_ref_id_t = Hash128;

/// Working or materialized head. std::nullopt always means the image root (no commits applied).
using head_commit_hash_t = std::optional<commit_hash_t>;

/// Incremental fold of first-parent commit hashes from the image root.
using transaction_chain_hash_t = Hash128;

/// Project history schema stored with each image edit state. Bumped only on incompatible
/// history layout changes. Independent of the package-level project_file_version.
constexpr std::uint32_t kImageEditSchemaVersion = 1;

/// Stable hash inputs for commit objects. Changing this invalidates all stored commit hashes.
constexpr std::uint32_t kCommitFormatVersion = 1;

/// Stable hash inputs for the incremental transaction-chain fold.
constexpr std::uint32_t kChainFormatVersion = 1;

enum class EditCommitKind : std::uint8_t {
  kEdit  = 0,
  kMerge = 1,
};

inline auto EditCommitKindToString(EditCommitKind kind) -> const char* {
  switch (kind) {
    case EditCommitKind::kEdit:
      return "edit";
    case EditCommitKind::kMerge:
      return "merge";
  }
  throw std::runtime_error("EditCommitKind: unknown enum value");
}

inline auto EditCommitKindFromString(const std::string& value) -> EditCommitKind {
  if (value == "edit") {
    return EditCommitKind::kEdit;
  }
  if (value == "merge") {
    return EditCommitKind::kMerge;
  }
  throw std::runtime_error("EditCommitKind: unknown kind string '" + value + "'");
}

inline auto EditCommitKindFromInt(int value) -> EditCommitKind {
  if (value == static_cast<int>(EditCommitKind::kEdit)) {
    return EditCommitKind::kEdit;
  }
  if (value == static_cast<int>(EditCommitKind::kMerge)) {
    return EditCommitKind::kMerge;
  }
  throw std::runtime_error("EditCommitKind: unknown kind integer " + std::to_string(value));
}

/// Encode a nullable head as a storage string. Empty means root.
inline auto HeadCommitHashToStorage(const head_commit_hash_t& head) -> std::string {
  return head.has_value() ? head->ToString() : std::string{};
}

inline auto HeadCommitHashFromStorage(const std::string& value) -> head_commit_hash_t {
  if (value.empty()) {
    return std::nullopt;
  }
  return Hash128::FromString(value);
}

}  // namespace alcedo
