// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Focused concurrency suite for the V2 text layout service.
//
// This is a separate executable from ui_unittests on purpose: the §11 TSan gate
// needs a binary small enough to reach gtest main under the thread sanitizer,
// and the deterministic unit suite that lives in
// lib/ui/text/venus_text_layout_service_unittests.cc is not the same thing as a
// race hunt. The cases here deliberately run many threads against one service.
//
// Every case carries a watchdog. The regressions this suite exists to catch
// (self-deadlock, workers that never stop, batches that never retire) all
// present as a HANG, and a hang produces no verdict at all.

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "flutter/fml/icu_util.h"
#include "flutter/lib/ui/text/font_collection.h"
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

class ServiceFixture {
 public:
  ServiceFixture() {
    fonts_ = std::make_shared<FontCollection>();
    // Without a font manager the shaper returns a null paragraph and every item
    // fails with kParagraphNull -- a "green" concurrency suite that lays out
    // nothing. The clone in the service copies whatever managers exist at
    // construction time, so this has to happen before the service is built.
    fonts_->SetupDefaultFontManager(0u);
    fonts_->RegisterTestFonts();
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

// One valid single-node input. The strings are owned by the caller and the
// service keeps zero references to them once the call returns.
struct Node {
  std::string text = "venus parallel text layout";
  std::string family = "Roboto";
  // locale is REQUIRED by DecodeAndValidate (kLocaleRef / kEmptyRequired);
  // omitting it makes every item fail while the call itself still returns Ok,
  // which is exactly how a concurrency test can look green while computing
  // nothing at all.
  std::string locale = "en-US";

  FlutterVenusTextLayoutInputV2 Input(bool item_timing) const {
    FlutterVenusTextLayoutInputV2 input{};
    input.struct_size = sizeof(FlutterVenusTextLayoutInputV2);
    input.abi_version = FLUTTER_VENUS_TEXT_LAYOUT_ABI_V2;
    input.flags =
        item_timing ? static_cast<uint32_t>(kFlutterVenusV2InputFlagItemTiming)
                    : 0u;
    input.text_length = static_cast<uint32_t>(text.size());
    input.font_family_length = static_cast<uint32_t>(family.size());
    input.locale_length = static_cast<uint32_t>(locale.size());
    input.font_weight = 400u;
    input.text_encoding = 1u;   // utf8
    input.text_direction = 1u;  // ltr
    input.text_align = 5u;      // start
    input.font_style = 1u;      // normal
    input.leading_distribution = 1u;
    input.text_width_basis = 1u;
    input.text_scaler_kind = 1u;
    input.max_lines_mode = 1u;  // unlimited
    input.overflow_mode = 1u;   // clip
    input.soft_wrap = 2u;       // true
    input.apply_height_to_first_ascent = 1u;
    input.apply_height_to_last_descent = 1u;
    input.height_mode = 1u;  // font metrics
    input.strut_mode = 1u;   // disabled
    input.text = reinterpret_cast<const uint8_t*>(text.data());
    input.font_family = reinterpret_cast<const uint8_t*>(family.data());
    input.locale = reinterpret_cast<const uint8_t*>(locale.data());
    input.min_width = 0.0;
    input.max_width = 200.0;
    input.min_height = 0.0;
    input.max_height = 400.0;
    input.font_size = 16.0;
    // Canonical inactive value for kNoScaling is exactly 1.0 (not 0.0); the
    // frozen truth table rejects anything else.
    input.scaler_parameter = 1.0;
    return input;
  }
};

}  // namespace

// --- sync: many threads, one slot each, all at once ------------------------

TEST(VenusTextLayoutSyncV2, ConcurrentRunnersShareOneArbiter) {
  ASSERT_TRUE(RunWithGuard([] {
    ServiceFixture f;
    constexpr int kRunners = kFlutterVenusV2MaxSlots;
    constexpr int kCallsPerRunner = 64;
    std::atomic<int> ok_calls{0};
    std::atomic<int> starved{0};
    std::vector<std::thread> runners;
    for (int i = 0; i < kRunners; ++i) {
      runners.emplace_back([&] {
        uint32_t slot = 0;
        uint64_t token = 0;
        if (f.api().acquire_slot(f.api().context, &slot, &token) !=
            kFlutterVenusV2Ok) {
          starved.fetch_add(1);
          return;
        }
        const Node node;
        for (int call = 0; call < kCallsPerRunner; ++call) {
          FlutterVenusTextLayoutInputV2 input = node.Input(true);
          FlutterVenusTextLayoutOutputV2 output{};
          if (f.api().layout_text_node_sync_v2(f.api().context, slot, token,
                                               &input,
                                               &output) == kFlutterVenusV2Ok) {
            // Call-level Ok is not enough: an item-level failure also returns
            // Ok with a canonical zero output.
            EXPECT_EQ(output.status, kFlutterVenusV2Ok)
                << "field=" << output.error_field_id
                << " detail=" << output.error_detail;
            EXPECT_GT(output.paragraph_width, 0.0);
            ok_calls.fetch_add(1);
          }
        }
        EXPECT_EQ(f.api().release_slot(f.api().context, slot, token),
                  kFlutterVenusV2Ok);
      });
    }
    for (auto& runner : runners) {
      runner.join();
    }
    EXPECT_EQ(starved.load(), 0);
    EXPECT_EQ(ok_calls.load(), kRunners * kCallsPerRunner);
    const auto counters = f.Counters();
    EXPECT_EQ(counters.active_slot_calls, 0u);
    EXPECT_EQ(counters.leased_slots, 0u);
    EXPECT_EQ(counters.accepted_total, counters.completed_total);
  })) << "sync concurrency case hung past the 30s guard";
}

// A foreign thread must never be able to use somebody else's lease.
TEST(VenusTextLayoutSlotV2, ForgedLeaseIsRejectedUnderConcurrency) {
  ASSERT_TRUE(RunWithGuard([] {
    ServiceFixture f;
    uint32_t slot = 0;
    uint64_t token = 0;
    ASSERT_EQ(f.api().acquire_slot(f.api().context, &slot, &token),
              kFlutterVenusV2Ok);
    std::atomic<bool> stop{false};
    std::thread owner([&] {
      const Node node;
      while (!stop.load()) {
        FlutterVenusTextLayoutInputV2 input = node.Input(false);
        FlutterVenusTextLayoutOutputV2 output{};
        EXPECT_EQ(f.api().layout_text_node_sync_v2(f.api().context, slot, token,
                                                   &input, &output),
                  kFlutterVenusV2Ok);
      }
    });
    const Node node;
    for (int i = 0; i < 256; ++i) {
      FlutterVenusTextLayoutInputV2 input = node.Input(false);
      FlutterVenusTextLayoutOutputV2 output{};
      EXPECT_EQ(f.api().layout_text_node_sync_v2(f.api().context, slot,
                                                 token + 1u, &input, &output),
                kFlutterVenusV2SlotNotOwned);
      EXPECT_EQ(f.api().release_slot(f.api().context, slot, token + 1u),
                kFlutterVenusV2SlotNotOwned);
    }
    stop.store(true);
    owner.join();
    EXPECT_EQ(f.api().release_slot(f.api().context, slot, token),
              kFlutterVenusV2Ok);
    EXPECT_EQ(f.Counters().leased_slots, 0u);
  })) << "forged lease case hung past the 30s guard";
}

// Reloading fonts while sync callers keep running is the exact interleave the
// R -> S lock order exists for. After P7 this is the only path that takes R.
TEST(VenusTextLayoutFontEpochV2, FontReloadRacesSyncCallers) {
  ASSERT_TRUE(RunWithGuard([] {
    ServiceFixture f;
    std::atomic<bool> stop{false};
    std::thread caller([&] {
      const Node node;
      uint32_t slot = 0;
      uint64_t token = 0;
      while (!stop.load()) {
        if (f.api().acquire_slot(f.api().context, &slot, &token) !=
            kFlutterVenusV2Ok) {
          std::this_thread::yield();
          continue;
        }
        FlutterVenusTextLayoutInputV2 input = node.Input(false);
        FlutterVenusTextLayoutOutputV2 output{};
        const int32_t status = f.api().layout_text_node_sync_v2(
            f.api().context, slot, token, &input, &output);
        EXPECT_TRUE(status == kFlutterVenusV2Ok ||
                    status == kFlutterVenusV2ShuttingDown);
        if (status == kFlutterVenusV2Ok) {
          EXPECT_EQ(output.status, kFlutterVenusV2Ok);
        }
        // The holder always returns its own lease, on its own thread, exactly
        // once -- including when the call above was rejected.
        int32_t released = f.api().release_slot(f.api().context, slot, token);
        for (int retry = 0; released == kFlutterVenusV2SlotBusy && retry < 100;
             ++retry) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          released = f.api().release_slot(f.api().context, slot, token);
        }
        EXPECT_EQ(released, kFlutterVenusV2Ok);
      }
    });
    const uint64_t epoch_before = f.Counters().font_epoch;
    for (uint32_t round = 0; round < 8; ++round) {
      EXPECT_EQ(f.service()->ReloadFonts(), kFlutterVenusV2Ok);
      // accepting must be back to 1 by the time ReloadFonts returns, otherwise
      // the caller thread above would spin forever on acquire.
      EXPECT_EQ(f.Counters().accepting, 1u);
    }
    stop.store(true);
    caller.join();
    const auto counters = f.Counters();
    EXPECT_EQ(counters.font_epoch, epoch_before + 8u);
    EXPECT_EQ(counters.leased_slots, 0u);
    EXPECT_EQ(counters.active_slot_calls, 0u);
    EXPECT_EQ(counters.accepting, 1u);
  })) << "font reload race case hung past the 30s guard";
}

}  // namespace testing
}  // namespace flutter

int main(int argc, char** argv) {
  // Line breaking goes through ICU; without the data every shaping call fails
  // with U_MISSING_RESOURCE_ERROR and the suite would "pass" having laid out
  // nothing. Run this binary from its build directory.
  fml::icu::InitializeICU("icudtl.dat");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
