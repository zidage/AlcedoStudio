//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoders/processor/raw_demosaic_method.hpp"
#include "edit/geometry/types.hpp"
#include "edit/runtime/develop_transient.hpp"
#include "gpu/transient_buffer_arena.hpp"
#include "gpu/transient_buffer_scope.hpp"

namespace alcedo {
namespace {

struct RecordingBackend {
  struct Slab {
    RecordingBackend*             owner = nullptr;
    std::size_t                   bytes  = 0;
    std::unique_ptr<std::byte[]> storage;

    Slab() = default;
    Slab(RecordingBackend* owner_ptr, std::size_t n)
        : owner(owner_ptr), bytes(n), storage(std::make_unique<std::byte[]>(n)) {}
    Slab(const Slab&)                    = delete;
    auto operator=(const Slab&) -> Slab& = delete;
    Slab(Slab&& other) noexcept { *this = std::move(other); }
    auto operator=(Slab&& other) noexcept -> Slab& {
      Reset();
      owner       = other.owner;
      bytes       = other.bytes;
      storage     = std::move(other.storage);
      other.owner = nullptr;
      other.bytes = 0;
      return *this;
    }
    ~Slab() { Reset(); }

    void Reset() noexcept {
      if (owner != nullptr && storage) {
        owner->events.push_back("free");
      }
      owner = nullptr;
      storage.reset();
      bytes = 0;
    }

    [[nodiscard]] auto DevicePointer() const -> void* { return storage.get(); }
    [[nodiscard]] auto Bytes() const -> std::size_t { return bytes; }
  };

  std::vector<std::string>  events;
  std::vector<std::size_t>  create_sizes;
  std::size_t                max_slab_bytes      = 0;
  std::size_t                max_transient_bytes = 0;

  auto CreateSlab(std::size_t n) -> Slab {
    events.push_back("create");
    create_sizes.push_back(n);
    return Slab(this, n);
  }
  [[nodiscard]] auto MaxSlabBytes() const -> std::size_t { return max_slab_bytes; }
  [[nodiscard]] auto MaxTransientBytes() const -> std::size_t { return max_transient_bytes; }
};

TEST(TransientBufferArena, SmallAllocationsShareAMinimumSlab) {
  RecordingBackend backend;
  TransientBufferArena<RecordingBackend> arena(backend);
  void* first  = arena.Allocate(16, 1);
  void* second = arena.Allocate(32, 1);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(backend.create_sizes,
            (std::vector<std::size_t>{TransientBufferArena<RecordingBackend>::kMinSlabBytes}));
  EXPECT_EQ(arena.slab_count(), 1U);
  EXPECT_EQ(arena.used_bytes(), 48U);
}

TEST(TransientBufferArena, LargeRequestRoundsUpToSlabQuantumAndDoesNotSpanSlabs) {
  RecordingBackend backend;
  TransientBufferArena<RecordingBackend> arena(backend);
  constexpr std::size_t kRequest = 20ull << 20;
  void*                  plane  = arena.Allocate(kRequest);
  ASSERT_NE(plane, nullptr);
  ASSERT_EQ(backend.create_sizes.size(), 1U);
  EXPECT_EQ(backend.create_sizes.front(), TransientBufferArena<RecordingBackend>::kSlabQuantumBytes);
  EXPECT_GE(backend.create_sizes.front(), kRequest);
}

TEST(TransientBufferArena, LivePointerStaysValidWhenASecondSlabIsAppended) {
  RecordingBackend backend;
  backend.max_slab_bytes = 512;
  TransientBufferArena<RecordingBackend> arena(backend);
  void* first = arena.Allocate(257, 256);
  ASSERT_NE(first, nullptr);
  static_cast<std::byte*>(first)[0] = std::byte{0x5A};
  void* second                       = arena.Allocate(512, 256);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first, second);
  EXPECT_EQ(arena.slab_count(), 2U);
  EXPECT_EQ(static_cast<std::byte*>(first)[0], std::byte{0x5A});
}

TEST(TransientBufferArena, TransientBudgetRejectsAnotherSlabWithoutMovingLivePointers) {
  RecordingBackend backend;
  backend.max_slab_bytes      = 400;
  backend.max_transient_bytes = 800;
  TransientBufferArena<RecordingBackend> arena(backend);
  void* first  = arena.Allocate(400);
  void* second = arena.Allocate(400);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(arena.slab_count(), 2U);
  try {
    (void)arena.Allocate(400);
    FAIL() << "expected transient budget error";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("exceed transient budget"), std::string::npos);
    EXPECT_NE(message.find("slabs=2"), std::string::npos);
    EXPECT_NE(message.find("requested=400"), std::string::npos);
  }
}

TEST(TransientBufferArena, ScopeRewindsLocalSlabOffsetsWithoutFreeing) {
  RecordingBackend backend;
  TransientBufferArena<RecordingBackend> arena(backend);
  arena.Reserve(4096);
  void* outer = arena.Allocate(100, 1);
  ASSERT_NE(outer, nullptr);
  const auto used_after_outer = arena.used_bytes();
  backend.events.clear();
  {
    TransientBufferScope<RecordingBackend> scope(arena);
    ASSERT_NE(arena.Allocate(200, 1), nullptr);
    EXPECT_GT(arena.used_bytes(), used_after_outer);
  }
  EXPECT_EQ(arena.used_bytes(), used_after_outer);
  EXPECT_TRUE(backend.events.empty());
  void* reused = arena.Allocate(100, 1);
  EXPECT_NE(reused, nullptr);
}

TEST(TransientBufferArena, ReserveClampsToMaxTransientInsteadOfThrowingOnTheRemainderSlab) {
  RecordingBackend backend;
  backend.max_slab_bytes      = 400;
  backend.max_transient_bytes = 800;
  TransientBufferArena<RecordingBackend> arena(backend);
  arena.Reserve(1000);
  EXPECT_EQ(arena.capacity_bytes(), 0U);
  EXPECT_TRUE(backend.create_sizes.empty());
  void* first  = arena.Allocate(400);
  void* second = arena.Allocate(400);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(arena.capacity_bytes(), 800U);
  EXPECT_EQ(arena.slab_count(), 2U);
}

TEST(TransientBufferArena, ReserveLargerThanMaxSlabDoesNotPreSplitIntoUnusableRemainder) {
  RecordingBackend backend;
  backend.max_slab_bytes      = 400;
  backend.max_transient_bytes = 800;
  TransientBufferArena<RecordingBackend> arena(backend);
  arena.Reserve(700);
  EXPECT_EQ(arena.capacity_bytes(), 0U);
  void* first  = arena.Allocate(350);
  void* second = arena.Allocate(350);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(arena.slab_count(), 2U);
  EXPECT_EQ(arena.capacity_bytes(), 800U);
}

TEST(TransientBufferArena, ReserveWithinOneSlabStillCreatesCapacityUpFront) {
  RecordingBackend backend;
  backend.max_slab_bytes      = 400;
  backend.max_transient_bytes = 800;
  TransientBufferArena<RecordingBackend> arena(backend);
  arena.Reserve(300);
  EXPECT_EQ(arena.capacity_bytes(), 300U);
  EXPECT_EQ(arena.slab_count(), 1U);
}

TEST(TransientBufferArena, BackgroundScratchAllocatesOnlyRequestedAlignedBytes) {
  RecordingBackend backend;
  TransientBufferArena<RecordingBackend> arena(backend);
  arena.SetAllocationPolicy(TransientAllocationPolicy::ExactRelease);
  EXPECT_THROW(arena.Reserve(1u << 20), std::runtime_error);
  constexpr std::size_t kAlign = 256;
  void* first  = arena.Allocate(64, kAlign);
  void* second = arena.Allocate(64, kAlign);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first, second);
  ASSERT_EQ(backend.create_sizes.size(), 2U);
  EXPECT_EQ(backend.create_sizes[0], kAlign);
  EXPECT_EQ(backend.create_sizes[1], kAlign);
  EXPECT_LT(backend.create_sizes[0], TransientBufferArena<RecordingBackend>::kMinSlabBytes);
  EXPECT_EQ(arena.slab_count(), 2U);
  arena.Reset();
  backend.create_sizes.clear();
  (void)arena.Allocate(96, kAlign);
  ASSERT_EQ(backend.create_sizes.size(), 1U);
  EXPECT_EQ(backend.create_sizes.front(), kAlign);
}

TEST(TransientBufferArena, BackgroundScratchReleasesStorageAfterLastUse) {
  RecordingBackend backend;
  TransientBufferArena<RecordingBackend> arena(backend);
  arena.SetAllocationPolicy(TransientAllocationPolicy::ExactRelease);
  void* outer = arena.Allocate(64, 256);
  ASSERT_NE(outer, nullptr);
  static_cast<std::byte*>(outer)[0] = std::byte{0x11};
  backend.events.clear();
  {
    TransientBufferScope<RecordingBackend> inner(arena);
    ASSERT_NE(arena.Allocate(32, 256), nullptr);
    EXPECT_EQ(arena.slab_count(), 2U);
  }
  ASSERT_FALSE(backend.events.empty());
  EXPECT_EQ(backend.events.back(), "free");
  EXPECT_EQ(arena.slab_count(), 1U);
  EXPECT_EQ(static_cast<std::byte*>(outer)[0], std::byte{0x11});
  backend.events.clear();
  arena.Reset();
  EXPECT_EQ(arena.slab_count(), 0U);
  EXPECT_EQ(arena.used_bytes(), 0U);
  ASSERT_FALSE(backend.events.empty());
  EXPECT_EQ(backend.events.back(), "free");
}

TEST(DevelopTransientFailure, DescribesResolvedDemosaicMethodNotEmptyPayloadString) {
  DevelopCompileSource source;
  source.kind        = DevelopInputKind::BayerCfa;
  source.host_extent = Extent2D{11808, 8754};
  const auto message = DescribeDevelopTransientFailure(
      source, RawDemosaicMethodToString(RawDemosaicMethod::NeuralEngine), true,
      "TransientBufferArena: allocation would exceed transient budget");
  EXPECT_NE(message.find("extent=11808x8754"), std::string::npos);
  EXPECT_NE(message.find("kind=0"), std::string::npos);
  EXPECT_NE(message.find("method=neural_engine"), std::string::npos);
  EXPECT_NE(message.find("hlr=1"), std::string::npos);
}

TEST(DevelopTransientSizing, NeuralConservativeBytesIncludeTileScratchAndExceedLegacy) {
  DevelopCompileSource source;
  source.kind        = DevelopInputKind::BayerCfa;
  source.host_extent = Extent2D{64, 64};
  EXPECT_EQ(ConservativeDevelopInitialBytes(source, RawDemosaicMethod::NeuralEngine),
            64U * 64U * kConservativeNeuralDevelopBytesPerPixel +
                kConservativeNeuralTileScratchBytes);
  EXPECT_GT(ConservativeDevelopInitialBytes(source, RawDemosaicMethod::NeuralEngine),
            ConservativeDevelopInitialBytes(source, RawDemosaicMethod::Legacy));
  EXPECT_EQ(ConservativeDevelopInitialBytes(source, RawDemosaicMethod::Legacy),
            64U * 64U * kConservativeDevelopBytesPerPixel);
  EXPECT_EQ(ConservativeDevelopInitialBytes(DevelopCompileSource{.kind = DevelopInputKind::DirectRgb}),
            0U);
}

TEST(TransientBufferArena, ExactReleaseCanFreeAFinishedSlabWhileLaterSlabsStay) {
  RecordingBackend backend;
  TransientBufferArena<RecordingBackend> arena(backend);
  arena.SetAllocationPolicy(TransientAllocationPolicy::ExactRelease);
  void* first  = arena.Allocate(64, 256);
  void* second = arena.Allocate(64, 256);
  void* third  = arena.Allocate(64, 256);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_NE(third, nullptr);
  static_cast<std::byte*>(first)[0]  = std::byte{0x11};
  static_cast<std::byte*>(third)[0]  = std::byte{0x33};
  EXPECT_EQ(arena.slab_count(), 3U);
  backend.events.clear();
  arena.ReleaseSlabContaining(second);
  EXPECT_EQ(backend.events.back(), "free");
  EXPECT_EQ(arena.slab_count(), 2U);
  EXPECT_EQ(static_cast<std::byte*>(first)[0], std::byte{0x11});
  EXPECT_EQ(static_cast<std::byte*>(third)[0], std::byte{0x33});
  arena.ReleaseSlabContaining(first);
  arena.ReleaseSlabContaining(third);
  EXPECT_EQ(arena.slab_count(), 0U);
}

}  // namespace
}  // namespace alcedo
