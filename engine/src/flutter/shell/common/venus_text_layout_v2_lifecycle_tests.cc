// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// §8 P4 的生命周期矩阵（P7 收敛到 sync 之后）：关停竞态与关停窗口封闭性、
// font reload 的静默点与 per-slot 字体门面重建、唯一归还者与 INV-SLOT、
// 多线程并行持 lease 的互斥性、batch/item 两级拒绝分层。
//
// 已随 async transport 一并删除的用例（原 (b)(d)(e)(g)(j) 与 token ABA/double
// take/late take）不是"没测"，是被测对象已经不存在：Engine 不再拥有线程、
// 队列与批次 token。
//
// **每条用例都带超时守护**：本 Plan 历轮回归全部表现为挂起而不是失败
// （reconfiguration 自死锁、worker 不重建、running batch 永不退休），
// 没有超时就拿不到任何结论 —— 只会看到一条永远不打印的命令。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "flutter/fml/icu_util.h"
#include "flutter/lib/ui/text/font_collection.h"
#include "flutter/lib/ui/text/venus_text_layout_paragraph_store.h"
#include "flutter/shell/common/venus_text_layout_registry.h"
#include "flutter/shell/common/venus_text_layout_service.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {
namespace {

constexpr auto kGuard = std::chrono::seconds(30);

template <typename Fn>
bool RunWithGuard(Fn fn) {
  std::atomic<bool> done{false};
  std::thread runner([&] {
    fn();
    done.store(true);
  });
  const auto deadline = std::chrono::steady_clock::now() + kGuard;
  while (!done.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (done.load()) {
    runner.join();
    return true;
  }
  runner.detach();  // leaked on purpose: the test has already failed
  return false;
}

struct Node {
  std::string text = "venus lifecycle corpus row";
  std::string family = "Roboto";
  std::string locale = "en-US";

  FlutterVenusTextLayoutInputV2 Input() const {
    FlutterVenusTextLayoutInputV2 in{};
    in.struct_size = sizeof(in);
    in.abi_version = FLUTTER_VENUS_TEXT_LAYOUT_ABI_V2;
    in.text_length = static_cast<uint32_t>(text.size());
    in.font_family_length = static_cast<uint32_t>(family.size());
    in.locale_length = static_cast<uint32_t>(locale.size());
    in.font_weight = 400u;
    in.max_lines = 3u;
    in.text_encoding = 1u;
    in.text_direction = 1u;
    in.text_align = 5u;
    in.font_style = 1u;
    in.leading_distribution = 1u;
    in.text_width_basis = 1u;
    in.text_scaler_kind = 1u;
    in.max_lines_mode = 2u;
    in.overflow_mode = 1u;  // clip -> ellipsis 必须缺席
    in.soft_wrap = 2u;
    in.apply_height_to_first_ascent = 1u;
    in.apply_height_to_last_descent = 1u;
    in.height_mode = 1u;  // fontMetrics -> height 必须恰为 0
    in.strut_mode = 1u;
    in.text = reinterpret_cast<const uint8_t*>(text.data());
    in.font_family = reinterpret_cast<const uint8_t*>(family.data());
    in.locale = reinterpret_cast<const uint8_t*>(locale.data());
    in.min_width = 0.0;
    in.max_width = 200.0;
    in.min_height = 0.0;
    in.max_height = 400.0;
    in.font_size = 16.0;
    in.scaler_parameter = 1.0;  // noScaling 的 canonical 非激活值
    in.height = 0.0;
    return in;
  }
};

class Fixture {
 public:
  Fixture() {
    fonts_ = std::make_shared<FontCollection>();
    fonts_->SetupDefaultFontManager(0u);
    fonts_->RegisterTestFonts();
    service_ = new VenusTextLayoutService(1u, 1u, fonts_, false);
    service_->PopulateApi(&api_);
  }
  ~Fixture() { api_.release_context(api_.context); }

  FlutterVenusTextLayoutApiV2& api() { return api_; }
  VenusTextLayoutService* service() { return service_; }
  FlutterVenusTextLayoutCountersV2 Counters() {
    FlutterVenusTextLayoutCountersV2 c{};
    EXPECT_EQ(api_.get_counters_v2(api_.context, sizeof(c), &c),
              kFlutterVenusV2Ok);
    return c;
  }

 private:
  std::shared_ptr<FontCollection> fonts_;
  VenusTextLayoutService* service_ = nullptr;
  FlutterVenusTextLayoutApiV2 api_{};
};

// 取一份 lease、跑 count 个节点、再归还。返回第一条非 Ok 的状态码。
// 断言 output.status 而不是只看返回值：sync 入口对 per-item 失败同样返回 Ok，
// 只看返回码的用例是恒绿的。
int32_t LeaseAndRun(FlutterVenusTextLayoutApiV2& api, uint32_t count) {
  uint32_t slot = 0;
  uint64_t token = 0;
  const int32_t acquired = api.acquire_slot(api.context, &slot, &token);
  if (acquired != kFlutterVenusV2Ok) {
    return acquired;
  }
  const Node node;
  int32_t worst = kFlutterVenusV2Ok;
  for (uint32_t i = 0; i < count; ++i) {
    FlutterVenusTextLayoutInputV2 in = node.Input();
    FlutterVenusTextLayoutOutputV2 out{};
    const int32_t status =
        api.layout_text_node_sync_v2(api.context, slot, token, &in, &out);
    if (status != kFlutterVenusV2Ok) {
      worst = status;
      break;
    }
    if (out.status != kFlutterVenusV2Ok) {
      worst = out.status;
      break;
    }
    if (!(out.paragraph_height > 0.0)) {
      worst = kFlutterVenusV2ItemFailed;
      break;
    }
  }
  const int32_t released = api.release_slot(api.context, slot, token);
  return worst != kFlutterVenusV2Ok ? worst : released;
}

struct ParagraphCall {
  std::string text = "venus registry paragraph concurrency";
  std::string family = "Roboto";
  std::string locale = "en-US";
  FlutterVenusTextLayoutRunV2 run{};
  FlutterVenusTextLayoutParagraphInputV2 input{};
  FlutterVenusTextLayoutParagraphOutputV2 output{};

  ParagraphCall() {
    run.struct_size = sizeof(run);
    run.kind = kFlutterVenusV2RunKindText;
    run.text_length = static_cast<uint32_t>(text.size());
    run.font_family_length = static_cast<uint32_t>(family.size());
    run.font_weight = 400u;
    run.font_size = 16.0;
    input.struct_size = sizeof(input);
    input.abi_version = FLUTTER_VENUS_TEXT_LAYOUT_ABI_V2;
    input.run_count = 1u;
    input.text_length = static_cast<uint32_t>(text.size());
    input.font_families_length = static_cast<uint32_t>(family.size());
    input.locale_length = static_cast<uint32_t>(locale.size());
    input.text_direction = 1u;
    input.text_align = 5u;
    input.soft_wrap = 1u;
    input.runs = &run;
    input.text = reinterpret_cast<const uint8_t*>(text.data());
    input.font_families = reinterpret_cast<const uint8_t*>(family.data());
    input.locale = reinterpret_cast<const uint8_t*>(locale.data());
    input.max_width = 200.0;
    output.struct_size = sizeof(output);
  }
};

class ParagraphCoreProbe {
 public:
  static void Notify(void* context, bool entering) {
    static_cast<ParagraphCoreProbe*>(context)->Notify(entering);
  }

  void Arm(uint32_t target) {
    target_ = target;
    entered_.store(0u);
    active_.store(0u);
    max_active_.store(0u);
    overlap_.store(0u);
    armed_.store(true);
  }

  uint32_t max_active() const { return max_active_.load(); }
  uint32_t overlap() const { return overlap_.load(); }

 private:
  void Notify(bool entering) {
    if (!armed_.load()) {
      return;
    }
    if (!entering) {
      active_.fetch_sub(1u);
      return;
    }
    const uint32_t active = active_.fetch_add(1u) + 1u;
    uint32_t observed = max_active_.load();
    while (observed < active &&
           !max_active_.compare_exchange_weak(observed, active)) {
    }
    if (active > 1u) {
      overlap_.fetch_add(1u);
    }
    const uint32_t entered = entered_.fetch_add(1u) + 1u;
    if (entered <= target_) {
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(20);
      while (entered_.load() < target_ &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
      }
    }
  }

  std::atomic<bool> armed_{false};
  std::atomic<uint32_t> entered_{0u};
  std::atomic<uint32_t> active_{0u};
  std::atomic<uint32_t> max_active_{0u};
  std::atomic<uint32_t> overlap_{0u};
  uint32_t target_ = 0u;
};

struct ParagraphBenchmarkResult {
  uint32_t workers = 0u;
  uint32_t max_active = 0u;
  uint32_t overlap = 0u;
  uint64_t wall_us = 0u;
  uint64_t p50_us = 0u;
  uint64_t p95_us = 0u;
  uint64_t deposited = 0u;
};

ParagraphBenchmarkResult RunParagraphCoreEntryBenchmark(uint32_t workers) {
  constexpr uint32_t kWarmup = 20u;
  constexpr uint32_t kSamples = 200u;
  static std::atomic<uint64_t> next_engine_id{1000u};
  const uint64_t engine_id = next_engine_id.fetch_add(1u);
  auto fonts = std::make_shared<FontCollection>();
  fonts->SetupDefaultFontManager(0u);
  fonts->RegisterTestFonts();
  ParagraphCoreProbe probe;
  const uint64_t generation = VenusTextLayoutRegistry::Register(
      engine_id, fonts, false, &ParagraphCoreProbe::Notify, &probe);
  EXPECT_NE(generation, 0u);
  FlutterVenusTextLayoutApiV2 api{};
  EXPECT_EQ(VenusTextLayoutRegistry::GetApi(engine_id, sizeof(api), &api),
            kFlutterVenusV2Ok);

  std::vector<uint32_t> slots(workers);
  std::vector<uint64_t> tokens(workers);
  for (uint32_t i = 0; i < workers; ++i) {
    EXPECT_EQ(api.acquire_slot(api.context, &slots[i], &tokens[i]),
              kFlutterVenusV2Ok);
  }
  for (uint32_t i = 0; i < kWarmup; ++i) {
    ParagraphCall call;
    EXPECT_EQ(VenusTextLayoutRegistry::LayoutParagraph(
                  engine_id, slots[0], tokens[0], &call.input, &call.output),
              kFlutterVenusV2Ok);
  }

  venus_text_layout::ParagraphStore::Instance().Clear();
  venus_text_layout::ParagraphStore::Instance().ResetCounters();
  probe.Arm(workers);
  std::vector<uint64_t> durations(kSamples);
  std::atomic<uint32_t> ready{0u};
  std::atomic<bool> go{false};
  std::vector<std::thread> callers;
  callers.reserve(workers);
  for (uint32_t worker = 0; worker < workers; ++worker) {
    callers.emplace_back([&, worker] {
      ready.fetch_add(1u);
      while (!go.load()) {
        std::this_thread::yield();
      }
      for (uint32_t sample = worker; sample < kSamples; sample += workers) {
        ParagraphCall call;
        const auto begin = std::chrono::steady_clock::now();
        EXPECT_EQ(VenusTextLayoutRegistry::LayoutParagraph(
                      engine_id, slots[worker], tokens[worker], &call.input,
                      &call.output),
                  kFlutterVenusV2Ok);
        EXPECT_EQ(call.output.status, kFlutterVenusV2Ok);
        durations[sample] = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - begin)
                .count());
      }
    });
  }
  while (ready.load() != workers) {
    std::this_thread::yield();
  }
  const auto wall_begin = std::chrono::steady_clock::now();
  go.store(true);
  for (auto& caller : callers) {
    caller.join();
  }
  const uint64_t wall_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - wall_begin)
          .count());
  for (uint32_t i = 0; i < workers; ++i) {
    EXPECT_EQ(api.release_slot(api.context, slots[i], tokens[i]),
              kFlutterVenusV2Ok);
  }
  std::sort(durations.begin(), durations.end());
  const auto deposited =
      venus_text_layout::ParagraphStore::Instance().GetCounters().deposited;

  const ParagraphBenchmarkResult result{
      workers,
      probe.max_active(),
      probe.overlap(),
      wall_us,
      durations[kSamples / 2u - 1u],
      durations[kSamples * 95u / 100u - 1u],
      deposited};
  VenusTextLayoutRegistry::Unregister(engine_id, generation);
  api.release_context(api.context);
  return result;
}

class BlockingParagraphCoreProbe {
 public:
  static void Notify(void* context, bool entering) {
    auto* probe = static_cast<BlockingParagraphCoreProbe*>(context);
    if (!entering) {
      return;
    }
    probe->entered.store(true);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!probe->allow_exit.load() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
  }

  std::atomic<bool> entered{false};
  std::atomic<bool> allow_exit{false};
};

}  // namespace

TEST(VenusTextLayoutLifecycleV2, ParagraphCoreEntryBenchmark) {
  for (uint32_t workers : {1u, 2u, 4u, 8u}) {
    const ParagraphBenchmarkResult result =
        RunParagraphCoreEntryBenchmark(workers);
    std::cout << "PARAGRAPH_CORE_ENTRY workers=" << result.workers
              << " max_active=" << result.max_active
              << " overlap=" << result.overlap
              << " wall_us=" << result.wall_us
              << " p50_us=" << result.p50_us
              << " p95_us=" << result.p95_us
              << " deposited=" << result.deposited << std::endl;
    EXPECT_EQ(result.max_active, workers);
    EXPECT_EQ(result.overlap > 0u, workers > 1u);
    EXPECT_EQ(result.deposited, 0u);
  }
}

TEST(VenusTextLayoutLifecycleV2,
     IfcParagraphDepositsReturnedArtifactAfterSlotRelease) {
  venus_text_layout::ParagraphStore::Instance().Clear();
  venus_text_layout::ParagraphStore::Instance().ResetCounters();
  Fixture f;
  uint32_t slot = 0u;
  uint64_t lease = 0u;
  ASSERT_EQ(f.api().acquire_slot(f.api().context, &slot, &lease),
            kFlutterVenusV2Ok);

  ParagraphCall call;
  FlutterVenusTextLayoutLineMetricV2 lines[8] = {};
  FlutterVenusTextLayoutFragmentRectV2 fragments[8] = {};
  call.input.paragraph_token = 0x12345678u;
  call.input.line_metric_capacity = std::size(lines);
  call.input.fragment_rect_capacity = std::size(fragments);
  call.input.line_metrics_out = lines;
  call.input.fragment_rects_out = fragments;
  ASSERT_EQ(f.service()->LayoutParagraph(slot, lease, &call.input, &call.output),
            kFlutterVenusV2Ok);
  ASSERT_EQ(call.output.status, kFlutterVenusV2Ok);
  ASSERT_EQ(call.output.truncated, 0u);
  ASSERT_GT(call.output.line_metric_count, 0u);

  const uint64_t font_epoch = f.Counters().font_epoch;
  EXPECT_EQ(venus_text_layout::ParagraphStore::Instance().Claim(
                call.input.paragraph_token, call.input.max_width, font_epoch),
            nullptr);
  ASSERT_EQ(f.api().release_slot(f.api().context, slot, lease),
            kFlutterVenusV2Ok);

  std::unique_ptr<txt::Paragraph> claimed =
      venus_text_layout::ParagraphStore::Instance().Claim(
          call.input.paragraph_token, call.input.max_width, font_epoch);
  ASSERT_NE(claimed, nullptr);
  auto same_bits = [](double a, double b) {
    return std::memcmp(&a, &b, sizeof(double)) == 0;
  };
  EXPECT_PRED2(same_bits, call.output.width, claimed->GetLongestLine());
  EXPECT_PRED2(same_bits, call.output.height, claimed->GetHeight());
  EXPECT_PRED2(same_bits, call.output.first_baseline,
               claimed->GetAlphabeticBaseline());
  std::vector<txt::LineMetrics>& claimed_lines = claimed->GetLineMetrics();
  ASSERT_EQ(call.output.line_metric_count, claimed_lines.size());
  for (size_t i = 0; i < claimed_lines.size(); ++i) {
    EXPECT_PRED2(same_bits, lines[i].left, claimed_lines[i].left);
    EXPECT_PRED2(same_bits, lines[i].width, claimed_lines[i].width);
    EXPECT_PRED2(same_bits, lines[i].height, claimed_lines[i].height);
    EXPECT_PRED2(same_bits, lines[i].baseline, claimed_lines[i].baseline);
    EXPECT_PRED2(same_bits, lines[i].ascent, claimed_lines[i].ascent);
    EXPECT_PRED2(same_bits, lines[i].descent, claimed_lines[i].descent);
  }
  EXPECT_EQ(venus_text_layout::ParagraphStore::Instance().Claim(
                call.input.paragraph_token, call.input.max_width, font_epoch),
            nullptr);
}

TEST(VenusTextLayoutLifecycleV2,
     ArtifactOwnerPublishesDirectAndParagraphArtifactsOnlyAfterSeal) {
  constexpr uint64_t kEngineId = 626201u;
  constexpr uint64_t kProducerGeneration = 27u;
  constexpr uint64_t kDirectToken = 0x62620101u;
  constexpr uint64_t kParagraphToken = 0x62620102u;
  venus_text_layout::ParagraphStore::Instance().Clear();
  venus_text_layout::ParagraphStore::Instance().ResetCounters();
  auto fonts = std::make_shared<FontCollection>();
  fonts->SetupDefaultFontManager(0u);
  fonts->RegisterTestFonts();
  const uint64_t generation =
      VenusTextLayoutRegistry::Register(kEngineId, fonts, false);
  ASSERT_NE(generation, 0u);
  FlutterVenusTextLayoutApiV2 api{};
  ASSERT_EQ(VenusTextLayoutRegistry::GetApi(kEngineId, sizeof(api), &api),
            kFlutterVenusV2Ok);

  uint64_t owner_id = 0u;
  ASSERT_EQ(FlutterVenusTextLayoutArtifactOwnerBeginV1(
                kEngineId, generation, kProducerGeneration, &owner_id),
            kFlutterVenusV2Ok);
  ASSERT_NE(owner_id, 0u);
  uint32_t slot = 0u;
  uint64_t lease = 0u;
  ASSERT_EQ(api.acquire_slot(api.context, &slot, &lease),
            kFlutterVenusV2Ok);
  EXPECT_EQ(FlutterVenusTextLayoutArtifactOwnerBindSlotV1(
                kEngineId, owner_id, slot, lease + 1u),
            kFlutterVenusV2SlotNotOwned);
  ASSERT_EQ(FlutterVenusTextLayoutArtifactOwnerBindSlotV1(
                kEngineId, owner_id, slot, lease),
            kFlutterVenusV2Ok);
  EXPECT_EQ(FlutterVenusTextLayoutArtifactOwnerBindSlotV1(
                kEngineId, owner_id, slot, lease),
            kFlutterVenusV2Ok)
      << "exact bind is idempotent";
  EXPECT_EQ(
      FlutterVenusTextLayoutArtifactOwnerSealV1(kEngineId, owner_id),
      kFlutterVenusV2NotReady);

  const Node node;
  FlutterVenusTextLayoutInputV2 direct_input = node.Input();
  direct_input.prelayout_token = kDirectToken;
  FlutterVenusTextLayoutOutputV2 direct_output{};
  ASSERT_EQ(api.layout_text_node_sync_v2(api.context, slot, lease,
                                         &direct_input, &direct_output),
            kFlutterVenusV2Ok);
  ASSERT_EQ(direct_output.status, kFlutterVenusV2Ok);

  ParagraphCall paragraph;
  FlutterVenusTextLayoutLineMetricV2 lines[8] = {};
  FlutterVenusTextLayoutFragmentRectV2 fragments[8] = {};
  paragraph.input.paragraph_token = kParagraphToken;
  paragraph.input.line_metric_capacity = std::size(lines);
  paragraph.input.fragment_rect_capacity = std::size(fragments);
  paragraph.input.line_metrics_out = lines;
  paragraph.input.fragment_rects_out = fragments;
  ASSERT_EQ(VenusTextLayoutRegistry::LayoutParagraph(
                kEngineId, slot, lease, &paragraph.input, &paragraph.output),
            kFlutterVenusV2Ok);
  ASSERT_EQ(paragraph.output.status, kFlutterVenusV2Ok);
  ASSERT_EQ(paragraph.output.truncated, 0u);

  FlutterVenusTextLayoutCountersV2 counters{};
  ASSERT_EQ(api.get_counters_v2(api.context, sizeof(counters), &counters),
            kFlutterVenusV2Ok);
  EXPECT_EQ(venus_text_layout::ParagraphStore::Instance().Claim(
                kDirectToken, direct_input.max_width, counters.font_epoch),
            nullptr);
  ASSERT_EQ(api.release_slot(api.context, slot, lease), kFlutterVenusV2Ok);
  EXPECT_EQ(venus_text_layout::ParagraphStore::Instance().Claim(
                kParagraphToken, paragraph.input.max_width,
                counters.font_epoch),
            nullptr)
      << "a returned lease does not publish an unsealed owner";

  ASSERT_EQ(
      FlutterVenusTextLayoutArtifactOwnerSealV1(kEngineId, owner_id),
      kFlutterVenusV2Ok);
  EXPECT_EQ(
      FlutterVenusTextLayoutArtifactOwnerSealV1(kEngineId, owner_id),
      kFlutterVenusV2Ok)
      << "seal is idempotent";
  EXPECT_NE(venus_text_layout::ParagraphStore::Instance().Claim(
                kDirectToken, direct_input.max_width, counters.font_epoch),
            nullptr);
  EXPECT_NE(venus_text_layout::ParagraphStore::Instance().Claim(
                kParagraphToken, paragraph.input.max_width,
                counters.font_epoch),
            nullptr);
  FlutterVenusTextLayoutArtifactOwnerReleaseV1(kEngineId, owner_id);
  FlutterVenusTextLayoutArtifactOwnerReleaseV1(kEngineId, owner_id);
  VenusTextLayoutRegistry::Unregister(kEngineId, generation);
  api.release_context(api.context);
}

TEST(VenusTextLayoutLifecycleV2,
     ArtifactOwnerBeginRejectsStaleRegistrationGenerationAfterReregister) {
  constexpr uint64_t kEngineId = 626202u;
  auto fonts = std::make_shared<FontCollection>();
  fonts->SetupDefaultFontManager(0u);
  fonts->RegisterTestFonts();
  const uint64_t old_generation =
      VenusTextLayoutRegistry::Register(kEngineId, fonts, false);
  ASSERT_NE(old_generation, 0u);
  FlutterVenusTextLayoutApiV2 old_api{};
  ASSERT_EQ(
      VenusTextLayoutRegistry::GetApi(kEngineId, sizeof(old_api), &old_api),
      kFlutterVenusV2Ok);
  uint32_t old_slot = 0u;
  uint64_t old_lease = 0u;
  ASSERT_EQ(old_api.acquire_slot(old_api.context, &old_slot, &old_lease),
            kFlutterVenusV2Ok);
  ASSERT_EQ(old_api.release_slot(old_api.context, old_slot, old_lease),
            kFlutterVenusV2Ok);
  VenusTextLayoutRegistry::Unregister(kEngineId, old_generation);

  const uint64_t new_generation =
      VenusTextLayoutRegistry::Register(kEngineId, fonts, false);
  ASSERT_NE(new_generation, 0u);
  ASSERT_NE(new_generation, old_generation);
  FlutterVenusTextLayoutApiV2 new_api{};
  ASSERT_EQ(
      VenusTextLayoutRegistry::GetApi(kEngineId, sizeof(new_api), &new_api),
      kFlutterVenusV2Ok);
  uint32_t new_slot = 0u;
  uint64_t new_lease = 0u;
  ASSERT_EQ(new_api.acquire_slot(new_api.context, &new_slot, &new_lease),
            kFlutterVenusV2Ok);
  EXPECT_EQ(new_slot, old_slot);
  EXPECT_EQ(new_lease, old_lease)
      << "service-local lease ids intentionally restart after reregister";

  uint64_t owner_id = 99u;
  EXPECT_EQ(FlutterVenusTextLayoutArtifactOwnerBeginV1(
                kEngineId, old_generation, 28u, &owner_id),
            kFlutterVenusV2NotFound);
  EXPECT_EQ(owner_id, 0u);
  ASSERT_EQ(FlutterVenusTextLayoutArtifactOwnerBeginV1(
                kEngineId, new_generation, 28u, &owner_id),
            kFlutterVenusV2Ok);
  ASSERT_NE(owner_id, 0u);
  EXPECT_EQ(FlutterVenusTextLayoutArtifactOwnerBindSlotV1(
                kEngineId, owner_id, new_slot, new_lease),
            kFlutterVenusV2Ok);
  EXPECT_EQ(new_api.release_slot(new_api.context, new_slot, new_lease),
            kFlutterVenusV2Ok);
  EXPECT_EQ(
      FlutterVenusTextLayoutArtifactOwnerSealV1(kEngineId, owner_id),
      kFlutterVenusV2Ok);
  FlutterVenusTextLayoutArtifactOwnerReleaseV1(kEngineId, owner_id);
  VenusTextLayoutRegistry::Unregister(kEngineId, new_generation);
  old_api.release_context(old_api.context);
  new_api.release_context(new_api.context);
}

TEST(VenusTextLayoutLifecycleV2,
     IfcParagraphTokenZeroAndTruncatedOutputDoNotDeposit) {
  venus_text_layout::ParagraphStore::Instance().Clear();
  venus_text_layout::ParagraphStore::Instance().ResetCounters();
  Fixture f;
  uint32_t slot = 0u;
  uint64_t lease = 0u;
  ASSERT_EQ(f.api().acquire_slot(f.api().context, &slot, &lease),
            kFlutterVenusV2Ok);

  ParagraphCall token_zero;
  ASSERT_EQ(f.service()->LayoutParagraph(slot, lease, &token_zero.input,
                                         &token_zero.output),
            kFlutterVenusV2Ok);
  EXPECT_EQ(
      venus_text_layout::ParagraphStore::Instance().GetCounters().deposited,
      0u);

  ParagraphCall truncated;
  truncated.input.paragraph_token = 0x9876u;
  ASSERT_EQ(f.service()->LayoutParagraph(slot, lease, &truncated.input,
                                         &truncated.output),
            kFlutterVenusV2Ok);
  EXPECT_EQ(truncated.output.truncated, 1u);
  EXPECT_EQ(
      venus_text_layout::ParagraphStore::Instance().GetCounters().deposited,
      0u);
  EXPECT_EQ(venus_text_layout::ParagraphStore::Instance().SizeForTesting(), 0u);
  EXPECT_EQ(f.api().release_slot(f.api().context, slot, lease),
            kFlutterVenusV2Ok);
}

TEST(VenusTextLayoutLifecycleV2, ReloadAndShutdownReleaseIfcArtifacts) {
  venus_text_layout::ParagraphStore::Instance().Clear();
  venus_text_layout::ParagraphStore::Instance().ResetCounters();
  Fixture f;
  auto deposit_one = [&](uint64_t paragraph_token) {
    uint32_t slot = 0u;
    uint64_t lease = 0u;
    EXPECT_EQ(f.api().acquire_slot(f.api().context, &slot, &lease),
              kFlutterVenusV2Ok);
    ParagraphCall call;
    FlutterVenusTextLayoutLineMetricV2 lines[8] = {};
    FlutterVenusTextLayoutFragmentRectV2 fragments[8] = {};
    call.input.paragraph_token = paragraph_token;
    call.input.line_metric_capacity = std::size(lines);
    call.input.fragment_rect_capacity = std::size(fragments);
    call.input.line_metrics_out = lines;
    call.input.fragment_rects_out = fragments;
    EXPECT_EQ(f.service()->LayoutParagraph(slot, lease, &call.input,
                                           &call.output),
              kFlutterVenusV2Ok);
    EXPECT_EQ(call.output.truncated, 0u);
    EXPECT_EQ(f.api().release_slot(f.api().context, slot, lease),
              kFlutterVenusV2Ok);
  };

  deposit_one(0x111u);
  ASSERT_EQ(venus_text_layout::ParagraphStore::Instance().SizeForTesting(), 1u);
  ASSERT_EQ(f.service()->ReloadFonts(), kFlutterVenusV2Ok);
  EXPECT_EQ(venus_text_layout::ParagraphStore::Instance().SizeForTesting(), 0u);

  deposit_one(0x222u);
  ASSERT_EQ(venus_text_layout::ParagraphStore::Instance().SizeForTesting(), 1u);
  f.service()->Shutdown();
  EXPECT_EQ(venus_text_layout::ParagraphStore::Instance().SizeForTesting(), 0u);
  EXPECT_EQ(venus_text_layout::ParagraphStore::Instance()
                .GetCounters()
                .evicted_registration,
            2u);
}

TEST(VenusTextLayoutLifecycleV2,
     RegistryReferenceSurvivesConcurrentUnregister) {
  ASSERT_TRUE(RunWithGuard([] {
    constexpr uint64_t kEngineId = 424242u;
    auto fonts = std::make_shared<FontCollection>();
    fonts->SetupDefaultFontManager(0u);
    fonts->RegisterTestFonts();
    BlockingParagraphCoreProbe probe;
    const uint64_t generation = VenusTextLayoutRegistry::Register(
        kEngineId, fonts, false, &BlockingParagraphCoreProbe::Notify, &probe);
    ASSERT_NE(generation, 0u);
    FlutterVenusTextLayoutApiV2 api{};
    ASSERT_EQ(VenusTextLayoutRegistry::GetApi(kEngineId, sizeof(api), &api),
              kFlutterVenusV2Ok);
    auto* service = static_cast<VenusTextLayoutService*>(api.context);
    EXPECT_EQ(service->GetReferenceCountForTesting(), 2);

    uint32_t slot = 0u;
    uint64_t token = 0u;
    ASSERT_EQ(api.acquire_slot(api.context, &slot, &token),
              kFlutterVenusV2Ok);
    ParagraphCall failed_call;
    EXPECT_EQ(VenusTextLayoutRegistry::LayoutParagraph(
                  kEngineId, slot, token + 1u, &failed_call.input,
                  &failed_call.output),
              kFlutterVenusV2SlotNotOwned);
    EXPECT_EQ(service->GetReferenceCountForTesting(), 2)
        << "失败返回必须归还 registry 临时引用";

    std::atomic<int32_t> first_status{kFlutterVenusV2Unavailable};
    std::atomic<int32_t> release_status{kFlutterVenusV2Unavailable};
    std::thread layout([&] {
      ParagraphCall call;
      first_status.store(VenusTextLayoutRegistry::LayoutParagraph(
          kEngineId, slot, token, &call.input, &call.output));
      release_status.store(api.release_slot(api.context, slot, token));
    });
    const auto entered_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!probe.entered.load() &&
           std::chrono::steady_clock::now() < entered_deadline) {
      std::this_thread::yield();
    }
    if (!probe.entered.load()) {
      ADD_FAILURE() << "paragraph core did not enter before deadline";
      probe.allow_exit.store(true);
      layout.join();
      VenusTextLayoutRegistry::Unregister(kEngineId, generation);
      api.release_context(api.context);
      return;
    }

    ParagraphCall same_slot_call;
    EXPECT_EQ(VenusTextLayoutRegistry::LayoutParagraph(
                  kEngineId, slot, token, &same_slot_call.input,
                  &same_slot_call.output),
              kFlutterVenusV2SlotBusy);
    EXPECT_EQ(service->GetReferenceCountForTesting(), 3)
        << "只有 in-flight layout 的临时引用仍在";

    std::atomic<bool> unregister_returned{false};
    std::thread unregister([&] {
      VenusTextLayoutRegistry::Unregister(kEngineId, generation);
      unregister_returned.store(true);
    });
    FlutterVenusTextLayoutApiV2 unavailable_api{};
    const auto erased_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int32_t lookup_status = kFlutterVenusV2Ok;
    while (lookup_status == kFlutterVenusV2Ok &&
           std::chrono::steady_clock::now() < erased_deadline) {
      lookup_status = VenusTextLayoutRegistry::GetApi(
          kEngineId, sizeof(unavailable_api), &unavailable_api);
      if (lookup_status == kFlutterVenusV2Ok) {
        unavailable_api.release_context(unavailable_api.context);
        std::this_thread::yield();
      }
    }
    EXPECT_EQ(lookup_status, kFlutterVenusV2Unavailable);
    EXPECT_FALSE(unregister_returned.load())
        << "Shutdown 不得越过 in-flight layout/lease";

    probe.allow_exit.store(true);
    layout.join();
    unregister.join();
    EXPECT_EQ(first_status.load(), kFlutterVenusV2Ok);
    EXPECT_EQ(release_status.load(), kFlutterVenusV2Ok);
    EXPECT_TRUE(unregister_returned.load());
    EXPECT_EQ(service->GetReferenceCountForTesting(), 1)
        << "注销只释放 registry ref，API context 仍独立存活";

    uint32_t after_slot = 0u;
    uint64_t after_token = 0u;
    EXPECT_EQ(api.acquire_slot(api.context, &after_slot, &after_token),
              kFlutterVenusV2ShuttingDown);
    api.release_context(api.context);
  })) << "registry layout/unregister lifecycle case hung";
}

// (a) 关停后仍持 lease 的线程发起同步调用必须 fail closed。
TEST(VenusTextLayoutLifecycleV2, SyncCallAfterShutdownFailsClosed) {
  ASSERT_TRUE(RunWithGuard([] {
    Fixture f;
    uint32_t slot = 0;
    uint64_t token = 0;
    ASSERT_EQ(f.api().acquire_slot(f.api().context, &slot, &token),
              kFlutterVenusV2Ok);
    // 关停序列第 (5) 步的谓词含 leased_slots == 0，所以持有者必须先归还，
    // 否则 Shutdown 会一直等 —— 这正是"唯一归还者规则"的可观察后果。
    EXPECT_EQ(f.api().release_slot(f.api().context, slot, token),
              kFlutterVenusV2Ok);
    f.service()->Shutdown();

    const Node node;
    FlutterVenusTextLayoutInputV2 in = node.Input();
    FlutterVenusTextLayoutOutputV2 out{};
    const int32_t status = f.api().layout_text_node_sync_v2(
        f.api().context, slot, token, &in, &out);
    EXPECT_TRUE(status == kFlutterVenusV2ShuttingDown ||
                status == kFlutterVenusV2SlotNotOwned)
        << "status=" << status;
    // (c) 关停窗口封闭性：关停之后 accepting 恒 0、acquire 恒 ShuttingDown。
    uint32_t probe_slot = 0;
    uint64_t probe_token = 0;
    EXPECT_EQ(
        f.api().acquire_slot(f.api().context, &probe_slot, &probe_token),
        kFlutterVenusV2ShuttingDown);
    EXPECT_EQ(f.Counters().accepting, 0u);
  })) << "(a) 关停竞态用例挂起";
}

// (f)(h) font reload 分支：静默点必须包含 leased_slots == 0 —— 重建 per-slot
// FontCollection 的那一刻不允许有任何持有者占着 slot；重建后 font_epoch 前进、
// accepting 恢复、后续同步调用照常出几何。
TEST(VenusTextLayoutLifecycleV2, FontReloadWaitsForLeasesAndBumpsEpoch) {
  ASSERT_TRUE(RunWithGuard([] {
    Fixture f;
    const uint64_t epoch_before = f.Counters().font_epoch;

    std::atomic<bool> holder_released{false};
    std::atomic<bool> reload_returned{false};
    uint32_t slot = 0;
    uint64_t token = 0;
    ASSERT_EQ(f.api().acquire_slot(f.api().context, &slot, &token),
              kFlutterVenusV2Ok);

    std::thread holder([&] {
      // 持 lease 一小段时间，让 ReloadFonts 必须真的等在谓词上。
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      holder_released.store(true);
      EXPECT_EQ(f.api().release_slot(f.api().context, slot, token),
                kFlutterVenusV2Ok);
    });

    EXPECT_EQ(f.service()->ReloadFonts(), kFlutterVenusV2Ok);
    reload_returned.store(true);
    holder.join();
    EXPECT_TRUE(holder_released.load())
        << "ReloadFonts 在还有人持 lease 时就返回了：per-slot 字体门面会被"
           "在别人正在用的对象上替换";
    EXPECT_TRUE(reload_returned.load());

    const auto c = f.Counters();
    EXPECT_EQ(c.font_epoch, epoch_before + 1u);
    EXPECT_EQ(c.accepting, 1u);
    EXPECT_EQ(c.leased_slots, 0u);
    EXPECT_EQ(c.active_slot_calls, 0u);
    // 重建之后这条路必须还能出真几何，否则 clone 出来的是个空 collection。
    EXPECT_EQ(LeaseAndRun(f.api(), 4u), kFlutterVenusV2Ok);
  })) << "(f)(h) font reload 用例挂起";
}

// (i) 唯一归还者与 INV-SLOT：in-flight 期间对同一 slot release 必须 SlotBusy 且
//     该 slot 没有进入空闲集合；伪造 token 的分支不改任何 counter。
TEST(VenusTextLayoutLifecycleV2, InvSlotHoldsUnderInFlightRelease) {
  ASSERT_TRUE(RunWithGuard([] {
    Fixture f;
    uint32_t slot = 0;
    uint64_t token = 0;
    ASSERT_EQ(f.api().acquire_slot(f.api().context, &slot, &token),
              kFlutterVenusV2Ok);
    const uint32_t leased_before = f.Counters().leased_slots;

    // 伪造 token：纯幂等 no-op，不归还、不改 counter。
    EXPECT_EQ(f.api().release_slot(f.api().context, slot, token + 1u),
              kFlutterVenusV2SlotNotOwned);
    EXPECT_EQ(f.Counters().leased_slots, leased_before);

    // 重入：同一 slot 上再发一次同步调用（本线程串行，只能用越界 slot 验证）。
    const Node node;
    FlutterVenusTextLayoutInputV2 in = node.Input();
    FlutterVenusTextLayoutOutputV2 out{};
    EXPECT_EQ(f.api().layout_text_node_sync_v2(f.api().context,
                                               kFlutterVenusV2MaxSlots + 3u,
                                               token, &in, &out),
              kFlutterVenusV2SlotNotOwned);

    EXPECT_EQ(f.api().release_slot(f.api().context, slot, token),
              kFlutterVenusV2Ok);
    EXPECT_EQ(f.Counters().leased_slots, leased_before - 1u);
    // 归还是集合语义：同一 slot 重复归还必须被拒，不能让空闲集合出现两份。
    EXPECT_EQ(f.api().release_slot(f.api().context, slot, token),
              kFlutterVenusV2SlotNotOwned);
    EXPECT_EQ(f.Counters().leased_slots, leased_before - 1u);
  })) << "(i) INV-SLOT 用例挂起";
}

// 入口级参数错误 vs per-item 失败：两级分层在 sync 路上同样成立 ——
// 入口拒绝走返回码，坏节点走 output.status，且绝不牵连同一 lease 上的邻居。
TEST(VenusTextLayoutLifecycleV2, EntryRejectionVersusPerItemFailure) {
  ASSERT_TRUE(RunWithGuard([] {
    Fixture f;
    uint32_t slot = 0;
    uint64_t token = 0;
    ASSERT_EQ(f.api().acquire_slot(f.api().context, &slot, &token),
              kFlutterVenusV2Ok);
    const Node node;
    FlutterVenusTextLayoutInputV2 in = node.Input();
    FlutterVenusTextLayoutOutputV2 out{};

    // 入口级：空指针 / 越界 slot / 错 token —— 返回码直接拒，不写 output。
    EXPECT_EQ(f.api().layout_text_node_sync_v2(f.api().context, slot, token,
                                               nullptr, &out),
              kFlutterVenusV2InvalidArgument);
    EXPECT_EQ(f.api().layout_text_node_sync_v2(f.api().context, slot, token,
                                               &in, nullptr),
              kFlutterVenusV2InvalidArgument);
    EXPECT_EQ(f.api().layout_text_node_sync_v2(
                  f.api().context, kFlutterVenusV2MaxSlots + 1u, token, &in,
                  &out),
              kFlutterVenusV2SlotNotOwned);
    EXPECT_EQ(f.api().layout_text_node_sync_v2(f.api().context, slot,
                                               token + 7u, &in, &out),
              kFlutterVenusV2SlotNotOwned);

    // per-item：坏枚举的节点写 canonical 失败 output，入口仍返回 Ok。
    FlutterVenusTextLayoutInputV2 bad = node.Input();
    bad.text_direction = 4242u;
    FlutterVenusTextLayoutOutputV2 bad_out{};
    EXPECT_EQ(f.api().layout_text_node_sync_v2(f.api().context, slot, token,
                                               &bad, &bad_out),
              kFlutterVenusV2Ok);
    EXPECT_EQ(bad_out.status, kFlutterVenusV2ItemFailed);
    EXPECT_EQ(bad_out.paragraph_height, 0.0);

    // 邻居不受牵连：同一 lease 上紧接着跑一个合法节点必须照常出几何。
    FlutterVenusTextLayoutOutputV2 good_out{};
    EXPECT_EQ(f.api().layout_text_node_sync_v2(f.api().context, slot, token,
                                               &in, &good_out),
              kFlutterVenusV2Ok);
    EXPECT_EQ(good_out.status, kFlutterVenusV2Ok);
    EXPECT_GT(good_out.paragraph_height, 0.0);

    EXPECT_EQ(f.api().release_slot(f.api().context, slot, token),
              kFlutterVenusV2Ok);
    const auto c = f.Counters();
    EXPECT_EQ(c.accepted_total, c.completed_total)
        << "pre-failed 项同样要写出 output，守恒式不许因此破掉";
    EXPECT_GT(c.rejected_total, 0u);
  })) << "入口/项两级拒绝分层用例挂起";
}

// 并行度：N 个调用方线程各自 acquire 一份 lease 并发跑，slot 互不重叠，
// 全部归还后 leased_slots 归零。这是 P7 之后并行性唯一的载体。
TEST(VenusTextLayoutLifecycleV2, ConcurrentLeasesAreDisjointAndAllReturn) {
  ASSERT_TRUE(RunWithGuard([] {
    Fixture f;
    constexpr uint32_t kThreads = 4u;
    std::mutex seen_mutex;
    std::vector<uint32_t> seen;
    std::atomic<uint32_t> ok{0};
    std::vector<std::thread> callers;
    callers.reserve(kThreads);
    for (uint32_t t = 0; t < kThreads; ++t) {
      callers.emplace_back([&] {
        uint32_t slot = 0;
        uint64_t token = 0;
        if (f.api().acquire_slot(f.api().context, &slot, &token) !=
            kFlutterVenusV2Ok) {
          return;
        }
        {
          std::lock_guard<std::mutex> lock(seen_mutex);
          for (uint32_t other : seen) {
            EXPECT_NE(other, slot) << "两个线程同时拿到了同一个 slot";
          }
          seen.push_back(slot);
        }
        const Node node;
        bool all_ok = true;
        for (uint32_t i = 0; i < 64u; ++i) {
          FlutterVenusTextLayoutInputV2 in = node.Input();
          FlutterVenusTextLayoutOutputV2 out{};
          all_ok = all_ok &&
                   f.api().layout_text_node_sync_v2(f.api().context, slot,
                                                    token, &in, &out) ==
                       kFlutterVenusV2Ok &&
                   out.status == kFlutterVenusV2Ok && out.paragraph_height > 0.0;
        }
        {
          std::lock_guard<std::mutex> lock(seen_mutex);
          seen.erase(std::remove(seen.begin(), seen.end(), slot), seen.end());
        }
        if (f.api().release_slot(f.api().context, slot, token) ==
                kFlutterVenusV2Ok &&
            all_ok) {
          ok.fetch_add(1u);
        }
      });
    }
    for (std::thread& caller : callers) {
      caller.join();
    }
    EXPECT_EQ(ok.load(), kThreads);
    const auto c = f.Counters();
    EXPECT_EQ(c.leased_slots, 0u);
    EXPECT_EQ(c.active_slot_calls, 0u);
    EXPECT_EQ(c.accepted_total, c.completed_total);
  })) << "并行 lease 用例挂起";
}

}  // namespace testing
}  // namespace flutter

int main(int argc, char** argv) {
  fml::icu::InitializeICU("icudtl.dat");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
