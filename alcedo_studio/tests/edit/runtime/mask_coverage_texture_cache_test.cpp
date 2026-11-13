//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SM8.1, SM8.2: the AI Mask coverage owner of the render workspace over a host resource factory.
// The factory records uploads and edge work and lets each test complete submissions, so the checks
// observe texture lifetimes, read lifetimes, and failures without a GPU.

#include "edit/runtime/mask_coverage_texture_cache.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ai_mask_edge_reference.hpp"
#include "edit/mask/ai_mask_coverage_texture.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/ai_mask_coverage_work.hpp"
#include "edit/runtime/mask_asset_reader.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

/// Host textures and submissions. A submission is busy until @ref Complete reaches it.
struct HostCoverageBackend {
  struct Texture2D {
    std::uint32_t          width  = 0;
    std::uint32_t          height = 0;
    std::vector<std::byte> storage;
    /// Shared with the backend: its use count is the number of live textures plus one.
    std::shared_ptr<int>   live;

    Texture2D()                                                      = default;
    Texture2D(const Texture2D&)                                      = delete;
    auto operator=(const Texture2D&) -> Texture2D&                   = delete;
    Texture2D(Texture2D&&) noexcept                                  = default;
    auto               operator=(Texture2D&&) noexcept -> Texture2D& = default;

    [[nodiscard]] auto Bytes() const -> std::size_t { return storage.size(); }
  };

  class CommandContext {
   public:
    [[nodiscard]] auto SubmissionId() const -> std::uint64_t { return submission_id_; }
    void               SetSubmissionId(std::uint64_t id) { submission_id_ = id; }

   private:
    std::uint64_t submission_id_ = 0;
  };

  auto CreateTexture2D(std::uint32_t width, std::uint32_t height, TextureFormat format)
      -> Texture2D {
    if (fail_next_create) {
      fail_next_create = false;
      throw std::runtime_error("host device is out of memory");
    }
    Texture2D texture;
    texture.width  = width;
    texture.height = height;
    texture.storage.resize(static_cast<std::size_t>(width) * height *
                           TextureFormatBytesPerPixel(format));
    texture.live = live;
    return texture;
  }

  void UploadTexture2D(Texture2D& texture, std::span<const std::byte> bytes, CommandContext&) {
    if (fail_next_upload) {
      fail_next_upload = false;
      throw std::runtime_error("host copy failed");
    }
    if (bytes.size() != texture.storage.size()) {
      throw std::runtime_error("upload size does not match the texture");
    }
    std::copy(bytes.begin(), bytes.end(), texture.storage.begin());
    ++uploads;
  }

  /// Device edge work of the coverage owner, executed at record time on the host.
  void RecordAiMaskEdge(const AiMaskEdgeDispatch&            dispatch,
                        const AiMaskEdgeTextures<Texture2D>& textures, CommandContext&) {
    if (fail_next_edge) {
      fail_next_edge = false;
      throw std::runtime_error("host edge pass failed");
    }
    const auto& source = textures.source->storage;
    const auto  result = ai_mask_test::ExecuteAiMaskEdgeDispatchOnHost(
        dispatch, std::span<const std::uint8_t>(
                      reinterpret_cast<const std::uint8_t*>(source.data()), source.size()));
    if (result.size() != textures.output->storage.size()) {
      throw std::runtime_error("edge output size does not match the texture");
    }
    std::memcpy(textures.output->storage.data(), result.data(), result.size());
    ++edge_work;
  }

  [[nodiscard]] auto IsResourceBusy(std::uint64_t submitted_on) const -> bool {
    return submitted_on != 0 && submitted_on > completed;
  }

  [[nodiscard]] auto   LiveTextures() const -> long { return live.use_count() - 1; }

  std::shared_ptr<int> live             = std::make_shared<int>(0);
  std::uint64_t        completed        = 0;
  int                  uploads          = 0;
  int                  edge_work        = 0;
  bool                 fail_next_create = false;
  bool                 fail_next_upload = false;
  bool                 fail_next_edge   = false;
};

using Cache = MaskCoverageTextureCache<HostCoverageBackend>;
using Read  = Cache::Read;

constexpr Extent2D   kAnalysis{48, 32};

/// In-memory Mask asset reader that counts reads.
class CountingReader final : public MaskAssetReader {
 public:
  void Put(MaskAsset asset) { assets_[std::string{asset.key.Value()}] = std::move(asset); }

  [[nodiscard]] auto Read(const MaskAssetKey& key) const -> MaskAsset override {
    ++reads;
    const auto it = assets_.find(std::string{key.Value()});
    if (it == assets_.end()) {
      throw std::runtime_error("Mask asset " + std::string{key.Value()} + " is missing");
    }
    return it->second;
  }

  mutable int reads = 0;

 private:
  std::unordered_map<std::string, MaskAsset> assets_;
};

class MaskCoverageTextureCacheTest : public ::testing::Test {
 protected:
  /// Start the next submission, as BeginRender or BeginCoverageWork do.
  void Begin() { context_.SetSubmissionId(++submission_); }
  /// The GPU completes every submission so far.
  void CompleteAll() { backend_.completed = submission_; }

  auto Candidate(const AiMaskCoveragePixels& pixels, AiMaskEdgeAdjustment edge = {}) -> Read {
    return cache_.AcquireCandidate(pixels, kAnalysis, edge, backend_, context_);
  }

  static auto Pixels(std::uint8_t value) -> AiMaskCoveragePixels {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kAnalysis.width) * kAnalysis.height,
                                     0);
    for (std::uint32_t y = 8; y < 24; ++y) {
      for (std::uint32_t x = 10; x < 30; ++x) {
        pixels[static_cast<std::size_t>(y) * kAnalysis.width + x] = value;
      }
    }
    return std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels));
  }

  static auto Bytes(const Read& read) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> bytes(read->texture.storage.size());
    std::memcpy(bytes.data(), read->texture.storage.data(), bytes.size());
    return bytes;
  }

  /// Edge result of the whole-image candidate @p pixels by the host function of the runtime.
  static auto EdgeReference(const AiMaskCoveragePixels& pixels, AiMaskEdgeAdjustment edge)
      -> AiMaskCoverageTexture {
    MaskAsset whole;
    whole.descriptor = AiMaskCandidateDescriptor(kAnalysis);
    whole.pixels     = *pixels;
    return MakeAiMaskCoverageTexture(whole, kAnalysis, edge);
  }

  HostCoverageBackend                 backend_;
  HostCoverageBackend::CommandContext context_;
  Cache                               cache_;
  std::uint64_t                       submission_ = 0;
};

auto StoredMask(const MaskAsset& asset, AiMaskEdgeAdjustment edge) -> AiSegmentationMaskSource {
  AiSegmentationMaskSource source;
  source.asset_key       = asset.key;
  source.descriptor      = asset.descriptor;
  source.analysis_extent = kAnalysis;
  source.edge            = edge;
  return source;
}

auto RectAsset() -> MaskAsset {
  MaskAsset asset;
  asset.descriptor.extent = Extent2D{12, 8};
  asset.descriptor.reference_bounds =
      NormalizedRect{12.0f / 48.0f, 8.0f / 32.0f, 12.0f / 48.0f, 8.0f / 32.0f};
  asset.pixels.assign(12 * 8, 0);
  for (std::uint32_t y = 1; y < 7; ++y) {
    for (std::uint32_t x = 1; x < 11; ++x) {
      asset.pixels[y * 12 + x] = 255;
    }
  }
  asset.key = MakeMaskAssetKey(asset.descriptor, asset.pixels);
  return asset;
}

TEST_F(MaskCoverageTextureCacheTest, CandidateReplacementKeepsPredecessorOnlyForActualReaders) {
  const auto first  = Pixels(100);
  const auto second = Pixels(200);
  const auto third  = Pixels(250);

  Begin();
  auto first_read = Candidate(first);
  EXPECT_EQ(Bytes(first_read), *first);
  CompleteAll();

  // A display reader still holds the first coverage when the second replaces it.
  Begin();
  const auto second_read = Candidate(second);
  EXPECT_EQ(Bytes(second_read), *second);
  EXPECT_EQ(cache_.RetiredCount(), 1U);
  CompleteAll();
  cache_.ReleaseIdle(backend_);
  EXPECT_EQ(cache_.RetiredCount(), 1U) << "a held read keeps the replaced texture";
  EXPECT_EQ(Bytes(first_read), *first) << "the replaced texture keeps its content";

  first_read.reset();
  cache_.ReleaseIdle(backend_);
  EXPECT_EQ(cache_.RetiredCount(), 0U);
  EXPECT_EQ(backend_.LiveTextures(), 1);

  // Without a reader, an in-flight submission alone keeps the replaced texture.
  Begin();
  (void)Candidate(third);  // Retires the second texture, which second_read holds.
  Begin();
  (void)Candidate(Pixels(10));  // Retires the third texture, busy in an incomplete submission.
  cache_.ReleaseIdle(backend_);
  EXPECT_EQ(cache_.RetiredCount(), 2U);
  CompleteAll();
  cache_.ReleaseIdle(backend_);
  EXPECT_EQ(cache_.RetiredCount(), 1U) << "only second_read keeps a replaced texture";
  EXPECT_EQ(Bytes(second_read), *second);
  EXPECT_EQ(backend_.LiveTextures(), 2);
  EXPECT_EQ(cache_.EntryCount(), 2U);
  EXPECT_EQ(cache_.UsedBytes(), 2 * first->size());
}

TEST_F(MaskCoverageTextureCacheTest, CandidateUploadHoldsPixelsUntilSubmissionCompletes) {
  auto                                                 pixels     = Pixels(180);
  const std::weak_ptr<const std::vector<std::uint8_t>> allocation = pixels;

  Begin();
  (void)Candidate(pixels);
  pixels.reset();  // The session composed a new coverage and released this allocation.
  EXPECT_TRUE(cache_.HoldsCandidateUploadPixels());
  EXPECT_FALSE(allocation.expired());
  cache_.ReleaseIdle(backend_);
  EXPECT_FALSE(allocation.expired()) << "the upload submission has not completed";

  CompleteAll();
  cache_.ReleaseIdle(backend_);
  EXPECT_FALSE(cache_.HoldsCandidateUploadPixels());
  EXPECT_TRUE(allocation.expired());

  // A later allocation never matches the released identity, even at the same address.
  Begin();
  const auto later = Pixels(180);
  (void)Candidate(later);
  EXPECT_EQ(backend_.uploads, 2);
}

TEST_F(MaskCoverageTextureCacheTest, CandidateEdgeChangeKeepsOriginalUploadAndOneCurrentResult) {
  const auto                 pixels = Pixels(220);
  const AiMaskEdgeAdjustment feather{3.0f, 0};
  const AiMaskEdgeAdjustment expand{0.0f, 2};

  Begin();
  const auto original = Candidate(pixels);
  EXPECT_EQ(original->descriptor, AiMaskCandidateDescriptor(kAnalysis));
  EXPECT_EQ(backend_.uploads, 1);

  // The edge result is a separate texture; the original upload stays current.
  const auto feathered = Candidate(pixels, feather);
  const auto reference = EdgeReference(pixels, feather);
  EXPECT_EQ(feathered->descriptor, reference.descriptor);
  EXPECT_LE(ai_mask_test::MaxR8Difference(Bytes(feathered), reference.pixels), 1);
  EXPECT_NE(feathered.get(), original.get());
  EXPECT_EQ(backend_.uploads, 1) << "the edge work reads the resident original upload";
  EXPECT_EQ(backend_.edge_work, 1);
  EXPECT_EQ(Candidate(pixels, feather).get(), feathered.get());
  EXPECT_EQ(backend_.edge_work, 1);

  // A second edge value replaces the edge result; the original upload is still reused.
  const auto expanded = Candidate(pixels, expand);
  EXPECT_EQ(Bytes(expanded), EdgeReference(pixels, expand).pixels);
  EXPECT_EQ(backend_.uploads, 1);
  EXPECT_EQ(backend_.edge_work, 2);
  EXPECT_EQ(cache_.RetiredCount(), 1U);
  EXPECT_EQ(Candidate(pixels).get(), original.get());
  EXPECT_EQ(backend_.uploads, 1);
  EXPECT_EQ(cache_.RetiredCount(), 2U) << "zero edge makes the original the current result";
}

TEST_F(MaskCoverageTextureCacheTest, InvalidCoverageInputsReportErrorsAndKeepCurrentEntries) {
  const auto pixels = Pixels(90);
  Begin();
  const auto current      = Candidate(pixels);
  const auto entries      = cache_.EntryCount();

  const auto expect_error = [&](auto acquire, const std::string& text) {
    try {
      (void)acquire();
      ADD_FAILURE() << "no error for: " << text;
    } catch (const std::runtime_error& error) {
      const std::string message = error.what();
      EXPECT_NE(message.find("AI Mask coverage:"), std::string::npos) << message;
      EXPECT_NE(message.find(text), std::string::npos) << message;
    }
  };
  expect_error([&] { return Candidate(nullptr); }, "no pixels");
  expect_error(
      [&] { return cache_.AcquireCandidate(pixels, Extent2D{48, 31}, {}, backend_, context_); },
      "analysis image needs 1488");
  expect_error(
      [&] { return cache_.AcquireCandidate(pixels, Extent2D{0, 32}, {}, backend_, context_); },
      "analysis image size 0 x 32");
  expect_error(
      [&] { return cache_.AcquireCandidate(pixels, Extent2D{4097, 1}, {}, backend_, context_); },
      "analysis image size 4097 x 1");
  expect_error([&] { return Candidate(pixels, AiMaskEdgeAdjustment{50.5f, 0}); }, "feather_px");
  expect_error([&] { return Candidate(pixels, AiMaskEdgeAdjustment{0.0f, -51}); }, "expand_px");

  CountingReader reader;
  auto           outside                = RectAsset();
  outside.descriptor.reference_bounds.x = 0.9f;
  expect_error(
      [&] { return cache_.AcquireStored(StoredMask(outside, {}), reader, backend_, context_); },
      "does not lie in the 48 x 32 analysis image");
  EXPECT_EQ(reader.reads, 0) << "validation precedes the asset read";

  EXPECT_EQ(cache_.EntryCount(), entries);
  EXPECT_EQ(backend_.uploads, 1);
  EXPECT_EQ(Candidate(pixels).get(), current.get());
}

TEST_F(MaskCoverageTextureCacheTest, AllocationOrUploadFailureKeepsCurrentCandidate) {
  const auto first = Pixels(60);
  Begin();
  const auto current        = Candidate(first);

  const auto replacement    = Pixels(61);
  backend_.fail_next_create = true;
  try {
    (void)Candidate(replacement);
    ADD_FAILURE() << "the allocation failure was not reported";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(),
                 "AI Mask coverage: allocating the 48 x 32 R8 texture failed: host device is out "
                 "of memory");
  }
  backend_.fail_next_upload = true;
  try {
    (void)Candidate(replacement, AiMaskEdgeAdjustment{1.0f, 1});
    ADD_FAILURE() << "the upload failure was not reported";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("AI Mask coverage: uploading the"), std::string::npos) << message;
    EXPECT_NE(message.find("host copy failed"), std::string::npos) << message;
  }

  EXPECT_EQ(cache_.RetiredCount(), 0U);
  EXPECT_EQ(cache_.EntryCount(), 1U);
  EXPECT_EQ(backend_.LiveTextures(), 1);
  EXPECT_EQ(cache_.UsedBytes(), first->size());
  EXPECT_EQ(Candidate(first).get(), current.get());
  EXPECT_EQ(Bytes(current), *first);
}

TEST_F(MaskCoverageTextureCacheTest, CancelledSubmissionDiscardsItsCoverageWrites) {
  const auto     asset = RectAsset();
  CountingReader reader;
  reader.Put(asset);
  const auto pixels = Pixels(140);

  Begin();
  const auto kept = cache_.AcquireStored(StoredMask(asset, {}), reader, backend_, context_);
  EXPECT_EQ(Bytes(kept), asset.pixels);
  EXPECT_EQ(kept->descriptor, asset.descriptor);
  CompleteAll();

  // A failed encode records a new edge result and uploads a candidate, then is cancelled.
  Begin();
  const auto cancelled = context_.SubmissionId();
  (void)cache_.AcquireStored(StoredMask(asset, AiMaskEdgeAdjustment{2.0f, 0}), reader, backend_,
                             context_);
  (void)Candidate(pixels);
  EXPECT_EQ(backend_.uploads, 2);
  EXPECT_EQ(backend_.edge_work, 1);
  cache_.DiscardWritesOf(cancelled);
  EXPECT_EQ(cache_.RetiredCount(), 2U);

  // The next submission writes them again; the completed stored upload stays.
  Begin();
  EXPECT_EQ(cache_.AcquireStored(StoredMask(asset, {}), reader, backend_, context_).get(),
            kept.get());
  (void)cache_.AcquireStored(StoredMask(asset, AiMaskEdgeAdjustment{2.0f, 0}), reader, backend_,
                             context_);
  (void)Candidate(pixels);
  EXPECT_EQ(backend_.uploads, 3);
  EXPECT_EQ(backend_.edge_work, 2);
  EXPECT_EQ(reader.reads, 1);
  CompleteAll();
  cache_.ReleaseIdle(backend_);
  EXPECT_EQ(cache_.RetiredCount(), 0U);
}

TEST_F(MaskCoverageTextureCacheTest, StoredCoverageSeparatesOriginalUploadFromEdgeResult) {
  const auto     asset = RectAsset();
  CountingReader reader;
  reader.Put(asset);
  const AiMaskEdgeAdjustment edge{1.5f, 1};

  Begin();
  const auto original = cache_.AcquireStored(StoredMask(asset, {}), reader, backend_, context_);
  EXPECT_EQ(original->descriptor, asset.descriptor);
  EXPECT_EQ(Bytes(original), asset.pixels);

  const auto adjusted  = cache_.AcquireStored(StoredMask(asset, edge), reader, backend_, context_);
  const auto reference = MakeAiMaskCoverageTexture(asset, kAnalysis, edge);
  EXPECT_EQ(adjusted->descriptor, reference.descriptor);
  EXPECT_LE(ai_mask_test::MaxR8Difference(Bytes(adjusted), reference.pixels), 1);
  // Margin 1 + ceil(4 * 1.5) + 1 = 8 on each side; the top side reaches the image edge.
  EXPECT_EQ(adjusted->descriptor.extent, (Extent2D{12 + 2 * 8, 8 + 2 * 8}));

  // Content keys: repeated acquisitions reuse both entries without a read or an upload.
  EXPECT_EQ(cache_.AcquireStored(StoredMask(asset, edge), reader, backend_, context_).get(),
            adjusted.get());
  EXPECT_EQ(cache_.AcquireStored(StoredMask(asset, {}), reader, backend_, context_).get(),
            original.get());
  EXPECT_EQ(reader.reads, 1) << "the edge work reads the resident original upload";
  EXPECT_EQ(backend_.uploads, 1);
  EXPECT_EQ(backend_.edge_work, 1);
}

TEST_F(MaskCoverageTextureCacheTest, ClearKeepsTexturesThatReadsStillUse) {
  const auto     asset = RectAsset();
  CountingReader reader;
  reader.Put(asset);
  Begin();
  auto       held  = Candidate(Pixels(70));
  const auto other = cache_.AcquireStored(StoredMask(asset, {}), reader, backend_, context_);
  (void)other;
  CompleteAll();

  cache_.Clear();
  EXPECT_EQ(cache_.EntryCount(), 2U) << "both textures still have reads";
  EXPECT_EQ(cache_.RetiredCount(), 2U);
  const auto pixels = Pixels(70);
  EXPECT_EQ(Bytes(held), *pixels);

  held.reset();
  cache_.ReleaseIdle(backend_);
  EXPECT_EQ(cache_.RetiredCount(), 1U);
  EXPECT_EQ(backend_.LiveTextures(), 1);
}

TEST_F(MaskCoverageTextureCacheTest, EdgeWorkReusesWorkTexturesOfTheSameOutputSize) {
  const auto pixels = Pixels(230);
  Begin();
  (void)Candidate(pixels, AiMaskEdgeAdjustment{2.0f, 3});
  // Expand 3 with Feather: two R8 morphology textures and one R32f Feather texture.
  EXPECT_EQ(cache_.WorkTextureCount(), 3U);
  EXPECT_EQ(cache_.WorkTextureBytes(), 2 * pixels->size() + 4 * pixels->size());
  const auto live = backend_.LiveTextures();

  // Another edge value of the same output size allocates only its result.
  CompleteAll();
  Begin();
  (void)Candidate(pixels, AiMaskEdgeAdjustment{4.0f, -5});
  EXPECT_EQ(cache_.WorkTextureCount(), 3U);
  EXPECT_EQ(backend_.LiveTextures(), live + 1) << "the replaced result is retired, not destroyed";
  CompleteAll();
  cache_.ReleaseIdle(backend_);
  EXPECT_EQ(backend_.LiveTextures(), live);

  // A stored result of another output size destroys the idle work textures of the old size.
  const auto     asset = RectAsset();
  CountingReader reader;
  reader.Put(asset);
  Begin();
  const auto stored = cache_.AcquireStored(StoredMask(asset, AiMaskEdgeAdjustment{0.0f, 2}), reader,
                                           backend_, context_);
  EXPECT_EQ(cache_.WorkTextureCount(), 1U) << "Expand 2 without Feather needs one work texture";
  EXPECT_EQ(cache_.WorkTextureBytes(), static_cast<std::size_t>(stored->descriptor.extent.width) *
                                           stored->descriptor.extent.height);

  CompleteAll();
  cache_.Clear();
  EXPECT_EQ(cache_.WorkTextureCount(), 0U);
  EXPECT_EQ(cache_.WorkTextureBytes(), 0U);
}

TEST_F(MaskCoverageTextureCacheTest, FailedEdgeWorkKeepsCurrentResultAndEarlierReads) {
  const auto                 pixels = Pixels(210);
  const AiMaskEdgeAdjustment shown{3.0f, 1};
  Begin();
  const auto current       = Candidate(pixels, shown);
  const auto current_bytes = Bytes(current);
  const auto entries       = cache_.EntryCount();
  const auto used          = cache_.UsedBytes();
  CompleteAll();

  Begin();
  const auto failed       = context_.SubmissionId();
  backend_.fail_next_edge = true;
  try {
    (void)Candidate(pixels, AiMaskEdgeAdjustment{6.0f, 2});
    ADD_FAILURE() << "the edge work failure was not reported";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(),
                 "AI Mask coverage: recording the Feather and Expand work failed: host edge pass "
                 "failed");
  }
  backend_.fail_next_create = true;
  try {
    (void)Candidate(pixels, AiMaskEdgeAdjustment{6.0f, 2});
    ADD_FAILURE() << "the output allocation failure was not reported";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(),
                 "AI Mask coverage: allocating the 48 x 32 R8 texture failed: host device is out "
                 "of memory");
  }
  // The caller cancels the failed work; nothing that the cancelled submission wrote is current.
  cache_.DiscardWritesOf(failed);
  EXPECT_EQ(cache_.EntryCount(), entries);
  EXPECT_EQ(cache_.UsedBytes(), used);
  EXPECT_EQ(cache_.RetiredCount(), 0U);

  // The published result stays current and unchanged; the original upload is still reused.
  Begin();
  EXPECT_EQ(Candidate(pixels, shown).get(), current.get());
  EXPECT_EQ(Bytes(current), current_bytes);
  EXPECT_EQ(backend_.uploads, 1);
  EXPECT_EQ(backend_.edge_work, 1);
}

}  // namespace
}  // namespace alcedo
