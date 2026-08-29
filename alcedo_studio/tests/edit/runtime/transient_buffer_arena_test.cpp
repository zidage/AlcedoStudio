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

TEST(DevelopTransientHighWater, SuggestsConservativeBytesThenObservedCapacityWithMargin) {
  DevelopCompileSource source;
  source.kind             = DevelopInputKind::BayerCfa;
  source.host_extent      = Extent2D{64, 64};
  DevelopTransientHighWaterCache cache;
  EXPECT_EQ(cache.SuggestInitial(source, 1), 64U * 64U * kConservativeDevelopBytesPerPixel);
  cache.Record(source, 1, 200000);
  EXPECT_EQ(cache.SuggestInitial(source, 1), ApplyDevelopTransientSafetyMargin(200000));
  EXPECT_EQ(ConservativeDevelopInitialBytes(DevelopCompileSource{.kind = DevelopInputKind::DirectRgb}),
            0U);
}

}  // namespace
}  // namespace alcedo
