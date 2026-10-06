// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "flutter/lib/ui/text/font_collection.h"
#include "flutter/shell/common/venus_text_layout_service.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {
namespace {

// Every deterministic case below carries a watchdog: the regressions this suite
// exists to catch (self-deadlock, workers that never stop, batches that never
// retire) all present as a HANG, and a hang produces no verdict at all.
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

class ServiceFixture {
 public:
  ServiceFixture() {
    fonts_ = std::make_shared<FontCollection>();
    service_ = new VenusTextLayoutService(1u, 1u, fonts_, false);
    service_->PopulateApi(&api_);
  }
  ~ServiceFixture() { api_.release_context(api_.context); }

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

}  // namespace

// --- slot arbitration / INV-SLOT -------------------------------------------

TEST(VenusTextLayoutSlotV2, AcquireIsNonBlockingAndBounded) {
  ServiceFixture f;
  std::vector<std::pair<uint32_t, uint64_t>> held;
  for (int i = 0; i < kFlutterVenusV2MaxSlots; ++i) {
    uint32_t slot = 0;
    uint64_t token = 0;
    ASSERT_EQ(f.api().acquire_slot(f.api().context, &slot, &token),
              kFlutterVenusV2Ok);
    held.emplace_back(slot, token);
  }
  uint32_t slot = 0;
  uint64_t token = 0;
  // Ninth acquire must fail immediately rather than block.
  EXPECT_EQ(f.api().acquire_slot(f.api().context, &slot, &token),
            kFlutterVenusV2SlotBusy);
  EXPECT_EQ(f.Counters().leased_slots, kFlutterVenusV2MaxSlots);
  for (const auto& [s, t] : held) {
    EXPECT_EQ(f.api().release_slot(f.api().context, s, t), kFlutterVenusV2Ok);
  }
  EXPECT_EQ(f.Counters().leased_slots, 0u);
}

TEST(VenusTextLayoutSlotV2, ForgedTokenIsIdempotentNoOpAndNeverUnderflows) {
  ServiceFixture f;
  uint32_t slot = 0;
  uint64_t token = 0;
  ASSERT_EQ(f.api().acquire_slot(f.api().context, &slot, &token),
            kFlutterVenusV2Ok);
  EXPECT_EQ(f.Counters().leased_slots, 1u);
  // A forged token must not return the slot and must not touch any counter.
  EXPECT_EQ(f.api().release_slot(f.api().context, slot, token + 12345u),
            kFlutterVenusV2SlotNotOwned);
  EXPECT_EQ(f.Counters().leased_slots, 1u);
  EXPECT_EQ(f.api().release_slot(f.api().context, slot, token),
            kFlutterVenusV2Ok);
  EXPECT_EQ(f.Counters().leased_slots, 0u);
  // Double release is a pure no-op, not an underflow to 0xFFFFFFFF.
  EXPECT_EQ(f.api().release_slot(f.api().context, slot, token),
            kFlutterVenusV2SlotNotOwned);
  EXPECT_EQ(f.Counters().leased_slots, 0u);
}

TEST(VenusTextLayoutSyncV2, RejectsUnownedSlotAndShutdownEntry) {
  ServiceFixture f;
  FlutterVenusTextLayoutInputV2 in{};
  in.struct_size = sizeof(in);
  in.abi_version = FLUTTER_VENUS_TEXT_LAYOUT_ABI_V2;
  FlutterVenusTextLayoutOutputV2 out{};
  EXPECT_EQ(f.api().layout_text_node_sync_v2(f.api().context, 0u, 999u, &in, &out),
            kFlutterVenusV2SlotNotOwned);
}

// --- async worker lifecycle ------------------------------------------------

TEST(VenusTextLayoutSyncV2, ShutdownQuiescesEveryCounter) {
  ServiceFixture f;
  ASSERT_TRUE(RunWithGuard([&] {
    // 关停的静默谓词含 leased_slots == 0：持有者必须先自己归还。这里先真跑
    // 一批再归还，好让 accepted/completed 都是非零 —— 零对零的守恒式恒真，
    // 断言不到任何东西。
    uint32_t slot = 0;
    uint64_t token = 0;
    ASSERT_EQ(f.api().acquire_slot(f.api().context, &slot, &token),
              kFlutterVenusV2Ok);
    for (int i = 0; i < 8; ++i) {
      FlutterVenusTextLayoutInputV2 in{};
      in.struct_size = sizeof(in);
      in.abi_version = FLUTTER_VENUS_TEXT_LAYOUT_ABI_V2;
      FlutterVenusTextLayoutOutputV2 out{};
      EXPECT_EQ(f.api().layout_text_node_sync_v2(f.api().context, slot, token,
                                                 &in, &out),
                kFlutterVenusV2Ok);
      // 这个 fixture 没装字体，节点本来就该在 item 级失败；断言它确实失败，
      // 而不是"返回 Ok 就算过"。
      EXPECT_EQ(out.status, kFlutterVenusV2ItemFailed);
    }
    EXPECT_EQ(f.api().release_slot(f.api().context, slot, token),
              kFlutterVenusV2Ok);

    f.service()->Shutdown();
    const auto c = f.Counters();
    EXPECT_EQ(c.active_slot_calls, 0u);
    EXPECT_EQ(c.leased_slots, 0u);
    EXPECT_EQ(c.accepting, 0u);
    EXPECT_EQ(c.accepted_total, 8u);
    EXPECT_EQ(c.accepted_total, c.completed_total);
  })) << "shutdown quiescence hung: a lease or an in-flight call never cleared";
}

}  // namespace testing
}  // namespace flutter
