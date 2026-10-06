// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_LIB_UI_TEXT_VENUS_TEXT_LAYOUT_PARAGRAPH_STORE_H_
#define FLUTTER_LIB_UI_TEXT_VENUS_TEXT_LAYOUT_PARAGRAPH_STORE_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "txt/paragraph.h"

namespace flutter {
namespace venus_text_layout {

// Phase 3 的交付表：worker 线程把**排好版的** txt::Paragraph 存进来，UI 线程认领。
//
// 为什么需要它：Phase 2 的 core 本来就在 Build + Layout 一个真正的
// txt::Paragraph，抽完九个几何量就把它析构掉；随后 Flutter 在 UI 线程上把同一件
// 事再做一遍。这张表就是"别扔"的那个落脚点 —— 总工作量没变，变的是它发生在
// 哪条线程、什么时刻。
//
// ## 三条设计约束（都不是可选项）
//
// 1. **认领即移交所有权**：Claim 成功后表里不再持有那份 paragraph。它是个
//    unique_ptr，不存在"两个人都以为自己拥有"的状态。认领方（Dart 侧的
//    ui.Paragraph）此后按 Dart GC 的生命周期管理它。
//
// 2. **键必须含 width 与 font_epoch**：
//    - width：同一段文字在不同约束宽度下断行不同，拿错宽度的那份等于画错内容；
//    - font_epoch：字体重载之后旧 paragraph 引用的是已被换掉的字体表。
//    两者任一对不上都必须判未命中，而不是"凑合用"。
//
// 3. **未命中是常态，不是异常**：真实页面的文字与约束宽度变化频繁。所有未命中
//    分类计数（见 Counters），因为**静默回退等于没做这个 Feature** —— 耗时数字
//    会好看，因为没人知道它其实一次都没命中。
//
// ## 线程
//
// 多个 worker 存、UI 线程取，全部入口上同一把锁。锁只护这张表自身的结构，
// 不护 paragraph 内容 —— paragraph 本身的跨线程安全由 P3-0 单独证过
// （venus_text_layout_p3_paragraph_handoff_tests.cc：真并发 10/10，
// 以及"共用字体表 → double-free"这条反向对照）。
// 环境条子（ambient note）。
//
// Venus 的文本节点走 Flutter 的 TextPainter，而它内部自己造 ui.Paragraph ——
// 那个对象在私有类 _TextPainterLayoutCacheWithOffset 上够不到，正式 SDK 又钉死
// 禁改。所以只能"把条子贴在当前线程上，让排版自己来取"。
//
// 抽成独立单元而不是埋在 Paragraph::layout 里，是为了能直接测 ——
// flutter::Paragraph 是 DartWrappable，构造要 Dart_Handle，在 host 单测里搭不起来。
// 逻辑埋在那里面就等于测不到，而这块的失效方式（过期条子被别人捡走）恰恰是
// 静默画错内容。
class AmbientPrelayoutNote {
 public:
  // 贴。`width_bits` 是宽度的位模式（整数比对，不走 double 语义）。
  static void Post(uint64_t token,
                   uint64_t font_epoch,
                   uint64_t width_bits,
                   uint64_t version);

  // 撕。只撕自己贴的那张：版本号对不上说明已经被别人换掉了，撕它等于撕别人的。
  static void Clear(uint64_t version);

  static uint64_t Version();

  // 取。宽度位模式相符才给，且**取走即失效**。
  // 返回 false 表示没有可取的条子（没贴 / 宽度不符 / 已被取走）。
  static bool TryTake(uint64_t width_bits,
                      uint64_t* out_token,
                      uint64_t* out_font_epoch);

  // 单测用：把当前线程的条子清空到初始态。
  static void ResetForTesting();
};

class ParagraphStore {
 public:
  // 未命中的分类。分类而不是一个总数：合并成一个数就分不清"没人提前排"
  // 和"排了但宽度对不上"，而这两者的处置完全不同。
  struct Counters {
    uint64_t deposited = 0;            // 存入总数
    uint64_t claimed = 0;              // 认领成功
    uint64_t miss_not_prewarmed = 0;   // 这个 token 从没被存过
    uint64_t miss_width_mismatch = 0;  // token 在，但排的是别的宽度
    uint64_t miss_font_epoch = 0;      // token 在，但字体已经换代
    uint64_t evicted_font_epoch = 0;   // 字体换代时整表作废掉的条数
    uint64_t evicted_capacity = 0;     // 超出容量上限被挤掉的条数
    uint64_t dropped_on_deposit = 0;   // 存入时同 token 已有旧值, 旧值被丢弃
    uint64_t miss_slot_busy = 0;       // 排它的那个 slot 还没交还, 本次不交付
    uint64_t released_by_slot = 0;     // 因 slot 交还而转为可认领的条数
    uint64_t miss_registration_generation = 0;  // artifact 所属 Engine 已换代
    uint64_t evicted_registration = 0;          // reload/shutdown/替换注册释放
    uint64_t evicted_cancelled = 0;             // 暂停接单期归还 slot 时释放
    uint64_t dropped_stale_registration = 0;    // 迟到结果属于旧注册代际
  };

  // 容量上限。交付表存的是排好版的 paragraph，不是几个数字 —— 无上限增长会把
  // "省下的 shaping 时间"换成"涨上去的常驻内存"，那不叫赢。
  // 超出后按插入顺序丢最旧的。
  static constexpr size_t kMaxEntries = 512;

  static ParagraphStore& Instance();

  void ActivateRegistration(uint64_t engine_id,
                            uint64_t registration_generation);

  int32_t BeginArtifactOwner(uint64_t engine_id,
                             uint64_t registration_generation,
                             uint64_t producer_generation,
                             uint64_t* out_owner_id);
  int32_t BindArtifactOwner(uint64_t engine_id,
                            uint64_t registration_generation,
                            uint64_t owner_id,
                            bool add_slot_binding = true);
  int32_t SealArtifactOwner(uint64_t engine_id,
                            uint64_t registration_generation,
                            uint64_t owner_id);
  void ReleaseArtifactOwner(uint64_t engine_id, uint64_t owner_id);

  // worker 线程调用。同一 token 已有旧值时，旧值被丢弃并计数 ——
  // 静默覆盖会让"为什么没命中"变成无从查起。
  //
  // `slot` 是排它的那个 slot。存入时该条处于 **pending**：在这个 slot 交还之前
  // 不可认领。理由见下面 NotifySlotReleased。
  int32_t Deposit(uint64_t token,
                  double width,
                  uint64_t font_epoch,
                  uint64_t engine_id,
                  uint64_t registration_generation,
                  uint64_t snapshot_generation,
                  uint32_t slot,
                  std::unique_ptr<txt::Paragraph> paragraph,
                  uint64_t artifact_owner_id = 0u,
                  uint64_t input_charge_bytes = 0u);

  // UI 线程调用。命中则移交所有权并从表中移除；未命中返回 nullptr 并记下分类。
  std::unique_ptr<txt::Paragraph> Claim(uint64_t token,
                                        double width,
                                        uint64_t font_epoch);

  // slot 交还时调用，把该 slot 名下所有 pending 条目转为可认领。
  //
  // ## 为什么要有这条规则
  //
  // paragraph 记得建它的那张字体表。UI 线程画它时若回头查了字体，而同一个
  // worker 正拿同一张表排下一段 —— 两个线程同时对一张"查不到就填"的哈希表操作，
  // 扩容时旧数组被释放两次，就是 double-free。
  //
  // P3-0 实测 Paint 不回头碰字体表（真并发 10/10，3000 画 vs 80~88 排），
  // 但那是**证据不是保证**：它依赖上游 SkParagraph 的内部实现。上游哪天给 Paint
  // 加个 fallback 兜底，这条就悄悄破了，而且破的时候是崩溃不是编译错误。
  //
  // 所以这里把它变成**结构保证**：一张字体表在任何时刻只有一个使用者 ——
  // 要么那个 worker 正在用（此时该 slot 的条目不可认领），要么已经交还、表闲置。
  //
  // 这是**交付时机规则，不是互斥锁**：slot 没还就不交付，还了就随便画。
  // UI 线程不会被 worker 阻塞。
  void NotifySlotReleased(uint64_t engine_id,
                          uint64_t registration_generation,
                          uint32_t slot,
                          bool make_claimable,
                          uint64_t artifact_owner_id = 0u);

  // 字体重载时整表作废：旧 paragraph 引用的字体表已经被换掉。
  void InvalidateForFontEpoch(uint64_t engine_id,
                              uint64_t registration_generation,
                              uint64_t new_font_epoch);

  void DiscardRegistrationArtifacts(uint64_t engine_id,
                                    uint64_t registration_generation);
  void DeactivateRegistration(uint64_t engine_id,
                              uint64_t registration_generation);

  // 页面销毁 / 热重启。
  void Clear();

  Counters GetCounters() const;
  void ResetCounters();
  size_t SizeForTesting() const;

 private:
  ParagraphStore() = default;

  struct Entry {
    double width = 0.0;
    uint64_t font_epoch = 0;
    uint64_t engine_id = 0;
    uint64_t registration_generation = 0;
    // Paragraph V2 尚无 snapshot generation；内部字段固定为 0，不能从无关
    // counter 猜值。唯一 V2 契约显式提供后再参与 Claim 校验。
    uint64_t snapshot_generation = 0;
    uint64_t sequence = 0;  // 插入序，用于容量淘汰
    uint64_t artifact_owner_id = 0u;
    uint64_t input_charge_bytes = 0u;
    uint32_t slot = 0;      // 排它的 slot
    bool claimable = false; // slot 交还前为 false
    std::unique_ptr<txt::Paragraph> paragraph;
  };

  enum class OwnerState { kOpen, kSealed, kFailed };

  struct ArtifactOwner {
    uint64_t engine_id = 0u;
    uint64_t registration_generation = 0u;
    uint64_t producer_generation = 0u;
    uint64_t input_charge_bytes = 0u;
    uint32_t entry_count = 0u;
    uint32_t bound_slots = 0u;
    OwnerState state = OwnerState::kOpen;
  };

  void RemoveEntryChargeLocked(const Entry& entry);
  std::unique_ptr<txt::Paragraph> EvictOldestLocked();

  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, Entry> entries_;
  std::unordered_map<uint64_t, ArtifactOwner> artifact_owners_;
  std::unordered_map<uint64_t, uint64_t> active_registrations_;
  uint64_t next_artifact_owner_id_ = 1u;
  uint64_t next_sequence_ = 1;
  uint64_t owned_input_charge_bytes_ = 0u;
  uint32_t owned_entry_count_ = 0u;
  size_t legacy_entry_count_ = 0u;
  Counters counters_;
};

}  // namespace venus_text_layout
}  // namespace flutter

#endif  // FLUTTER_LIB_UI_TEXT_VENUS_TEXT_LAYOUT_PARAGRAPH_STORE_H_
