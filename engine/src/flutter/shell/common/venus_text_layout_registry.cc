// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/venus_text_layout_registry.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "flutter/lib/ui/text/venus_text_layout_paragraph_store.h"
#include "flutter/shell/common/venus_text_layout_service.h"

namespace flutter {
namespace {

// V2 ABI is frozen field-by-field; both trees carry the identical assertions.
// P7 之后 transport 只剩 sync：bit1 (旧 Async) 永久退休并保留,不再复用。
static_assert(kFlutterVenusV2CapabilitySync == 0x01u);
static_assert(kFlutterVenusV2CapabilityItemTiming == 0x04u);
static_assert(kFlutterVenusV2CapabilityFontFingerprint == 0x08u);
static_assert(kFlutterVenusV2CapabilityKnownMask == 0x0du);
// §8 P7 冻结: RequiredSyncWinner = Sync | FontFingerprint。ItemTiming 退出
// 生产必需集(仍在 KnownMask 内, 是可选能力)。
static_assert(kFlutterVenusV2CapabilityRequired == 0x09u);
static_assert((kFlutterVenusV2CapabilityRequired &
               kFlutterVenusV2CapabilityKnownMask) ==
              kFlutterVenusV2CapabilityRequired,
              "required must be a subset of what we know");
static_assert((kFlutterVenusV2CapabilityKnownMask & 0x02u) == 0u,
              "bit1 is the retired async transport; never re-use it");
static_assert(kFlutterVenusV2InputFlagKnownMask == 0x01u);
static_assert(kFlutterVenusV2OutputFlagKnownMask == 0x01u);
// 2026-08-27: 追加 leading_placeholder_width (IFC 续排的行内起始偏移),
// 184 -> 192。**既有字段的偏移一个都没动** —— 追加在末尾是唯一安全的扩展方式,
// 插在中间会让两侧同名字段读到彼此的位模式 (静默, 值看着还像真的)。
static_assert(sizeof(FlutterVenusTextLayoutInputV2) == 192u);
// 2026-08-27: 追加 content_height (首行 ascent+descent), 136 -> 144。
// 追加在末尾, 既有字段偏移一个没动。
static_assert(sizeof(FlutterVenusTextLayoutOutputV2) == 144u);
static_assert(offsetof(FlutterVenusTextLayoutOutputV2, content_height) == 136u);
// 2026-08-28: 段落入口 (一个 IFC 一次排完) 的四张新表。**独立结构体**, 与上面
// 那套单节点的表零重叠 —— 理由见公共头里的说明 (ApiV2 已冻结, 且两者语义不同)。
// 与引擎那棵树逐条相同, 两边同帧改。
static_assert(sizeof(FlutterVenusTextLayoutRunV2) == 104u);
static_assert(alignof(FlutterVenusTextLayoutRunV2) == 8u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, struct_size) == 0u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, kind) == 4u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, text_offset) == 8u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, text_length) == 12u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, font_family_offset) == 16u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, font_family_length) == 20u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, font_weight) == 24u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, text_color_argb) == 28u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, font_style) == 32u);
// reserved0 更名启用为 height_mode —— 偏移与宽度一字节未变, ABI 等价。
static_assert(offsetof(FlutterVenusTextLayoutRunV2, height_mode) == 34u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, font_size) == 40u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, letter_spacing) == 48u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, word_spacing) == 56u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, height) == 64u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, placeholder_width) == 72u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, placeholder_height) == 80u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2,
                       placeholder_baseline_offset) == 88u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2,
                       placeholder_alignment) == 96u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2,
                       placeholder_baseline) == 98u);
// 原 uint32 reserved1 拆成 text_vertical_align + reserved1, 偏移与结构体大小未变。
static_assert(offsetof(FlutterVenusTextLayoutRunV2, text_vertical_align) ==
              100u);
static_assert(offsetof(FlutterVenusTextLayoutRunV2, reserved1) == 102u);
static_assert(sizeof(FlutterVenusTextLayoutLineMetricV2) == 56u);
static_assert(alignof(FlutterVenusTextLayoutLineMetricV2) == 8u);
static_assert(offsetof(FlutterVenusTextLayoutLineMetricV2, struct_size) == 0u);
static_assert(offsetof(FlutterVenusTextLayoutLineMetricV2, line_number) == 4u);
static_assert(offsetof(FlutterVenusTextLayoutLineMetricV2, left) == 8u);
static_assert(offsetof(FlutterVenusTextLayoutLineMetricV2, width) == 16u);
static_assert(offsetof(FlutterVenusTextLayoutLineMetricV2, height) == 24u);
static_assert(offsetof(FlutterVenusTextLayoutLineMetricV2, baseline) == 32u);
static_assert(offsetof(FlutterVenusTextLayoutLineMetricV2, ascent) == 40u);
static_assert(offsetof(FlutterVenusTextLayoutLineMetricV2, descent) == 48u);
static_assert(sizeof(FlutterVenusTextLayoutFragmentRectV2) == 48u);
static_assert(alignof(FlutterVenusTextLayoutFragmentRectV2) == 8u);
static_assert(offsetof(FlutterVenusTextLayoutFragmentRectV2, struct_size) == 0u);
static_assert(offsetof(FlutterVenusTextLayoutFragmentRectV2, run_index) == 4u);
static_assert(offsetof(FlutterVenusTextLayoutFragmentRectV2, line_number) == 8u);
static_assert(offsetof(FlutterVenusTextLayoutFragmentRectV2, reserved0) == 12u);
static_assert(offsetof(FlutterVenusTextLayoutFragmentRectV2, left) == 16u);
static_assert(offsetof(FlutterVenusTextLayoutFragmentRectV2, top) == 24u);
static_assert(offsetof(FlutterVenusTextLayoutFragmentRectV2, right) == 32u);
static_assert(offsetof(FlutterVenusTextLayoutFragmentRectV2, bottom) == 40u);
static_assert(sizeof(FlutterVenusTextLayoutParagraphInputV2) == 168u);
static_assert(alignof(FlutterVenusTextLayoutParagraphInputV2) == 8u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, struct_size) == 0u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, abi_version) == 4u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, run_count) == 8u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, text_length) == 12u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2,
                       font_families_length) == 16u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, locale_length) == 20u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, text_direction) == 24u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, text_align) == 26u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, soft_wrap) == 28u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, reserved0) == 30u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, max_lines) == 32u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2,
                       line_metric_capacity) == 36u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2,
                       fragment_rect_capacity) == 40u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, runs) == 48u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, text) == 56u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, font_families) == 64u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, locale) == 72u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, line_metrics_out) == 80u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, fragment_rects_out) == 88u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, max_width) == 96u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2,
                       paragraph_height_multiple) == 104u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, paragraph_token) == 112u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, ellipsis) == 120u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, ellipsis_length) == 128u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, reserved1) == 132u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2,
                       strut_font_family_offset) == 136u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2,
                       strut_font_family_length) == 140u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2,
                       strut_font_weight) == 144u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2,
                       strut_font_style) == 148u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2, strut_enabled) ==
              150u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2,
                       strut_font_size) == 152u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphInputV2,
                       strut_height_multiple) == 160u);
static_assert(sizeof(FlutterVenusTextLayoutParagraphOutputV2) == 72u);
static_assert(alignof(FlutterVenusTextLayoutParagraphOutputV2) == 8u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphOutputV2, struct_size) == 0u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphOutputV2, status) == 4u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphOutputV2, error_field_id) == 8u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphOutputV2, error_detail) == 12u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphOutputV2,
                       line_metric_count) == 16u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphOutputV2,
                       fragment_rect_count) == 20u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphOutputV2, truncated) == 24u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphOutputV2, reserved0) == 28u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphOutputV2, width) == 32u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphOutputV2, height) == 40u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphOutputV2, first_baseline) == 48u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphOutputV2,
                       min_intrinsic_width) == 56u);
static_assert(offsetof(FlutterVenusTextLayoutParagraphOutputV2,
                       max_intrinsic_width) == 64u);
using ParagraphV2Fn = int32_t (*)(
    uint64_t, uint32_t, uint64_t,
    const FlutterVenusTextLayoutParagraphInputV2*,
    FlutterVenusTextLayoutParagraphOutputV2*);
static_assert(std::is_same_v<decltype(&FlutterVenusTextLayoutParagraphV2),
                             ParagraphV2Fn>);
using ArtifactOwnerBeginV1Fn = int32_t (*)(uint64_t,
                                            uint64_t,
                                            uint64_t,
                                            uint64_t*);
using ArtifactOwnerBindSlotV1Fn =
    int32_t (*)(uint64_t, uint64_t, uint32_t, uint64_t);
using ArtifactOwnerSealV1Fn = int32_t (*)(uint64_t, uint64_t);
using ArtifactOwnerReleaseV1Fn = void (*)(uint64_t, uint64_t);
static_assert(std::is_same_v<
              decltype(&FlutterVenusTextLayoutArtifactOwnerBeginV1),
              ArtifactOwnerBeginV1Fn>);
static_assert(std::is_same_v<
              decltype(&FlutterVenusTextLayoutArtifactOwnerBindSlotV1),
              ArtifactOwnerBindSlotV1Fn>);
static_assert(std::is_same_v<
              decltype(&FlutterVenusTextLayoutArtifactOwnerSealV1),
              ArtifactOwnerSealV1Fn>);
static_assert(std::is_same_v<
              decltype(&FlutterVenusTextLayoutArtifactOwnerReleaseV1),
              ArtifactOwnerReleaseV1Fn>);
static_assert(sizeof(FlutterVenusTextLayoutCountersV2) == 88u);
static_assert(sizeof(FlutterVenusTextLayoutApiV2) == 128u);
// reserved0 更名启用为 text_color_argb —— 偏移与宽度一字节未变, ABI 等价。
static_assert(offsetof(FlutterVenusTextLayoutInputV2, text_color_argb) == 68u);
static_assert(offsetof(FlutterVenusTextLayoutInputV2, text) == 72u);
static_assert(offsetof(FlutterVenusTextLayoutInputV2, min_width) == 104u);
static_assert(offsetof(FlutterVenusTextLayoutInputV2, prelayout_token) == 176u);
static_assert(
    offsetof(FlutterVenusTextLayoutInputV2, leading_placeholder_width) == 184u);
static_assert(offsetof(FlutterVenusTextLayoutOutputV2, paragraph_width) == 32u);
static_assert(offsetof(FlutterVenusTextLayoutOutputV2, first_line_left) == 96u);
static_assert(offsetof(FlutterVenusTextLayoutOutputV2, end_ns) == 128u);
static_assert(offsetof(FlutterVenusTextLayoutCountersV2, accepted_total) == 16u);
static_assert(offsetof(FlutterVenusTextLayoutCountersV2, font_sha256) == 48u);
static_assert(offsetof(FlutterVenusTextLayoutCountersV2, accepting) == 80u);
static_assert(offsetof(FlutterVenusTextLayoutApiV2, context) == 80u);
static_assert(offsetof(FlutterVenusTextLayoutApiV2, acquire_slot) == 96u);
static_assert(offsetof(FlutterVenusTextLayoutApiV2, layout_text_node_sync_v2) ==
              112u);
static_assert(offsetof(FlutterVenusTextLayoutApiV2, get_counters_v2) == 120u);

struct Entry {
  uint64_t generation = 0u;
  VenusTextLayoutService* service = nullptr;
};

std::mutex g_registry_mutex;
std::unordered_map<uint64_t, Entry> g_registry;
std::atomic<uint64_t> g_next_generation{1u};

class ScopedServiceReference final {
 public:
  explicit ScopedServiceReference(VenusTextLayoutService* service)
      : service_(service) {}
  ~ScopedServiceReference() { service_->Release(); }

  ScopedServiceReference(const ScopedServiceReference&) = delete;
  ScopedServiceReference& operator=(const ScopedServiceReference&) = delete;

 private:
  VenusTextLayoutService* const service_;
};

}  // namespace

uint64_t VenusTextLayoutRegistry::Register(
    uint64_t engine_id,
    const std::shared_ptr<FontCollection>& fonts,
    bool impeller_enabled,
    void (*paragraph_core_probe_for_testing)(void*, bool),
    void* paragraph_core_probe_context) {
  if (engine_id == 0u || fonts == nullptr) {
    return 0u;
  }
  const uint64_t generation =
      g_next_generation.fetch_add(1u, std::memory_order_relaxed);
  auto* service = new (std::nothrow)
      VenusTextLayoutService(engine_id, generation, fonts, impeller_enabled,
                             paragraph_core_probe_for_testing,
                             paragraph_core_probe_context);
  if (service == nullptr) {
    return 0u;
  }

  VenusTextLayoutService* replaced = nullptr;
  {
    std::scoped_lock lock(g_registry_mutex);
    const auto found = g_registry.find(engine_id);
    if (found != g_registry.end()) {
      replaced = found->second.service;
      found->second = Entry{generation, service};
    } else {
      g_registry.emplace(engine_id, Entry{generation, service});
    }
  }
  if (replaced != nullptr) {
    replaced->Shutdown();
    replaced->Release();
  }
  return generation;
}

void VenusTextLayoutRegistry::Unregister(uint64_t engine_id,
                                         uint64_t registration_generation) {
  VenusTextLayoutService* service = nullptr;
  {
    std::scoped_lock lock(g_registry_mutex);
    const auto found = g_registry.find(engine_id);
    if (found == g_registry.end() ||
        found->second.generation != registration_generation) {
      return;
    }
    service = found->second.service;
    g_registry.erase(found);
  }
  service->Shutdown();
  service->Release();
}

int32_t VenusTextLayoutRegistry::GetApi(uint64_t engine_id,
                                        uint32_t caller_struct_size,
                                        FlutterVenusTextLayoutApiV2* out_api) {
  // Frozen attach order: ABI/struct_size -> capability mask -> registrar adopts.
  if (engine_id == 0u || out_api == nullptr ||
      caller_struct_size < sizeof(FlutterVenusTextLayoutApiV2)) {
    return kFlutterVenusV2AbiMismatch;
  }
  VenusTextLayoutService* service = nullptr;
  {
    std::scoped_lock lock(g_registry_mutex);
    const auto found = g_registry.find(engine_id);
    if (found == g_registry.end() || found->second.service == nullptr) {
      return kFlutterVenusV2Unavailable;
    }
    service = found->second.service;
    service->Retain();
  }
  FlutterVenusTextLayoutApiV2 complete_api = {};
  service->PopulateApi(&complete_api);
  if ((complete_api.capabilities & kFlutterVenusV2CapabilityRequired) !=
      kFlutterVenusV2CapabilityRequired) {
    service->Release();
    return kFlutterVenusV2Unavailable;
  }
  // The table copy carries the retained reference; exactly one release_context
  // returns it.
  std::memcpy(out_api, &complete_api, sizeof(complete_api));
  return kFlutterVenusV2Ok;
}

int32_t VenusTextLayoutRegistry::LayoutParagraph(
    uint64_t engine_id,
    uint32_t slot,
    uint64_t lease_token,
    const FlutterVenusTextLayoutParagraphInputV2* input,
    FlutterVenusTextLayoutParagraphOutputV2* output) {
  if (output == nullptr) {
    return kFlutterVenusV2InvalidArgument;
  }
  if (output->struct_size != sizeof(*output)) {
    return kFlutterVenusV2AbiMismatch;
  }
  std::memset(output, 0, sizeof(*output));
  output->struct_size = static_cast<uint32_t>(sizeof(*output));
  auto finish = [output](int32_t status) {
    output->status = status;
    return status;
  };
  if (input == nullptr) {
    return finish(kFlutterVenusV2InvalidArgument);
  }
  if (engine_id == 0u) {
    return finish(kFlutterVenusV2Unavailable);
  }
  VenusTextLayoutService* service = nullptr;
  {
    std::scoped_lock lock(g_registry_mutex);
    const auto found = g_registry.find(engine_id);
    if (found == g_registry.end() || found->second.service == nullptr) {
      return finish(kFlutterVenusV2Unavailable);
    }
    service = found->second.service;
    service->Retain();
  }
  const ScopedServiceReference retained(service);
  return service->LayoutParagraph(slot, lease_token, input, output);
}

int32_t VenusTextLayoutRegistry::BeginArtifactOwner(
    uint64_t engine_id,
    uint64_t registration_generation,
    uint64_t producer_generation,
    uint64_t* out_owner_id) {
  if (out_owner_id == nullptr) {
    return kFlutterVenusV2InvalidArgument;
  }
  *out_owner_id = 0u;
  if (engine_id == 0u || registration_generation == 0u ||
      producer_generation == 0u) {
    return kFlutterVenusV2InvalidArgument;
  }
  VenusTextLayoutService* service = nullptr;
  {
    std::scoped_lock lock(g_registry_mutex);
    const auto found = g_registry.find(engine_id);
    if (found == g_registry.end() || found->second.service == nullptr ||
        found->second.generation != registration_generation) {
      return kFlutterVenusV2NotFound;
    }
    service = found->second.service;
    service->Retain();
  }
  const ScopedServiceReference retained(service);
  return service->BeginArtifactOwner(registration_generation,
                                     producer_generation, out_owner_id);
}

int32_t VenusTextLayoutRegistry::BindArtifactOwner(uint64_t engine_id,
                                                   uint64_t owner_id,
                                                   uint32_t slot,
                                                   uint64_t lease_token) {
  if (engine_id == 0u || owner_id == 0u || lease_token == 0u) {
    return kFlutterVenusV2InvalidArgument;
  }
  VenusTextLayoutService* service = nullptr;
  {
    std::scoped_lock lock(g_registry_mutex);
    const auto found = g_registry.find(engine_id);
    if (found == g_registry.end() || found->second.service == nullptr) {
      return kFlutterVenusV2NotFound;
    }
    service = found->second.service;
    service->Retain();
  }
  const ScopedServiceReference retained(service);
  return service->BindArtifactOwner(owner_id, slot, lease_token);
}

int32_t VenusTextLayoutRegistry::SealArtifactOwner(uint64_t engine_id,
                                                   uint64_t owner_id) {
  if (engine_id == 0u || owner_id == 0u) {
    return kFlutterVenusV2InvalidArgument;
  }
  VenusTextLayoutService* service = nullptr;
  {
    std::scoped_lock lock(g_registry_mutex);
    const auto found = g_registry.find(engine_id);
    if (found == g_registry.end() || found->second.service == nullptr) {
      return kFlutterVenusV2NotFound;
    }
    service = found->second.service;
    service->Retain();
  }
  const ScopedServiceReference retained(service);
  return service->SealArtifactOwner(owner_id);
}

void VenusTextLayoutRegistry::ReleaseArtifactOwner(uint64_t engine_id,
                                                   uint64_t owner_id) {
  if (engine_id == 0u || owner_id == 0u) {
    return;
  }
  // owner_id is process-unique. Release must remain useful and idempotent even
  // if this engine id has already been re-registered to a newer service.
  venus_text_layout::ParagraphStore::Instance().ReleaseArtifactOwner(engine_id,
                                                                      owner_id);
}

}  // namespace flutter

// 段落入口 —— **独立导出符号**, 不进 ApiV2 那张已冻结的函数表。
// 符号不存在 = 引擎不支持段落粒度, 调用方 (venus) 退回单节点那套。
extern "C" FLUTTER_EXPORT int32_t FlutterVenusTextLayoutParagraphV2(
    uint64_t engine_id,
    uint32_t slot,
    uint64_t lease_token,
    const FlutterVenusTextLayoutParagraphInputV2* input,
    FlutterVenusTextLayoutParagraphOutputV2* output) {
  return flutter::VenusTextLayoutRegistry::LayoutParagraph(
      engine_id, slot, lease_token, input, output);
}

extern "C" FLUTTER_EXPORT int32_t
FlutterVenusTextLayoutArtifactOwnerBeginV1(
    uint64_t engine_id,
    uint64_t registration_generation,
    uint64_t producer_generation,
    uint64_t* out_owner_id) {
  return flutter::VenusTextLayoutRegistry::BeginArtifactOwner(
      engine_id, registration_generation, producer_generation, out_owner_id);
}

extern "C" FLUTTER_EXPORT int32_t
FlutterVenusTextLayoutArtifactOwnerBindSlotV1(uint64_t engine_id,
                                              uint64_t owner_id,
                                              uint32_t slot,
                                              uint64_t lease_token) {
  return flutter::VenusTextLayoutRegistry::BindArtifactOwner(
      engine_id, owner_id, slot, lease_token);
}

extern "C" FLUTTER_EXPORT int32_t
FlutterVenusTextLayoutArtifactOwnerSealV1(uint64_t engine_id,
                                          uint64_t owner_id) {
  return flutter::VenusTextLayoutRegistry::SealArtifactOwner(engine_id,
                                                              owner_id);
}

extern "C" FLUTTER_EXPORT void FlutterVenusTextLayoutArtifactOwnerReleaseV1(
    uint64_t engine_id,
    uint64_t owner_id) {
  flutter::VenusTextLayoutRegistry::ReleaseArtifactOwner(engine_id, owner_id);
}

extern "C" FLUTTER_EXPORT int32_t
FlutterVenusTextLayoutGetApiV2(uint64_t engine_id,
                               uint32_t caller_struct_size,
                               FlutterVenusTextLayoutApiV2* out_api) {
  return flutter::VenusTextLayoutRegistry::GetApi(engine_id, caller_struct_size,
                                                  out_api);
}
