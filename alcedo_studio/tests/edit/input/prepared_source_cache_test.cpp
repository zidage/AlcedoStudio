//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

#include "edit/input/prepared_source_cache.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "prepared_raw_test_support.hpp"

namespace alcedo {
namespace {

auto MakeEncoded(std::uint8_t tag) -> std::vector<std::byte> {
  std::vector<std::byte> bytes(32);
  bytes[0] = std::byte{tag};
  bytes[1] = std::byte{0xA5};
  return bytes;
}

auto MakeUnpacker(int* calls) -> PreparedSourceCache::UnpackFn {
  return [calls](std::span<const std::byte>, DecodeRes decode_res) {
    ++*calls;
    const auto pattern = gpu_dag_test::MakeRggbPattern();
    return RawInputLoader::FromUnpackedCfa(
        gpu_dag_test::MakeU16CfaPlane(32, 32, pattern), pattern, gpu_dag_test::DefaultLinearization(),
        gpu_dag_test::FullSensor(32, 32), decode_res);
  };
}

TEST(GpuDagRawInput, PreparedSourceKeyIncludesEncodedHashKindDecodeResAndPreparationVersion) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto full    = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(32, 32, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(32, 32), DecodeRes::FULL);
  const auto half = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(32, 32, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(32, 32), DecodeRes::HALF);

  EXPECT_EQ(full.source_key.encoded_content_hash, half.source_key.encoded_content_hash);
  EXPECT_EQ(full.source_key.preparation_version, kRawInputPreparationVersion);
  EXPECT_EQ(full.source_key.downsample_passes, 0);
  EXPECT_EQ(half.source_key.downsample_passes, 1);
  EXPECT_EQ(full.source_key.input_kind, RawInputKind::BayerRaw);
  EXPECT_NE(full.source_key.Lookup(), half.source_key.Lookup());
  EXPECT_EQ(full.content_key.content_hash, full.source_key.encoded_content_hash);
}

TEST(GpuDagRawInput, PreparedSourceCacheReusesHostResultForSameEncodedKeyWithoutCallingUnpack) {
  int                  unpack_calls = 0;
  PreparedSourceCache  cache(MakeUnpacker(&unpack_calls));
  const auto           encoded = MakeEncoded(1);

  const auto first  = cache.AcquireEncoded(encoded, DecodeRes::FULL);
  const auto second = cache.AcquireEncoded(encoded, DecodeRes::FULL);

  EXPECT_EQ(unpack_calls, 1);
  EXPECT_EQ(cache.GetStats().libraw_open_unpack_count, 1U);
  EXPECT_EQ(cache.GetStats().hits, 1U);
  EXPECT_EQ(cache.GetStats().misses, 1U);
  EXPECT_EQ(first.Shared().get(), second.Shared().get());
  EXPECT_EQ(first.Key(), second.Key());
  EXPECT_EQ(first.Key().downsample_passes, 0);
}

TEST(GpuDagRawInput, PreparedSourceCacheSharesHostResultAcrossPreviewAndExportQuality) {
  int                 unpack_calls = 0;
  PreparedSourceCache cache(MakeUnpacker(&unpack_calls));
  const auto          encoded = MakeEncoded(2);

  const auto preview = cache.AcquireEncoded(encoded, DecodeRes::FULL);
  const auto export_frame = cache.AcquireEncoded(encoded, DecodeRes::FULL);

  EXPECT_EQ(unpack_calls, 1);
  EXPECT_EQ(preview.Shared().get(), export_frame.Shared().get());
  EXPECT_EQ(preview.Get().pixels.bytes.get(), export_frame.Get().pixels.bytes.get());
}

TEST(GpuDagRawInput, PreparedSourceCacheUsesNewKeyForDecodeResChangeWithoutReplacingLeasedEntry) {
  int                 unpack_calls = 0;
  PreparedSourceCache cache(MakeUnpacker(&unpack_calls));
  const auto          encoded = MakeEncoded(3);

  const auto full = cache.AcquireEncoded(encoded, DecodeRes::FULL);
  const auto full_ptr = full.Shared();
  const auto half = cache.AcquireEncoded(encoded, DecodeRes::HALF);

  EXPECT_EQ(unpack_calls, 2);
  EXPECT_EQ(cache.EntryCount(), 2U);
  EXPECT_EQ(full.Shared().get(), full_ptr.get());
  EXPECT_NE(full.Shared().get(), half.Shared().get());
  EXPECT_NE(full.Key().Lookup(), half.Key().Lookup());
  EXPECT_EQ(full.Get().host_extent, (Extent2D{32, 32}));
  EXPECT_EQ(half.Get().host_extent, (Extent2D{16, 16}));
}

TEST(GpuDagRawInput, PreparedSourceCacheReusesMatchingSourceAfterSwitchingEncodedBuffers) {
  int                 unpack_calls = 0;
  PreparedSourceCache cache(MakeUnpacker(&unpack_calls));
  const auto          first_bytes  = MakeEncoded(4);
  const auto          second_bytes = MakeEncoded(5);

  const auto a1 = cache.AcquireEncoded(first_bytes, DecodeRes::FULL);
  const auto b  = cache.AcquireEncoded(second_bytes, DecodeRes::FULL);
  const auto a2 = cache.AcquireEncoded(first_bytes, DecodeRes::FULL);

  EXPECT_EQ(unpack_calls, 2);
  EXPECT_EQ(cache.GetStats().hits, 1U);
  EXPECT_EQ(cache.GetStats().misses, 2U);
  EXPECT_EQ(a1.Shared().get(), a2.Shared().get());
  EXPECT_NE(a1.Shared().get(), b.Shared().get());
}

TEST(GpuDagRawInput, PreparedSourceCacheDoesNotEvictLeasedEntryWhenHostBudgetIsExceeded) {
  int                 unpack_calls = 0;
  PreparedSourceCache cache(MakeUnpacker(&unpack_calls));
  cache.SetHostByteBudget(1);
  const auto first_bytes  = MakeEncoded(6);
  const auto second_bytes = MakeEncoded(7);

  const auto leased = cache.AcquireEncoded(first_bytes, DecodeRes::FULL);
  const auto other  = cache.AcquireEncoded(second_bytes, DecodeRes::FULL);

  EXPECT_EQ(unpack_calls, 2);
  EXPECT_TRUE(static_cast<bool>(leased));
  EXPECT_EQ(leased.Get().host_extent, (Extent2D{32, 32}));
  EXPECT_EQ(other.Get().host_extent, (Extent2D{32, 32}));
  EXPECT_NE(leased.Shared().get(), other.Shared().get());
  EXPECT_GE(cache.EntryCount(), 1U);
}

}  // namespace
}  // namespace alcedo
