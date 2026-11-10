// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "font_collection.h"

#include <algorithm>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "flutter/fml/logging.h"
#include "flutter/fml/trace_event.h"
#include "txt/platform.h"
#include "txt/text_style.h"

namespace txt {

namespace {

// Venus worker 共享段落缓存的条数上限。
// 依据（PHK110，8000 行中英混排压测，复用距离直方图）：首屏重复排版的未命中
// 距离集中在 128–256 次插入之间；256 条时首屏命中 89%、宽度重排 97%，128 条
// 只有 14% / 47%。旧方案是每个 slot 门面各 128 条、共 8 份，命中按 slot 分散。
constexpr int kVenusSharedParagraphCacheMaxEntries = 256;

// Venus worker 共享段落缓存的字节预算（估算字节，见
// ParagraphCache::venusEstimateBytes；每条 ≈ Run 数 × 2272B 固定开销）。
// 依据（同一压测）：最长行 ≈136KB/条，16MiB ≈ 120 条最长行，盖住一屏多的重复
// 段落；真机压测末 Native 110MB（无缓存 92 / 旧方案 218），退出后 92MB（无缓存
// 74 / 旧方案 202），比 32MiB 档少 20MB 而首屏、重排、跳转速度持平。
constexpr size_t kVenusSharedParagraphCacheByteBudget = 16u * 1024u * 1024u;

}  // namespace

FontCollection::FontCollection() : enable_font_fallback_(true) {}

FontCollection::~FontCollection() {
  if (skt_collection_) {
    skt_collection_->clearCaches();
  }
}

size_t FontCollection::GetFontManagersCount() const {
  std::shared_lock lock(venus_font_mutex_);
  return GetFontManagerOrder().size();
}

void FontCollection::SetupDefaultFontManager(
    uint32_t font_initialization_data) {
  std::unique_lock lock(venus_font_mutex_);
  default_font_manager_ = GetDefaultFontManager(font_initialization_data);
  skt_collection_.reset();
  ++venus_font_epoch_;
}

void FontCollection::SetDefaultFontManager(sk_sp<SkFontMgr> font_manager) {
  std::unique_lock lock(venus_font_mutex_);
  default_font_manager_ = std::move(font_manager);
  skt_collection_.reset();
  ++venus_font_epoch_;
}

void FontCollection::SetAssetFontManager(sk_sp<SkFontMgr> font_manager) {
  std::unique_lock lock(venus_font_mutex_);
  asset_font_manager_ = std::move(font_manager);
  skt_collection_.reset();
  ++venus_font_epoch_;
}

void FontCollection::SetDynamicFontManager(sk_sp<SkFontMgr> font_manager) {
  std::unique_lock lock(venus_font_mutex_);
  dynamic_font_manager_ = std::move(font_manager);
  skt_collection_.reset();
  ++venus_font_epoch_;
}

void FontCollection::SetTestFontManager(sk_sp<SkFontMgr> font_manager) {
  std::unique_lock lock(venus_font_mutex_);
  test_font_manager_ = std::move(font_manager);
  skt_collection_.reset();
  ++venus_font_epoch_;
}

// Return the available font managers in the order they should be queried.
std::vector<sk_sp<SkFontMgr>> FontCollection::GetFontManagerOrder() const {
  std::vector<sk_sp<SkFontMgr>> order;
  if (dynamic_font_manager_) {
    order.push_back(dynamic_font_manager_);
  }
  if (asset_font_manager_) {
    order.push_back(asset_font_manager_);
  }
  if (test_font_manager_) {
    order.push_back(test_font_manager_);
  }
  if (default_font_manager_) {
    order.push_back(default_font_manager_);
  }
  return order;
}

void FontCollection::DisableFontFallback() {
  std::unique_lock lock(venus_font_mutex_);
  enable_font_fallback_ = false;
  if (skt_collection_) {
    skt_collection_->disableFontFallback();
  }
  ++venus_font_epoch_;
}

void FontCollection::ClearFontFamilyCache() {
  std::unique_lock lock(venus_font_mutex_);
  if (skt_collection_) {
    skt_collection_->clearCaches();
  }
  ++venus_font_epoch_;
}

FontCollection::VenusFontMutationLease
FontCollection::AcquireVenusFontMutationLease() {
  return VenusFontMutationLease(venus_font_mutex_);
}

uint64_t FontCollection::ClearFontFamilyCacheWithVenusLease(
    const VenusFontMutationLease& lease) {
  FML_DCHECK(lease.owns_lock());
  if (skt_collection_) {
    skt_collection_->clearCaches();
  }
  return ++venus_font_epoch_;
}

FontCollection::VenusFontReadLease FontCollection::AcquireVenusFontReadLease()
    const {
  return VenusFontReadLease(venus_font_mutex_);
}

uint64_t FontCollection::GetVenusFontEpochWithReadLease(
    const VenusFontReadLease& lease) const {
  FML_DCHECK(lease.owns_lock());
  return venus_font_epoch_;
}

sk_sp<skia::textlayout::FontCollection>
FontCollection::CreateSktFontCollection() {
  std::unique_lock lock(venus_font_mutex_);
  if (!skt_collection_) {
    skt_collection_ = sk_make_sp<skia::textlayout::FontCollection>();

    std::vector<SkString> default_font_families;
    for (const std::string& family : GetDefaultFontFamilies()) {
      default_font_families.emplace_back(family);
    }
    skt_collection_->setDefaultFontManager(default_font_manager_,
                                           default_font_families);
    skt_collection_->setAssetFontManager(asset_font_manager_);
    skt_collection_->setDynamicFontManager(dynamic_font_manager_);
    skt_collection_->setTestFontManager(test_font_manager_);
    if (!enable_font_fallback_) {
      skt_collection_->disableFontFallback();
    }
  }

  return skt_collection_;
}

std::shared_ptr<FontCollection> FontCollection::CloneForVenusWorker(
    uint64_t* out_font_epoch) const {
  // 独占而非共享锁：共享段落缓存按字体代懒建，两次并发克隆不能各建一份。
  std::unique_lock lock(venus_font_mutex_);
  auto clone = std::make_shared<FontCollection>();
  clone->default_font_manager_ = default_font_manager_;
  clone->asset_font_manager_ = asset_font_manager_;
  clone->dynamic_font_manager_ = dynamic_font_manager_;
  clone->test_font_manager_ = test_font_manager_;
  clone->enable_font_fallback_ = enable_font_fallback_;
  clone->venus_font_epoch_ = venus_font_epoch_;
  if (!venus_shared_pcache_ ||
      venus_shared_pcache_epoch_ != venus_font_epoch_) {
    venus_shared_pcache_ = std::make_shared<skia::textlayout::ParagraphCache>();
    venus_shared_pcache_->venusConfigureShared(
        kVenusSharedParagraphCacheMaxEntries,
        kVenusSharedParagraphCacheByteBudget);
    venus_shared_pcache_epoch_ = venus_font_epoch_;
  }
  if (!venus_shared_vcache_ ||
      venus_shared_vcache_epoch_ != venus_font_epoch_) {
    venus_shared_vcache_ =
        skia::textlayout::FontCollection::MakeVenusSharedVariationCache();
    venus_shared_vcache_epoch_ = venus_font_epoch_;
  }
  const auto skt_clone = clone->CreateSktFontCollection();
  skt_clone->setVenusSharedParagraphCache(venus_shared_pcache_);
  skt_clone->setVenusSharedVariationCache(venus_shared_vcache_);
  if (out_font_epoch != nullptr) {
    *out_font_epoch = venus_font_epoch_;
  }
  return clone;
}

void FontCollection::VenusTrimSharedParagraphCache() {
  std::shared_ptr<skia::textlayout::ParagraphCache> cache;
  {
    std::shared_lock lock(venus_font_mutex_);
    cache = venus_shared_pcache_;
  }
  // 在字体锁外清：reset 自己持缓存锁，条目析构在两把锁之外。
  if (cache) {
    cache->reset();
  }
}

uint64_t FontCollection::GetVenusFontEpoch() const {
  std::shared_lock lock(venus_font_mutex_);
  return venus_font_epoch_;
}

}  // namespace txt
