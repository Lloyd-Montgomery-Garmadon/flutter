// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"

#include <sstream>
#include <thread>
#include <vector>

#include "flutter/testing/testing.h"
#include "third_party/skia/modules/skparagraph/include/ParagraphCache.h"  // nogncheck
#include "txt/asset_font_manager.h"
#include "txt/font_collection.h"
#include "txt/platform.h"
#include "txt/typeface_font_asset_provider.h"

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

namespace {

constexpr char kVariableFamily[] = "VenusVariableTest";

// 只注册一个带 wght 轴的可变字体。
void SetupVariableFont(FontCollection& font_collection,
                       sk_sp<SkTypeface>* registered) {
  auto data = flutter::testing::OpenFixtureAsSkData(
      "RobotoSlab-VariableFont_wght.ttf");
  ASSERT_TRUE(data);
  *registered = GetDefaultFontManager()->makeFromData(data);
  ASSERT_TRUE(*registered);
  auto provider = std::make_unique<TypefaceFontAssetProvider>();
  provider->RegisterTypeface(*registered, kVariableFamily);
  font_collection.SetAssetFontManager(
      sk_make_sp<AssetFontManager>(std::move(provider)));
}

// 与官版 ParagraphBuilderSkia::TxtToSkia 相同：每个文字样式都带 wght 坐标。
sk_sp<SkTypeface> FindWeight(const std::shared_ptr<FontCollection>& collection,
                             int weight) {
  SkFontArguments::VariationPosition::Coordinate coordinate{
      SkSetFourByteTag('w', 'g', 'h', 't'), static_cast<float>(weight)};
  const skia::textlayout::FontArguments args(
      SkFontArguments().setVariationDesignPosition({&coordinate, 1}));
  auto faces = collection->CreateSktFontCollection()->findTypefaces(
      {SkString(kVariableFamily)},
      SkFontStyle(weight, SkFontStyle::kNormal_Width,
                  SkFontStyle::kUpright_Slant),
      args);
  return faces.empty() ? nullptr : faces.front();
}

}  // namespace

// Venus worker 克隆共用一份字重变体：同字体同字重全进程（同字体代）只克隆一个
// 字体对象。官版每个 skparagraph FontCollection 各克隆一份，8 个 worker 槽就是 8 份
// FreeType 面与可变字体数据。
TEST_F(FontCollectionTests, VenusWorkerClonesShareOneVariationClonePerEpoch) {
  FontCollection font_collection;
  sk_sp<SkTypeface> registered;
  SetupVariableFont(font_collection, &registered);

  uint64_t epoch_a = 0u;
  uint64_t epoch_b = 0u;
  auto clone_a = font_collection.CloneForVenusWorker(&epoch_a);
  auto clone_b = font_collection.CloneForVenusWorker(&epoch_b);
  const auto face_a = FindWeight(clone_a, 500);
  const auto face_b = FindWeight(clone_b, 500);
  ASSERT_TRUE(face_a);
  ASSERT_TRUE(face_b);
  ASSERT_NE(face_a.get(), registered.get()) << "前置：确实走了字重克隆";
  ASSERT_EQ(face_a.get(), face_b.get());
  ASSERT_NE(FindWeight(clone_a, 700).get(), face_a.get()) << "不同字重各一份";

  // 字体换代：新代新共享表，旧克隆仍拿旧对象。
  font_collection.SetDynamicFontManager(nullptr);
  uint64_t epoch_c = 0u;
  auto clone_c = font_collection.CloneForVenusWorker(&epoch_c);
  ASSERT_EQ(epoch_c, epoch_a + 1u);
  ASSERT_NE(FindWeight(clone_c, 500).get(), face_a.get());
  ASSERT_EQ(FindWeight(clone_a, 500).get(), face_a.get());
}

// 多个 worker 线程各用自己的克隆同时查同一字重：全部拿到同一个字体对象。
TEST_F(FontCollectionTests, VenusWorkerClonesShareVariationAcrossThreads) {
  FontCollection font_collection;
  sk_sp<SkTypeface> registered;
  SetupVariableFont(font_collection, &registered);

  constexpr int kThreads = 8;
  std::vector<std::shared_ptr<FontCollection>> clones;
  for (int i = 0; i < kThreads; ++i) {
    clones.push_back(font_collection.CloneForVenusWorker(nullptr));
  }
  std::vector<SkTypeface*> seen(kThreads * 2, nullptr);
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i] {
      for (int round = 0; round < 200; ++round) {
        seen[i * 2] = FindWeight(clones[i], 500).get();
        seen[i * 2 + 1] = FindWeight(clones[i], 300 + (round % 2) * 400).get();
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  for (int i = 0; i < kThreads; ++i) {
    ASSERT_NE(seen[i * 2], nullptr);
    ASSERT_EQ(seen[i * 2], seen[0]);
  }
  ASSERT_EQ(FindWeight(clones[0], 700).get(), FindWeight(clones[1], 700).get());
}

}  // namespace testing
}  // namespace txt
