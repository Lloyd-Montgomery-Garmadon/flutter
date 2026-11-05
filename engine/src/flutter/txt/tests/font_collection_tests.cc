// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"

#include <sstream>

#include "third_party/skia/modules/skparagraph/include/ParagraphCache.h"  // nogncheck
#include "txt/font_collection.h"

namespace txt {
namespace testing {

class FontCollectionTests : public ::testing::Test {
 public:
  FontCollectionTests() {}

  void SetUp() override {}
};

TEST_F(FontCollectionTests, SettingUpDefaultFontManagerClearsCache) {
  FontCollection font_collection;
  sk_sp<skia::textlayout::FontCollection> sk_font_collection =
      font_collection.CreateSktFontCollection();
  ASSERT_EQ(sk_font_collection->getFallbackManager().get(), nullptr);
  font_collection.SetupDefaultFontManager(0);
  sk_font_collection = font_collection.CreateSktFontCollection();
  ASSERT_NE(sk_font_collection->getFallbackManager().get(), nullptr);
}

// Venus worker 克隆共用一份段落缓存；base 自己的缓存不参与，且仍是单线程官版。
TEST_F(FontCollectionTests, VenusWorkerClonesShareOneParagraphCachePerEpoch) {
  FontCollection font_collection;
  font_collection.SetupDefaultFontManager(0);
  auto* base_cache =
      font_collection.CreateSktFontCollection()->getParagraphCache();
  skia::textlayout::ParagraphCache::VenusStats stats;
  base_cache->venusGetStats(&stats);
  ASSERT_FALSE(stats.shared);

  uint64_t epoch_a = 0u;
  uint64_t epoch_b = 0u;
  auto clone_a = font_collection.CloneForVenusWorker(&epoch_a);
  auto clone_b = font_collection.CloneForVenusWorker(&epoch_b);
  auto* cache_a = clone_a->CreateSktFontCollection()->getParagraphCache();
  auto* cache_b = clone_b->CreateSktFontCollection()->getParagraphCache();
  ASSERT_EQ(epoch_a, epoch_b);
  ASSERT_EQ(cache_a, cache_b);
  ASSERT_NE(cache_a, base_cache);
  cache_a->venusGetStats(&stats);
  ASSERT_TRUE(stats.shared);
  ASSERT_EQ(stats.maxEntries, 256);
  ASSERT_EQ(stats.byteBudget, 16u * 1024u * 1024u);
  ASSERT_EQ(stats.count, 0);

  // 字体换代 → 新代新实例；旧克隆仍指旧实例。
  font_collection.SetDynamicFontManager(nullptr);
  uint64_t epoch_c = 0u;
  auto clone_c = font_collection.CloneForVenusWorker(&epoch_c);
  ASSERT_EQ(epoch_c, epoch_a + 1u);
  ASSERT_NE(clone_c->CreateSktFontCollection()->getParagraphCache(), cache_a);
  ASSERT_EQ(clone_a->CreateSktFontCollection()->getParagraphCache(), cache_a);

  // 空缓存 trim 是无操作；没克隆过的 collection trim 也是无操作。
  font_collection.VenusTrimSharedParagraphCache();
  FontCollection untouched;
  untouched.VenusTrimSharedParagraphCache();
}
}  // namespace testing
}  // namespace txt
