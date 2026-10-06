// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/venus_text_layout_service.h"

#include "flutter/lib/ui/text/venus_text_layout_paragraph_store.h"
#include "flutter/shell/version/version.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <thread>

#include "flutter/fml/time/time_point.h"

namespace flutter {
namespace {

namespace core = venus_text_layout;
namespace wire = venus_text_layout::wire;

uint64_t NowNs() {
  return static_cast<uint64_t>(fml::TimePoint::Now().ToEpochDelta().ToNanoseconds());
}

uint64_t ThisThreadHash() {
  return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

constexpr uint32_t kInputStructSize =
    static_cast<uint32_t>(sizeof(FlutterVenusTextLayoutInputV2));
constexpr uint32_t kOutputStructSize =
    static_cast<uint32_t>(sizeof(FlutterVenusTextLayoutOutputV2));
constexpr uint32_t kParagraphInputStructSize =
    static_cast<uint32_t>(sizeof(FlutterVenusTextLayoutParagraphInputV2));
constexpr uint32_t kParagraphOutputStructSize =
    static_cast<uint32_t>(sizeof(FlutterVenusTextLayoutParagraphOutputV2));

uint64_t DirectInputChargeBytes(
    const FlutterVenusTextLayoutInputV2& input) {
  return sizeof(input) + static_cast<uint64_t>(input.text_length) +
         input.font_family_length + input.locale_length +
         input.ellipsis_length;
}

uint64_t ParagraphInputChargeBytes(
    const FlutterVenusTextLayoutParagraphInputV2& input) {
  return sizeof(input) +
         static_cast<uint64_t>(input.run_count) *
             sizeof(FlutterVenusTextLayoutRunV2) +
         input.text_length + input.font_families_length + input.locale_length +
         input.ellipsis_length;
}

// Canonical failure output: nine doubles are +0.0, counters zero, error fields
// non-zero. Mirrors the Phase 1 result-item contract exactly.
void WriteFailure(FlutterVenusTextLayoutOutputV2* out,
                  const core::ItemFailure& failure,
                  uint32_t slot) {
  std::memset(out, 0, sizeof(*out));
  out->struct_size = kOutputStructSize;
  out->status = kFlutterVenusV2ItemFailed;
  out->error_field_id = static_cast<uint32_t>(failure.field);
  out->error_detail = static_cast<uint32_t>(failure.detail);
  out->worker_slot = slot;
}

bool DecodeUtf8ToUtf16(const uint8_t* bytes,
                       uint32_t length,
                       std::u16string* out) {
  out->clear();
  uint32_t i = 0;
  while (i < length) {
    const uint8_t b0 = bytes[i];
    uint32_t cp = 0;
    uint32_t extra = 0;
    if (b0 < 0x80u) {
      cp = b0;
    } else if ((b0 & 0xE0u) == 0xC0u) {
      cp = b0 & 0x1Fu;
      extra = 1;
      if (cp == 0) return false;  // overlong
    } else if ((b0 & 0xF0u) == 0xE0u) {
      cp = b0 & 0x0Fu;
      extra = 2;
    } else if ((b0 & 0xF8u) == 0xF0u) {
      cp = b0 & 0x07u;
      extra = 3;
    } else {
      return false;
    }
    if (i + extra >= length + 0u && extra > 0 && i + extra > length - 1) return false;
    for (uint32_t k = 1; k <= extra; ++k) {
      const uint8_t bx = bytes[i + k];
      if ((bx & 0xC0u) != 0x80u) return false;
      cp = (cp << 6) | (bx & 0x3Fu);
    }
    if (extra == 2 && cp < 0x800u) return false;
    if (extra == 3 && cp < 0x10000u) return false;
    if (cp > 0x10FFFFu) return false;
    if (cp >= 0xD800u && cp <= 0xDFFFu) return false;
    if (cp < 0x10000u) {
      out->push_back(static_cast<char16_t>(cp));
    } else {
      cp -= 0x10000u;
      out->push_back(static_cast<char16_t>(0xD800u + (cp >> 10)));
      out->push_back(static_cast<char16_t>(0xDC00u + (cp & 0x3FFu)));
    }
    i += extra + 1;
  }
  return true;
}

bool FiniteNonNegative(double v) {
  return std::isfinite(v) && v >= 0.0;
}

int32_t WriteParagraphStatus(FlutterVenusTextLayoutParagraphOutputV2* out,
                             int32_t status) {
  out->status = status;
  return status;
}

bool RangeFits(uint32_t offset, uint32_t length, uint32_t total) {
  return offset <= total && length <= total - offset;
}

bool Utf8RangeFits(const uint8_t* bytes,
                   uint32_t total,
                   uint32_t offset,
                   uint32_t length) {
  if (!RangeFits(offset, length, total) || length == 0u) {
    return RangeFits(offset, length, total);
  }
  const uint32_t end = offset + length;
  return (offset == 0u || (bytes[offset] & 0xC0u) != 0x80u) &&
         (end == total || (bytes[end] & 0xC0u) != 0x80u);
}

bool ValidUtf8(const uint8_t* bytes, uint32_t length) {
  if (length == 0u) {
    return true;
  }
  std::u16string scratch;
  return bytes != nullptr && DecodeUtf8ToUtf16(bytes, length, &scratch);
}

int32_t ValidateParagraphInput(
    const FlutterVenusTextLayoutParagraphInputV2& input) {
  if (input.struct_size != kParagraphInputStructSize ||
      input.abi_version != FLUTTER_VENUS_TEXT_LAYOUT_ABI_V2) {
    return kFlutterVenusV2AbiMismatch;
  }
  const bool valid_strut =
      input.strut_enabled == 0u
          ? input.strut_font_family_offset == 0u &&
                input.strut_font_family_length == 0u &&
                input.strut_font_weight == 0u && input.strut_font_style == 0u &&
                input.strut_font_size == 0.0 &&
                input.strut_height_multiple == 0.0
          : input.strut_enabled == 1u &&
                (input.strut_font_family_length == 0u ||
                 input.font_families != nullptr) &&
                Utf8RangeFits(input.font_families, input.font_families_length,
                              input.strut_font_family_offset,
                              input.strut_font_family_length) &&
                input.strut_font_weight >= 1u &&
                input.strut_font_weight <= 1000u &&
                input.strut_font_style <= 1u && input.strut_font_size > 0.0 &&
                input.strut_font_size <= 4096.0 &&
                // 0 合法: CSS `line-height: 0` 的 strut 就是零高 (strut_enabled
                // 本身是 presence 位, 不靠倍数 > 0 表达"有 strut")。
                input.strut_height_multiple >= 0.0 &&
                input.strut_height_multiple <= 16.0;
  if (input.reserved0 != 0u || input.reserved1 != 0u || !valid_strut ||
      input.run_count == 0u ||
      input.run_count > kFlutterVenusV2MaxParagraphRuns ||
      input.text_length > kFlutterVenusV2MaxTextBytes ||
      input.font_families_length > kFlutterVenusV2MaxFontFamilyBytes ||
      input.locale_length > kFlutterVenusV2MaxLocaleBytes ||
      input.ellipsis_length > kFlutterVenusV2MaxEllipsisBytes ||
      input.line_metric_capacity > kFlutterVenusV2MaxLineMetrics ||
      input.fragment_rect_capacity > kFlutterVenusV2MaxFragmentRects ||
      input.runs == nullptr ||
      (input.text_length != 0u && input.text == nullptr) ||
      (input.font_families_length != 0u && input.font_families == nullptr) ||
      (input.locale_length != 0u && input.locale == nullptr) ||
      (input.ellipsis_length != 0u && input.ellipsis == nullptr) ||
      ((input.line_metric_capacity == 0u) !=
       (input.line_metrics_out == nullptr)) ||
      ((input.fragment_rect_capacity == 0u) !=
       (input.fragment_rects_out == nullptr)) ||
      input.text_direction < 1u || input.text_direction > 2u ||
      input.text_align < 1u || input.text_align > 6u ||
      input.soft_wrap > 1u ||
      input.max_lines > kFlutterVenusV2MaxLineMetrics ||
      !FiniteNonNegative(input.max_width) ||
      !FiniteNonNegative(input.paragraph_height_multiple) ||
      input.paragraph_height_multiple > 16.0 ||
      !ValidUtf8(input.text, input.text_length) ||
      !ValidUtf8(input.font_families, input.font_families_length) ||
      !ValidUtf8(input.locale, input.locale_length) ||
      !ValidUtf8(input.ellipsis, input.ellipsis_length)) {
    return kFlutterVenusV2InvalidArgument;
  }

  for (uint32_t i = 0u; i < input.run_count; ++i) {
    const FlutterVenusTextLayoutRunV2& run = input.runs[i];
    if (run.struct_size != sizeof(FlutterVenusTextLayoutRunV2)) {
      return kFlutterVenusV2AbiMismatch;
    }
    if (run.height_mode >
            static_cast<uint16_t>(wire::HeightMode::kMultiplier) ||
        run.reserved1 != 0u ||
        run.kind > kFlutterVenusV2RunKindLineBreak || run.font_style > 1u ||
        run.placeholder_alignment > 6u || run.placeholder_baseline > 1u ||
        !std::isfinite(run.font_size) ||
        !std::isfinite(run.letter_spacing) ||
        !std::isfinite(run.word_spacing) || !std::isfinite(run.height) ||
        !FiniteNonNegative(run.placeholder_width) ||
        !FiniteNonNegative(run.placeholder_height) ||
        !FiniteNonNegative(run.placeholder_baseline_offset)) {
      return kFlutterVenusV2InvalidArgument;
    }
    if (run.kind == kFlutterVenusV2RunKindText) {
      // 文本 run 只收 baseline / top / bottom (对齐行盒); middle 等档位没有实现。
      if (run.text_vertical_align != 0u && run.text_vertical_align != 1u &&
          run.text_vertical_align != 3u) {
        return kFlutterVenusV2InvalidArgument;
      }
      if (!Utf8RangeFits(input.text, input.text_length, run.text_offset,
                         run.text_length) ||
          !Utf8RangeFits(input.font_families, input.font_families_length,
                         run.font_family_offset, run.font_family_length) ||
          run.font_weight < 1u || run.font_weight > 1000u ||
          // CSS Fonts 4 §3.1: font-size 计算值为 0 是合法值, 不是错误 (常见于
          // 清除 inline-block 间隙); 负值仍不是合法 CSS 长度。
          run.font_size < 0.0 || run.font_size > 4096.0 || run.height < 0.0 ||
          run.height > 16.0) {
        return kFlutterVenusV2InvalidArgument;
      }
      // FontMetrics 下 height 不参与, 与单节点 InputV2 同一条"非活动值必须规范"。
      uint64_t height_bits = 0u;
      std::memcpy(&height_bits, &run.height, sizeof(height_bits));
      if (run.height_mode ==
              static_cast<uint16_t>(wire::HeightMode::kFontMetrics) &&
          height_bits != 0u) {
        return kFlutterVenusV2InvalidArgument;
      }
    } else if (run.height_mode != 0u || run.text_vertical_align != 0u ||
               run.text_offset != 0u ||
               run.text_length != 0u ||
               run.font_family_offset != 0u ||
               run.font_family_length != 0u) {
      return kFlutterVenusV2InvalidArgument;
    }
  }
  return kFlutterVenusV2Ok;
}

}  // namespace

VenusTextLayoutService::VenusTextLayoutService(
    uint64_t engine_id,
    uint64_t registration_generation,
    std::shared_ptr<FontCollection> fonts,
    bool impeller_enabled,
    void (*paragraph_core_probe_for_testing)(void*, bool),
    void* paragraph_core_probe_context)
    : engine_id_(engine_id),
      registration_generation_(registration_generation),
      fonts_(std::move(fonts)),
      impeller_enabled_(impeller_enabled),
      paragraph_core_probe_for_testing_(paragraph_core_probe_for_testing),
      paragraph_core_probe_context_(paragraph_core_probe_context) {
  slots_.resize(kFlutterVenusV2MaxSlots);
  // Per-slot facade over the shared frozen font manager snapshot.
  //
  // CloneForVenusWorker is mandatory here, not an optimisation: GetFontCollection
  // hands back the SAME txt::FontCollection every time, and that object owns
  // mutable SkParagraph caches (SkTHash). Handing one instance to all eight slots
  // let concurrent sync callers corrupt those tables -- reproduced as a Skia
  // fatal "check(fCount == oldCount)" the moment more than one runner shaped at
  // once. The clone keeps the immutable SkFontMgr snapshot shared and gives each
  // slot its own caches, which is what "each slot owns a facade" is supposed to
  // mean.
  const std::shared_ptr<txt::FontCollection> base =
      fonts_ ? fonts_->GetFontCollection() : nullptr;
  for (Slot& slot : slots_) {
    uint64_t slot_font_epoch = 0u;
    slot.fonts = base ? base->CloneForVenusWorker(&slot_font_epoch) : nullptr;
    if (slot.fonts) {
      font_epoch_ = slot_font_epoch;
    }
  }
  venus_text_layout::ParagraphStore::Instance().ActivateRegistration(
      engine_id_, registration_generation_);
}

VenusTextLayoutService::~VenusTextLayoutService() = default;

void VenusTextLayoutService::PopulateApi(FlutterVenusTextLayoutApiV2* api) {
  std::memset(api, 0, sizeof(*api));
  api->abi_version = FLUTTER_VENUS_TEXT_LAYOUT_ABI_V2;
  api->struct_size = static_cast<uint32_t>(sizeof(FlutterVenusTextLayoutApiV2));
  api->engine_id = engine_id_;
  api->registration_generation = registration_generation_;
  api->capabilities = kFlutterVenusV2CapabilityRequired;
  api->max_slots = kFlutterVenusV2MaxSlots;
  // engine_revision was left as the memset zeros, so every consumer that
  // validates it (Venus requires 40 lowercase hex + NUL) rejected the table
  // with InvalidArgument and the whole attach silently never happened.
  // GetFlutterEngineVersion() is the build-stamped commit; anything shorter or
  // non-canonical is copied as-is and will be rejected on purpose rather than
  // padded into something that looks valid.
  const char* revision = GetFlutterEngineVersion();
  if (revision != nullptr) {
    std::strncpy(api->engine_revision, revision,
                 FLUTTER_VENUS_TEXT_LAYOUT_ENGINE_REVISION_BYTES - 1u);
    api->engine_revision[FLUTTER_VENUS_TEXT_LAYOUT_ENGINE_REVISION_BYTES - 1u] =
        '\0';
  }
  api->context = this;
  api->release_context = &VenusTextLayoutService::ReleaseContextThunk;
  api->acquire_slot = &VenusTextLayoutService::AcquireSlotThunk;
  api->release_slot = &VenusTextLayoutService::ReleaseSlotThunk;
  api->layout_text_node_sync_v2 = &VenusTextLayoutService::LayoutSyncThunk;
  api->get_counters_v2 = &VenusTextLayoutService::GetCountersThunk;
}

void VenusTextLayoutService::Retain() {
  refs_.fetch_add(1, std::memory_order_relaxed);
}

void VenusTextLayoutService::Release() {
  if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    Shutdown();
    delete this;
  }
}

int32_t VenusTextLayoutService::GetReferenceCountForTesting() const {
  return refs_.load(std::memory_order_relaxed);
}

void VenusTextLayoutService::ReleaseContextThunk(void* ctx) {
  if (ctx) static_cast<VenusTextLayoutService*>(ctx)->Release();
}
int32_t VenusTextLayoutService::AcquireSlotThunk(void* ctx,
                                                 uint32_t* out_slot,
                                                 uint64_t* out_token) {
  if (!ctx || !out_slot || !out_token) return kFlutterVenusV2InvalidArgument;
  return static_cast<VenusTextLayoutService*>(ctx)->AcquireSlot(out_slot, out_token);
}
int32_t VenusTextLayoutService::ReleaseSlotThunk(void* ctx,
                                                 uint32_t slot,
                                                 uint64_t token) {
  if (!ctx) return kFlutterVenusV2InvalidArgument;
  return static_cast<VenusTextLayoutService*>(ctx)->ReleaseSlot(slot, token);
}
int32_t VenusTextLayoutService::LayoutSyncThunk(
    void* ctx,
    uint32_t slot,
    uint64_t token,
    const FlutterVenusTextLayoutInputV2* input,
    FlutterVenusTextLayoutOutputV2* output) {
  if (!ctx || !input || !output) return kFlutterVenusV2InvalidArgument;
  return static_cast<VenusTextLayoutService*>(ctx)->LayoutSync(slot, token, input, output);
}
int32_t VenusTextLayoutService::GetCountersThunk(
    void* ctx,
    uint32_t caller_struct_size,
    FlutterVenusTextLayoutCountersV2* out) {
  if (!ctx || !out) return kFlutterVenusV2InvalidArgument;
  return static_cast<VenusTextLayoutService*>(ctx)->GetCounters(caller_struct_size, out);
}


// ---------------------------------------------------------------------------
// Slot arbiter. Non-blocking; no waiting queue, no ticket. INV-SLOT: every
// member of the free set has zero active calls and appears exactly once.
// ---------------------------------------------------------------------------
int32_t VenusTextLayoutService::AcquireSlot(uint32_t* out_slot,
                                            uint64_t* out_token) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (accepting_ == 0u) return kFlutterVenusV2ShuttingDown;
  // 字体表自愈: 交出 slot 之前先确认它那份克隆不比 base 旧。
  //
  // ## 为什么必须在这里, 而不是构造期一次性克隆
  //
  // 构造函数跑在 VenusTextLayoutRegistry::Register() 里, 而 engine.cc 的调用
  // 顺序是:
  //   engine.cc:256  Register(engine_id, font_collection_, ...)
  //   engine.cc:270  SetupDefaultFontManager()      <-- 在后面
  //
  // 于是构造期克隆出来的 8 份 FontCollection **全都没有 font manager**,
  // 每一项 layout 都返回空 paragraph (kSemanticsCombination/kParagraphNull),
  // 而计数器看着"守恒且已静默" —— 真机上表现为 prewarmCompleted:1 /
  // deposited:0, 跑完了、没报错、一条都没进交付表。
  //
  // ## 为什么用 epoch 判新鲜, 而不是"记得在对的时机再 reconfigure 一次"
  //
  // 顺序依赖正是上面那个 bug 的来源。靠"记得调一次"守不住: 换个 embedder、
  // 换个启动路径、多一次字体注册, 就又错位了。epoch 是 FontCollection 自己在
  // 每次 Set*FontManager / RegisterFonts 时自增的 (font_collection.cc:35-103),
  // 拿它当判据, 无论谁在什么时候动了字体, 下一次取 slot 都会自动重克隆。
  const std::shared_ptr<txt::FontCollection> base =
      fonts_ ? fonts_->GetFontCollection() : nullptr;
  const uint64_t base_epoch = base ? base->GetVenusFontEpoch() : 0u;
  for (uint32_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].lease_token == 0u) {
      if (base != nullptr &&
          (slots_[i].fonts == nullptr ||
           slots_[i].font_epoch != base_epoch)) {
        uint64_t cloned_epoch = 0u;
        slots_[i].fonts = base->CloneForVenusWorker(&cloned_epoch);
        slots_[i].font_epoch = cloned_epoch;
        slot_font_refreshes_ += 1u;
      }
      slots_[i].lease_token = next_lease_token_++;
      leased_slots_ += 1u;
      *out_slot = i;
      *out_token = slots_[i].lease_token;
      return kFlutterVenusV2Ok;
    }
  }
  return kFlutterVenusV2SlotBusy;
}

void VenusTextLayoutService::ReturnSlotLocked(uint32_t slot) {
  // P3-2 交付时机规则：这个 slot 名下 pending 的 paragraph 到此刻才可交付。
  // 接在这里而不是 Venus 侧, 是因为"slot 何时交还"这个事件本来就发生在这里 ——
  // 放在事件发生的地方, 调用方不可能绕过它。
  venus_text_layout::ParagraphStore::Instance().NotifySlotReleased(
      engine_id_, registration_generation_, slot,
      accepting_ != 0u && !shutting_down_, slots_[slot].artifact_owner_id);
  slots_[slot].lease_token = 0u;
  slots_[slot].artifact_owner_id = 0u;
  if (leased_slots_ > 0u) leased_slots_ -= 1u;
  if (leased_slots_ == 0u) quiesce_cv_.notify_all();
}

int32_t VenusTextLayoutService::ReleaseSlot(uint32_t slot, uint64_t token) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (slot >= slots_.size() || token == 0u || slots_[slot].lease_token != token) {
    // Pure idempotent no-op: no return, no counter change (would underflow).
    return kFlutterVenusV2SlotNotOwned;
  }
  if (slots_[slot].active_call) return kFlutterVenusV2SlotBusy;
  ReturnSlotLocked(slot);
  return kFlutterVenusV2Ok;
}

int32_t VenusTextLayoutService::BeginArtifactOwner(
    uint64_t expected_registration_generation,
    uint64_t producer_generation,
    uint64_t* out_owner_id) {
  if (out_owner_id == nullptr || producer_generation == 0u) {
    return kFlutterVenusV2InvalidArgument;
  }
  *out_owner_id = 0u;
  std::unique_lock<std::mutex> lock(mutex_);
  if (expected_registration_generation != registration_generation_) {
    return kFlutterVenusV2NotFound;
  }
  if (accepting_ == 0u || shutting_down_) {
    return kFlutterVenusV2ShuttingDown;
  }
  return venus_text_layout::ParagraphStore::Instance().BeginArtifactOwner(
      engine_id_, registration_generation_, producer_generation, out_owner_id);
}

int32_t VenusTextLayoutService::BindArtifactOwner(uint64_t owner_id,
                                                  uint32_t slot,
                                                  uint64_t lease_token) {
  if (owner_id == 0u || lease_token == 0u) {
    return kFlutterVenusV2InvalidArgument;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  if (accepting_ == 0u || shutting_down_) {
    return kFlutterVenusV2ShuttingDown;
  }
  if (slot >= slots_.size() || slots_[slot].lease_token != lease_token) {
    return kFlutterVenusV2SlotNotOwned;
  }
  if (slots_[slot].active_call) {
    return kFlutterVenusV2SlotBusy;
  }
  const bool already_bound = slots_[slot].artifact_owner_id == owner_id;
  if (!already_bound && slots_[slot].artifact_owner_id != 0u) {
    return kFlutterVenusV2SlotBusy;
  }
  const int32_t status =
      venus_text_layout::ParagraphStore::Instance().BindArtifactOwner(
          engine_id_, registration_generation_, owner_id, !already_bound);
  if (status == kFlutterVenusV2Ok && !already_bound) {
    slots_[slot].artifact_owner_id = owner_id;
  }
  return status;
}

int32_t VenusTextLayoutService::SealArtifactOwner(uint64_t owner_id) {
  if (owner_id == 0u) {
    return kFlutterVenusV2InvalidArgument;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  if (shutting_down_) {
    return kFlutterVenusV2ShuttingDown;
  }
  return venus_text_layout::ParagraphStore::Instance().SealArtifactOwner(
      engine_id_, registration_generation_, owner_id);
}

// ---------------------------------------------------------------------------
// Validation + decode. Shared by both transports so a node reaches the core in
// exactly one shape.
// ---------------------------------------------------------------------------
bool VenusTextLayoutService::DecodeAndValidate(
    const FlutterVenusTextLayoutInputV2& in,
    OwnedNode* node) {
  auto fail = [&](wire::ErrorFieldId f, wire::ItemErrorDetail d) {
    node->pre_failed = true;
    node->failure = core::ItemFailure{wire::ItemStatus::kInvalidRange, f, d};
    return false;
  };
  if (in.struct_size != kInputStructSize || in.abi_version != FLUTTER_VENUS_TEXT_LAYOUT_ABI_V2 ||
      (in.flags & ~static_cast<uint32_t>(kFlutterVenusV2InputFlagKnownMask)) != 0u ||
      in.worker_slot != 0u) {
    return fail(wire::ErrorFieldId::kStructSize, wire::ItemErrorDetail::kNonzeroFlagsOrReserved);
  }
  if (in.text == nullptr || in.text_length == 0u ||
      in.text_length > static_cast<uint32_t>(kFlutterVenusV2MaxTextBytes)) {
    return fail(wire::ErrorFieldId::kTextRef, wire::ItemErrorDetail::kEmptyRequired);
  }
  // font-family: 允许"不指定" —— 空引用 = 用 FontCollection 的默认字体。
  //
  // ## 为什么必须允许
  //
  // Venus 里 HTML 的 font-family 本来就不生效, 一律回退到 app 的默认字体, 所以
  // 真实页面的 renderStyle.fontFamily 恒为 null。原来这里"空即拒", 于是真实页面
  // 的预排准入率是 **0%** —— 整个 Feature 在真实页面上等于没做。
  //
  // ## 为什么这样安全
  //
  // 空引用落到 ApplyFontFamilyChain 就是 families 为空 → paragraph_style
  // .font_family = "" 且 text_style->font_families = {} —— 这与 UI 线程那边
  // `TextStyle(fontFamily: null)` 生成的 txt::TextStyle **完全一致**, 而两边
  // 用的是同一份 FontCollection (worker 那份是克隆, 共享同一个 SkFontMgr),
  // 默认字体解析结果相同。
  //
  // ## 规范未启用值
  //
  // "不指定"的唯一合法形态是 **指针为空且长度为 0**。指针空但长度非 0、
  // 或指针非空但长度 0, 都是编码方出了 bug, 仍然拒 —— 未启用字段必须精确为零,
  // 否则"看起来能跑"的错误编码会悄悄混进来。
  const bool family_unspecified =
      in.font_family == nullptr && in.font_family_length == 0u;
  if (!family_unspecified &&
      (in.font_family == nullptr || in.font_family_length == 0u ||
       in.font_family_length >
           static_cast<uint32_t>(kFlutterVenusV2MaxFontFamilyBytes))) {
    return fail(wire::ErrorFieldId::kFontFamilyRef, wire::ItemErrorDetail::kEmptyRequired);
  }
  if (in.locale == nullptr || in.locale_length == 0u ||
      in.locale_length > static_cast<uint32_t>(kFlutterVenusV2MaxLocaleBytes)) {
    return fail(wire::ErrorFieldId::kLocaleRef, wire::ItemErrorDetail::kEmptyRequired);
  }
  const bool wants_ellipsis =
      in.overflow_mode == static_cast<uint16_t>(wire::OverflowMode::kEllipsis);
  if (!wants_ellipsis && (in.ellipsis != nullptr || in.ellipsis_length != 0u)) {
    return fail(wire::ErrorFieldId::kEllipsisRef, wire::ItemErrorDetail::kNoncanonicalInactiveValue);
  }
  if (!FiniteNonNegative(in.min_width) || !FiniteNonNegative(in.max_width) ||
      !FiniteNonNegative(in.min_height) || !FiniteNonNegative(in.max_height)) {
    return fail(wire::ErrorFieldId::kMinWidth, wire::ItemErrorDetail::kNonFinite);
  }
  if (in.min_width > in.max_width || in.min_height > in.max_height) {
    return fail(wire::ErrorFieldId::kMinWidth, wire::ItemErrorDetail::kMinGreaterThanMax);
  }
  // 行内起始偏移: 与四个尺寸同一条 FiniteNonNegative 口径。
  // 超过可用宽是调用方算错了 —— 那样占位会把整行吃光, 断行结果毫无意义,
  // 与其排出一个没人看得懂的段落, 不如当场拒掉。
  if (!FiniteNonNegative(in.leading_placeholder_width)) {
    return fail(wire::ErrorFieldId::kMinWidth, wire::ItemErrorDetail::kNonFinite);
  }
  // **`> 0` 这个前置不能少。** 0 是"不占位"的合法表达, 而 max_width 可以是 0
  // (零宽容器里的文字)。少了前置就变成 `0 >= 0` 成立 —— 把一批根本没送占位的
  // 条目一起拒掉。实测 venus css/cascade_stress 因此 11 条被拒、整页落回 Dart,
  // 而错误码 (MinWidth/kMinGreaterThanMax) 看着完全像是调用方算错了偏移。
  if (in.leading_placeholder_width > 0.0 &&
      in.leading_placeholder_width >= in.max_width) {
    return fail(wire::ErrorFieldId::kMinWidth, wire::ItemErrorDetail::kMinGreaterThanMax);
  }
  // CSS Fonts 4 §3.1: font-size 计算值为 0 是合法值, 不是错误 (常见于清除
  // inline-block 间隙); 负值仍不是合法 CSS 长度。判据页 WPT css-grid/subgrid
  // standalone-axis-size-005/006 的 `<div style="font-size: 0">`。
  if (!std::isfinite(in.font_size) || in.font_size < 0.0 || in.font_size > 4096.0) {
    return fail(wire::ErrorFieldId::kFontSize, wire::ItemErrorDetail::kNonFinite);
  }
  if (!std::isfinite(in.letter_spacing)) {
    return fail(wire::ErrorFieldId::kLetterSpacing, wire::ItemErrorDetail::kNonFinite);
  }
  if (!std::isfinite(in.word_spacing)) {
    return fail(wire::ErrorFieldId::kWordSpacing, wire::ItemErrorDetail::kNonFinite);
  }
  if (!std::isfinite(in.height)) {
    return fail(wire::ErrorFieldId::kHeight, wire::ItemErrorDetail::kNonFinite);
  }
  if (!std::isfinite(in.scaler_parameter)) {
    return fail(wire::ErrorFieldId::kScalerParameter, wire::ItemErrorDetail::kNonFinite);
  }
  if (in.font_weight < 1u || in.font_weight > 1000u) {
    return fail(wire::ErrorFieldId::kFontWeight, wire::ItemErrorDetail::kAboveMax);
  }

  // Every semantic enum is range checked before it is cast. Without this an
  // out-of-range wire value (e.g. text_direction = 4242) was static_cast
  // straight into the wire view and handed to the core, so "fail closed" only
  // held for pointers, sizes and doubles -- the enums were fail OPEN. The
  // stress driver's `malformed` mix reported zero item failures precisely
  // because of this, i.e. the injection looked like it was not firing.
  struct EnumBound {
    uint16_t value;
    uint16_t min;
    uint16_t max;
    wire::ErrorFieldId field;
  };
  const EnumBound bounds[] = {
      {in.text_encoding, 1u, 1u, wire::ErrorFieldId::kTextEncoding},
      {in.text_direction, 1u, 2u, wire::ErrorFieldId::kTextDirection},
      {in.text_align, 1u, 6u, wire::ErrorFieldId::kTextAlign},
      {in.font_style, 1u, 2u, wire::ErrorFieldId::kFontStyle},
      {in.leading_distribution, 1u, 2u, wire::ErrorFieldId::kLeadingDistribution},
      {in.text_width_basis, 1u, 2u, wire::ErrorFieldId::kTextWidthBasis},
      {in.text_scaler_kind, 1u, 2u, wire::ErrorFieldId::kTextScalerKind},
      {in.max_lines_mode, 1u, 2u, wire::ErrorFieldId::kMaxLinesMode},
      {in.overflow_mode, 1u, 2u, wire::ErrorFieldId::kOverflowMode},
      {in.soft_wrap, 1u, 2u, wire::ErrorFieldId::kSoftWrap},
      {in.apply_height_to_first_ascent, 1u, 2u, wire::ErrorFieldId::kFirstAscent},
      {in.apply_height_to_last_descent, 1u, 2u, wire::ErrorFieldId::kLastDescent},
      {in.height_mode, 1u, 2u, wire::ErrorFieldId::kHeightMode},
      {in.strut_mode, 1u, 1u, wire::ErrorFieldId::kStrutMode},
  };
  for (const EnumBound& bound : bounds) {
    if (bound.value < bound.min || bound.value > bound.max) {
      return fail(bound.field, wire::ItemErrorDetail::kNoncanonicalInactiveValue);
    }
  }
  // Combination truth table -- mirrored field for field from the Phase 1 debug
  // oracle (venus_text_layout_batch_oracle.cc), which is the frozen authority.
  // Inventing a stricter rule here is not "safer": the first attempt required
  // scaler_parameter == 0.0 for kNoScaling, while the canonical inactive value
  // is 1.0, so every item of the real corpus was rejected on device.
  const bool unlimited_lines =
      in.max_lines_mode == static_cast<uint16_t>(wire::MaxLinesMode::kUnlimited);
  if (unlimited_lines) {
    if (in.max_lines != 0u) {
      return fail(wire::ErrorFieldId::kMaxLines,
                  wire::ItemErrorDetail::kNoncanonicalInactiveValue);
    }
  } else if (in.max_lines < 1u || in.max_lines > 1024u) {
    return fail(wire::ErrorFieldId::kMaxLines,
                in.max_lines == 0u ? wire::ItemErrorDetail::kBelowMin
                                   : wire::ItemErrorDetail::kAboveMax);
  }
  if (in.soft_wrap == static_cast<uint16_t>(wire::SoftWrap::kFalse)) {
    return fail(wire::ErrorFieldId::kSemanticsCombination,
                wire::ItemErrorDetail::kUnsupportedCombination);
  }
  if (wants_ellipsis && unlimited_lines) {
    return fail(wire::ErrorFieldId::kSemanticsCombination,
                wire::ItemErrorDetail::kUnsupportedCombination);
  }
  if (in.text_scaler_kind ==
      static_cast<uint16_t>(wire::TextScalerKind::kNoScaling)) {
    // Bit pattern, not a float compare: the canonical inactive value is exactly
    // 1.0 and nothing else, including a differently-encoded 1.0.
    uint64_t scaler_bits = 0u;
    std::memcpy(&scaler_bits, &in.scaler_parameter, sizeof(scaler_bits));
    if (scaler_bits != 0x3ff0000000000000ull) {
      return fail(wire::ErrorFieldId::kScalerParameter,
                  wire::ItemErrorDetail::kNoncanonicalInactiveValue);
    }
  } else {
    if (in.scaler_parameter <= 0.0 || in.scaler_parameter > 16.0) {
      return fail(wire::ErrorFieldId::kScalerParameter,
                  in.scaler_parameter <= 0.0 ? wire::ItemErrorDetail::kBelowMin
                                             : wire::ItemErrorDetail::kAboveMax);
    }
    const double effective_size = in.font_size * in.scaler_parameter;
    if (!std::isfinite(effective_size) || effective_size > 4096.0) {
      return fail(wire::ErrorFieldId::kScalerParameter,
                  wire::ItemErrorDetail::kAboveMax);
    }
  }
  if (in.height_mode == static_cast<uint16_t>(wire::HeightMode::kFontMetrics)) {
    uint64_t height_bits = 0u;
    std::memcpy(&height_bits, &in.height, sizeof(height_bits));
    if (height_bits != 0u) {
      return fail(wire::ErrorFieldId::kHeight,
                  wire::ItemErrorDetail::kNoncanonicalInactiveValue);
    }
  } else if (in.height < 0.0 || in.height > 16.0) {
    return fail(wire::ErrorFieldId::kHeight,
                in.height < 0.0 ? wire::ItemErrorDetail::kBelowMin
                                : wire::ItemErrorDetail::kAboveMax);
  }

  node->view.text_color_argb = in.text_color_argb;
  node->text_utf8.assign(reinterpret_cast<const char*>(in.text), in.text_length);
  if (in.font_family != nullptr && in.font_family_length != 0u) {
    node->font_family_utf8.assign(reinterpret_cast<const char*>(in.font_family),
                                  in.font_family_length);
  } else {
    // 不指定 -> 空串。ApplyFontFamilyChain 会把它切成空 vector, txt 用
    // FontCollection 的默认字体, 与 UI 线程 fontFamily:null 同路。
    node->font_family_utf8.clear();
  }
  node->locale_utf8.assign(reinterpret_cast<const char*>(in.locale), in.locale_length);
  if (wants_ellipsis && in.ellipsis && in.ellipsis_length) {
    node->ellipsis_utf8.assign(reinterpret_cast<const char*>(in.ellipsis), in.ellipsis_length);
  }
  if (!DecodeUtf8ToUtf16(reinterpret_cast<const uint8_t*>(node->text_utf8.data()),
                         static_cast<uint32_t>(node->text_utf8.size()), &node->text_u16)) {
    return fail(wire::ErrorFieldId::kTextUtf8, wire::ItemErrorDetail::kMalformedUtf8);
  }
  if (!node->ellipsis_utf8.empty() &&
      !DecodeUtf8ToUtf16(reinterpret_cast<const uint8_t*>(node->ellipsis_utf8.data()),
                         static_cast<uint32_t>(node->ellipsis_utf8.size()), &node->ellipsis_u16)) {
    return fail(wire::ErrorFieldId::kEllipsisUtf8, wire::ItemErrorDetail::kMalformedUtf8);
  }
  wire::ParsedItemView& v = node->view;
  v.min_width = in.min_width;   v.max_width = in.max_width;
  v.prelayout_token = in.prelayout_token;  // Phase 3: 对 Engine 不透明, 只当键用
  v.min_height = in.min_height; v.max_height = in.max_height;
  v.font_size = in.font_size;   v.letter_spacing = in.letter_spacing;
  v.word_spacing = in.word_spacing; v.height = in.height;
  v.scaler_parameter = in.scaler_parameter;
  v.leading_placeholder_width = in.leading_placeholder_width;
  v.font_weight = in.font_weight;   v.max_lines = in.max_lines;
  v.text_encoding = static_cast<wire::TextEncoding>(in.text_encoding);
  v.text_direction = static_cast<wire::TextDirection>(in.text_direction);
  v.text_align = static_cast<wire::TextAlign>(in.text_align);
  v.font_style = static_cast<wire::FontStyle>(in.font_style);
  v.leading_distribution = static_cast<wire::LeadingDistribution>(in.leading_distribution);
  v.text_width_basis = static_cast<wire::TextWidthBasis>(in.text_width_basis);
  v.text_scaler_kind = static_cast<wire::TextScalerKind>(in.text_scaler_kind);
  v.max_lines_mode = static_cast<wire::MaxLinesMode>(in.max_lines_mode);
  v.overflow_mode = static_cast<wire::OverflowMode>(in.overflow_mode);
  v.soft_wrap = static_cast<wire::SoftWrap>(in.soft_wrap);
  v.apply_height_to_first_ascent =
      static_cast<wire::ApplyHeightToFirstAscent>(in.apply_height_to_first_ascent);
  v.apply_height_to_last_descent =
      static_cast<wire::ApplyHeightToLastDescent>(in.apply_height_to_last_descent);
  v.height_mode = static_cast<wire::HeightMode>(in.height_mode);
  v.strut_mode = static_cast<wire::StrutMode>(in.strut_mode);
  return true;
}

void VenusTextLayoutService::RunNode(OwnedNode& node,
                                     const std::shared_ptr<txt::FontCollection>& fonts,
                                     uint32_t slot,
                                     uint64_t artifact_owner_id,
                                     uint64_t input_charge_bytes,
                                     bool item_timing,
                                     uint64_t enqueue_ns,
                                     FlutterVenusTextLayoutOutputV2* out) {
  const uint64_t begin = item_timing ? NowNs() : 0u;
  int32_t delivery_status = kFlutterVenusV2Ok;
  if (node.pre_failed) {
    WriteFailure(out, node.failure, slot);
  } else {
    // Re-point the borrowed UTF-8 views at the owned strings right before the
    // core runs; vector reallocation can therefore never leave them dangling.
    node.view.text = node.text_utf8;
    node.view.font_family = node.font_family_utf8;
    node.view.locale = node.locale_utf8;
    node.view.ellipsis = node.ellipsis_utf8;
    core::ParsedNodeView view;
    view.view = node.view;
    view.text = node.text_u16;
    view.ellipsis = node.ellipsis_u16;
    core::NodeMetrics m;
    // Phase 3：带了 prelayout_token 才要 paragraph —— 不带的走 Phase 2 原路，
    // 一个字节的额外开销都不产生。
    const uint64_t token = node.view.prelayout_token;
    std::unique_ptr<txt::Paragraph> laid_out;
    const core::ItemFailure f = core::LayoutTextNodeCore(
        view, fonts, impeller_enabled_, &m, token != 0u ? &laid_out : nullptr);
    if (f.ok() && laid_out != nullptr) {
      // 存进交付表。此刻**仍持有 slot**，所以这一条是 pending —— 要等
      // ReturnSlotLocked 放行才可认领。见 ParagraphStore::NotifySlotReleased。
      uint64_t epoch = 0u;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        epoch = font_epoch_;
      }
      delivery_status = venus_text_layout::ParagraphStore::Instance().Deposit(
          token, node.view.max_width, epoch, engine_id_,
          registration_generation_, /*snapshot_generation=*/0u, slot,
          std::move(laid_out), artifact_owner_id, input_charge_bytes);
    }
    if (!f.ok()) {
      WriteFailure(out, f, slot);
    } else {
      std::memset(out, 0, sizeof(*out));
      out->struct_size = kOutputStructSize;
      out->status = delivery_status;
      out->line_count = m.line_count;
      out->flags = m.did_exceed_max_lines
                       ? static_cast<uint32_t>(kFlutterVenusV2OutputFlagDidExceedMaxLines)
                       : 0u;
      out->worker_slot = slot;
      out->content_height = m.content_height;
  out->paragraph_width = m.paragraph_width;
      out->paragraph_height = m.paragraph_height;
      out->box_width = m.box_width;
      out->box_height = m.box_height;
      out->min_intrinsic_width = m.min_intrinsic_width;
      out->max_intrinsic_width = m.max_intrinsic_width;
      out->alphabetic_baseline = m.alphabetic_baseline;
      out->ideographic_baseline = m.ideographic_baseline;
      out->first_line_left = m.first_line_left;
      // float 位模式塞进原 reserved0。memcpy 而不是 reinterpret_cast:
      // 后者是严格别名 UB, 优化开高了会被静默改写。
      {
        const float last_line = static_cast<float>(m.last_line_width);
        std::memcpy(&out->last_line_width_bits, &last_line, sizeof(float));
      }
    }
  }
  if (item_timing) {
    out->thread_id_hash = ThisThreadHash();
    out->enqueue_ns = enqueue_ns ? enqueue_ns : begin;
    out->begin_ns = begin;
    out->end_ns = NowNs();
  }
}

// ---------------------------------------------------------------------------
// Synchronous transport. Atomic entry: accepting + lease + re-entrancy +
// active_slot_calls++ all inside one critical section.
// ---------------------------------------------------------------------------
int32_t VenusTextLayoutService::LayoutSync(
    uint32_t slot,
    uint64_t token,
    const FlutterVenusTextLayoutInputV2* input,
    FlutterVenusTextLayoutOutputV2* output) {
  std::shared_ptr<txt::FontCollection> fonts;
  uint64_t artifact_owner_id = 0u;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (accepting_ == 0u) { rejected_total_ += 1u; return kFlutterVenusV2ShuttingDown; }
    if (slot >= slots_.size() || token == 0u || slots_[slot].lease_token != token) {
      rejected_total_ += 1u;
      return kFlutterVenusV2SlotNotOwned;
    }
    if (slots_[slot].active_call) { rejected_total_ += 1u; return kFlutterVenusV2SlotBusy; }
    slots_[slot].active_call = true;
    active_slot_calls_ += 1u;
    accepted_total_ += 1u;
    fonts = slots_[slot].fonts;
    artifact_owner_id = slots_[slot].artifact_owner_id;
  }

  OwnedNode node;
  DecodeAndValidate(*input, &node);
  const bool item_timing =
      (input->flags & static_cast<uint32_t>(kFlutterVenusV2InputFlagItemTiming)) != 0u;
  RunNode(node, fonts, slot, artifact_owner_id,
          DirectInputChargeBytes(*input), item_timing, 0u, output);

  {
    std::unique_lock<std::mutex> lock(mutex_);
    slots_[slot].active_call = false;
    if (active_slot_calls_ > 0u) active_slot_calls_ -= 1u;
    completed_total_ += 1u;
    if (active_slot_calls_ == 0u) quiesce_cv_.notify_all();
  }
  return kFlutterVenusV2Ok;
}



// ---------------------------------------------------------------------------
// 段落粒度 (一个 IFC 一次排完)
// ---------------------------------------------------------------------------
namespace {

/// UTF-8 -> UTF-16。与单节点那条路同口径 (GetRectsForRange 用 UTF-16 码元下标)。
std::u16string Utf8ToUtf16(const uint8_t* bytes, uint32_t length) {
  std::u16string out;
  if (bytes == nullptr || length == 0u) return out;
  out.reserve(length);
  uint32_t i = 0;
  while (i < length) {
    uint32_t cp = 0;
    const uint8_t b0 = bytes[i];
    uint32_t extra = 0;
    if (b0 < 0x80u) { cp = b0; extra = 0; }
    else if ((b0 & 0xE0u) == 0xC0u) { cp = b0 & 0x1Fu; extra = 1; }
    else if ((b0 & 0xF0u) == 0xE0u) { cp = b0 & 0x0Fu; extra = 2; }
    else if ((b0 & 0xF8u) == 0xF0u) { cp = b0 & 0x07u; extra = 3; }
    else { cp = 0xFFFDu; extra = 0; }
    if (i + extra >= length) { cp = 0xFFFDu; extra = 0; }
    for (uint32_t k = 1; k <= extra; ++k) {
      cp = (cp << 6) | (bytes[i + k] & 0x3Fu);
    }
    i += extra + 1;
    if (cp >= 0x10000u) {
      cp -= 0x10000u;
      out.push_back(static_cast<char16_t>(0xD800u + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00u + (cp & 0x3FFu)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
  }
  return out;
}

std::string SliceUtf8(const uint8_t* base, uint32_t base_len, uint32_t offset,
                      uint32_t length) {
  if (base == nullptr || offset >= base_len) return std::string();
  const uint32_t end = std::min(base_len, offset + length);
  return std::string(reinterpret_cast<const char*>(base + offset), end - offset);
}

}  // namespace

int32_t VenusTextLayoutService::LayoutParagraph(
    uint32_t slot,
    uint64_t token,
    const FlutterVenusTextLayoutParagraphInputV2* input,
    FlutterVenusTextLayoutParagraphOutputV2* output) {
  if (output == nullptr) {
    return kFlutterVenusV2InvalidArgument;
  }
  if (output->struct_size != kParagraphOutputStructSize) {
    return kFlutterVenusV2AbiMismatch;
  }
  std::memset(output, 0, sizeof(*output));
  output->struct_size = kParagraphOutputStructSize;
  if (input == nullptr) {
    return WriteParagraphStatus(output, kFlutterVenusV2InvalidArgument);
  }
  const int32_t validation = ValidateParagraphInput(*input);
  if (validation != kFlutterVenusV2Ok) {
    return WriteParagraphStatus(output, validation);
  }
  std::shared_ptr<txt::FontCollection> fonts;
  uint64_t layout_font_epoch = 0u;
  uint64_t artifact_owner_id = 0u;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (accepting_ == 0u) {
      rejected_total_ += 1u;
      return WriteParagraphStatus(output, kFlutterVenusV2ShuttingDown);
    }
    if (slot >= slots_.size() || token == 0u || slots_[slot].lease_token != token) {
      rejected_total_ += 1u;
      return WriteParagraphStatus(output, kFlutterVenusV2SlotNotOwned);
    }
    if (slots_[slot].active_call) {
      rejected_total_ += 1u;
      return WriteParagraphStatus(output, kFlutterVenusV2SlotBusy);
    }
    slots_[slot].active_call = true;
    active_slot_calls_ += 1u;
    accepted_total_ += 1u;
    fonts = slots_[slot].fonts;
    layout_font_epoch = font_epoch_;
    artifact_owner_id = slots_[slot].artifact_owner_id;
  }

  venus_text_layout::ParagraphInputView view;
  view.locale = SliceUtf8(input->locale, input->locale_length, 0u,
                          input->locale_length);
  view.max_width = input->max_width;
  view.height_multiple = input->paragraph_height_multiple;
  view.strut_enabled = input->strut_enabled != 0u;
  if (view.strut_enabled) {
    view.strut_font_family = SliceUtf8(
        input->font_families, input->font_families_length,
        input->strut_font_family_offset, input->strut_font_family_length);
    view.strut_font_size = input->strut_font_size;
    view.strut_font_weight = static_cast<int>(input->strut_font_weight);
    view.strut_italic = input->strut_font_style != 0u;
    view.strut_height_multiple = input->strut_height_multiple;
  }
  view.text_align =
      static_cast<venus_text_layout_batch::TextAlign>(input->text_align);
  view.text_direction =
      static_cast<venus_text_layout_batch::TextDirection>(input->text_direction);
  view.soft_wrap = input->soft_wrap != 0u;
  view.max_lines = input->max_lines;
  view.runs.reserve(input->run_count);
  for (uint32_t i = 0; i < input->run_count; ++i) {
    const FlutterVenusTextLayoutRunV2& r = input->runs[i];
    venus_text_layout::ParagraphRunView rv;
    rv.is_placeholder = r.kind == kFlutterVenusV2RunKindPlaceholder;
    rv.is_line_break = r.kind == kFlutterVenusV2RunKindLineBreak;
    if (!rv.is_placeholder && !rv.is_line_break) {
      rv.text = Utf8ToUtf16(input->text + r.text_offset, r.text_length);
    }
    rv.font_family = SliceUtf8(input->font_families, input->font_families_length,
                               r.font_family_offset, r.font_family_length);
    rv.font_size = r.font_size;
    rv.font_weight = static_cast<int>(r.font_weight);
    rv.italic = r.font_style != 0u;
    rv.letter_spacing = r.letter_spacing;
    rv.word_spacing = r.word_spacing;
    rv.height_multiple = r.height;
    rv.height_declared =
        r.height_mode == static_cast<uint16_t>(wire::HeightMode::kMultiplier);
    rv.color_argb = r.text_color_argb;
    rv.placeholder_width = r.placeholder_width;
    rv.placeholder_height = r.placeholder_height;
    rv.placeholder_baseline_offset = r.placeholder_baseline_offset;
    rv.placeholder_alignment = r.placeholder_alignment;
    rv.text_vertical_align = r.text_vertical_align;
    view.runs.push_back(std::move(rv));
  }

  venus_text_layout::ParagraphMetrics metrics;
  std::unique_ptr<txt::Paragraph> laid_out;
  if (paragraph_core_probe_for_testing_ != nullptr) {
    paragraph_core_probe_for_testing_(paragraph_core_probe_context_, true);
  }
  const venus_text_layout::ItemFailure failure =
      venus_text_layout::LayoutParagraphCore(view, fonts, impeller_enabled_,
                                             &metrics,
                                             input->paragraph_token != 0u
                                                 ? &laid_out
                                                 : nullptr);
  if (paragraph_core_probe_for_testing_ != nullptr) {
    paragraph_core_probe_for_testing_(paragraph_core_probe_context_, false);
  }
  output->status = failure.ok() ? kFlutterVenusV2Ok
                                : kFlutterVenusV2ItemFailed;
  output->error_field_id = static_cast<uint32_t>(failure.field);
  output->error_detail = static_cast<uint32_t>(failure.detail);
  if (failure.ok()) {
    output->width = metrics.width;
    output->height = metrics.height;
    output->first_baseline = metrics.first_baseline;
    output->min_intrinsic_width = metrics.min_intrinsic_width;
    output->max_intrinsic_width = metrics.max_intrinsic_width;
  }
  // **截断必须可见。** 悄悄少给几条片段, 与"这个 run 只有一行"长得一模一样,
  // 而后者会让调用方把跨行元素当成单行元素排 —— 正是要根治的那个形状。
  output->truncated = 0u;
  uint32_t lines_written = 0u;
  if (failure.ok() && input->line_metrics_out != nullptr) {
    for (const auto& l : metrics.lines) {
      if (lines_written >= input->line_metric_capacity) { output->truncated = 1u; break; }
      FlutterVenusTextLayoutLineMetricV2& dst = input->line_metrics_out[lines_written++];
      dst.struct_size = sizeof(FlutterVenusTextLayoutLineMetricV2);
      dst.line_number = l.line_number;
      dst.left = l.left; dst.width = l.width; dst.height = l.height;
      dst.baseline = l.baseline; dst.ascent = l.ascent; dst.descent = l.descent;
    }
  } else if (failure.ok() && !metrics.lines.empty()) {
    output->truncated = 1u;
  }
  uint32_t frags_written = 0u;
  if (failure.ok() && input->fragment_rects_out != nullptr) {
    for (const auto& f : metrics.fragments) {
      if (frags_written >= input->fragment_rect_capacity) { output->truncated = 1u; break; }
      FlutterVenusTextLayoutFragmentRectV2& dst = input->fragment_rects_out[frags_written++];
      dst.struct_size = sizeof(FlutterVenusTextLayoutFragmentRectV2);
      dst.run_index = f.run_index;
      dst.line_number = f.line_number;
      dst.reserved0 = 0u;
      dst.left = f.left; dst.top = f.top; dst.right = f.right; dst.bottom = f.bottom;
    }
  } else if (failure.ok() && !metrics.fragments.empty()) {
    output->truncated = 1u;
  }
  output->line_metric_count = lines_written;
  output->fragment_rect_count = frags_written;

  if (failure.ok() && output->truncated == 0u && laid_out != nullptr) {
    // Paragraph V2 当前没有 snapshot generation。固定为 0，直到唯一 V2
    // 契约显式提供；不能从无关 counter 猜值。metrics 与该指针来自同一次 Layout。
    const int32_t deposit_status =
        venus_text_layout::ParagraphStore::Instance().Deposit(
            input->paragraph_token, input->max_width, layout_font_epoch,
            engine_id_, registration_generation_, /*snapshot_generation=*/0u,
            slot, std::move(laid_out), artifact_owner_id,
            ParagraphInputChargeBytes(*input));
    if (deposit_status != kFlutterVenusV2Ok) {
      output->status = deposit_status;
    }
  }

  {
    std::unique_lock<std::mutex> lock(mutex_);
    slots_[slot].active_call = false;
    if (active_slot_calls_ > 0u) active_slot_calls_ -= 1u;
    completed_total_ += 1u;
    if (active_slot_calls_ == 0u) quiesce_cv_.notify_all();
  }
  return output->status;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
int32_t VenusTextLayoutService::GetCounters(
    uint32_t caller_struct_size,
    FlutterVenusTextLayoutCountersV2* out) {
  if (caller_struct_size < sizeof(FlutterVenusTextLayoutCountersV2)) {
    return kFlutterVenusV2AbiMismatch;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  out->struct_size = static_cast<uint32_t>(sizeof(FlutterVenusTextLayoutCountersV2));
  out->abi_version = FLUTTER_VENUS_TEXT_LAYOUT_ABI_V2;
  out->active_slot_calls = active_slot_calls_;
  out->leased_slots = leased_slots_;
  out->accepted_total = accepted_total_;
  out->completed_total = completed_total_;
  out->rejected_total = rejected_total_;
  out->font_epoch = font_epoch_;
  std::memset(out->font_sha256, 0, sizeof(out->font_sha256));
  out->accepting = accepting_;

  return kFlutterVenusV2Ok;
}

int32_t VenusTextLayoutService::ReloadFonts() {
  std::unique_lock<std::mutex> reconfigure(reconfigure_mutex_);  // (1)
  uint32_t saved_accepting = 0u;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (shutting_down_) {
      return kFlutterVenusV2ShuttingDown;
    }
    saved_accepting = accepting_;
    accepting_ = 0u;  // (2)
  }
  venus_text_layout::ParagraphStore::Instance().DiscardRegistrationArtifacts(
      engine_id_, registration_generation_);
  {
    std::unique_lock<std::mutex> lock(mutex_);  // (5)
    // leased_slots_ 必须在谓词里而不是只当断言：第 (8) 步重建 per-slot
    // FontCollection 需要「此刻没有任何持有者占着 slot」，而 active_slot_calls_
    // 只说明持有者不在调用内。同步 runner 收到 ShuttingDown 后自行归还。
    quiesce_cv_.wait(lock, [&] {
      return active_slot_calls_ == 0u && leased_slots_ == 0u;
    });
  }
  {
    std::unique_lock<std::mutex> lock(mutex_);
    // (8) 重建 snapshot 与全部 per-slot collection。此刻没有任何调用在使用它们。
    const std::shared_ptr<txt::FontCollection> base =
        fonts_ ? fonts_->GetFontCollection() : nullptr;
    for (Slot& slot : slots_) {
      uint64_t slot_font_epoch = 0u;
      slot.fonts = base ? base->CloneForVenusWorker(&slot_font_epoch) : nullptr;
    }
    font_epoch_ += 1u;  // (9)
    // 旧 paragraph 引用的是刚被换掉的那批 per-slot 字体门面, 整表作废。
    venus_text_layout::ParagraphStore::Instance().InvalidateForFontEpoch(
        engine_id_, registration_generation_, font_epoch_);
    accepting_ = saved_accepting;
  }
  return kFlutterVenusV2Ok;  // (11) reconfigure 在析构时释放
}

void VenusTextLayoutService::Shutdown() {
  std::unique_lock<std::mutex> reconfigure(reconfigure_mutex_);
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (shutting_down_) {
      return;
    }
    shutting_down_ = true;
    accepting_ = 0u;
  }
  // 先撤销代际，再等 in-flight/lease：迟到结果会在 Deposit 处 fail closed。
  venus_text_layout::ParagraphStore::Instance().DeactivateRegistration(
      engine_id_, registration_generation_);
  {
    std::unique_lock<std::mutex> lock(mutex_);
    // 与 ReloadFonts 同一条谓词：持有者必须先把自己的 lease 归还。
    quiesce_cv_.wait(lock, [&] {
      return active_slot_calls_ == 0u && leased_slots_ == 0u;
    });
  }
}

}  // namespace flutter
