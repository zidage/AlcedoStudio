//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/lut_library_publication.hpp"

#include <utility>
#include <variant>

#include "utils/lut/lut_inventory_digest.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;

/// Root-relative `/` path of @p path when it lies inside @p root.
auto RelativeInside(const fs::path& path, const fs::path& root) -> std::optional<std::string> {
  const fs::path relative = path.lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty() || *relative.begin() == "..") {
    return std::nullopt;
  }
  std::string utf8 = LutPathToUtf8(relative);
  if (!IsSafeLutRelativePath(utf8)) {
    return std::nullopt;
  }
  return utf8;
}

}  // namespace

LutLibraryPublication::LutLibraryPublication(fs::path root) : root_(std::move(root)) {}

void LutLibraryPublication::PublishInventory(
    LutLibraryInventory inventory, std::optional<std::vector<LutPackageReceipt>> receipts) {
  std::unique_lock lock(state_mutex_);
  inventory_ = std::move(inventory);
  if (receipts) {
    receipts_ = std::move(*receipts);
  }
}

void LutLibraryPublication::SetRoot(fs::path root) {
  std::unique_lock lock(state_mutex_);
  root_ = std::move(root);
}

void LutLibraryPublication::SetUserState(LutLibraryUserState state) {
  std::unique_lock lock(state_mutex_);
  user_state_ = std::move(state);
}

auto LutLibraryPublication::ReferenceForEntry(const LutLibraryEntry& entry) -> LutReference {
  if (!entry.managed_package_id.empty() && entry.IsOfficial() && entry.header.metadata &&
      !entry.header.metadata->id.empty()) {
    return OfficialLutReference{entry.managed_package_id, entry.header.metadata->id};
  }
  return LibraryLutReference{entry.relative_path};
}

auto LutLibraryPublication::EntryIdOf(const LutLibraryEntry& entry) -> std::string {
  return DescribeLutReference(ReferenceForEntry(entry));
}

auto LutLibraryPublication::LockContentForRemoval() const -> std::unique_lock<std::shared_mutex> {
  return std::unique_lock(content_mutex_);
}

void LutLibraryPublication::SetMissingObserver(std::function<void(const LutReference&)> observer) {
  std::unique_lock lock(observer_mutex_);
  missing_observer_ = std::move(observer);
}

void LutLibraryPublication::ReportMissing(const LutReference& reference) const {
  std::shared_lock lock(observer_mutex_);
  if (missing_observer_) {
    missing_observer_(reference);
  }
}

auto LutLibraryPublication::CandidateInRoot(std::string_view relative_path) const
    -> std::optional<Candidate> {
  if (!IsSafeLutRelativePath(relative_path)) {
    return std::nullopt;
  }
  Candidate candidate{root_ / LutPathFromUtf8(relative_path), {}};
  if (const LutLibraryEntry* entry = FindLutLibraryEntry(inventory_, relative_path)) {
    candidate.content_sha256 = entry->sha256;
  }
  return candidate;
}

auto LutLibraryPublication::CandidatesLocked(const LutReference& reference) const
    -> std::vector<Candidate> {
  std::vector<Candidate> candidates;
  const auto             add = [&candidates](std::optional<Candidate> candidate) {
    if (candidate) {
      candidates.push_back(std::move(*candidate));
    }
  };
  if (const auto* official = std::get_if<OfficialLutReference>(&reference)) {
    for (const LutLibraryEntry& entry : inventory_.entries) {
      if (entry.managed_package_id == official->package_id && entry.IsOfficial() &&
          entry.header.metadata && entry.header.metadata->id == official->lut_id) {
        add(CandidateInRoot(entry.relative_path));
        break;
      }
    }
  } else if (const auto* library = std::get_if<LibraryLutReference>(&reference)) {
    add(CandidateInRoot(library->relative_path));
  } else if (const auto* file = std::get_if<FileLutReference>(&reference)) {
    const fs::path exact = LutPathFromUtf8(file->path);
    Candidate      direct{exact, {}};
    if (const auto relative = RelativeInside(exact, root_)) {
      if (const LutLibraryEntry* entry = FindLutLibraryEntry(inventory_, *relative)) {
        direct.content_sha256 = entry->sha256;
      }
    }
    candidates.push_back(std::move(direct));
    // A library moved by migration keeps its relative paths (plan 4.2).
    for (auto it = user_state_.previous_roots.rbegin(); it != user_state_.previous_roots.rend();
         ++it) {
      if (const auto relative = RelativeInside(exact, LutPathFromUtf8(*it))) {
        add(CandidateInRoot(*relative));
      }
    }
  }
  return candidates;
}

auto LutLibraryPublication::Resolve(const LutReference& reference) const -> LutResourceResolution {
  if (IsEmptyLutReference(reference)) {
    return {};
  }
  std::vector<Candidate> candidates;
  {
    // Only the published values are read under the lock; file checks run after it is released.
    std::shared_lock lock(state_mutex_);
    candidates = CandidatesLocked(reference);
  }
  LutResourceResolution missing;
  missing.status = LutResourceStatus::kMissing;
  for (const Candidate& candidate : candidates) {
    LutResourceResolution resolution = ResolveLutFile(candidate.path);
    if (resolution.status == LutResourceStatus::kAvailable) {
      resolution.content_sha256 = candidate.content_sha256;
      return resolution;
    }
    if (missing.path.empty()) {
      missing.path = candidate.path;
    }
  }
  ReportMissing(reference);
  return missing;
}

void LutLibraryPublication::ReadResource(
    const LutReference&                                      reference,
    const std::function<void(const LutResourceResolution&)>& visitor) const {
  std::shared_lock content(content_mutex_);
  visitor(Resolve(reference));
}

}  // namespace alcedo
