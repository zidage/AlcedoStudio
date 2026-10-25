//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/mask/ai_mask_coverage_texture.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/ai_mask_coverage_work.hpp"
#include "edit/runtime/mask_asset_reader.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {

/// Largest number of samples per axis that the raster Mask pass averages for one render pixel.
inline constexpr std::uint32_t kRasterMaskMaximumSamplesPerAxis = 8;

/**
 * @brief Samples per axis of the raster Mask pass (SM6 R7): the texel footprint of one render
 *        pixel, rounded up, in [1, 8].
 *
 * `2^mip_level` is the largest number of texels that one render pixel spans along a render axis.
 * The 1e-3 allowance keeps a footprint of exactly N texels at N samples despite float rounding.
 */
[[nodiscard]] inline auto      RasterMaskSamplesPerAxis(const TextureSamplingPlan& plan)
    -> std::uint32_t {
  const float footprint = std::exp2(plan.mip_level);
  const auto  samples   = static_cast<std::int64_t>(std::ceil(footprint - 1.0e-3f));
  return static_cast<std::uint32_t>(
      std::clamp<std::int64_t>(samples, 1, kRasterMaskMaximumSamplesPerAxis));
}

/// Device bytes that the committed coverage textures of one render workspace may keep (SM6 R6).
inline constexpr std::size_t kMaskCoverageTextureByteBudget = 256ull << 20;

/// One device R8 coverage texture and the normalized ReferenceSpace rectangle that its texels
/// cover.
template <class Backend>
struct MaskCoverageImage {
  typename Backend::Texture2D texture{};
  MaskAssetDescriptor         descriptor{};
};

/**
 * @brief Scoped read of a coverage texture from @ref MaskCoverageTextureCache.
 *
 * While a read exists, the cache does not destroy or reuse its texture. A GPU command that samples
 * the texture is tracked separately through the submission of the acquisition. Release the read
 * when the command is recorded or the reader is done; the cache then destroys a replaced texture on
 * its own thread after the GPU completes.
 */
template <class Backend>
using MaskCoverageRead = std::shared_ptr<const MaskCoverageImage<Backend>>;

/**
 * @brief Device R8 coverage of AI Masks: the owner of original coverage uploads and of the
 *        coverage after Feather and Expand (SM6 R6, SM8.1).
 *
 * Two acquisitions exist, one per origin of the original coverage:
 *
 * - @ref AcquireStored: a committed AI Mask. Its original coverage is the stored asset, read
 *   through the workspace reader. Entries are keyed by content (asset key and analysis size, plus
 *   the exact edge bits), so no entry can become stale. They stay for reuse under
 *   @ref kMaskCoverageTextureByteBudget: a new entry evicts the least recently used entries that
 *   no read, no current frame, and no in-flight submission uses.
 * - @ref AcquireCandidate: the Mask candidate of the AI Mask edit session. Its original coverage is
 *   a shared read of the session allocation; the allocation identity identifies the pixels. The
 *   cache keeps one current original upload and one current edge result. A replacement retires the
 *   previous entry, which stays only while a read or an in-flight submission uses it.
 *
 * Each acquisition separates the original-coverage (source) stage from the edge-result stage. With
 * no Feather and no Expand, the read is the source texture and no edge work runs. Otherwise the
 * edge result is a separate texture of @ref AiMaskCoverageWork::output_rect. The edge result is
 * computed on the host (@ref MakeAiMaskCoverageTexture) and uploaded until each GPU backend runs
 * the operation (SM8.2 to SM8.4).
 *
 * A failed acquisition throws and changes no current entry; earlier reads stay valid.
 *
 * Owner: BasicRenderWorkspace. Thread: the render thread of the workspace for every call; a read
 * may be released on any thread. Released by BasicRenderWorkspace::ReleaseSessionResources (an
 * image change in the editor, the end of each batch task). The cache and its backend must outlive
 * every read.
 *
 * @tparam Backend Resource factory with CreateTexture2D, UploadTexture2D, and IsResourceBusy.
 */
template <class Backend>
class MaskCoverageTextureCache {
 public:
  using Texture2D      = typename Backend::Texture2D;
  using CommandContext = typename Backend::CommandContext;
  using Image                                               = MaskCoverageImage<Backend>;
  using Read                                                = MaskCoverageRead<Backend>;

  MaskCoverageTextureCache()                                = default;
  MaskCoverageTextureCache(const MaskCoverageTextureCache&) = delete;
  auto operator=(const MaskCoverageTextureCache&) -> MaskCoverageTextureCache& = delete;

  /**
   * @brief Coverage of the committed AI Mask @p source after its Feather and Expand, for the
   *        current submission of @p command_context.
   *
   * @throws std::runtime_error from the input validation (@ref MakeAiMaskCoverageWork), the
   *         reader, the texture allocation, or the upload. The cache then keeps no entry for the
   *         source.
   */
  auto AcquireStored(const AiSegmentationMaskSource& source, const MaskAssetReader& reader,
                     Backend& backend, CommandContext& command_context) -> Read {
    const auto work =
        MakeAiMaskCoverageWork(source.descriptor, source.analysis_extent, source.edge);
    const auto key        = MakeStoredKey(source);
    const auto submission = command_context.SubmissionId();
    ++lru_clock_;
    for (auto& entry : stored_) {
      if (entry.key == key) {
        Touch(entry, submission);
        return entry.image;
      }
    }

    const auto asset = reader.Read(source.asset_key);
    if (asset.descriptor != source.descriptor) {
      throw std::runtime_error("AI Mask coverage: the asset " + std::string{asset.key.Value()} +
                               " does not have the size and bounds of its AI Mask");
    }
    const auto bytes = static_cast<std::size_t>(work.output_rect.width) * work.output_rect.height;
    EvictStoredFor(bytes, backend, submission);
    Entry entry;
    if (work.HasEdgeAdjustment()) {
      const auto edge_result = MakeAiMaskCoverageTexture(work, asset.pixels);
      entry = Upload(edge_result.descriptor, edge_result.pixels, backend, command_context);
    } else {
      entry = Upload(work.source, asset.pixels, backend, command_context);
    }
    entry.key = key;
    Touch(entry, submission);
    stored_.push_back(std::move(entry));
    return stored_.back().image;
  }

  /**
   * @brief Coverage of the Mask candidate @p pixels after @p edge, for the current submission of
   *        @p command_context.
   *
   * @p pixels covers the whole analysis image of @p analysis_extent. The same allocation and
   * analysis size reuse the current original upload; with the same edge they also reuse the
   * current edge result. A new allocation replaces the original upload, and a new allocation or
   * edge replaces the edge result. After an original upload, the cache holds @p pixels until the
   * upload submission completes (@ref ReleaseIdle). Each entry keeps only a weak identity, which
   * cannot match another allocation.
   *
   * @throws std::runtime_error with an "AI Mask coverage:" message for a null read, a pixel count
   *         that differs from the analysis size, the input validation of
   *         @ref MakeAiMaskCoverageWork, the texture allocation, or the upload. The current
   *         entries and earlier reads then stay unchanged.
   */
  auto AcquireCandidate(const AiMaskCoveragePixels& pixels, Extent2D analysis_extent,
                        const AiMaskEdgeAdjustment& edge, Backend& backend,
                        CommandContext& command_context) -> Read {
    if (!pixels) {
      throw std::runtime_error("AI Mask coverage: the candidate has no pixels");
    }
    const auto work =
        MakeAiMaskCoverageWork(AiMaskCandidateDescriptor(analysis_extent), analysis_extent, edge);
    const auto expected = static_cast<std::size_t>(analysis_extent.width) * analysis_extent.height;
    if (pixels->size() != expected) {
      throw std::runtime_error("AI Mask coverage: the candidate has " +
                               std::to_string(pixels->size()) + " pixels; the " +
                               std::to_string(analysis_extent.width) + " x " +
                               std::to_string(analysis_extent.height) + " analysis image needs " +
                               std::to_string(expected));
    }
    const auto submission = command_context.SubmissionId();
    ++lru_clock_;

    if (!work.HasEdgeAdjustment()) {
      if (!Matches(candidate_source_, pixels, analysis_extent, edge)) {
        auto entry = Upload(work.source, *pixels, backend, command_context);
        Install(candidate_source_, std::move(entry), pixels, analysis_extent, edge, pixels);
      }
      // The original coverage is the current result: an earlier edge result is not current.
      Retire(candidate_result_);
      Touch(candidate_source_->entry, submission);
      return candidate_source_->entry.image;
    }

    if (!Matches(candidate_result_, pixels, analysis_extent, edge)) {
      const auto edge_result = MakeAiMaskCoverageTexture(work, *pixels);
      auto entry = Upload(edge_result.descriptor, edge_result.pixels, backend, command_context);
      Install(candidate_result_, std::move(entry), pixels, analysis_extent, edge, nullptr);
    }
    // An original upload of another allocation is no longer current.
    if (candidate_source_.has_value() && !SameAllocation(candidate_source_->identity, pixels)) {
      Retire(candidate_source_);
    }
    Touch(candidate_result_->entry, submission);
    return candidate_result_->entry.image;
  }

  /**
   * @brief Destroy retired textures that no read and no in-flight submission uses, and release
   *        candidate pixels whose upload submission completed.
   */
  void ReleaseIdle(const Backend& backend) {
    DestroyIdleRetired(backend);
    for (auto* candidate : {&candidate_source_, &candidate_result_}) {
      if (candidate->has_value() && (*candidate)->upload_owner &&
          !backend.IsResourceBusy((*candidate)->entry.upload_submission)) {
        (*candidate)->upload_owner.reset();
      }
    }
  }

  /**
   * @brief Retire every entry whose upload belongs to the cancelled @p submission.
   *
   * A cancelled submission is never submitted, and Metal discards its recorded command buffer, so
   * the texture of such an entry has no defined content. The next acquisition uploads again.
   */
  void DiscardUploadsOf(std::uint64_t submission) {
    for (auto it = stored_.begin(); it != stored_.end();) {
      if (it->upload_submission == submission) {
        Retire(std::move(*it));
        it = stored_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto* candidate : {&candidate_source_, &candidate_result_}) {
      if (candidate->has_value() && (*candidate)->entry.upload_submission == submission) {
        Retire(*candidate);
      }
    }
  }

  /**
   * @brief Drop every entry. An entry that a read still uses moves to the retired entries until
   *        @ref ReleaseIdle finds it idle.
   * @pre The GPU no longer uses the entries (the caller waited for idle).
   */
  void Clear() {
    for (auto& entry : stored_) {
      Retire(std::move(entry));
    }
    stored_.clear();
    Retire(candidate_source_);
    Retire(candidate_result_);
    std::erase_if(retired_, [this](const Entry& entry) {
      if (entry.image.use_count() != 1) {
        return false;
      }
      used_bytes_ -= entry.bytes;
      return true;
    });
  }

  /// Number of live textures: stored, current candidate, and retired.
  [[nodiscard]] auto EntryCount() const -> std::size_t {
    return stored_.size() + retired_.size() + (candidate_source_.has_value() ? 1 : 0) +
           (candidate_result_.has_value() ? 1 : 0);
  }
  /// Number of replaced textures that wait for a read or a submission to finish.
  [[nodiscard]] auto RetiredCount() const -> std::size_t { return retired_.size(); }
  /// Bytes of every live texture.
  [[nodiscard]] auto UsedBytes() const -> std::size_t { return used_bytes_; }
  /// Number of uploaded textures since construction; a hit does not count.
  [[nodiscard]] auto UploadCount() const -> std::uint64_t { return upload_count_; }
  /// True while the cache holds candidate pixels for an upload that has not completed.
  [[nodiscard]] auto HoldsCandidateUploadPixels() const -> bool {
    return (candidate_source_.has_value() && candidate_source_->upload_owner) ||
           (candidate_result_.has_value() && candidate_result_->upload_owner);
  }

 private:
  struct Entry {
    std::string            key;
    std::shared_ptr<Image> image;
    std::size_t            bytes                = 0;
    /// Submission that recorded the upload of the texture.
    std::uint64_t          upload_submission    = 0;
    std::uint64_t          last_used_submission = 0;
    std::uint64_t          lru_tick             = 0;
  };

  /// A current candidate entry and the original coverage it comes from.
  struct CandidateEntry {
    Entry                                          entry;
    std::weak_ptr<const std::vector<std::uint8_t>> identity;
    /// The candidate pixels of an original upload until the upload submission completes.
    AiMaskCoveragePixels                           upload_owner;
    Extent2D                                       analysis_extent{};
    AiMaskEdgeAdjustment                           edge{};
  };

  /// Asset key, analysis size, and the exact bits of the edge values.
  static auto MakeStoredKey(const AiSegmentationMaskSource& source) -> std::string {
    return std::string{source.asset_key.Value()} + "/" +
           std::to_string(source.analysis_extent.width) + "x" +
           std::to_string(source.analysis_extent.height) + "/" +
           std::to_string(std::bit_cast<std::uint32_t>(source.edge.feather_px)) + "/" +
           std::to_string(source.edge.expand_px);
  }

  /// True when @p identity names the allocation of @p pixels. An expired identity keeps its
  /// control block, so it never matches a later allocation at the same address.
  static auto SameAllocation(const std::weak_ptr<const std::vector<std::uint8_t>>& identity,
                             const AiMaskCoveragePixels&                           pixels) -> bool {
    return !identity.owner_before(pixels) && !pixels.owner_before(identity);
  }

  static auto Matches(const std::optional<CandidateEntry>& candidate,
                      const AiMaskCoveragePixels& pixels, Extent2D analysis_extent,
                      const AiMaskEdgeAdjustment& edge) -> bool {
    return candidate.has_value() && SameAllocation(candidate->identity, pixels) &&
           candidate->analysis_extent == analysis_extent && candidate->edge == edge;
  }

  static auto IsIdle(const Entry& entry, const Backend& backend) -> bool {
    return entry.image.use_count() == 1 && !backend.IsResourceBusy(entry.last_used_submission);
  }

  void Touch(Entry& entry, std::uint64_t submission) {
    entry.last_used_submission = submission;
    entry.lru_tick             = lru_clock_;
  }

  /// Allocate and fill one R8 texture. Changes no entry on failure.
  auto Upload(const MaskAssetDescriptor& descriptor, std::span<const std::uint8_t> pixels,
              Backend& backend, CommandContext& command_context) -> Entry {
    const auto width  = descriptor.extent.width;
    const auto height = descriptor.extent.height;
    const auto size   = std::to_string(width) + " x " + std::to_string(height);
    auto       image  = std::make_shared<Image>();
    image->descriptor = descriptor;
    try {
      image->texture = backend.CreateTexture2D(width, height, TextureFormat::R8);
    } catch (const std::exception& error) {
      throw std::runtime_error("AI Mask coverage: allocating the " + size +
                               " R8 texture failed: " + error.what());
    }
    try {
      backend.UploadTexture2D(image->texture,
                              std::span<const std::byte>(
                                  reinterpret_cast<const std::byte*>(pixels.data()), pixels.size()),
                              command_context);
    } catch (const std::exception& error) {
      throw std::runtime_error("AI Mask coverage: uploading the " + size +
                               " R8 texture failed: " + error.what());
    }
    Entry entry;
    entry.image             = std::move(image);
    entry.bytes             = pixels.size();
    entry.upload_submission = command_context.SubmissionId();
    used_bytes_ += entry.bytes;
    ++upload_count_;
    return entry;
  }

  /// @p upload_owner is the read that the upload copied from, or null for a host-computed buffer.
  void Install(std::optional<CandidateEntry>& slot, Entry entry, const AiMaskCoveragePixels& pixels,
               Extent2D analysis_extent, const AiMaskEdgeAdjustment& edge,
               AiMaskCoveragePixels upload_owner) {
    Retire(slot);
    CandidateEntry candidate;
    candidate.entry           = std::move(entry);
    candidate.identity        = pixels;
    candidate.upload_owner    = std::move(upload_owner);
    candidate.analysis_extent = analysis_extent;
    candidate.edge            = edge;
    slot                      = std::move(candidate);
  }

  void Retire(std::optional<CandidateEntry>& slot) {
    if (slot.has_value()) {
      Retire(std::move(slot->entry));
      slot.reset();
    }
  }

  void Retire(Entry entry) {
    if (entry.image) {
      retired_.push_back(std::move(entry));
    }
  }

  void DestroyIdleRetired(const Backend& backend) {
    std::erase_if(retired_, [this, &backend](const Entry& entry) {
      if (!IsIdle(entry, backend)) {
        return false;
      }
      used_bytes_ -= entry.bytes;
      return true;
    });
  }

  void EvictStoredFor(std::size_t new_bytes, const Backend& backend, std::uint64_t submission) {
    DestroyIdleRetired(backend);
    while (!stored_.empty() && used_bytes_ + new_bytes > kMaskCoverageTextureByteBudget) {
      auto victim = stored_.end();
      for (auto it = stored_.begin(); it != stored_.end(); ++it) {
        if (it->last_used_submission == submission || !IsIdle(*it, backend)) {
          continue;
        }
        if (victim == stored_.end() || it->lru_tick < victim->lru_tick) {
          victim = it;
        }
      }
      if (victim == stored_.end()) {
        return;
      }
      used_bytes_ -= victim->bytes;
      stored_.erase(victim);
    }
  }

  std::vector<Entry>            stored_;
  std::optional<CandidateEntry> candidate_source_;
  std::optional<CandidateEntry> candidate_result_;
  std::vector<Entry>            retired_;
  std::size_t                   used_bytes_   = 0;
  std::uint64_t                 lru_clock_    = 0;
  std::uint64_t                 upload_count_ = 0;
};

}  // namespace alcedo
