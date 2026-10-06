// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_VENUS_TEXT_LAYOUT_SERVICE_H_
#define FLUTTER_SHELL_COMMON_VENUS_TEXT_LAYOUT_SERVICE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "flutter/lib/ui/text/font_collection.h"
#include "flutter/lib/ui/text/venus_text_layout_node_core.h"
#include "flutter/shell/platform/common/public/flutter_venus_text_layout.h"

namespace flutter {

// Engine-owned V2 text layout service. It reaches no embedder object and no
// UI/scene-graph type at all; the only cross-DSO surface is
// FlutterVenusTextLayoutApiV2. The B1 boundary guard scans this file for those
// forbidden identifiers as plain tokens, so do not name them even in prose.
//
// P7 收敛之后只剩同步 transport：调用方在自己的线程上持一份 slot lease 直接调
// LayoutTextNodeCore，Engine 不拥有任何线程、不排队、不缓存调用方地址。
//
// Two locks, global order is always R -> S; no path takes R while holding S:
//   R = reconfigure_mutex_  (serialises the font-reload sequence only)
//   S = mutex_              (guards all shared state)
class VenusTextLayoutService final {
 public:
  VenusTextLayoutService(uint64_t engine_id,
                         uint64_t registration_generation,
                         std::shared_ptr<FontCollection> fonts,
                         bool impeller_enabled,
                         void (*paragraph_core_probe_for_testing)(void*, bool) =
                             nullptr,
                         void* paragraph_core_probe_context = nullptr);

  /// 段落粒度 (一个 IFC 一次排完)。与 LayoutSync 同一套 slot/lease 语义 ——
  /// 复用同一把锁与同一组计数, 不另开一条并发路径。
  ///
  /// 是 public 而 LayoutSync 是 private: 后者只经 ApiV2 的函数表被调 (有 Thunk),
  /// 而 ApiV2 已冻结加不进新指针, 所以段落走独立导出符号 -> 注册表直接调它。
  int32_t LayoutParagraph(uint32_t slot,
                          uint64_t token,
                          const FlutterVenusTextLayoutParagraphInputV2* input,
                          FlutterVenusTextLayoutParagraphOutputV2* output);

  int32_t BeginArtifactOwner(uint64_t expected_registration_generation,
                             uint64_t producer_generation,
                             uint64_t* out_owner_id);
  int32_t BindArtifactOwner(uint64_t owner_id,
                            uint32_t slot,
                            uint64_t lease_token);
  int32_t SealArtifactOwner(uint64_t owner_id);

  // Fills the caller-owned table. PopulateApi itself does not retain; its owner
  // must transfer one reference to release_context.
  void PopulateApi(FlutterVenusTextLayoutApiV2* api);
  void Retain();
  void Release();
  void Shutdown();
  int32_t GetReferenceCountForTesting() const;

  // §5 的 font reload / hot restart 分支（步骤 1-11）。关停接单 -> 让 async worker
  // 排空后退出 -> 等到四个计数（含 leased_slots）全部归零 -> join -> 退休未取批次
  // -> 重建每 slot 的字体门面 -> font_epoch++ -> 按 saved_worker_count 重建 worker
  // -> 恢复 accepting。重建 FontCollection 之前必须确认没有任何持有者还占着 slot，
  // 否则会在别人正在用的对象上做替换。
  int32_t ReloadFonts();

 private:
  ~VenusTextLayoutService();

  // One node, fully validated and decoded at submit time. The UTF-8 views in
  // `view` are re-pointed at the owned strings immediately before the core runs,
  // so vector reallocation can never leave them dangling.
  struct OwnedNode {
    venus_text_layout::wire::ParsedItemView view;
    std::string text_utf8;
    std::string font_family_utf8;
    std::string locale_utf8;
    std::string ellipsis_utf8;
    std::u16string text_u16;
    std::u16string ellipsis_u16;
    bool pre_failed = false;
    venus_text_layout::ItemFailure failure;
  };

  struct Slot {
    std::shared_ptr<txt::FontCollection> fonts;
    // 这份克隆是照着 base 的哪一代字体做的。AcquireSlot 用它判新鲜:
    // 与 base 的 GetVenusFontEpoch() 不等就重克隆。见 AcquireSlot 的注释 ——
    // 构造期克隆早于 SetupDefaultFontManager, 不自愈就是全员空字体表。
    uint64_t font_epoch = 0u;
    uint64_t lease_token = 0u;  // 0 == free
    uint64_t artifact_owner_id = 0u;
    bool active_call = false;
  };

  // ---- C ABI trampolines -------------------------------------------------
  static void ReleaseContextThunk(void* ctx);
  static int32_t AcquireSlotThunk(void* ctx, uint32_t* out_slot, uint64_t* out_token);
  static int32_t ReleaseSlotThunk(void* ctx, uint32_t slot, uint64_t token);
  static int32_t LayoutSyncThunk(void* ctx,
                                 uint32_t slot,
                                 uint64_t token,
                                 const FlutterVenusTextLayoutInputV2* input,
                                 FlutterVenusTextLayoutOutputV2* output);
  static int32_t GetCountersThunk(void* ctx,
                                  uint32_t caller_struct_size,
                                  FlutterVenusTextLayoutCountersV2* out);

  // ---- implementations ---------------------------------------------------
  int32_t AcquireSlot(uint32_t* out_slot, uint64_t* out_token);
  int32_t ReleaseSlot(uint32_t slot, uint64_t token);
  int32_t LayoutSync(uint32_t slot,
                     uint64_t token,
                     const FlutterVenusTextLayoutInputV2* input,
                     FlutterVenusTextLayoutOutputV2* output);
  int32_t GetCounters(uint32_t caller_struct_size,
                      FlutterVenusTextLayoutCountersV2* out);

  void ReturnSlotLocked(uint32_t slot);
  bool DecodeAndValidate(const FlutterVenusTextLayoutInputV2& input,
                         OwnedNode* out_node);
  void RunNode(OwnedNode& node,
               const std::shared_ptr<txt::FontCollection>& fonts,
               uint32_t slot,
               uint64_t artifact_owner_id,
               uint64_t input_charge_bytes,
               bool item_timing,
               uint64_t enqueue_ns,
               FlutterVenusTextLayoutOutputV2* out);

  const uint64_t engine_id_;
  const uint64_t registration_generation_;
  const std::shared_ptr<FontCollection> fonts_;
  const bool impeller_enabled_;
  void (*const paragraph_core_probe_for_testing_)(void*, bool);
  void* const paragraph_core_probe_context_;

  std::mutex reconfigure_mutex_;                 // R
  std::mutex mutex_;                             // S
  std::condition_variable quiesce_cv_;

  uint32_t accepting_ = 1u;
  uint64_t next_lease_token_ = 1u;
  uint32_t active_slot_calls_ = 0u;
  uint32_t leased_slots_ = 0u;
  bool shutting_down_ = false;

  std::vector<Slot> slots_;

  uint64_t accepted_total_ = 0u;
  uint64_t completed_total_ = 0u;
  uint64_t rejected_total_ = 0u;
  // slot 字体表被重克隆的次数。**必须可观测** —— 自愈如果静默, 就分不清
  // "顺序本来就对" 和 "自愈救回来了", 也就没法证明这条修复真的在起作用。
  uint64_t slot_font_refreshes_ = 0u;
  uint64_t font_epoch_ = 1u;

  std::atomic<int32_t> refs_{1};
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_VENUS_TEXT_LAYOUT_SERVICE_H_
