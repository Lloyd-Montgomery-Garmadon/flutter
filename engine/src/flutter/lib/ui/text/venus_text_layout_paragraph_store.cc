// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/text/venus_text_layout_paragraph_store.h"

// nogncheck: 这是一个**无依赖的纯 C 公共头** (只有 typedef 与函数声明)。
// 正经做法是让 //flutter/lib/ui 依赖 :common_cpp_library_headers, 但那个
// source_set 会顺带把 desktop_library_implementation 配置加到整个 lib/ui 上,
// 为了一个头文件改掉一整层的编译配置, 代价比收益大。
//
// nogncheck 只关掉 gn 的依赖检查, **include 照常发生** —— 本 TU 里的定义与
// 公共头里的声明不符仍然是编译错误, 签名漂移防线没有丢。
#include "flutter/shell/platform/common/public/flutter_venus_text_layout.h"  // nogncheck

#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace flutter {
namespace venus_text_layout {
namespace {

// width 用**逐 bit** 比，不用 ==。
//
// 理由不是洁癖：约束宽度是从 Dart 一路传下来的 double，中间经过 wire 编码与
// 解码。用 == 的话 -0.0 会和 0.0 判等，而这两者在断行上不是一回事；NaN 则会
// 判不等于自己，导致同一份预排永远认领不回来且计数落在 width_mismatch 上，
// 看着像"宽度总在变"，实际是比较写错了。
bool SameWidthBits(double a, double b) {
  return std::memcmp(&a, &b, sizeof(double)) == 0;
}

}  // namespace

namespace {
struct AmbientState {
  uint64_t token = 0u;
  uint64_t font_epoch = 0u;
  uint64_t width_bits = 0u;
  uint64_t version = 0u;
};
// 只在贴它的那条线程上有效。用 TLS 而不是全局：后台 isolate 同样会走到排版，
// 全局会让两条线程互相取走对方的条子。
thread_local AmbientState g_ambient;
}  // namespace

void AmbientPrelayoutNote::Post(uint64_t token,
                                uint64_t font_epoch,
                                uint64_t width_bits,
                                uint64_t version) {
  g_ambient.token = token;
  g_ambient.font_epoch = font_epoch;
  g_ambient.width_bits = width_bits;
  g_ambient.version = version;
}

void AmbientPrelayoutNote::Clear(uint64_t version) {
  if (g_ambient.version != version) {
    // 已经被别人换掉了。撕它等于撕别人的那张。
    return;
  }
  g_ambient.token = 0u;
}

uint64_t AmbientPrelayoutNote::Version() {
  return g_ambient.version;
}

bool AmbientPrelayoutNote::TryTake(uint64_t width_bits,
                                   uint64_t* out_token,
                                   uint64_t* out_font_epoch) {
  if (g_ambient.token == 0u || g_ambient.width_bits != width_bits) {
    return false;
  }
  *out_token = g_ambient.token;
  *out_font_epoch = g_ambient.font_epoch;
  g_ambient.token = 0u;  // 取走即失效
  return true;
}

void AmbientPrelayoutNote::ResetForTesting() {
  g_ambient = AmbientState{};
}

ParagraphStore& ParagraphStore::Instance() {
  static ParagraphStore* instance = new ParagraphStore();
  return *instance;
}

void ParagraphStore::ActivateRegistration(uint64_t engine_id,
                                          uint64_t registration_generation) {
  if (engine_id == 0u || registration_generation == 0u) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  active_registrations_[engine_id] = registration_generation;
}

int32_t ParagraphStore::BeginArtifactOwner(
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
  std::lock_guard<std::mutex> lock(mutex_);
  const auto active = active_registrations_.find(engine_id);
  if (active == active_registrations_.end() ||
      active->second != registration_generation) {
    return kFlutterVenusV2NotFound;
  }
  if (artifact_owners_.size() >=
      kFlutterVenusTextLayoutArtifactOwnerV1MaxConcurrentOwners) {
    return kFlutterVenusV2Backpressure;
  }
  uint64_t owner_id = next_artifact_owner_id_++;
  while (owner_id == 0u || artifact_owners_.count(owner_id) != 0u) {
    owner_id = next_artifact_owner_id_++;
  }
  ArtifactOwner owner;
  owner.engine_id = engine_id;
  owner.registration_generation = registration_generation;
  owner.producer_generation = producer_generation;
  artifact_owners_.emplace(owner_id, owner);
  *out_owner_id = owner_id;
  return kFlutterVenusV2Ok;
}

int32_t ParagraphStore::BindArtifactOwner(
    uint64_t engine_id,
    uint64_t registration_generation,
    uint64_t owner_id,
    bool add_slot_binding) {
  if (engine_id == 0u || registration_generation == 0u || owner_id == 0u) {
    return kFlutterVenusV2InvalidArgument;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto active = active_registrations_.find(engine_id);
  if (active == active_registrations_.end() ||
      active->second != registration_generation) {
    return kFlutterVenusV2NotFound;
  }
  const auto found = artifact_owners_.find(owner_id);
  if (found == artifact_owners_.end() || found->second.engine_id != engine_id ||
      found->second.registration_generation != registration_generation) {
    return kFlutterVenusV2NotFound;
  }
  if (found->second.state == OwnerState::kFailed) {
    return kFlutterVenusV2ItemFailed;
  }
  if (found->second.state != OwnerState::kOpen) {
    return kFlutterVenusV2NotReady;
  }
  if (add_slot_binding) {
    found->second.bound_slots += 1u;
  }
  return kFlutterVenusV2Ok;
}

int32_t ParagraphStore::SealArtifactOwner(
    uint64_t engine_id,
    uint64_t registration_generation,
    uint64_t owner_id) {
  if (engine_id == 0u || registration_generation == 0u || owner_id == 0u) {
    return kFlutterVenusV2InvalidArgument;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto active = active_registrations_.find(engine_id);
  if (active == active_registrations_.end() ||
      active->second != registration_generation) {
    return kFlutterVenusV2NotFound;
  }
  const auto found = artifact_owners_.find(owner_id);
  if (found == artifact_owners_.end() || found->second.engine_id != engine_id ||
      found->second.registration_generation != registration_generation) {
    return kFlutterVenusV2NotFound;
  }
  if (found->second.state == OwnerState::kFailed) {
    return kFlutterVenusV2ItemFailed;
  }
  if (found->second.state == OwnerState::kSealed) {
    return kFlutterVenusV2Ok;
  }
  if (found->second.bound_slots != 0u) {
    return kFlutterVenusV2NotReady;
  }
  found->second.state = OwnerState::kSealed;
  return kFlutterVenusV2Ok;
}

void ParagraphStore::ReleaseArtifactOwner(uint64_t engine_id,
                                          uint64_t owner_id) {
  std::vector<std::unique_ptr<txt::Paragraph>> released;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto owner = artifact_owners_.find(owner_id);
    if (owner == artifact_owners_.end() ||
        owner->second.engine_id != engine_id) {
      return;
    }
    released.reserve(owner->second.entry_count);
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.artifact_owner_id != owner_id) {
        ++it;
        continue;
      }
      RemoveEntryChargeLocked(it->second);
      released.push_back(std::move(it->second.paragraph));
      it = entries_.erase(it);
    }
    artifact_owners_.erase(owner);
  }
  // Paragraph destruction can release a large native graph. Keep it outside
  // the store lock so unrelated Claim/Deposit calls are not serialized on it.
}

int32_t ParagraphStore::Deposit(uint64_t token,
                                double width,
                                uint64_t font_epoch,
                                uint64_t engine_id,
                                uint64_t registration_generation,
                                uint64_t snapshot_generation,
                                uint32_t slot,
                                std::unique_ptr<txt::Paragraph> paragraph,
                                uint64_t artifact_owner_id,
                                uint64_t input_charge_bytes) {
  if (token == 0u || paragraph == nullptr || engine_id == 0u ||
      registration_generation == 0u) {
    return kFlutterVenusV2InvalidArgument;
  }
  std::unique_ptr<txt::Paragraph> displaced;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto active = active_registrations_.find(engine_id);
  if (active == active_registrations_.end() ||
      active->second != registration_generation) {
    counters_.dropped_stale_registration += 1;
    const auto owner = artifact_owners_.find(artifact_owner_id);
    if (owner != artifact_owners_.end()) {
      owner->second.state = OwnerState::kFailed;
    }
    return artifact_owner_id == 0u ? kFlutterVenusV2Ok
                                   : kFlutterVenusV2NotFound;
  }

  ArtifactOwner* owner = nullptr;
  if (artifact_owner_id != 0u) {
    const auto found = artifact_owners_.find(artifact_owner_id);
    if (found == artifact_owners_.end() ||
        found->second.engine_id != engine_id ||
        found->second.registration_generation != registration_generation) {
      return kFlutterVenusV2NotFound;
    }
    owner = &found->second;
    if (owner->state == OwnerState::kFailed) {
      return kFlutterVenusV2ItemFailed;
    }
    if (owner->state != OwnerState::kOpen) {
      return kFlutterVenusV2NotReady;
    }
    if (input_charge_bytes == 0u) {
      owner->state = OwnerState::kFailed;
      return kFlutterVenusV2InvalidArgument;
    }
  }

  auto existing = entries_.find(token);
  if (existing != entries_.end()) {
    if (artifact_owner_id != 0u ||
        existing->second.artifact_owner_id != 0u) {
      // Owned token is a version identity. Any duplicate (including one from
      // legacy owner 0) fails closed and never overwrites the first artifact.
      if (owner != nullptr) {
        owner->state = OwnerState::kFailed;
      }
      return kFlutterVenusV2ItemFailed;
    }
    // owner 0 keeps the legacy replace-in-place behavior.
    counters_.dropped_on_deposit += 1u;
    displaced = std::move(existing->second.paragraph);
    RemoveEntryChargeLocked(existing->second);
    entries_.erase(existing);
  }

  if (owner != nullptr) {
    const uint64_t owner_bytes_limit =
        kFlutterVenusTextLayoutArtifactOwnerV1MaxInputChargeBytesPerOwner;
    const uint64_t process_bytes_limit =
        kFlutterVenusTextLayoutArtifactOwnerV1MaxInputChargeBytesPerProcess;
    if (owner->entry_count >=
            kFlutterVenusTextLayoutArtifactOwnerV1MaxEntriesPerOwner ||
        owned_entry_count_ >=
            kFlutterVenusTextLayoutArtifactOwnerV1MaxEntriesPerProcess ||
        input_charge_bytes > owner_bytes_limit - owner->input_charge_bytes ||
        input_charge_bytes >
            process_bytes_limit - owned_input_charge_bytes_) {
      owner->state = OwnerState::kFailed;
      return kFlutterVenusV2Backpressure;
    }
    owner->entry_count += 1u;
    owner->input_charge_bytes += input_charge_bytes;
    owned_entry_count_ += 1u;
    owned_input_charge_bytes_ += input_charge_bytes;
  } else {
    if (legacy_entry_count_ >= kMaxEntries) {
      displaced = EvictOldestLocked();
    }
    legacy_entry_count_ += 1u;
  }

  Entry entry;
  entry.width = width;
  entry.font_epoch = font_epoch;
  entry.engine_id = engine_id;
  entry.registration_generation = registration_generation;
  entry.snapshot_generation = snapshot_generation;
  entry.sequence = next_sequence_++;
  entry.artifact_owner_id = artifact_owner_id;
  entry.input_charge_bytes = input_charge_bytes;
  entry.slot = slot;
  entry.claimable = false;  // 等它那个 slot 交还
  entry.paragraph = std::move(paragraph);
  entries_.emplace(token, std::move(entry));
  counters_.deposited += 1;
  return kFlutterVenusV2Ok;
}

std::unique_ptr<txt::Paragraph> ParagraphStore::Claim(uint64_t token,
                                                      double width,
                                                      uint64_t font_epoch) {
  std::unique_ptr<txt::Paragraph> discarded;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(token);
  if (it == entries_.end()) {
    counters_.miss_not_prewarmed += 1;
    return nullptr;
  }
  const auto active = active_registrations_.find(it->second.engine_id);
  if (active == active_registrations_.end() ||
      active->second != it->second.registration_generation) {
    counters_.miss_registration_generation += 1;
    discarded = std::move(it->second.paragraph);
    RemoveEntryChargeLocked(it->second);
    entries_.erase(it);
    return nullptr;
  }
  if (it->second.artifact_owner_id != 0u) {
    const auto owner = artifact_owners_.find(it->second.artifact_owner_id);
    if (owner == artifact_owners_.end() ||
        owner->second.engine_id != it->second.engine_id ||
        owner->second.registration_generation !=
            it->second.registration_generation) {
      counters_.miss_registration_generation += 1;
      discarded = std::move(it->second.paragraph);
      RemoveEntryChargeLocked(it->second);
      entries_.erase(it);
      return nullptr;
    }
    if (owner->second.state != OwnerState::kSealed) {
      // Open/failed owners are not published deliveries. Keep their entries
      // until the producer either seals successfully or releases the owner.
      counters_.miss_slot_busy += 1;
      return nullptr;
    }
  }
  // 分类的顺序是有意的：先判字体换代，再判宽度。
  // 字体换代时整条已经不可用，此时若报成 width_mismatch，读数据的人会以为
  // 是"宽度老在变"，去优化一个不存在的问题。
  if (it->second.font_epoch != font_epoch) {
    counters_.miss_font_epoch += 1;
    discarded = std::move(it->second.paragraph);
    RemoveEntryChargeLocked(it->second);
    entries_.erase(it);  // 已经不可能再命中，顺手清掉
    return nullptr;
  }
  if (!it->second.claimable) {
    // 排它的那个 slot 还没交还 —— 那张字体表此刻仍有使用者。
    // 不删：slot 一交还它就可认领了。
    counters_.miss_slot_busy += 1;
    return nullptr;
  }
  if (!SameWidthBits(it->second.width, width)) {
    counters_.miss_width_mismatch += 1;
    // **不删**：同一个 token 下一帧可能又用回原来的宽度（比如来回滚动、
    // 或者容器宽度在两个值之间抖动）。删掉等于把下一次命中也扔了。
    return nullptr;
  }
  std::unique_ptr<txt::Paragraph> paragraph = std::move(it->second.paragraph);
  RemoveEntryChargeLocked(it->second);
  entries_.erase(it);  // 认领即移交所有权，表里不再持有
  counters_.claimed += 1;
  return paragraph;
}

void ParagraphStore::NotifySlotReleased(uint64_t engine_id,
                                        uint64_t registration_generation,
                                        uint32_t slot,
                                        bool make_claimable,
                                        uint64_t artifact_owner_id) {
  std::vector<std::unique_ptr<txt::Paragraph>> cancelled_paragraphs;
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t released = 0;
  uint64_t cancelled = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    Entry& entry = it->second;
    if (entry.engine_id != engine_id ||
        entry.registration_generation != registration_generation ||
        entry.slot != slot ||
        entry.artifact_owner_id != artifact_owner_id || entry.claimable) {
      ++it;
      continue;
    }
    if (make_claimable) {
      entry.claimable = true;
      released += 1;
      ++it;
    } else {
      RemoveEntryChargeLocked(entry);
      cancelled_paragraphs.push_back(std::move(entry.paragraph));
      it = entries_.erase(it);
      cancelled += 1;
    }
  }
  if (artifact_owner_id != 0u) {
    const auto owner = artifact_owners_.find(artifact_owner_id);
    if (owner != artifact_owners_.end() &&
        owner->second.engine_id == engine_id &&
        owner->second.registration_generation == registration_generation) {
      if (owner->second.bound_slots > 0u) {
        owner->second.bound_slots -= 1u;
      }
      if (!make_claimable) {
        owner->second.state = OwnerState::kFailed;
      }
    }
  }
  counters_.released_by_slot += released;
  counters_.evicted_cancelled += cancelled;
}

void ParagraphStore::InvalidateForFontEpoch(uint64_t engine_id,
                                            uint64_t registration_generation,
                                            uint64_t new_font_epoch) {
  std::vector<std::unique_ptr<txt::Paragraph>> removed_paragraphs;
  size_t removed = 0u;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& owner : artifact_owners_) {
      if (owner.second.engine_id == engine_id &&
          owner.second.registration_generation == registration_generation) {
        owner.second.state = OwnerState::kFailed;
      }
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.engine_id == engine_id &&
          it->second.registration_generation == registration_generation &&
          it->second.font_epoch != new_font_epoch) {
        RemoveEntryChargeLocked(it->second);
        removed_paragraphs.push_back(std::move(it->second.paragraph));
        it = entries_.erase(it);
        removed += 1u;
      } else {
        ++it;
      }
    }
    counters_.evicted_font_epoch += removed;
  }
}

void ParagraphStore::DiscardRegistrationArtifacts(
    uint64_t engine_id,
    uint64_t registration_generation) {
  std::vector<std::unique_ptr<txt::Paragraph>> removed_paragraphs;
  uint64_t removed = 0u;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& owner : artifact_owners_) {
      if (owner.second.engine_id == engine_id &&
          owner.second.registration_generation == registration_generation) {
        owner.second.state = OwnerState::kFailed;
      }
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.engine_id == engine_id &&
          it->second.registration_generation == registration_generation) {
        RemoveEntryChargeLocked(it->second);
        removed_paragraphs.push_back(std::move(it->second.paragraph));
        it = entries_.erase(it);
        removed += 1u;
      } else {
        ++it;
      }
    }
    counters_.evicted_registration += removed;
  }
}

void ParagraphStore::DeactivateRegistration(
    uint64_t engine_id,
    uint64_t registration_generation) {
  std::vector<std::unique_ptr<txt::Paragraph>> removed_paragraphs;
  uint64_t removed = 0u;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto active = active_registrations_.find(engine_id);
    if (active != active_registrations_.end() &&
        active->second == registration_generation) {
      active_registrations_.erase(active);
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.engine_id == engine_id &&
          it->second.registration_generation == registration_generation) {
        RemoveEntryChargeLocked(it->second);
        removed_paragraphs.push_back(std::move(it->second.paragraph));
        it = entries_.erase(it);
        removed += 1u;
      } else {
        ++it;
      }
    }
    for (auto it = artifact_owners_.begin(); it != artifact_owners_.end();) {
      if (it->second.engine_id == engine_id &&
          it->second.registration_generation == registration_generation) {
        it = artifact_owners_.erase(it);
      } else {
        ++it;
      }
    }
    counters_.evicted_registration += removed;
  }
}

void ParagraphStore::Clear() {
  std::unordered_map<uint64_t, Entry> removed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    removed.swap(entries_);
    artifact_owners_.clear();
    owned_input_charge_bytes_ = 0u;
    owned_entry_count_ = 0u;
    legacy_entry_count_ = 0u;
  }
}

ParagraphStore::Counters ParagraphStore::GetCounters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counters_;
}

void ParagraphStore::ResetCounters() {
  std::lock_guard<std::mutex> lock(mutex_);
  counters_ = Counters{};
}

size_t ParagraphStore::SizeForTesting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

void ParagraphStore::RemoveEntryChargeLocked(const Entry& entry) {
  if (entry.artifact_owner_id == 0u) {
    if (legacy_entry_count_ > 0u) {
      legacy_entry_count_ -= 1u;
    }
    return;
  }
  if (owned_entry_count_ > 0u) {
    owned_entry_count_ -= 1u;
  }
  owned_input_charge_bytes_ =
      entry.input_charge_bytes <= owned_input_charge_bytes_
          ? owned_input_charge_bytes_ - entry.input_charge_bytes
          : 0u;
  const auto owner = artifact_owners_.find(entry.artifact_owner_id);
  if (owner == artifact_owners_.end()) {
    return;
  }
  if (owner->second.entry_count > 0u) {
    owner->second.entry_count -= 1u;
  }
  owner->second.input_charge_bytes =
      entry.input_charge_bytes <= owner->second.input_charge_bytes
          ? owner->second.input_charge_bytes - entry.input_charge_bytes
          : 0u;
}

std::unique_ptr<txt::Paragraph> ParagraphStore::EvictOldestLocked() {
  auto oldest = entries_.end();
  uint64_t oldest_sequence = UINT64_MAX;
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->second.artifact_owner_id == 0u &&
        it->second.sequence < oldest_sequence) {
      oldest_sequence = it->second.sequence;
      oldest = it;
    }
  }
  if (oldest != entries_.end()) {
    std::unique_ptr<txt::Paragraph> paragraph =
        std::move(oldest->second.paragraph);
    RemoveEntryChargeLocked(oldest->second);
    entries_.erase(oldest);
    counters_.evicted_capacity += 1;
    return paragraph;
  }
  return nullptr;
}

}  // namespace venus_text_layout
}  // namespace flutter

// 导出的 C 入口。放在这里而不是 registry.cc: 它们只包这张条子, 和 wave
// 派发那条路没关系; 放这儿 host 单测才够得着(测试目标只依赖 //flutter/lib/ui)。
// 签名漂移由本 TU include 公共头来兜。
extern "C" FLUTTER_EXPORT uint64_t
FlutterVenusTextLayoutPostPrelayoutNoteV2(uint64_t token,
                                          uint64_t font_epoch,
                                          double width) {
  if (token == 0u || !std::isfinite(width)) {
    // 宽度无限 -> 不贴。见头文件：layoutTemplate 恒以 infinity 排版，
    // 贴了就会被它取走。返回 0 让 Clear 成为空操作。
    return 0u;
  }
  uint64_t width_bits = 0u;
  std::memcpy(&width_bits, &width, sizeof(width_bits));
  const uint64_t version =
      flutter::venus_text_layout::AmbientPrelayoutNote::Version() + 1u;
  flutter::venus_text_layout::AmbientPrelayoutNote::Post(token, font_epoch,
                                                         width_bits, version);
  return version;
}

extern "C" FLUTTER_EXPORT void FlutterVenusTextLayoutClearPrelayoutNoteV2(
    uint64_t note_version) {
  // 不必单独挡 note_version == 0（"没贴成"的返回值）：那说明这条线程上要么
  // 从没贴过（token 本来就是 0），要么现存的是**别人**贴的、版本号非 0 —— 两种
  // 情况下面的版本比对都不会动它。这里曾经有一道 `if (note_version == 0) return;`,
  // 变异测试证明删掉它没有任何用例变红, 就是死代码。
  flutter::venus_text_layout::AmbientPrelayoutNote::Clear(note_version);
}

extern "C" FLUTTER_EXPORT uint32_t
FlutterVenusTextLayoutCopyPrelayoutStoreCountersV2(uint64_t* out_counters,
                                                   uint32_t capacity) {
  const flutter::venus_text_layout::ParagraphStore::Counters c =
      flutter::venus_text_layout::ParagraphStore::Instance().GetCounters();
  const uint64_t values[] = {
      c.deposited,        c.claimed,           c.miss_not_prewarmed,
      c.miss_width_mismatch, c.miss_font_epoch, c.miss_slot_busy,
      c.released_by_slot, c.evicted_font_epoch, c.evicted_capacity,
      c.dropped_on_deposit,
  };
  const uint32_t available = static_cast<uint32_t>(std::size(values));
  if (out_counters == nullptr || capacity == 0u) {
    return available;
  }
  const uint32_t written = capacity < available ? capacity : available;
  std::memcpy(out_counters, values, written * sizeof(uint64_t));
  return written;
}
