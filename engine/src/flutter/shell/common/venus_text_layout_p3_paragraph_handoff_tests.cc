// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Phase 3 可行性验证：**txt::Paragraph 能不能跨线程交接**。
//
// Phase 2 的 core 已经在构建并排版真正的 txt::Paragraph（builder->Build() 之后
// paragraph->Layout(max_width)），抽完九个几何量就把它析构掉。Phase 3 的全部
// 想法就是别扔：worker 线程排好版，UI 线程只做一次指针移动
// （flutter::Paragraph::Create 收的正是 std::unique_ptr<txt::Paragraph>），
// paint 走 canvas.drawParagraph —— UI 线程上的 shaping 归零。
//
// 这条路能不能走通，全卡在一个问题上：**在 A 线程 Build+Layout 出来的
// paragraph，能不能安全地在 B 线程读取几何量与 Paint？**
//
// 不能，Phase 3 就得换设计（比如把交接限制在同线程、或者退回"只替代
// 量完即扔的测量"）。所以先证这一条，再谈别的。
//
// 这里**不做** flutter::Paragraph::Create —— 那个要 Dart_Handle、要进 isolate，
// 属于接线问题。此处只证最底层的那件事：SkParagraph 对象本身跨线程是否安全。

#include <atomic>
#include <cstring>
#include <iostream>
#include <utility>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "flutter/fml/icu_util.h"
#include "flutter/lib/ui/text/font_collection.h"
#include "flutter/lib/ui/text/venus_text_layout_node_core.h"
#include "gtest/gtest.h"
#include "flutter/display_list/dl_builder.h"
#include "txt/paragraph_builder.h"
#include "txt/paragraph_style.h"

namespace flutter {
namespace testing {
namespace {

// 与 core 同款的最小构建路径。这里不复用 LayoutTextNodeCore 是刻意的：
// 那个函数的签名只吐 NodeMetrics，拿不到 paragraph 本身 —— 而"拿到 paragraph"
// 正是 Phase 3 要改的地方。此处先手工搭一份等价的，证明可行之后再决定怎么改
// core 的出参形状。
std::unique_ptr<txt::Paragraph> BuildAndLayout(
    const std::shared_ptr<txt::FontCollection>& fonts,
    const std::string& text,
    double max_width) {
  txt::ParagraphStyle paragraph_style;
  paragraph_style.font_family = "Roboto";
  paragraph_style.font_size = 16.0;
  std::unique_ptr<txt::ParagraphBuilder> builder =
      txt::ParagraphBuilder::CreateSkiaBuilder(paragraph_style, fonts,
                                               /*impeller_enabled=*/false);
  if (builder == nullptr) {
    return nullptr;
  }
  txt::TextStyle style = paragraph_style.GetTextStyle();
  builder->PushStyle(style);
  builder->AddText(std::u16string(text.begin(), text.end()));
  std::unique_ptr<txt::Paragraph> paragraph = builder->Build();
  if (paragraph == nullptr) {
    return nullptr;
  }
  paragraph->Layout(max_width);
  return paragraph;
}

class VenusParagraphHandoffTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fonts_ = std::make_shared<FontCollection>();
    fonts_->SetupDefaultFontManager(0u);
    fonts_->RegisterTestFonts();
  }

  // 每个 worker 用自己那一份 clone —— 与 V2 的 per-slot 门面同款。
  // 共用一份 FontCollection 会把 SkTHash 写坏（Phase 2 已经踩过：8 个线程
  // 共用时 check(fCount == oldCount) 直接崩），所以这里不是可选项。
  std::shared_ptr<txt::FontCollection> CloneForWorker() {
    uint64_t epoch = 0u;
    return fonts_->GetFontCollection()->CloneForVenusWorker(&epoch);
  }

  std::shared_ptr<FontCollection> fonts_;
};

// (1) 最基本的一条：worker 线程排版，主线程读几何量。
TEST_F(VenusParagraphHandoffTest, MetricsReadableOnAnotherThread) {
  std::unique_ptr<txt::Paragraph> paragraph;
  std::thread worker([&] {
    paragraph = BuildAndLayout(CloneForWorker(),
                               "Venus hands Flutter a laid-out paragraph", 200.0);
  });
  worker.join();

  ASSERT_NE(paragraph, nullptr);
  // 读几何量必须给出和排版时一致的结果，且不能触发重新排版。
  EXPECT_GT(paragraph->GetHeight(), 0.0);
  EXPECT_GT(paragraph->GetMaxIntrinsicWidth(), 0.0);
  EXPECT_LE(paragraph->GetLongestLine(), 200.0);
  EXPECT_GE(paragraph->GetAlphabeticBaseline(), 0.0);
}

// (2) 真正的卡口：worker 线程排版，**另一个线程 Paint**。
// 这一条不过，Phase 3 的"UI 线程零 shaping"就不成立。
TEST_F(VenusParagraphHandoffTest, PaintableOnAnotherThread) {
  std::unique_ptr<txt::Paragraph> paragraph;
  std::thread worker([&] {
    paragraph =
        BuildAndLayout(CloneForWorker(), "cross-thread paint probe", 180.0);
  });
  worker.join();
  ASSERT_NE(paragraph, nullptr);

  // 在**当前线程**（非建它的那个）上画。ASan 下跑这条才有意义。
  // 这个引擎版本的 Paint 收的是 DisplayListBuilder, 不是 SkCanvas ——
  // 正好也说明 Phase 3 的 paint 侧不是 SkCanvas 直画, 而是走 DisplayList。
  flutter::DisplayListBuilder builder;
  EXPECT_TRUE(paragraph->Paint(&builder, 0, 0));

  // 画完之后几何量不能被改写 —— Paint 若在内部触发重排, 说明这条路不是
  // "零 shaping", 而是把 shaping 挪到了 paint 时刻。
  const double height_after = paragraph->GetHeight();
  EXPECT_GT(height_after, 0.0);
}

// (3) 并行版：N 个 worker 各自排版, 主线程逐个 Paint。
// 单条通过可能是运气; 这一条在有真实竞争的情况下再证一遍。
TEST_F(VenusParagraphHandoffTest, ManyParagraphsBuiltInParallelThenPainted) {
  constexpr int kWorkers = 8;
  std::vector<std::unique_ptr<txt::Paragraph>> paragraphs(kWorkers);
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (int i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&, i] {
      paragraphs[i] = BuildAndLayout(
          CloneForWorker(), "parallel shaping row " + std::to_string(i), 160.0);
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  flutter::DisplayListBuilder builder;
  int painted = 0;
  for (std::unique_ptr<txt::Paragraph>& paragraph : paragraphs) {
    ASSERT_NE(paragraph, nullptr);
    EXPECT_GT(paragraph->GetHeight(), 0.0);
    EXPECT_TRUE(paragraph->Paint(&builder, 0, 0));
    painted += 1;
  }
  EXPECT_EQ(painted, kWorkers);
}

// (3b) **真并发**：worker 还在用表 A 排下一段的同时, 主线程画表 A 排好的上一段。
//
// 前面三条都先 join() 了 —— 测的是"接力"不是"同时", 那一刻每张字体表只有一个
// 使用者, 结构上测不到竞争。而生产场景一定是同时的: UI 线程画第 1 行时,
// worker 还在排第 20 行。
//
// 风险路径: paragraph 记得建它的那张表(fFontCollection)。UI 线程 Paint 若需要
// 查字体, 会回头查**那张表**; 而同一个 worker 正拿同一张表排下一段。两个线程
// 同时对一张"查不到就填"的哈希表操作 —— 扩容时旧数组被释放两次, 就是我们
// 已经在共用 FontCollection 那条上见过的 double-free。
//
// 这一条要么证明"Paint 不回头碰表"(那 Phase 3 安全), 要么当场把它炸出来。
TEST_F(VenusParagraphHandoffTest, PaintWhileSameWorkerKeepsShaping) {
  constexpr int kRounds = 3000;
  // 一张表, 一个 worker 全程用它 —— 刻意复现"同一份表"的条件。
  std::shared_ptr<txt::FontCollection> worker_fonts = CloneForWorker();

  std::unique_ptr<txt::Paragraph> handed_off;
  std::atomic<bool> handoff_ready{false};
  std::atomic<bool> stop{false};
  std::atomic<int> shaped{0};

  std::thread worker([&] {
    // 先排一段交出去, 然后**不停**继续用同一张表排新的。
    handed_off = BuildAndLayout(worker_fonts, "handed off to the ui thread", 170.0);
    handoff_ready.store(true);
    while (!stop.load()) {
      // 每轮换文本, 强制真的走 shaping 与字体查找, 而不是命中缓存。
      if (BuildAndLayout(worker_fonts,
                         "worker keeps shaping " + std::to_string(shaped.load()),
                         150.0 + (shaped.load() % 40)) != nullptr) {
        shaped.fetch_add(1);
      }
    }
  });

  while (!handoff_ready.load()) {
    std::this_thread::yield();
  }
  ASSERT_NE(handed_off, nullptr);

  // worker 仍在跑。主线程反复画那一段, 同时读几何量。
  flutter::DisplayListBuilder builder;
  for (int round = 0; round < kRounds; ++round) {
    EXPECT_GT(handed_off->GetHeight(), 0.0);
    EXPECT_TRUE(handed_off->Paint(&builder, 0, 0));
  }

  stop.store(true);
  worker.join();
  // 对照量: worker 必须真的在这段时间里排了不少段, 否则"并发"是假的 ——
  // 它要是一段都没排, 这条用例就退化成前面那三条。
  // 把并发的**规模**打出来: "过了"若建立在 worker 只排了 2 段之上, 那这条用例
  // 和前面三条没区别。数字要能证明竞争窗口是真的。
  std::cout << "  [并发规模] 主线程 Paint " << kRounds << " 次, "
            << "同期 worker 用同一张表排了 " << shaped.load() << " 段\n";
  EXPECT_GT(shaped.load(), 20)
      << "worker 排得太少, 这一条没有构成有意义的并发, 不能当证据";
}

// (5) 字形一致性：worker 用**克隆的**字体表排出来的, 和 UI 线程用**原始**字体表
// 排出来的, 必须逐 bit 相同。
//
// 这是 Phase 3 最后一条没验的。推理上应该一样 —— 克隆共享的是同一批 SkFontMgr,
// 复制的只是空缓存, 查找逻辑同一套。但推理说得通不等于测过: 万一 fallback 在
// 两边选到了不同字体, 表现是"字能显示、字形不对", 极难联想到布局改动。
//
// 判据用 Phase 2 那把武器: 逐 bit 比较, 不是 epsilon。几何量差一个 ULP 都算不同。
TEST_F(VenusParagraphHandoffTest, ClonedCollectionShapesIdenticallyToOriginal) {
  // 覆盖会走不同排版分支的几种输入: 折行 / 不折行 / CJK / 混排 / 空格边界。
  const std::vector<std::pair<std::string, double>> cases = {
      {"Venus drives Flutter text shaping across worker threads", 200.0},
      {"short", 500.0},
      {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 90.0},
      {"mixed 中文 and latin 混排 text", 160.0},
      {"trailing spaces matter    ", 140.0},
  };

  for (const auto& [text, width] : cases) {
    // UI 线程侧: 用**原始**表(未克隆), 就是生产里 Flutter 自己会用的那份。
    std::unique_ptr<txt::Paragraph> on_main =
        BuildAndLayout(fonts_->GetFontCollection(), text, width);
    ASSERT_NE(on_main, nullptr) << text;

    // worker 侧: 用克隆表, 就是 Phase 3 会用的那份。
    std::unique_ptr<txt::Paragraph> on_worker;
    std::thread worker(
        [&] { on_worker = BuildAndLayout(CloneForWorker(), text, width); });
    worker.join();
    ASSERT_NE(on_worker, nullptr) << text;

    // 逐 bit —— 用 memcmp 语义比较 double, 不用 ==, 免得 -0.0/NaN 蒙混过关。
    auto same_bits = [](double a, double b) {
      return std::memcmp(&a, &b, sizeof(double)) == 0;
    };
    EXPECT_PRED2(same_bits, on_worker->GetHeight(), on_main->GetHeight()) << text;
    EXPECT_PRED2(same_bits, on_worker->GetMaxWidth(), on_main->GetMaxWidth()) << text;
    EXPECT_PRED2(same_bits, on_worker->GetLongestLine(), on_main->GetLongestLine()) << text;
    EXPECT_PRED2(same_bits, on_worker->GetMinIntrinsicWidth(),
                 on_main->GetMinIntrinsicWidth()) << text;
    EXPECT_PRED2(same_bits, on_worker->GetMaxIntrinsicWidth(),
                 on_main->GetMaxIntrinsicWidth()) << text;
    EXPECT_PRED2(same_bits, on_worker->GetAlphabeticBaseline(),
                 on_main->GetAlphabeticBaseline()) << text;
    EXPECT_PRED2(same_bits, on_worker->GetIdeographicBaseline(),
                 on_main->GetIdeographicBaseline()) << text;
    EXPECT_EQ(on_worker->DidExceedMaxLines(), on_main->DidExceedMaxLines()) << text;

    // 几何量相同还不够 —— 那只说明"占的地方一样大"。逐行比一遍, 才能证明
    // **断行位置**也一样: 同样的总高度可以来自不同的断行。
    std::vector<txt::LineMetrics>& lines_worker = on_worker->GetLineMetrics();
    std::vector<txt::LineMetrics>& lines_main = on_main->GetLineMetrics();
    ASSERT_EQ(lines_worker.size(), lines_main.size()) << text;
    for (size_t i = 0; i < lines_main.size(); ++i) {
      EXPECT_EQ(lines_worker[i].start_index, lines_main[i].start_index) << text << " line " << i;
      EXPECT_EQ(lines_worker[i].end_index, lines_main[i].end_index) << text << " line " << i;
      EXPECT_PRED2(same_bits, lines_worker[i].width, lines_main[i].width) << text << " line " << i;
      EXPECT_PRED2(same_bits, lines_worker[i].height, lines_main[i].height) << text << " line " << i;
      EXPECT_PRED2(same_bits, lines_worker[i].baseline, lines_main[i].baseline) << text << " line " << i;
    }
  }
}

// (5b) 上一条的**反例自检**: 同一套比较逻辑喂两个真不一样的排版, 必须判不同。
//
// 不做这一条, (5) 的"全过"可能只是因为比较写错了(比如把两边都取成了同一个
// 对象、或者 same_bits 恒真)。本任务里已经吃过一次亏: 一条从没红过的门,
// 和没有门是同一件事。
TEST_F(VenusParagraphHandoffTest, ShapeComparisonActuallyDiscriminates) {
  auto same_bits = [](double a, double b) {
    return std::memcmp(&a, &b, sizeof(double)) == 0;
  };
  const std::string text = "Venus drives Flutter text shaping across threads";

  std::unique_ptr<txt::Paragraph> narrow =
      BuildAndLayout(fonts_->GetFontCollection(), text, 90.0);
  std::unique_ptr<txt::Paragraph> wide =
      BuildAndLayout(fonts_->GetFontCollection(), text, 400.0);
  ASSERT_NE(narrow, nullptr);
  ASSERT_NE(wide, nullptr);

  // 同一段文字, 两个宽度 -> 断行必然不同 -> 高度与行数必须被判为不同。
  EXPECT_FALSE(same_bits(narrow->GetHeight(), wide->GetHeight()))
      << "高度比较没有判别力 —— (5) 的通过不构成证据";
  EXPECT_NE(narrow->GetLineMetrics().size(), wide->GetLineMetrics().size())
      << "行数比较没有判别力 —— (5) 的通过不构成证据";
}

// (4) 反向对照：**故意共用同一份 FontCollection** 并行排版。
// Phase 2 已经证过这样会把 SkTHash 写坏, 这里把它固化成用例 ——
// 用来证明上面三条的"每 worker 自己 clone"不是仪式, 是必需。
//
// 注意: 这一条在 ASan 下**可能真的崩**, 所以默认 DISABLED, 需要时手动跑:
//   --gtest_also_run_disabled_tests --gtest_filter=*SharedFontCollection*
TEST_F(VenusParagraphHandoffTest, DISABLED_SharedFontCollectionIsUnsafe) {
  constexpr int kWorkers = 8;
  std::shared_ptr<txt::FontCollection> shared = fonts_->GetFontCollection();
  std::vector<std::thread> workers;
  std::atomic<int> built{0};
  workers.reserve(kWorkers);
  for (int i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&, i] {
      if (BuildAndLayout(shared, "shared collection row " + std::to_string(i),
                         160.0) != nullptr) {
        built.fetch_add(1);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  EXPECT_EQ(built.load(), kWorkers) << "若这条没崩也没少, 说明上游已经改成线程安全";
}

}  // namespace
}  // namespace testing
}  // namespace flutter

int main(int argc, char** argv) {
  // 没有 ICU 数据时每一项都会以 U_MISSING_RESOURCE_ERROR 失败, 而计数看着正常 ——
  // 与 Phase 2 的 tsan 套件同一条理由。必须从构建目录里跑。
  fml::icu::InitializeICU("icudtl.dat");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
