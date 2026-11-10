// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_TXT_SRC_TXT_FONT_COLLECTION_H_
#define FLUTTER_TXT_SRC_TXT_FONT_COLLECTION_H_

#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "flutter/fml/macros.h"
#include "third_party/googletest/googletest/include/gtest/gtest_prod.h"  // nogncheck
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkRefCnt.h"
#include "third_party/skia/modules/skparagraph/include/FontCollection.h"  // nogncheck
#include "txt/asset_font_manager.h"
#include "txt/text_style.h"

namespace txt {

class FontCollection : public std::enable_shared_from_this<FontCollection> {
 public:
  using VenusFontMutationLease = std::unique_lock<std::shared_mutex>;
  using VenusFontReadLease = std::shared_lock<std::shared_mutex>;

  FontCollection();

  ~FontCollection();

  size_t GetFontManagersCount() const;

  void SetupDefaultFontManager(uint32_t font_initialization_data);
  void SetDefaultFontManager(sk_sp<SkFontMgr> font_manager);
  void SetAssetFontManager(sk_sp<SkFontMgr> font_manager);
  void SetDynamicFontManager(sk_sp<SkFontMgr> font_manager);
  void SetTestFontManager(sk_sp<SkFontMgr> font_manager);

  // Do not provide alternative fonts that can match characters which are
  // missing from the requested font family.
  void DisableFontFallback();

  // Remove all entries in the font family cache.
  void ClearFontFamilyCache();

  // Compound mutations such as dynamic typeface registration plus cache
  // invalidation must hold one exclusive lease across both operations.
  VenusFontMutationLease AcquireVenusFontMutationLease();
  uint64_t ClearFontFamilyCacheWithVenusLease(
      const VenusFontMutationLease& lease);

  // Worker shaping holds this lease for the entire paragraph operation so a
  // shared FontMgr cannot be mutated while SkParagraph is reading it.
  VenusFontReadLease AcquireVenusFontReadLease() const;
  uint64_t GetVenusFontEpochWithReadLease(
      const VenusFontReadLease& lease) const;

  // Construct a Skia text layout FontCollection based on this collection.
  sk_sp<skia::textlayout::FontCollection> CreateSktFontCollection();

  // Creates a facade with its own font-family caches while retaining the same
  // immutable font-manager snapshot. Its SkParagraph shaping cache is one
  // instance shared by every clone made from this collection at the same
  // font epoch (see venus_shared_pcache_). The clone's own caches are safe to
  // use on one worker thread only; the shared paragraph cache takes its own
  // lock.
  std::shared_ptr<FontCollection> CloneForVenusWorker(
      uint64_t* out_font_epoch) const;

  // 清空当前字体代的共享段落缓存（页面退出 / 内存压力）。缓存对象保留，条目
  // 全部释放；节点在锁外析构，正在拷贝命中值的 worker 不受影响。无缓存时无操作。
  void VenusTrimSharedParagraphCache();

  uint64_t GetVenusFontEpoch() const;

 private:
  sk_sp<SkFontMgr> default_font_manager_;
  sk_sp<SkFontMgr> asset_font_manager_;
  sk_sp<SkFontMgr> dynamic_font_manager_;
  sk_sp<SkFontMgr> test_font_manager_;
  bool enable_font_fallback_;
  uint64_t venus_font_epoch_ = 1u;

  mutable std::shared_mutex venus_font_mutex_;

  // An equivalent font collection usable by the Skia text shaper library.
  sk_sp<skia::textlayout::FontCollection> skt_collection_;

  // Venus worker 门面共用的 SkParagraph 段落缓存，**一代字体一个实例**：
  // 按 venus_font_epoch_ 懒建，字体换代后下一次克隆换新实例；旧克隆（仍租用中的
  // slot）继续写旧实例，新克隆只读写新实例，旧字体的 Run 不可能串进新代。克隆以
  // shared_ptr 持有，最后一个持有者释放时销毁。UI 线程自己的 skt_collection_
  // 不参与共享：它的缓存行为与官版逐字节一致，也不会被 worker 的锁阻塞。
  mutable std::shared_ptr<skia::textlayout::ParagraphCache> venus_shared_pcache_;
  mutable uint64_t venus_shared_pcache_epoch_ = 0u;

  // Venus worker 门面共用的字重变体表（skparagraph cloneTypeface），与上面的段落
  // 缓存同一生命周期：一代字体一张表，所有 worker 克隆同字体同字重只克隆一个字体对象
  // （一份 FreeType 面与可变字体数据）。UI 线程的 skt_collection_ 不参与，保持官版。
  mutable std::shared_ptr<
      skia::textlayout::FontCollection::VenusSharedVariationCache>
      venus_shared_vcache_;
  mutable uint64_t venus_shared_vcache_epoch_ = 0u;

  std::vector<sk_sp<SkFontMgr>> GetFontManagerOrder() const;

  FML_DISALLOW_COPY_AND_ASSIGN(FontCollection);
};

}  // namespace txt

#endif  // FLUTTER_TXT_SRC_TXT_FONT_COLLECTION_H_
