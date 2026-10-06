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

  // Creates a facade with independent SkParagraph caches while retaining the
  // same immutable font-manager snapshot. The clone is safe to use on one
  // worker thread only.
  std::shared_ptr<FontCollection> CloneForVenusWorker(
      uint64_t* out_font_epoch) const;

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

  std::vector<sk_sp<SkFontMgr>> GetFontManagerOrder() const;

  FML_DISALLOW_COPY_AND_ASSIGN(FontCollection);
};

}  // namespace txt

#endif  // FLUTTER_TXT_SRC_TXT_FONT_COLLECTION_H_
