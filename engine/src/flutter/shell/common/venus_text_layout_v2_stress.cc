// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Million-call stress driver for the V2 text layout service.
//
//   venus_text_layout_v2_stress --transport sync|async --threads 1|2|4|8
//       --iterations <n> --mix normal,warm,unique,malformed
//       --interleave backpressure,fontreload,shutdown
//       --deadline-seconds <n> --json <path>
//
// --deadline-seconds is the mechanism that turns the three hang-shaped
// regressions this Feature has already produced (reconfiguration self-deadlock,
// workers that are never rebuilt, running batches that never retire) into a
// non-zero exit with a counters dump, instead of a command that simply never
// prints anything.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "flutter/fml/icu_util.h"
#include "flutter/lib/ui/text/font_collection.h"
#include "flutter/shell/common/venus_text_layout_service.h"

namespace {

using flutter::FontCollection;
using flutter::VenusTextLayoutService;

struct Options {
  std::string transport = "sync";
  uint32_t threads = 1;
  uint64_t iterations = 1000;
  std::string mix = "normal";
  std::string interleave;
  uint64_t deadline_seconds = 900;
  std::string json_path;
};

bool Contains(const std::string& csv, const char* token) {
  return csv.find(token) != std::string::npos;
}

uint64_t MonotonicNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// One node. `unique` mixes in the iteration counter so every call is a real
// cache miss; `warm` reuses one corpus so the paragraph cache is hot.
struct NodeBuilder {
  std::string family = "Roboto";
  // Required by DecodeAndValidate; leaving it empty would make every item fail
  // while the run still looked "conserved and quiesced".
  std::string locale = "en-US";
  std::string warm_text = "venus stress warm corpus";
  std::string scratch;

  FlutterVenusTextLayoutInputV2 Build(const Options& options,
                                      uint64_t iteration,
                                      bool* out_expect_failure) {
    *out_expect_failure = false;
    const bool unique = Contains(options.mix, "unique");
    const bool malformed =
        Contains(options.mix, "malformed") && (iteration % 97u == 96u);
    if (unique) {
      scratch = warm_text + " #" + std::to_string(iteration);
    } else {
      scratch = warm_text;
    }

    FlutterVenusTextLayoutInputV2 input{};
    input.struct_size = sizeof(FlutterVenusTextLayoutInputV2);
    input.abi_version = FLUTTER_VENUS_TEXT_LAYOUT_ABI_V2;
    input.flags = static_cast<uint32_t>(kFlutterVenusV2InputFlagItemTiming);
    input.text_length = static_cast<uint32_t>(scratch.size());
    input.font_family_length = static_cast<uint32_t>(family.size());
    input.locale_length = static_cast<uint32_t>(locale.size());
    input.font_weight = 400u;
    input.text_encoding = 1u;
    input.text_direction = 1u;
    input.text_align = 5u;
    input.font_style = 1u;
    input.leading_distribution = 1u;
    input.text_width_basis = 1u;
    input.text_scaler_kind = 1u;
    input.max_lines_mode = 1u;
    input.overflow_mode = 1u;
    input.soft_wrap = 2u;
    input.apply_height_to_first_ascent = 1u;
    input.apply_height_to_last_descent = 1u;
    input.height_mode = 1u;
    input.strut_mode = 1u;
    input.text = reinterpret_cast<const uint8_t*>(scratch.data());
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
    if (malformed) {
      // Item-level rejection: the batch is still accepted and the item still
      // writes a canonical failure output, so the conservation law holds.
      input.text_direction = 4242u;
      *out_expect_failure = true;
    }
    return input;
  }
};

void DumpCounters(FILE* out, const FlutterVenusTextLayoutCountersV2& c) {
  fprintf(out,
          // 键名必须是 native_counters —— 判定器与 soak schema 用的是这个名字，
          // 叫 counters 会让它读到 None 并把"未归零"报成事实。
          "  \"native_counters\": {\n"
          "    \"active_slot_calls\": %u,\n"
          "    \"leased_slots\": %u,\n"
          "    \"accepted_total\": %llu,\n"
          "    \"completed_total\": %llu,\n"
          "    \"rejected_total\": %llu,\n"
          "    \"font_epoch\": %llu,\n"
          "    \"accepting\": %u\n"
          "  }",
          c.active_slot_calls, c.leased_slots,
          (unsigned long long)c.accepted_total,
          (unsigned long long)c.completed_total,
          (unsigned long long)c.rejected_total,
          (unsigned long long)c.font_epoch, c.accepting);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const char* value = (i + 1 < argc) ? argv[i + 1] : nullptr;
    if (flag == "--transport" && value) {
      options.transport = value;
      ++i;
    } else if (flag == "--threads" && value) {
      options.threads = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
      ++i;
    } else if (flag == "--iterations" && value) {
      options.iterations = std::strtoull(value, nullptr, 10);
      ++i;
    } else if (flag == "--mix" && value) {
      options.mix = value;
      ++i;
    } else if (flag == "--interleave" && value) {
      options.interleave = value;
      ++i;
    } else if (flag == "--deadline-seconds" && value) {
      options.deadline_seconds = std::strtoull(value, nullptr, 10);
      ++i;
    } else if (flag == "--json" && value) {
      options.json_path = value;
      ++i;
    } else {
      fprintf(stderr, "unknown flag: %s\n", flag.c_str());
      return 2;
    }
  }
  const bool is_async = options.transport == "async";
  if (!is_async && options.transport != "sync") {
    fprintf(stderr, "--transport must be sync or async\n");
    return 2;
  }
  if (options.threads == 0 ||
      options.threads > static_cast<uint32_t>(kFlutterVenusV2MaxSlots)) {
    fprintf(stderr, "--threads must be in [1, %d]\n", kFlutterVenusV2MaxSlots);
    return 2;
  }

  // Same reason as the tsan suite: no ICU data means every item fails while
  // the counters still look conserved and quiesced. Run from the build dir.
  fml::icu::InitializeICU("icudtl.dat");

  auto fonts = std::make_shared<FontCollection>();
  // Same reason as the tsan suite: no font manager means kParagraphNull on
  // every item while the counters still look conserved.
  fonts->SetupDefaultFontManager(0u);
  fonts->RegisterTestFonts();
  auto* service = new VenusTextLayoutService(1u, 1u, fonts, false);
  FlutterVenusTextLayoutApiV2 api{};
  service->PopulateApi(&api);

  const uint64_t started_ns = MonotonicNs();
  const uint64_t deadline_ns =
      started_ns + options.deadline_seconds * 1000000000ull;
  std::atomic<uint64_t> completed{0};
  std::atomic<bool> stalled{false};
  std::atomic<uint64_t> item_failures{0};

  const uint64_t per_thread = options.iterations / options.threads;
  // backpressure 这一档随 async 队列一起消失了：sync 路上的"满"就是 slot 用尽，
  // 由 acquire 的重试环覆盖。这里显式拒绝，免得旧命令行静默跑成别的东西。
  if (Contains(options.interleave, "backpressure")) {
    fprintf(stderr,
            "--interleave backpressure was removed with the async queue\n");
    return 2;
  }
  const bool interleave_fontreload = Contains(options.interleave, "fontreload");
  const bool interleave_shutdown = Contains(options.interleave, "shutdown");

  std::vector<std::thread> workers;
  for (uint32_t index = 0; index < options.threads; ++index) {
    workers.emplace_back([&, index] {
      NodeBuilder builder;
      // 每 kChunk 条重新取一次 lease。整轮只取一次的话，ReloadFonts 的静默点
      // (leased_slots == 0) 要等到负载全部跑完才成立，--interleave fontreload
      // 就退化成"跑完之后 reload 四次"，什么都没交错到。
      constexpr uint64_t kChunk = 2000;
      uint64_t done = 0;
      while (done < per_thread && MonotonicNs() <= deadline_ns) {
        uint32_t slot = 0;
        uint64_t token = 0;
        int32_t acquired = kFlutterVenusV2ShuttingDown;
        // reload 窗口内 accepting 是 0，acquire 会被拒；那是瞬态，不是故障。
        while (MonotonicNs() <= deadline_ns) {
          acquired = api.acquire_slot(api.context, &slot, &token);
          if (acquired != kFlutterVenusV2ShuttingDown &&
              acquired != kFlutterVenusV2SlotBusy) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        if (acquired != kFlutterVenusV2Ok) {
          stalled.store(true);
          break;
        }
        const uint64_t chunk = std::min<uint64_t>(kChunk, per_thread - done);
        uint64_t ran = 0;
        bool interrupted = false;
        for (; ran < chunk && MonotonicNs() <= deadline_ns; ++ran) {
          bool expect_failure = false;
          FlutterVenusTextLayoutInputV2 input = builder.Build(
              options, done + ran + index * per_thread, &expect_failure);
          FlutterVenusTextLayoutOutputV2 output{};
          const int32_t status = api.layout_text_node_sync_v2(
              api.context, slot, token, &input, &output);
          if (status == kFlutterVenusV2ShuttingDown) {
            // §5 的 reload 窗口：持有者被 fail-closed，必须归还 lease 让静默点
            // 成立，之后重取。把它当硬失败，就是把设计好的瞬态报成缺陷 ——
            // 上一版驱动正是这样把 sync/fontreload 判成 stalled 的。
            interrupted = true;
            break;
          }
          if (status != kFlutterVenusV2Ok) {
            stalled.store(true);
            interrupted = true;
            break;
          }
          if (output.status != kFlutterVenusV2Ok) {
            item_failures.fetch_add(1);
          }
          completed.fetch_add(1);
        }
        done += ran;
        int32_t released = api.release_slot(api.context, slot, token);
        for (int retry = 0; released == kFlutterVenusV2SlotBusy && retry < 100;
             ++retry) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          released = api.release_slot(api.context, slot, token);
        }
        if (released != kFlutterVenusV2Ok) {
          stalled.store(true);
          break;
        }
        if (stalled.load()) {
          break;
        }
        if (interrupted) {
          // 让出 CPU，等 reload 那一侧把 accepting 恢复。
          std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
      }
      if (done < per_thread) {
        stalled.store(true);
      }
    });
  }

  // Interleaved lifecycle disturbance runs on the main thread while the load
  // threads keep going. font reload must leave the worker count restored,
  // otherwise every later submit would report NotReady and the run would look
  // "quiescent" while doing nothing at all.
  if (interleave_fontreload || interleave_shutdown) {
    for (int round = 0; round < 4; ++round) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (MonotonicNs() > deadline_ns) {
        break;
      }
      if (interleave_fontreload) {
        // 静默点含 leased_slots == 0：负载线程整轮只取一次 lease，所以这一步
        // 会一直等到它们跑完才返回。这是设计使然，不是卡住。
        if (service->ReloadFonts() != kFlutterVenusV2Ok) {
          stalled.store(true);
          break;
        }
      }
    }
  }

  for (auto& worker : workers) {
    worker.join();
  }

  FlutterVenusTextLayoutCountersV2 counters{};
  api.get_counters_v2(api.context, sizeof(counters), &counters);
  const uint64_t wall_ns = MonotonicNs() - started_ns;

  const bool conserved = counters.accepted_total == counters.completed_total;
  const bool zeroed =
      counters.active_slot_calls == 0 && counters.leased_slots == 0;
  const bool forward_progress =
      completed.load() >= per_thread * options.threads;

  FILE* out = stdout;
  if (!options.json_path.empty()) {
    out = fopen(options.json_path.c_str(), "w");
    if (out == nullptr) {
      fprintf(stderr, "cannot write %s\n", options.json_path.c_str());
      return 2;
    }
  }
  fprintf(out,
          "{\n"
          "  \"schema\": \"venus_text_layout_v2_stress_v1\",\n"
          "  \"transport\": \"%s\",\n"
          "  \"threads\": %u,\n"
          "  \"iterations\": %llu,\n"
          "  \"completed\": %llu,\n"
          "  \"item_failures\": %llu,\n"
          "  \"mix\": \"%s\",\n"
          "  \"interleave\": \"%s\",\n"
          "  \"wall_ns\": %llu,\n"
          "  \"stalled\": %s,\n"
          "  \"conserved\": %s,\n"
          "  \"zeroed\": %s,\n"
          "  \"forward_progress\": %s,\n",
          options.transport.c_str(), options.threads,
          (unsigned long long)(per_thread * options.threads),
          (unsigned long long)completed.load(),
          (unsigned long long)item_failures.load(), options.mix.c_str(),
          options.interleave.c_str(), (unsigned long long)wall_ns,
          stalled.load() ? "true" : "false", conserved ? "true" : "false",
          zeroed ? "true" : "false", forward_progress ? "true" : "false");
  DumpCounters(out, counters);
  fprintf(out, "\n}\n");
  if (out != stdout) {
    fclose(out);
  }

  if (stalled.load()) {
    fprintf(stderr, "FAIL: deadline exceeded or a call was rejected\n");
    DumpCounters(stderr, counters);
    fprintf(stderr, "\n");
  }
  service->Shutdown();
  api.release_context(api.context);

  if (stalled.load()) {
    return 1;
  }
  if (!conserved || !zeroed || !forward_progress) {
    fprintf(stderr, "FAIL: conserved=%d zeroed=%d forward_progress=%d\n",
            conserved, zeroed, forward_progress);
    return 1;
  }
  return 0;
}
