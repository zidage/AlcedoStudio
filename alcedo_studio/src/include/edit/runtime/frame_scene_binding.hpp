//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <utility>

#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/scene_work_member.hpp"

namespace alcedo {

/**
 * @brief Frame-local physical location of the current scene image.
 *
 * Grade execution uses CachedImage (Develop / Camera Color) or WorkImage (one
 * scene-work member). DRT/Post may also write DisplayImage, which is the existing
 * display GraphValueId. The binding does not own GPU memory, does not store a
 * revision or extent, and must not be written into the workspace or published.
 *
 * Lifetime: PlanExecutor stack for one BeginRender/EndRender. References stay
 * valid through GraphImageCache leases or SceneWorkImagePair ownership.
 */
struct FrameSceneBinding {
  enum class Kind : std::uint8_t { CachedImage, WorkImage, DisplayImage };

  Kind            kind   = Kind::CachedImage;
  GraphValueId    graph_id{};
  SceneWorkMember member = SceneWorkMember::Member0;

  /** @brief Read-only GraphImageCache result such as Camera Color or Develop. */
  [[nodiscard]] static auto CachedImage(GraphValueId id) -> FrameSceneBinding {
    FrameSceneBinding binding;
    binding.kind     = Kind::CachedImage;
    binding.graph_id = std::move(id);
    return binding;
  }

  /** @brief One SceneWorkImagePair member. Content is frame-local and unpublished. */
  [[nodiscard]] static auto WorkImage(SceneWorkMember work_member) -> FrameSceneBinding {
    FrameSceneBinding binding;
    binding.kind   = Kind::WorkImage;
    binding.member = work_member;
    return binding;
  }

  /**
   * @brief Existing display output GraphValueId.
   *
   * Frame sink and export readers receive this lease. Work members must not use
   * this kind.
   */
  [[nodiscard]] static auto DisplayImage(GraphValueId id) -> FrameSceneBinding {
    FrameSceneBinding binding;
    binding.kind     = Kind::DisplayImage;
    binding.graph_id = std::move(id);
    return binding;
  }

  [[nodiscard]] auto IsCachedImage() const -> bool { return kind == Kind::CachedImage; }
  [[nodiscard]] auto IsWorkImage() const -> bool { return kind == Kind::WorkImage; }
  [[nodiscard]] auto IsDisplayImage() const -> bool { return kind == Kind::DisplayImage; }
};

/**
 * @brief Work member that a Grade writes when @p scene is its complete input.
 *
 * The first Grade, whose input is a cached key-stage image, writes Member0.
 * Later Grades write the peer of their work-image input.
 */
[[nodiscard]] inline auto DestinationWorkMember(const FrameSceneBinding& scene) -> SceneWorkMember {
  return scene.IsWorkImage() ? PeerOf(scene.member) : SceneWorkMember::Member0;
}

[[nodiscard]] inline auto operator==(const FrameSceneBinding& lhs, const FrameSceneBinding& rhs)
    -> bool {
  if (lhs.kind != rhs.kind) {
    return false;
  }
  if (lhs.kind == FrameSceneBinding::Kind::WorkImage) {
    return lhs.member == rhs.member;
  }
  return lhs.graph_id == rhs.graph_id;
}

[[nodiscard]] inline auto operator!=(const FrameSceneBinding& lhs, const FrameSceneBinding& rhs)
    -> bool {
  return !(lhs == rhs);
}

}  // namespace alcedo
