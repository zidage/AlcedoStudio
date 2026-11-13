//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/semantic_query_embedding_cache.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace alcedo {
namespace {

constexpr const char* kModelKey = "model-a";

/// An encoder that counts its calls and returns a vector that identifies the text.
struct CountingEncoder {
  int calls_ = 0;

  auto For(float value) -> SemanticQueryEmbeddingCache::Encoder {
    return [this, value] {
      ++calls_;
      return std::vector<float>{value, 1.0F};
    };
  }
};

TEST(SemanticQueryEmbeddingCacheTest, CachedTextIsNotEncodedAgain) {
  SemanticQueryEmbeddingCache cache;
  CountingEncoder             encoder;

  const auto first  = cache.GetOrEmbed(kModelKey, "mountain lake", encoder.For(1.0F));
  const auto second = cache.GetOrEmbed(kModelKey, "mountain lake", encoder.For(2.0F));

  EXPECT_EQ(encoder.calls_, 1);
  EXPECT_EQ(first, (std::vector<float>{1.0F, 1.0F}));
  EXPECT_EQ(second, first);
  EXPECT_EQ(cache.Size(), 1U);
}

TEST(SemanticQueryEmbeddingCacheTest, CacheKeyIncludesTheModelKey) {
  SemanticQueryEmbeddingCache cache;
  CountingEncoder             encoder;

  const auto model_a = cache.GetOrEmbed("model-a", "a photo", encoder.For(1.0F));
  const auto model_b = cache.GetOrEmbed("model-b", "a photo", encoder.For(2.0F));

  EXPECT_EQ(encoder.calls_, 2);
  EXPECT_EQ(model_a, (std::vector<float>{1.0F, 1.0F}));
  EXPECT_EQ(model_b, (std::vector<float>{2.0F, 1.0F}));
  EXPECT_EQ(cache.Size(), 2U);
}

TEST(SemanticQueryEmbeddingCacheTest, TextIsNotLowercased) {
  SemanticQueryEmbeddingCache cache;
  CountingEncoder             encoder;

  static_cast<void>(cache.GetOrEmbed(kModelKey, "Sunset", encoder.For(1.0F)));
  static_cast<void>(cache.GetOrEmbed(kModelKey, "sunset", encoder.For(2.0F)));

  EXPECT_EQ(encoder.calls_, 2);
}

TEST(SemanticQueryEmbeddingCacheTest, LeastRecentlyUsedEntryIsRemovedAtCapacity) {
  ASSERT_EQ(SemanticQueryEmbeddingCache::kDefaultCapacity, 64U);
  SemanticQueryEmbeddingCache cache;
  CountingEncoder             encoder;
  const auto                  text = [](int index) { return "query " + std::to_string(index); };

  // Entries 1 to 64 fill the cache; a hit on entry 1 makes entry 2 the least recently used.
  for (int index = 1; index <= 64; ++index) {
    static_cast<void>(cache.GetOrEmbed(kModelKey, text(index), encoder.For(1.0F)));
  }
  static_cast<void>(cache.GetOrEmbed(kModelKey, text(1), encoder.For(1.0F)));
  EXPECT_EQ(encoder.calls_, 64);

  // The 65th insert removes entry 2.
  static_cast<void>(cache.GetOrEmbed(kModelKey, text(65), encoder.For(1.0F)));
  EXPECT_EQ(encoder.calls_, 65);
  EXPECT_EQ(cache.Size(), 64U);

  static_cast<void>(cache.GetOrEmbed(kModelKey, text(1), encoder.For(1.0F)));
  EXPECT_EQ(encoder.calls_, 65) << "entry 1 was used recently and must stay";

  static_cast<void>(cache.GetOrEmbed(kModelKey, text(2), encoder.For(1.0F)));
  EXPECT_EQ(encoder.calls_, 66) << "entry 2 was the least recently used and must be encoded again";
}

TEST(SemanticQueryEmbeddingCacheTest, FailedEncodeStoresNoEntry) {
  SemanticQueryEmbeddingCache cache;
  int                         calls = 0;

  EXPECT_THROW(static_cast<void>(cache.GetOrEmbed(kModelKey, "harbor", [&]() -> std::vector<float> {
                 ++calls;
                 throw std::runtime_error("Semantic runtime failed to start.");
               })),
               std::runtime_error);
  EXPECT_EQ(cache.Size(), 0U);

  const auto embedding = cache.GetOrEmbed(kModelKey, "harbor", [&] {
    ++calls;
    return std::vector<float>{0.5F, 0.5F};
  });
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(embedding, (std::vector<float>{0.5F, 0.5F}));
  EXPECT_EQ(cache.Size(), 1U);
}

}  // namespace
}  // namespace alcedo
