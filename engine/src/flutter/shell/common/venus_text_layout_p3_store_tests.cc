// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// P3-1 出口：交付表本身。
//
// 要证两件事：
//   1. **采用路径与自排路径逐 bit 相同** —— 认领回来的那份，和 UI 线程自己排的
//      比，几何量与逐行断行位置全等。不然这个 Feature 就是在悄悄画错内容。
//   2. **每一条未命中都真的会红** —— §三 那张表里的每个计数，都要有一条用例把它
//      打出来。全是 0 的计数器等于没有计数器：静默回退会让耗时数字很好看，
//      因为没人知道它其实一次都没命中。

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "flutter/display_list/dl_builder.h"
#include "flutter/fml/icu_util.h"
#include "flutter/lib/ui/text/font_collection.h"
#include "flutter/lib/ui/text/venus_text_layout_node_core.h"
#include "flutter/lib/ui/text/venus_text_layout_paragraph_store.h"
#include "flutter/shell/platform/common/public/flutter_venus_text_layout.h"
#include "flutter/testing/testing.h"
#include "gtest/gtest.h"
#include "txt/asset_font_manager.h"
#include "txt/paragraph_builder.h"
#include "txt/paragraph_style.h"
#include "txt/platform.h"
#include "txt/typeface_font_asset_provider.h"

namespace flutter {
namespace testing {
namespace {

using venus_text_layout::ParagraphStore;

constexpr uint64_t kEngineId = 1u;
constexpr uint64_t kRegistrationGeneration = 11u;
constexpr uint64_t kSnapshotGenerationUnavailable = 0u;

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
  builder->PushStyle(paragraph_style.GetTextStyle());
  builder->AddText(std::u16string(text.begin(), text.end()));
  std::unique_ptr<txt::Paragraph> paragraph = builder->Build();
  if (paragraph == nullptr) {
    return nullptr;
  }
  paragraph->Layout(max_width);
  return paragraph;
}

bool SameBits(double a, double b) {
  return std::memcmp(&a, &b, sizeof(double)) == 0;
}

class VenusParagraphStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fonts_ = std::make_shared<FontCollection>();
    fonts_->SetupDefaultFontManager(0u);
    fonts_->RegisterTestFonts();
    ParagraphStore::Instance().Clear();
    ParagraphStore::Instance().ResetCounters();
    ParagraphStore::Instance().ActivateRegistration(kEngineId,
                                                    kRegistrationGeneration);
  }

  std::shared_ptr<txt::FontCollection> CloneForWorker() {
    uint64_t epoch = 0u;
    return fonts_->GetFontCollection()->CloneForVenusWorker(&epoch);
  }

  // 真实 Roboto 字体文件: 断言行高精确值要确定的度量。
  void UseRobotoFixture() {
    auto data = OpenFixtureAsSkData("Roboto-Regular.ttf");
    ASSERT_NE(data, nullptr);
    auto typeface = txt::GetDefaultFontManager()->makeFromData(std::move(data));
    ASSERT_NE(typeface, nullptr);
    SkString family_name;
    typeface->getFamilyName(&family_name);
    ASSERT_STREQ(family_name.c_str(), "Roboto");
    auto provider = std::make_unique<txt::TypefaceFontAssetProvider>();
    provider->RegisterTypeface(std::move(typeface), "Roboto");
    fonts_->GetFontCollection()->SetAssetFontManager(
        sk_make_sp<txt::AssetFontManager>(std::move(provider)));
  }

  // 存入并立刻交还 slot。既有用例测的是 width / font_epoch / 容量那几条，
  // 不该被 slot 规则淹没 —— slot 规则另有专门用例（见 SlotBusy 那几条）。
  void DepositReady(uint64_t token,
                    double width,
                    uint64_t font_epoch,
                    const std::string& text) {
    constexpr uint32_t kSlot = 0;
    ParagraphStore::Instance().Deposit(
        token, width, font_epoch, kEngineId, kRegistrationGeneration,
        kSnapshotGenerationUnavailable, kSlot,
        BuildAndLayout(CloneForWorker(), text, width));
    ParagraphStore::Instance().NotifySlotReleased(
        kEngineId, kRegistrationGeneration, kSlot, true);
  }

  uint64_t BeginArtifactOwner(uint64_t producer_generation) {
    uint64_t owner_id = 0u;
    EXPECT_EQ(ParagraphStore::Instance().BeginArtifactOwner(
                  kEngineId, kRegistrationGeneration, producer_generation,
                  &owner_id),
              kFlutterVenusV2Ok);
    EXPECT_NE(owner_id, 0u);
    return owner_id;
  }

  int32_t DepositOwned(
      uint64_t token,
      uint32_t slot,
      uint64_t owner_id,
      uint64_t input_charge_bytes,
      const std::shared_ptr<txt::FontCollection>& worker_fonts) {
    return ParagraphStore::Instance().Deposit(
        token, 100.0, 1u, kEngineId, kRegistrationGeneration,
        kSnapshotGenerationUnavailable, slot,
        BuildAndLayout(worker_fonts, "owned", 100.0), owner_id,
        input_charge_bytes);
  }

  std::shared_ptr<FontCollection> fonts_;
};

// ---- 1. 采用路径 vs 自排路径, 逐 bit ------------------------------------

TEST_F(VenusParagraphStoreTest, ClaimedParagraphMatchesSelfLaidOutBitForBit) {
  const std::vector<std::pair<std::string, double>> cases = {
      {"Venus hands Flutter a paragraph that is already laid out", 200.0},
      {"short", 500.0},
      {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 90.0},
      {"mixed 中文 and latin 混排 text", 160.0},
  };

  uint64_t token = 1;
  for (const auto& [text, width] : cases) {
    // worker 线程排好并存入 —— 与生产路径同款(自己的克隆字体表)。
    std::thread worker([&] { DepositReady(token, width, /*font_epoch=*/7u, text); });
    worker.join();

    std::unique_ptr<txt::Paragraph> claimed =
        ParagraphStore::Instance().Claim(token, width, /*font_epoch=*/7u);
    ASSERT_NE(claimed, nullptr) << text;

    // UI 线程自己排一份做对照 —— 生产里这就是回退路径会得到的东西。
    std::unique_ptr<txt::Paragraph> self =
        BuildAndLayout(fonts_->GetFontCollection(), text, width);
    ASSERT_NE(self, nullptr) << text;

    EXPECT_PRED2(SameBits, claimed->GetHeight(), self->GetHeight()) << text;
    EXPECT_PRED2(SameBits, claimed->GetLongestLine(), self->GetLongestLine()) << text;
    EXPECT_PRED2(SameBits, claimed->GetMinIntrinsicWidth(),
                 self->GetMinIntrinsicWidth()) << text;
    EXPECT_PRED2(SameBits, claimed->GetMaxIntrinsicWidth(),
                 self->GetMaxIntrinsicWidth()) << text;
    EXPECT_PRED2(SameBits, claimed->GetAlphabeticBaseline(),
                 self->GetAlphabeticBaseline()) << text;
    EXPECT_EQ(claimed->DidExceedMaxLines(), self->DidExceedMaxLines()) << text;

    // 逐行断行位置。几何量相同只说明"占的地方一样大" ——
    // 同样的总高度可以来自不同的断行。
    std::vector<txt::LineMetrics>& lines_claimed = claimed->GetLineMetrics();
    std::vector<txt::LineMetrics>& lines_self = self->GetLineMetrics();
    ASSERT_EQ(lines_claimed.size(), lines_self.size()) << text;
    for (size_t i = 0; i < lines_self.size(); ++i) {
      EXPECT_EQ(lines_claimed[i].start_index, lines_self[i].start_index) << text;
      EXPECT_EQ(lines_claimed[i].end_index, lines_self[i].end_index) << text;
      EXPECT_PRED2(SameBits, lines_claimed[i].width, lines_self[i].width) << text;
    }
    token += 1;
  }

  const ParagraphStore::Counters counters =
      ParagraphStore::Instance().GetCounters();
  EXPECT_EQ(counters.claimed, cases.size());
  EXPECT_EQ(counters.miss_not_prewarmed, 0u);
  EXPECT_EQ(counters.miss_width_mismatch, 0u);
}

// ---- 2. 认领即移交所有权 -------------------------------------------------

TEST_F(VenusParagraphStoreTest, ClaimTransfersOwnershipExactlyOnce) {
  DepositReady(42, 100.0, 1u, "once");
  ASSERT_EQ(ParagraphStore::Instance().SizeForTesting(), 1u);

  EXPECT_NE(ParagraphStore::Instance().Claim(42, 100.0, 1u), nullptr);
  EXPECT_EQ(ParagraphStore::Instance().SizeForTesting(), 0u)
      << "认领之后表里不该再持有它 —— 否则就有两个所有者";
  // 第二次认领必须落空, 且计入"从没被存过"。
  EXPECT_EQ(ParagraphStore::Instance().Claim(42, 100.0, 1u), nullptr);
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().miss_not_prewarmed, 1u);
}

TEST_F(VenusParagraphStoreTest,
       IfcCoreMetricsAndClaimedArtifactComeFromTheSameLayoutObject) {
  venus_text_layout::ParagraphInputView input;
  input.max_width = 96.0;
  venus_text_layout::ParagraphRunView run;
  run.text = u"same IFC artifact wraps across lines";
  run.font_family = "Roboto";
  run.font_size = 16.0;
  input.runs.push_back(std::move(run));

  venus_text_layout::ParagraphMetrics metrics;
  std::unique_ptr<txt::Paragraph> artifact;
  const venus_text_layout::ItemFailure failure =
      venus_text_layout::LayoutParagraphCore(input, CloneForWorker(),
                                             /*impeller_enabled=*/false,
                                             &metrics, &artifact);
  ASSERT_TRUE(failure.ok());
  ASSERT_NE(artifact, nullptr);
  txt::Paragraph* const deposited_object = artifact.get();

  EXPECT_PRED2(SameBits, metrics.width, artifact->GetLongestLine());
  EXPECT_PRED2(SameBits, metrics.height, artifact->GetHeight());
  EXPECT_PRED2(SameBits, metrics.first_baseline,
               artifact->GetAlphabeticBaseline());
  std::vector<txt::LineMetrics>& artifact_lines = artifact->GetLineMetrics();
  ASSERT_EQ(metrics.lines.size(), artifact_lines.size());
  for (size_t i = 0; i < artifact_lines.size(); ++i) {
    EXPECT_PRED2(SameBits, metrics.lines[i].left, artifact_lines[i].left);
    EXPECT_PRED2(SameBits, metrics.lines[i].width, artifact_lines[i].width);
    EXPECT_PRED2(SameBits, metrics.lines[i].height, artifact_lines[i].height);
    EXPECT_PRED2(SameBits, metrics.lines[i].baseline,
                 artifact_lines[i].baseline);
    EXPECT_PRED2(SameBits, metrics.lines[i].ascent, artifact_lines[i].ascent);
    EXPECT_PRED2(SameBits, metrics.lines[i].descent, artifact_lines[i].descent);
  }

  constexpr uint64_t kToken = 0xabcdu;
  constexpr uint32_t kSlot = 2u;
  ParagraphStore::Instance().Deposit(kToken, input.max_width, 7u, kEngineId,
                                     kRegistrationGeneration,
                                     kSnapshotGenerationUnavailable, kSlot,
                                     std::move(artifact));
  ParagraphStore::Instance().NotifySlotReleased(
      kEngineId, kRegistrationGeneration, kSlot, true);
  std::unique_ptr<txt::Paragraph> claimed =
      ParagraphStore::Instance().Claim(kToken, input.max_width, 7u);
  ASSERT_NE(claimed, nullptr);
  EXPECT_EQ(claimed.get(), deposited_object);
}

TEST_F(VenusParagraphStoreTest, ZeroFontRunMetricsMatchThePaintArtifact) {
  venus_text_layout::ParagraphInputView input;
  input.max_width = 1000.0;
  venus_text_layout::ParagraphRunView run;
  run.text = u"x";
  run.font_family = "Roboto";
  run.font_size = 0.0;
  input.runs.push_back(run);

  venus_text_layout::ParagraphMetrics metrics;
  std::unique_ptr<txt::Paragraph> artifact;
  ASSERT_TRUE(venus_text_layout::LayoutParagraphCore(
                  input, CloneForWorker(), /*impeller_enabled=*/false,
                  &metrics, &artifact)
                  .ok());
  ASSERT_NE(artifact, nullptr);
  std::vector<txt::LineMetrics>& lines = artifact->GetLineMetrics();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_DOUBLE_EQ(metrics.height, 0.0);
  EXPECT_DOUBLE_EQ(artifact->GetHeight(), 0.0);
  EXPECT_DOUBLE_EQ(lines[0].height, 0.0);
  EXPECT_DOUBLE_EQ(lines[0].baseline, 0.0);

  // A declared zero line-height keeps its glyph run in line metrics.
  input.runs[0].height_declared = true;
  input.runs[0].height_multiple = 0.0;
  ASSERT_TRUE(venus_text_layout::LayoutParagraphCore(
                  input, CloneForWorker(), /*impeller_enabled=*/false,
                  &metrics, &artifact)
                  .ok());
  ASSERT_NE(artifact, nullptr);
  std::vector<txt::LineMetrics>& declared_lines = artifact->GetLineMetrics();
  ASSERT_EQ(declared_lines.size(), 1u);
  ASSERT_FALSE(declared_lines[0].run_metrics.empty());
  const auto& font_metrics =
      declared_lines[0].run_metrics.begin()->second.font_metrics;
  EXPECT_DOUBLE_EQ(metrics.height, 0.0);
  EXPECT_DOUBLE_EQ(declared_lines[0].baseline, 0.0);
  EXPECT_FLOAT_EQ(-font_metrics.fAscent + font_metrics.fDescent, 0.0);

  // Venus also supplies a zero-size strut for an explicit zero line-height.
  input.strut_enabled = true;
  input.strut_font_family = "Roboto";
  input.strut_font_size = 0.0;
  input.strut_height_multiple = 0.0;
  ASSERT_TRUE(venus_text_layout::LayoutParagraphCore(
                  input, CloneForWorker(), /*impeller_enabled=*/false,
                  &metrics, &artifact)
                  .ok());
  ASSERT_NE(artifact, nullptr);
  EXPECT_DOUBLE_EQ(metrics.height, 0.0);
  EXPECT_DOUBLE_EQ(metrics.first_baseline, 0.0);
  EXPECT_DOUBLE_EQ(artifact->GetAlphabeticBaseline(), 0.0);
}

TEST_F(VenusParagraphStoreTest,
       IfcFragmentsUseExactLineRangesAndExcludeTrailingWhitespace) {
  venus_text_layout::ParagraphInputView input;
  input.max_width = 72.0;

  venus_text_layout::ParagraphRunView first;
  first.text = u"alpha beta ";
  first.font_family = "Roboto";
  first.font_size = 16.0;
  input.runs.push_back(first);

  venus_text_layout::ParagraphRunView line_break;
  line_break.is_line_break = true;
  input.runs.push_back(line_break);

  venus_text_layout::ParagraphRunView second;
  second.text = u"gamma delta trailing   ";
  second.font_family = "Roboto";
  second.font_size = 24.0;
  input.runs.push_back(second);

  venus_text_layout::ParagraphMetrics metrics;
  std::unique_ptr<txt::Paragraph> artifact;
  const venus_text_layout::ItemFailure failure =
      venus_text_layout::LayoutParagraphCore(input, CloneForWorker(),
                                             /*impeller_enabled=*/false,
                                             &metrics, &artifact);
  ASSERT_TRUE(failure.ok());
  ASSERT_NE(artifact, nullptr);

  std::vector<txt::LineMetrics>& lines = artifact->GetLineMetrics();
  ASSERT_GT(lines.size(), 2u);
  ASSERT_FALSE(metrics.fragments.empty());
  for (const venus_text_layout::ParagraphFragmentOut& fragment :
       metrics.fragments) {
    ASSERT_LT(fragment.line_number, lines.size());
    const txt::LineMetrics& line = lines[fragment.line_number];
    EXPECT_EQ(line.line_number, fragment.line_number);
    EXPECT_LE(fragment.right, line.left + line.width)
        << "fragment geometry must use the paragraph's visible line range";
  }

  // The final run ends in collapsible spaces. They stay in the paint artifact,
  // but its published fragment geometry must stop at the exact visible range.
  const auto last_fragment =
      std::find_if(metrics.fragments.rbegin(), metrics.fragments.rend(),
                   [](const venus_text_layout::ParagraphFragmentOut& fragment) {
                     return fragment.run_index == 2u;
                   });
  ASSERT_NE(last_fragment, metrics.fragments.rend());
  const txt::LineMetrics& last_line = lines[last_fragment->line_number];
  EXPECT_PRED2(SameBits, last_fragment->right,
               last_line.left + last_line.width);
}

TEST_F(VenusParagraphStoreTest, TrailingLineBreakHasNoPaintFragment) {
  venus_text_layout::ParagraphInputView input;
  input.max_width = 374.0;

  venus_text_layout::ParagraphRunView text;
  text.text = u"hello";
  text.font_family = "Roboto";
  text.font_size = 16.0;
  input.runs.push_back(text);

  venus_text_layout::ParagraphRunView line_break;
  line_break.is_line_break = true;
  input.runs.push_back(line_break);

  venus_text_layout::ParagraphMetrics metrics;
  std::unique_ptr<txt::Paragraph> artifact;
  const venus_text_layout::ItemFailure failure =
      venus_text_layout::LayoutParagraphCore(input, CloneForWorker(),
                                             /*impeller_enabled=*/false,
                                             &metrics, &artifact);
  ASSERT_TRUE(failure.ok());
  ASSERT_NE(artifact, nullptr);
  ASSERT_EQ(metrics.fragments.size(), 1u);
  EXPECT_EQ(metrics.fragments.front().run_index, 0u);
}

TEST_F(VenusParagraphStoreTest,
       CssLineHeightIsExactWithoutClampingTallerInlineContent) {
  UseRobotoFixture();

  venus_text_layout::ParagraphInputView input;
  input.max_width = 100.0;
  input.strut_enabled = true;
  input.strut_font_family = "Roboto";
  input.strut_font_size = 10.0;
  input.strut_font_weight = 400;
  input.strut_height_multiple = 1.0;

  venus_text_layout::ParagraphRunView text;
  text.text = u"x";
  text.font_family = "Roboto";
  text.font_size = 10.0;
  text.height_multiple = 1.0;
  input.runs.push_back(text);

  venus_text_layout::ParagraphMetrics metrics;
  std::unique_ptr<txt::Paragraph> artifact;
  ASSERT_TRUE(venus_text_layout::LayoutParagraphCore(
                  input, CloneForWorker(), /*impeller_enabled=*/false,
                  &metrics, &artifact)
                  .ok());
  ASSERT_EQ(metrics.lines.size(), 1u);
  EXPECT_DOUBLE_EQ(metrics.lines[0].height, 10.0);
  EXPECT_DOUBLE_EQ(metrics.height, 10.0);

  venus_text_layout::ParagraphRunView line_break;
  line_break.is_line_break = true;
  input.runs.push_back(line_break);
  venus_text_layout::ParagraphRunView placeholder;
  placeholder.is_placeholder = true;
  placeholder.placeholder_width = 10.0;
  placeholder.placeholder_height = 10.0;
  placeholder.placeholder_alignment = 3u;
  input.runs.push_back(placeholder);

  metrics = {};
  artifact.reset();
  ASSERT_TRUE(venus_text_layout::LayoutParagraphCore(
                  input, CloneForWorker(), /*impeller_enabled=*/false,
                  &metrics, &artifact)
                  .ok());
  ASSERT_EQ(metrics.lines.size(), 2u);
  EXPECT_DOUBLE_EQ(metrics.lines[0].height, 10.0);
  EXPECT_DOUBLE_EQ(metrics.lines[1].height, 10.0);
  EXPECT_DOUBLE_EQ(metrics.height, 20.0);
  ASSERT_EQ(metrics.fragments.size(), 2u);
  EXPECT_DOUBLE_EQ(metrics.fragments[1].top, 10.0);

  input.runs.back().placeholder_height = 20.0;
  metrics = {};
  artifact.reset();
  ASSERT_TRUE(venus_text_layout::LayoutParagraphCore(
                  input, CloneForWorker(), /*impeller_enabled=*/false,
                  &metrics, &artifact)
                  .ok());
  ASSERT_EQ(metrics.lines.size(), 2u);
  EXPECT_DOUBLE_EQ(metrics.lines[0].height, 10.0);
  EXPECT_DOUBLE_EQ(metrics.lines[1].height, 20.0);
  EXPECT_DOUBLE_EQ(metrics.height, 30.0);
  ASSERT_EQ(metrics.fragments.size(), 2u);
  EXPECT_DOUBLE_EQ(metrics.fragments[1].top, 10.0);
  EXPECT_DOUBLE_EQ(metrics.fragments[1].bottom, 30.0);
}

// CSS 2.1 §10.8.1 `line-height: 0`: 文本行内盒高 0 (half-leading 收成基线附近一点),
// 行盒只由原子盒与零高 strut 撑。Skia 原本把倍数 0 当"未设置"按字体自然度量排 ——
// 判据页 WPT css-grid/alignment/grid-item-no-aspect-ratio-stretch-1 (body line-height:0,
// 270 高 inline-grid 行 Chrome 行距恰为 270)。
TEST_F(VenusParagraphStoreTest, CssZeroLineHeightTextDoesNotGrowTheLineBox) {
  UseRobotoFixture();
  const auto layout = [this](const venus_text_layout::ParagraphInputView& in) {
    venus_text_layout::ParagraphMetrics metrics;
    std::unique_ptr<txt::Paragraph> artifact;
    EXPECT_TRUE(venus_text_layout::LayoutParagraphCore(
                    in, CloneForWorker(), /*impeller_enabled=*/false, &metrics,
                    &artifact)
                    .ok());
    return metrics;
  };
  const auto strut_input = [](double strut_height) {
    venus_text_layout::ParagraphInputView in;
    in.max_width = 1000.0;
    in.strut_enabled = true;
    in.strut_font_family = "Roboto";
    in.strut_font_size = 10.0;
    in.strut_font_weight = 400;
    in.strut_height_multiple = strut_height;
    return in;
  };
  const auto text_run = [](const std::u16string& text, double height,
                           bool declared) {
    venus_text_layout::ParagraphRunView run;
    run.text = text;
    run.font_family = "Roboto";
    run.font_size = 10.0;
    run.height_multiple = height;
    run.height_declared = declared;
    return run;
  };
  venus_text_layout::ParagraphRunView atom;
  atom.is_placeholder = true;
  atom.placeholder_width = 10.0;
  atom.placeholder_height = 270.0;
  atom.placeholder_baseline_offset = 270.0;

  // 对照组先排: 段落缓存里先留下"未声明"那份 run 度量 —— 缓存 key 若不区分
  // 覆盖位, 下一格会命中它读回自然高。
  {
    auto in = strut_input(0.0);
    in.runs = {atom, text_run(u" x", 0.0, false)};
    const auto metrics = layout(in);
    ASSERT_EQ(metrics.lines.size(), 1u);
    EXPECT_GT(metrics.lines[0].height, 270.0)
        << "对照组: 未声明行高的文本按字体自然高度撑高行盒";
  }
  {
    auto in = strut_input(0.0);
    in.runs = {atom, text_run(u" x", 0.0, true)};
    const auto metrics = layout(in);
    ASSERT_EQ(metrics.lines.size(), 1u);
    EXPECT_DOUBLE_EQ(metrics.lines[0].height, 270.0)
        << "零行高文本不抬行盒, 行盒 = 原子盒";
    EXPECT_DOUBLE_EQ(metrics.height, 270.0);
    const auto text_fragment =
        std::find_if(metrics.fragments.begin(), metrics.fragments.end(),
                     [](const venus_text_layout::ParagraphFragmentOut& f) {
                       return f.run_index == 1u;
                     });
    ASSERT_NE(text_fragment, metrics.fragments.end());
    EXPECT_GT(text_fragment->bottom - text_fragment->top, 0.0)
        << "片段矩形仍是字形内容区, 不是零高的 CSS run 盒";
  }
  {
    auto in = strut_input(0.0);
    in.runs = {atom};
    const auto metrics = layout(in);
    ASSERT_EQ(metrics.lines.size(), 1u);
    EXPECT_DOUBLE_EQ(metrics.lines[0].height, 270.0)
        << "只有原子盒的行: 零高 strut 不加下沉";
  }
  // CSS `vertical-align: top` 对齐的是**最终行盒上沿** (CSS 2.1 §10.8.1), 不是
  // Skia kTop 的"字体上沿"。零行高时零高点比字体上沿高 (A-D)/2, kTop 会把行盒往上
  // 撑出这一截 —— 判据页 css-grid stretch-1 test 页真机行距 275 (Chrome 270),
  // 盒顶 23 (Chrome 18)。行高大于 normal 时 half-leading 让两者同样分开。
  {
    auto in = strut_input(0.0);
    venus_text_layout::ParagraphRunView top_atom = atom;
    top_atom.placeholder_alignment = 1u;
    in.runs = {top_atom, text_run(u" x", 0.0, true)};
    const auto metrics = layout(in);
    ASSERT_EQ(metrics.lines.size(), 1u);
    EXPECT_DOUBLE_EQ(metrics.lines[0].height, 270.0)
        << "零行高 + top 占位: 行盒 = 占位盒";
    const auto top_fragment =
        std::find_if(metrics.fragments.begin(), metrics.fragments.end(),
                     [](const venus_text_layout::ParagraphFragmentOut& f) {
                       return f.run_index == 0u;
                     });
    ASSERT_NE(top_fragment, metrics.fragments.end());
    EXPECT_DOUBLE_EQ(top_fragment->top, 0.0) << "top 占位贴行盒上沿";
  }
  {
    auto in = strut_input(3.0);
    venus_text_layout::ParagraphRunView top_atom = atom;
    top_atom.placeholder_alignment = 1u;
    top_atom.placeholder_height = 10.0;
    top_atom.placeholder_baseline_offset = 10.0;
    in.runs = {top_atom, text_run(u" x", 3.0, true)};
    const auto metrics = layout(in);
    ASSERT_EQ(metrics.lines.size(), 1u);
    EXPECT_DOUBLE_EQ(metrics.lines[0].height, 30.0);
    const auto top_fragment =
        std::find_if(metrics.fragments.begin(), metrics.fragments.end(),
                     [](const venus_text_layout::ParagraphFragmentOut& f) {
                       return f.run_index == 0u;
                     });
    ASSERT_NE(top_fragment, metrics.fragments.end());
    EXPECT_DOUBLE_EQ(top_fragment->top, 0.0)
        << "行高 30 (字号 10): top 占位贴行盒上沿, 不是字体上沿 (差一个 half-leading)";
  }
  // 同字体相邻两段, 一段声明 0、一段未声明: Skia 按字体属性合并整形块时只比
  // fHeight (两者都是 0), 合并后未声明那段会被一起压成零高。
  {
    auto normal_only = strut_input(0.0);
    normal_only.runs = {text_run(u"y", 0.0, false)};
    const double natural = layout(normal_only).height;
    ASSERT_GT(natural, 0.0);
    auto mixed = strut_input(0.0);
    mixed.runs = {text_run(u"x", 0.0, true), text_run(u"y", 0.0, false)};
    EXPECT_DOUBLE_EQ(layout(mixed).height, natural)
        << "未声明那段仍按字体自然高度占行";
  }
  // 绘制: 零高 run 的 CSS 盒高 0, 而 Skia 逐样式迭代遇到高 0 的裁剪盒整段跳过
  // (TextLine.cpp `clip.height() == 0 -> continue`) —— 多个样式块 (多个 span) 的
  // 零行高段落字形全丢; 单个样式块走快路径不受影响。真机: 三个 span 一个字都不画。
  const auto painted_width = [this, &strut_input, &text_run](double height) {
    auto in = strut_input(height);
    auto first = text_run(u"AB ", height, true);
    first.color_argb = 0xFF000000u;
    auto second = text_run(u"CD", height, true);
    second.color_argb = 0xFFFF0000u;
    in.runs = {first, second};
    venus_text_layout::ParagraphMetrics metrics;
    std::unique_ptr<txt::Paragraph> artifact;
    EXPECT_TRUE(venus_text_layout::LayoutParagraphCore(
                    in, CloneForWorker(), /*impeller_enabled=*/false, &metrics,
                    &artifact)
                    .ok());
    DisplayListBuilder builder;
    artifact->Paint(&builder, 0, 0);
    return builder.Build()->GetBounds().GetWidth();
  };
  const auto control_width = painted_width(2.0);
  ASSERT_GT(control_width, 0.0f);
  EXPECT_NEAR(painted_width(0.0), control_width, 1.0f)
      << "零行高 + 两个样式块: 两段字形都要画出来";
  // 反判据: 非零行高声明与否逐位相同。
  {
    auto undeclared = strut_input(2.0);
    undeclared.runs = {atom, text_run(u" x", 2.0, false)};
    auto declared = strut_input(2.0);
    declared.runs = {atom, text_run(u" x", 2.0, true)};
    const auto a = layout(undeclared);
    const auto b = layout(declared);
    ASSERT_EQ(a.lines.size(), b.lines.size());
    for (size_t i = 0; i < a.lines.size(); ++i) {
      EXPECT_TRUE(SameBits(a.lines[i].height, b.lines[i].height));
      EXPECT_TRUE(SameBits(a.lines[i].baseline, b.lines[i].baseline));
    }
    ASSERT_EQ(a.fragments.size(), b.fragments.size());
    for (size_t i = 0; i < a.fragments.size(); ++i) {
      EXPECT_TRUE(SameBits(a.fragments[i].top, b.fragments[i].top));
      EXPECT_TRUE(SameBits(a.fragments[i].bottom, b.fragments[i].bottom));
    }
  }
  // 开关关闭 (Flutter 其余段落): 覆盖位 + 高 0 仍等同"未设置", Skia 原行为不变。
  const auto txt_height = [this](bool honor, bool override_height) {
    txt::ParagraphStyle paragraph_style;
    paragraph_style.font_family = "Roboto";
    paragraph_style.font_size = 10.0;
    paragraph_style.honor_zero_height_override = honor;
    auto builder = txt::ParagraphBuilder::CreateSkiaBuilder(
        paragraph_style, CloneForWorker(), /*impeller_enabled=*/false);
    txt::TextStyle text_style = paragraph_style.GetTextStyle();
    text_style.height = 0.0;
    text_style.has_height_override = override_height;
    text_style.half_leading = true;
    builder->PushStyle(text_style);
    builder->AddText(u"x");
    auto paragraph = builder->Build();
    paragraph->Layout(100.0);
    return paragraph->GetHeight();
  };
  EXPECT_DOUBLE_EQ(txt_height(false, true), txt_height(false, false));
  EXPECT_LT(txt_height(true, true), txt_height(false, false));
}

// ---- 2b. publication owner 生命周期 --------------------------------------

TEST_F(VenusParagraphStoreTest,
       OwnedPublicationExceedsLegacyCapacityButWaitsForReleaseAndSeal) {
  constexpr uint64_t kLegacyToken = 9000u;
  constexpr uint64_t kFirstOwnedToken = 10000u;
  constexpr uint32_t kSlot = 7u;
  const uint64_t owner_id = BeginArtifactOwner(/*producer_generation=*/101u);
  ASSERT_NE(owner_id, 0u);
  ASSERT_EQ(ParagraphStore::Instance().BindArtifactOwner(
                kEngineId, kRegistrationGeneration, owner_id),
            kFlutterVenusV2Ok);

  DepositReady(kLegacyToken, 100.0, 1u, "legacy");
  const std::shared_ptr<txt::FontCollection> worker_fonts = CloneForWorker();
  for (uint64_t i = 0u; i < ParagraphStore::kMaxEntries + 1u; ++i) {
    ASSERT_EQ(DepositOwned(kFirstOwnedToken + i, kSlot, owner_id,
                           /*input_charge_bytes=*/1u, worker_fonts),
              kFlutterVenusV2Ok);
  }

  EXPECT_EQ(ParagraphStore::Instance().SizeForTesting(),
            ParagraphStore::kMaxEntries + 2u);
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().evicted_capacity, 0u);
  EXPECT_NE(ParagraphStore::Instance().Claim(kLegacyToken, 100.0, 1u),
            nullptr)
      << "owned entries must not consume or evict the legacy capacity";

  EXPECT_EQ(ParagraphStore::Instance().SealArtifactOwner(
                kEngineId, kRegistrationGeneration, owner_id),
            kFlutterVenusV2NotReady)
      << "a publication cannot seal while its worker slot is still bound";
  ParagraphStore::Instance().NotifySlotReleased(
      kEngineId, kRegistrationGeneration, kSlot, true, owner_id);
  EXPECT_EQ(ParagraphStore::Instance().Claim(kFirstOwnedToken, 100.0, 1u),
            nullptr)
      << "returning the slot alone must not publish an open owner";
  ASSERT_EQ(ParagraphStore::Instance().SealArtifactOwner(
                kEngineId, kRegistrationGeneration, owner_id),
            kFlutterVenusV2Ok);
  EXPECT_NE(ParagraphStore::Instance().Claim(kFirstOwnedToken, 100.0, 1u),
            nullptr);
  EXPECT_NE(ParagraphStore::Instance().Claim(
                kFirstOwnedToken + ParagraphStore::kMaxEntries, 100.0, 1u),
            nullptr);

  ParagraphStore::Instance().ReleaseArtifactOwner(kEngineId, owner_id);
  EXPECT_EQ(ParagraphStore::Instance().SizeForTesting(), 0u);
}

TEST_F(VenusParagraphStoreTest,
       DuplicateTokenFailsCurrentOwnerAndReleaseStaysOwnerScoped) {
  constexpr uint64_t kFirstToken = 20000u;
  constexpr uint64_t kSecondToken = 20001u;
  constexpr uint32_t kFirstSlot = 8u;
  constexpr uint32_t kSecondSlot = 9u;
  const uint64_t first_owner = BeginArtifactOwner(/*producer_generation=*/201u);
  const uint64_t second_owner = BeginArtifactOwner(/*producer_generation=*/202u);
  ASSERT_NE(first_owner, 0u);
  ASSERT_NE(second_owner, 0u);
  ASSERT_EQ(ParagraphStore::Instance().BindArtifactOwner(
                kEngineId, kRegistrationGeneration, first_owner),
            kFlutterVenusV2Ok);
  ASSERT_EQ(ParagraphStore::Instance().BindArtifactOwner(
                kEngineId, kRegistrationGeneration, second_owner),
            kFlutterVenusV2Ok);

  const std::shared_ptr<txt::FontCollection> worker_fonts = CloneForWorker();
  ASSERT_EQ(DepositOwned(kFirstToken, kFirstSlot, first_owner, 1u,
                         worker_fonts),
            kFlutterVenusV2Ok);
  ASSERT_EQ(DepositOwned(kSecondToken, kSecondSlot, second_owner, 1u,
                         worker_fonts),
            kFlutterVenusV2Ok);
  EXPECT_EQ(DepositOwned(kFirstToken, kSecondSlot, second_owner, 1u,
                         worker_fonts),
            kFlutterVenusV2ItemFailed);
  EXPECT_EQ(DepositOwned(kSecondToken + 1u, kSecondSlot, second_owner, 1u,
                         worker_fonts),
            kFlutterVenusV2ItemFailed)
      << "a collision must fail the entire depositing publication";
  EXPECT_EQ(ParagraphStore::Instance().SealArtifactOwner(
                kEngineId, kRegistrationGeneration, second_owner),
            kFlutterVenusV2ItemFailed);

  ParagraphStore::Instance().ReleaseArtifactOwner(kEngineId, second_owner);
  ParagraphStore::Instance().ReleaseArtifactOwner(kEngineId, second_owner);
  EXPECT_EQ(ParagraphStore::Instance().SizeForTesting(), 1u)
      << "release must remove only the selected owner's unclaimed artifact";

  ParagraphStore::Instance().NotifySlotReleased(
      kEngineId, kRegistrationGeneration, kFirstSlot, true, first_owner);
  ASSERT_EQ(ParagraphStore::Instance().SealArtifactOwner(
                kEngineId, kRegistrationGeneration, first_owner),
            kFlutterVenusV2Ok);
  EXPECT_NE(ParagraphStore::Instance().Claim(kFirstToken, 100.0, 1u), nullptr)
      << "the first artifact must survive another owner's token collision";
  ParagraphStore::Instance().ReleaseArtifactOwner(kEngineId, first_owner);
}

TEST_F(VenusParagraphStoreTest,
       OwnerAndProcessBudgetsFailStickyAndEmptyOwnersAreBounded) {
  const uint64_t owner = BeginArtifactOwner(/*producer_generation=*/301u);
  ASSERT_NE(owner, 0u);
  ASSERT_EQ(ParagraphStore::Instance().BindArtifactOwner(
                kEngineId, kRegistrationGeneration, owner),
            kFlutterVenusV2Ok);
  const std::shared_ptr<txt::FontCollection> worker_fonts = CloneForWorker();
  ASSERT_EQ(
      DepositOwned(
          30000u, 10u, owner,
          kFlutterVenusTextLayoutArtifactOwnerV1MaxInputChargeBytesPerOwner,
          worker_fonts),
      kFlutterVenusV2Ok);
  EXPECT_EQ(DepositOwned(30001u, 10u, owner, 1u, worker_fonts),
            kFlutterVenusV2Backpressure);
  EXPECT_EQ(DepositOwned(30002u, 10u, owner, 1u, worker_fonts),
            kFlutterVenusV2ItemFailed);
  EXPECT_EQ(ParagraphStore::Instance().SealArtifactOwner(
                kEngineId, kRegistrationGeneration, owner),
            kFlutterVenusV2ItemFailed);
  ParagraphStore::Instance().ReleaseArtifactOwner(kEngineId, owner);

  const uint64_t process_owner_a =
      BeginArtifactOwner(/*producer_generation=*/302u);
  const uint64_t process_owner_b =
      BeginArtifactOwner(/*producer_generation=*/303u);
  const uint64_t rejected_owner =
      BeginArtifactOwner(/*producer_generation=*/304u);
  ASSERT_NE(process_owner_a, 0u);
  ASSERT_NE(process_owner_b, 0u);
  ASSERT_NE(rejected_owner, 0u);
  ASSERT_EQ(ParagraphStore::Instance().BindArtifactOwner(
                kEngineId, kRegistrationGeneration, process_owner_a),
            kFlutterVenusV2Ok);
  ASSERT_EQ(ParagraphStore::Instance().BindArtifactOwner(
                kEngineId, kRegistrationGeneration, process_owner_b),
            kFlutterVenusV2Ok);
  ASSERT_EQ(ParagraphStore::Instance().BindArtifactOwner(
                kEngineId, kRegistrationGeneration, rejected_owner),
            kFlutterVenusV2Ok);
  ASSERT_EQ(
      DepositOwned(
          30010u, 11u, process_owner_a,
          kFlutterVenusTextLayoutArtifactOwnerV1MaxInputChargeBytesPerOwner,
          worker_fonts),
      kFlutterVenusV2Ok);
  ASSERT_EQ(
      DepositOwned(
          30011u, 12u, process_owner_b,
          kFlutterVenusTextLayoutArtifactOwnerV1MaxInputChargeBytesPerOwner,
          worker_fonts),
      kFlutterVenusV2Ok);
  EXPECT_EQ(DepositOwned(30012u, 13u, rejected_owner, 1u, worker_fonts),
            kFlutterVenusV2Backpressure);
  EXPECT_EQ(DepositOwned(30013u, 13u, rejected_owner, 1u, worker_fonts),
            kFlutterVenusV2ItemFailed);
  ParagraphStore::Instance().ReleaseArtifactOwner(kEngineId, process_owner_a);

  const uint64_t replacement_owner =
      BeginArtifactOwner(/*producer_generation=*/305u);
  ASSERT_NE(replacement_owner, 0u);
  ASSERT_EQ(ParagraphStore::Instance().BindArtifactOwner(
                kEngineId, kRegistrationGeneration, replacement_owner),
            kFlutterVenusV2Ok);
  EXPECT_EQ(DepositOwned(30014u, 14u, replacement_owner, 1u, worker_fonts),
            kFlutterVenusV2Ok)
      << "releasing an owner must return its process budget";
  ParagraphStore::Instance().ReleaseArtifactOwner(kEngineId, process_owner_b);
  ParagraphStore::Instance().ReleaseArtifactOwner(kEngineId, rejected_owner);
  ParagraphStore::Instance().ReleaseArtifactOwner(kEngineId,
                                                   replacement_owner);

  std::vector<uint64_t> empty_owners;
  empty_owners.reserve(
      kFlutterVenusTextLayoutArtifactOwnerV1MaxConcurrentOwners);
  for (uint64_t i = 0u;
       i < kFlutterVenusTextLayoutArtifactOwnerV1MaxConcurrentOwners; ++i) {
    const uint64_t empty_owner = BeginArtifactOwner(400u + i);
    ASSERT_NE(empty_owner, 0u);
    empty_owners.push_back(empty_owner);
  }
  uint64_t over_limit_owner = 99u;
  EXPECT_EQ(ParagraphStore::Instance().BeginArtifactOwner(
                kEngineId, kRegistrationGeneration, 999u,
                &over_limit_owner),
            kFlutterVenusV2Backpressure);
  EXPECT_EQ(over_limit_owner, 0u);
  for (uint64_t empty_owner : empty_owners) {
    ParagraphStore::Instance().ReleaseArtifactOwner(kEngineId, empty_owner);
  }
  const uint64_t after_release =
      BeginArtifactOwner(/*producer_generation=*/1000u);
  EXPECT_NE(after_release, 0u)
      << "releasing empty owners must return the owner-count reservation";
  ParagraphStore::Instance().ReleaseArtifactOwner(kEngineId, after_release);
  EXPECT_EQ(ParagraphStore::Instance().SizeForTesting(), 0u);
}

// ---- 3. 每一条 miss 分类都必须真的会红 ----------------------------------

TEST_F(VenusParagraphStoreTest, MissNotPrewarmedIsCounted) {
  EXPECT_EQ(ParagraphStore::Instance().Claim(999, 100.0, 1u), nullptr);
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().miss_not_prewarmed, 1u);
}

TEST_F(VenusParagraphStoreTest, MissWidthMismatchIsCountedAndKeepsEntry) {
  DepositReady(7, 120.0, 1u, "w");
  EXPECT_EQ(ParagraphStore::Instance().Claim(7, 121.0, 1u), nullptr);
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().miss_width_mismatch, 1u);
  // **不删**：同一 token 下一帧可能又用回原宽度(来回滚动、容器宽度抖动)。
  // 删掉等于把下一次命中也扔了。
  EXPECT_EQ(ParagraphStore::Instance().SizeForTesting(), 1u);
  EXPECT_NE(ParagraphStore::Instance().Claim(7, 120.0, 1u), nullptr)
      << "宽度对不上不该毁掉这一条, 原宽度回来时仍应命中";
}

TEST_F(VenusParagraphStoreTest, MissFontEpochIsCountedAndDropsEntry) {
  DepositReady(8, 120.0, /*font_epoch=*/3u, "f");
  EXPECT_EQ(ParagraphStore::Instance().Claim(8, 120.0, /*font_epoch=*/4u), nullptr);
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().miss_font_epoch, 1u);
  // 字体换代之后这一条永远不可能再命中, 留着只是占内存。
  EXPECT_EQ(ParagraphStore::Instance().SizeForTesting(), 0u);
}

// 分类不能合并：字体换代时若报成 width_mismatch, 读数据的人会以为"宽度老在变",
// 去优化一个不存在的问题。这条用例把两者的区分钉住。
TEST_F(VenusParagraphStoreTest, FontEpochMissIsNotReportedAsWidthMiss) {
  DepositReady(9, 120.0, 3u, "x");
  // 宽度**和**字体代际同时不符 —— 必须报字体那条(它更根本)。
  EXPECT_EQ(ParagraphStore::Instance().Claim(9, 999.0, 4u), nullptr);
  const ParagraphStore::Counters c = ParagraphStore::Instance().GetCounters();
  EXPECT_EQ(c.miss_font_epoch, 1u);
  EXPECT_EQ(c.miss_width_mismatch, 0u);
}

TEST_F(VenusParagraphStoreTest, FontReloadInvalidatesWholeStore) {
  for (uint64_t i = 0; i < 5; ++i) {
    DepositReady(i + 1u, 100.0, /*font_epoch=*/2u, "row " + std::to_string(i));
  }
  ASSERT_EQ(ParagraphStore::Instance().SizeForTesting(), 5u);
  ParagraphStore::Instance().InvalidateForFontEpoch(
      kEngineId, kRegistrationGeneration, /*new_font_epoch=*/3u);
  EXPECT_EQ(ParagraphStore::Instance().SizeForTesting(), 0u);
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().evicted_font_epoch, 5u);
}

TEST_F(VenusParagraphStoreTest, CapacityIsBoundedAndEvictionIsCounted) {
  // 交付表存的是排好版的 paragraph, 不是几个数字。无上限增长会把"省下的
  // shaping 时间"换成"涨上去的常驻内存" —— 那不叫赢。
  for (uint64_t i = 0; i < ParagraphStore::kMaxEntries + 16; ++i) {
    DepositReady(i + 1u, 100.0, 1u, "row " + std::to_string(i));
  }
  EXPECT_LE(ParagraphStore::Instance().SizeForTesting(),
            ParagraphStore::kMaxEntries);
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().evicted_capacity, 16u);
}

TEST_F(VenusParagraphStoreTest, DepositOverSameTokenIsCountedNotSilent) {
  DepositReady(5, 100.0, 1u, "a");
  DepositReady(5, 100.0, 1u, "b");
  EXPECT_EQ(ParagraphStore::Instance().SizeForTesting(), 1u);
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().dropped_on_deposit, 1u)
      << "静默覆盖会让'为什么没命中'无从查起";
}

TEST_F(VenusParagraphStoreTest,
       RegistrationReplacementMakesOldArtifactUnclaimable) {
  DepositReady(61u, 100.0, 1u, "old generation");
  ParagraphStore::Instance().ActivateRegistration(kEngineId,
                                                  kRegistrationGeneration + 1u);
  EXPECT_EQ(ParagraphStore::Instance().Claim(61u, 100.0, 1u), nullptr);
  EXPECT_EQ(
      ParagraphStore::Instance().GetCounters().miss_registration_generation,
      1u);
  EXPECT_EQ(ParagraphStore::Instance().SizeForTesting(), 0u);
}

TEST_F(VenusParagraphStoreTest, LateDepositAfterShutdownGenerationIsDropped) {
  ParagraphStore::Instance().DeactivateRegistration(kEngineId,
                                                    kRegistrationGeneration);
  ParagraphStore::Instance().Deposit(
      62u, 100.0, 1u, kEngineId, kRegistrationGeneration,
      kSnapshotGenerationUnavailable, /*slot=*/0u,
      BuildAndLayout(CloneForWorker(), "late", 100.0));
  EXPECT_EQ(ParagraphStore::Instance().SizeForTesting(), 0u);
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().dropped_stale_registration,
            1u);
}

// ---- 3b. slot 交还规则 --------------------------------------------------

// slot 没交还之前不许交付：那张字体表此刻仍有使用者。
TEST_F(VenusParagraphStoreTest, PendingUntilSlotReleased) {
  constexpr uint32_t kSlot = 3;
  ParagraphStore::Instance().Deposit(
      11, 100.0, 1u, kEngineId, kRegistrationGeneration,
      kSnapshotGenerationUnavailable, kSlot,
      BuildAndLayout(CloneForWorker(), "pending", 100.0));

  EXPECT_EQ(ParagraphStore::Instance().Claim(11, 100.0, 1u), nullptr)
      << "slot 还没交还就交付了 —— 结构保证失效";
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().miss_slot_busy, 1u);
  // **不删**：slot 一交还它就该可认领。
  EXPECT_EQ(ParagraphStore::Instance().SizeForTesting(), 1u);

  ParagraphStore::Instance().NotifySlotReleased(
      kEngineId, kRegistrationGeneration, kSlot, true);
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().released_by_slot, 1u);
  EXPECT_NE(ParagraphStore::Instance().Claim(11, 100.0, 1u), nullptr)
      << "slot 交还之后仍然认领不到";
}

// 交还的是**别的** slot 不算数 —— 否则这条规则形同虚设。
TEST_F(VenusParagraphStoreTest, ReleasingAnotherSlotDoesNotUnlock) {
  ParagraphStore::Instance().Deposit(
      12, 100.0, 1u, kEngineId, kRegistrationGeneration,
      kSnapshotGenerationUnavailable, /*slot=*/2,
      BuildAndLayout(CloneForWorker(), "s2", 100.0));
  ParagraphStore::Instance().NotifySlotReleased(
      kEngineId, kRegistrationGeneration, /*slot=*/5, true);
  EXPECT_EQ(ParagraphStore::Instance().Claim(12, 100.0, 1u), nullptr)
      << "交还别的 slot 就把它放行了 —— 规则被绕过";
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().miss_slot_busy, 1u);
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().released_by_slot, 0u);
}

TEST_F(VenusParagraphStoreTest,
       ReleasingSameSlotFromAnotherRegistrationDoesNotUnlock) {
  constexpr uint32_t kSlot = 4u;
  ParagraphStore::Instance().Deposit(
      13u, 100.0, 1u, kEngineId, kRegistrationGeneration,
      kSnapshotGenerationUnavailable, kSlot,
      BuildAndLayout(CloneForWorker(), "scoped slot", 100.0));
  ParagraphStore::Instance().NotifySlotReleased(
      kEngineId, kRegistrationGeneration + 1u, kSlot, true);
  EXPECT_EQ(ParagraphStore::Instance().Claim(13u, 100.0, 1u), nullptr);
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().miss_slot_busy, 1u);
}

TEST_F(VenusParagraphStoreTest,
       SlotReleasedDuringShutdownDropsPendingArtifact) {
  constexpr uint32_t kSlot = 6u;
  ParagraphStore::Instance().Deposit(
      14u, 100.0, 1u, kEngineId, kRegistrationGeneration,
      kSnapshotGenerationUnavailable, kSlot,
      BuildAndLayout(CloneForWorker(), "cancelled", 100.0));
  ParagraphStore::Instance().NotifySlotReleased(
      kEngineId, kRegistrationGeneration, kSlot, false);
  EXPECT_EQ(ParagraphStore::Instance().SizeForTesting(), 0u);
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().evicted_cancelled, 1u);
}

// 一次交还应放行该 slot 名下的**全部** pending 条目。
TEST_F(VenusParagraphStoreTest, SlotReleaseUnlocksAllItsEntries) {
  constexpr uint32_t kSlot = 1;
  for (uint64_t i = 20; i < 25; ++i) {
    ParagraphStore::Instance().Deposit(
        i, 100.0, 1u, kEngineId, kRegistrationGeneration,
        kSnapshotGenerationUnavailable, kSlot,
        BuildAndLayout(CloneForWorker(), "row " + std::to_string(i), 100.0));
  }
  ParagraphStore::Instance().NotifySlotReleased(
      kEngineId, kRegistrationGeneration, kSlot, true);
  EXPECT_EQ(ParagraphStore::Instance().GetCounters().released_by_slot, 5u);
  int claimed = 0;
  for (uint64_t i = 20; i < 25; ++i) {
    if (ParagraphStore::Instance().Claim(i, 100.0, 1u) != nullptr) claimed += 1;
  }
  EXPECT_EQ(claimed, 5);
}

// ---- 3c. token 一次性 ---------------------------------------------------

// token 认领即消费。不清掉的话，下一帧换了宽度会拿同一个 token 再查一次，
// 把 miss 计数污染成"宽度老在变" —— 而真相是它早就被认领走了。
//
// 这条用例守的是 Paragraph::layout 里那行 `venus_prelayout_token_ = 0u`。
// 交付表这一侧的等价保证是"认领后 erase"，两边一致才不会出现
// "表里没有了但 token 还在查"的状态。
TEST_F(VenusParagraphStoreTest, ClaimedTokenIsConsumedNotReusable) {
  DepositReady(31, 100.0, 1u, "consume me");
  EXPECT_NE(ParagraphStore::Instance().Claim(31, 100.0, 1u), nullptr);

  const ParagraphStore::Counters before = ParagraphStore::Instance().GetCounters();
  // 同一个 token 再查 —— 应记为 not_prewarmed(它确实已经不在表里),
  // **不该**记成 width_mismatch。
  EXPECT_EQ(ParagraphStore::Instance().Claim(31, 999.0, 1u), nullptr);
  const ParagraphStore::Counters after = ParagraphStore::Instance().GetCounters();
  EXPECT_EQ(after.miss_not_prewarmed, before.miss_not_prewarmed + 1);
  EXPECT_EQ(after.miss_width_mismatch, before.miss_width_mismatch)
      << "已被认领走的 token 被记成了宽度不符 —— 会把'为什么没命中'指向错误方向";
}

// ---- 3d. 环境条子 -------------------------------------------------------

namespace {
using venus_text_layout::AmbientPrelayoutNote;

uint64_t Bits(double v) {
  uint64_t b = 0u;
  std::memcpy(&b, &v, sizeof(b));
  return b;
}
}  // namespace

class VenusAmbientNoteTest : public ::testing::Test {
 protected:
  void SetUp() override { AmbientPrelayoutNote::ResetForTesting(); }
};

TEST_F(VenusAmbientNoteTest, TakenOnlyWhenWidthBitsMatch) {
  AmbientPrelayoutNote::Post(77, 3, Bits(120.0), 1);
  uint64_t token = 0, epoch = 0;
  EXPECT_FALSE(AmbientPrelayoutNote::TryTake(Bits(121.0), &token, &epoch))
      << "宽度不符也给了 —— 会画出按别的宽度断行的内容";
  EXPECT_TRUE(AmbientPrelayoutNote::TryTake(Bits(120.0), &token, &epoch));
  EXPECT_EQ(token, 77u);
  EXPECT_EQ(epoch, 3u);
}

// TextPainter 内部那个只含一个空格的 layoutTemplate 恒以 infinity 排版。
// 调用方只在宽度有限时贴条子，于是它永远取不走。这条把"永远"钉住。
TEST_F(VenusAmbientNoteTest, InfinityWidthNeverMatchesAFiniteNote) {
  AmbientPrelayoutNote::Post(88, 1, Bits(200.0), 1);
  uint64_t token = 0, epoch = 0;
  EXPECT_FALSE(AmbientPrelayoutNote::TryTake(
      Bits(std::numeric_limits<double>::infinity()), &token, &epoch))
      << "layoutTemplate 取走了条子 —— 屏幕上会用一个空格的排版画整段文字";
}

TEST_F(VenusAmbientNoteTest, TakeIsOneShot) {
  AmbientPrelayoutNote::Post(99, 1, Bits(150.0), 1);
  uint64_t token = 0, epoch = 0;
  EXPECT_TRUE(AmbientPrelayoutNote::TryTake(Bits(150.0), &token, &epoch));
  EXPECT_FALSE(AmbientPrelayoutNote::TryTake(Bits(150.0), &token, &epoch))
      << "同一张条子被取了两次";
}

// **这条就是"过期条子"那个洞。**
//
// 贴了条子, 但那次排版被 _performTextLayout 开头"输入没变"的守卫跳过了 ——
// 没有人来取。若不撕, 它就留在线程上; 下一个文本节点宽度一旦撞上就会捡走它,
// 画出上一个节点的内容。
//
// 修复前(没有 Clear)这条必然红: 第二次 TryTake 会成功并交出 token 101。
TEST_F(VenusAmbientNoteTest, StaleNoteCannotBeTakenByTheNextNode) {
  const uint64_t note = AmbientPrelayoutNote::Version() + 1;
  AmbientPrelayoutNote::Post(101, 1, Bits(180.0), note);
  // ……这次排版被跳过了，没人来取。调用方在 finally 里撕掉。
  AmbientPrelayoutNote::Clear(note);

  // 下一个文本节点，恰好也是 180 宽。
  uint64_t token = 0, epoch = 0;
  EXPECT_FALSE(AmbientPrelayoutNote::TryTake(Bits(180.0), &token, &epoch))
      << "过期条子被下一个节点捡走了 —— 它会画出上一个节点的内容";
}

// 撕只撕自己贴的那张。版本号对不上说明已经被别人换掉了 ——
// 这时候撕，撕掉的是别人正等着用的那张。
TEST_F(VenusAmbientNoteTest, ClearOnlyRemovesItsOwnNote) {
  AmbientPrelayoutNote::Post(111, 1, Bits(100.0), /*version=*/5);
  AmbientPrelayoutNote::Post(222, 1, Bits(100.0), /*version=*/6);  // 别人换上的
  AmbientPrelayoutNote::Clear(/*version=*/5);  // 迟到的撕

  uint64_t token = 0, epoch = 0;
  EXPECT_TRUE(AmbientPrelayoutNote::TryTake(Bits(100.0), &token, &epoch))
      << "迟到的 Clear 把别人的条子撕了";
  EXPECT_EQ(token, 222u);
}

// ---- 3e. 导出的 C 入口 --------------------------------------------------
//
// 这两个符号是 Venus 侧唯一能贴条子的地方（应用代码对着禁改的正式 SDK 分析，
// 用不了 dart:ui 上的新 API）。它们错了，采用路径就整条不通或者静默画错。

class VenusPrelayoutNoteCApiTest : public ::testing::Test {
 protected:
  void SetUp() override { AmbientPrelayoutNote::ResetForTesting(); }
};

TEST_F(VenusPrelayoutNoteCApiTest, PostThenTakeRoundTrips) {
  const uint64_t v = FlutterVenusTextLayoutPostPrelayoutNoteV2(42, 7, 260.0);
  EXPECT_NE(v, 0u);
  uint64_t token = 0, epoch = 0;
  ASSERT_TRUE(AmbientPrelayoutNote::TryTake(Bits(260.0), &token, &epoch));
  EXPECT_EQ(token, 42u);
  EXPECT_EQ(epoch, 7u);
}

// 宽度无限时**不贴**。Release 没有 assert，这条边界只能由 C 入口自己守。
TEST_F(VenusPrelayoutNoteCApiTest, InfiniteWidthPostsNothing) {
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(FlutterVenusTextLayoutPostPrelayoutNoteV2(42, 7, inf), 0u)
      << "宽度无限也贴了 —— layoutTemplate 会取走它, 画出一个空格的排版";
  uint64_t token = 0, epoch = 0;
  EXPECT_FALSE(AmbientPrelayoutNote::TryTake(Bits(inf), &token, &epoch));
}

TEST_F(VenusPrelayoutNoteCApiTest, ZeroTokenPostsNothing) {
  EXPECT_EQ(FlutterVenusTextLayoutPostPrelayoutNoteV2(0, 7, 100.0), 0u);
}

// 撕干净: 贴了没人取, 撕掉之后同宽度的下一个节点取不到。
TEST_F(VenusPrelayoutNoteCApiTest, ClearRemovesTheNoteItPosted) {
  const uint64_t v = FlutterVenusTextLayoutPostPrelayoutNoteV2(9, 1, 300.0);
  FlutterVenusTextLayoutClearPrelayoutNoteV2(v);
  uint64_t token = 0, epoch = 0;
  EXPECT_FALSE(AmbientPrelayoutNote::TryTake(Bits(300.0), &token, &epoch))
      << "撕过的条子还能被取走";
}

// Clear(0) 必须是空操作 —— 调用方拿到 0 说明压根没贴, 此时若去撕
// "当前版本", 撕掉的是别人的条子。
//
// 守住它的是 AmbientPrelayoutNote::Clear 里的版本比对, 不是 C 入口里的判空
// (那道判空经变异测试证明是死代码, 已删)。
TEST_F(VenusPrelayoutNoteCApiTest, ClearZeroIsANoOp) {
  AmbientPrelayoutNote::Post(55, 1, Bits(400.0), /*version=*/1);
  FlutterVenusTextLayoutClearPrelayoutNoteV2(0);
  uint64_t token = 0, epoch = 0;
  EXPECT_TRUE(AmbientPrelayoutNote::TryTake(Bits(400.0), &token, &epoch))
      << "Clear(0) 把别人的条子撕了";
  EXPECT_EQ(token, 55u);
}

// 计数数组的下标是**公共契约**（头文件里写死的顺序），调用方按下标取。
// 有人重排 Counters 的字段声明就会静默漂移: 证据里的 miss 分类全串位,
// 而那正是判断"这个 Feature 有没有真命中"的唯一依据。
TEST_F(VenusParagraphStoreTest, CounterArrayOrderMatchesTheDocumentedContract) {
  ParagraphStore::Instance().ResetCounters();
  DepositReady(1, 100.0, /*font_epoch=*/1u, "abc");
  EXPECT_EQ(ParagraphStore::Instance().Claim(9, 100.0, 1), nullptr);   // 未预排
  EXPECT_EQ(ParagraphStore::Instance().Claim(1, 101.0, 1), nullptr);   // 宽度不符
  EXPECT_NE(ParagraphStore::Instance().Claim(1, 100.0, 1), nullptr);   // 命中

  uint64_t got[10] = {};
  ASSERT_EQ(FlutterVenusTextLayoutCopyPrelayoutStoreCountersV2(got, 10), 10u);
  EXPECT_EQ(got[0], 1u) << "[0] deposited";
  EXPECT_EQ(got[1], 1u) << "[1] claimed";
  EXPECT_EQ(got[2], 1u) << "[2] miss_not_prewarmed";
  EXPECT_EQ(got[3], 1u) << "[3] miss_width_mismatch";
  EXPECT_EQ(got[6], 1u) << "[6] released_by_slot";
  ParagraphStore::Instance().Clear();
  ParagraphStore::Instance().ResetCounters();
}

TEST_F(VenusParagraphStoreTest, CounterArrayReportsCapacityWhenAskedWithNull) {
  EXPECT_EQ(FlutterVenusTextLayoutCopyPrelayoutStoreCountersV2(nullptr, 0), 10u);
  uint64_t two[2] = {};
  EXPECT_EQ(FlutterVenusTextLayoutCopyPrelayoutStoreCountersV2(two, 2), 2u)
      << "capacity 不足时应写满前 capacity 个并如实返回, 不是失败也不是越界写";
}

// ---- 3f. font-family 链 --------------------------------------------------
//
// 线材上 `font_family` 那一段承载的是**整条 CSS font-family 链**(US 连接)。
// 拆错的后果分两种, 都很难查:
//   - 拆漏了 fallback -> 主字体缺字形时 worker 用了别的字形 -> 宽度不对 -> 画错;
//   - 老调用方(单个字体名)被拆成空 -> 整批用默认字体 -> 同样画错。
// 所以逐条钉。

TEST(VenusFontFamilyChainTest, SingleNameStaysSingle) {
  // 向后兼容: 老调用方送的是一个名字, 里面没有分隔符。
  const std::vector<std::string> got =
      venus_text_layout::SplitFontFamiliesForTesting("VenusMvpRoboto");
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], "VenusMvpRoboto");
}

TEST(VenusFontFamilyChainTest, ChainKeepsOrder) {
  // 顺序就是回落顺序 —— 反了就是"主字体没生效"。
  const std::vector<std::string> got = venus_text_layout::SplitFontFamiliesForTesting(
      std::string("PingFang SC") + '\x1f' + "Helvetica" + '\x1f' + "Arial");
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0], "PingFang SC");
  EXPECT_EQ(got[1], "Helvetica");
  EXPECT_EQ(got[2], "Arial");
}

TEST(VenusFontFamilyChainTest, EmptyPiecesAreDropped) {
  // 首尾/连续分隔符不该变成空字体名 —— 空名字会让 SkParagraph 回落到系统默认,
  // 而那正是"看着有字但宽度不对"的来源。
  const std::string joined =
      std::string("\x1f") + "A" + '\x1f' + '\x1f' + "B" + '\x1f';
  const std::vector<std::string> got =
      venus_text_layout::SplitFontFamiliesForTesting(joined);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0], "A");
  EXPECT_EQ(got[1], "B");
}

// **这条打在交付物那一层。** 上面三条只测拆分函数 —— 变异「只取链首、
// fallback 全丢」时它们全部照绿, 而那正是这次改动的核心行为。
// 核心与本用例走同一个入口 ApplyFontFamilyChain, 漏不掉。
TEST(VenusFontFamilyChainTest, ChainReachesBothStyles) {
  txt::ParagraphStyle paragraph_style;
  txt::TextStyle text_style;
  venus_text_layout::ApplyFontFamilyChain(
      std::string("PingFang SC") + '\x1f' + "Helvetica" + '\x1f' + "Arial",
      &paragraph_style, &text_style);

  // ParagraphStyle 只收一个默认字体 —— 链首。
  EXPECT_EQ(paragraph_style.font_family, "PingFang SC");
  // TextStyle 收整条链 —— 主字体缺字形时按顺序回落。**丢了 fallback 就是画错**。
  ASSERT_EQ(text_style.font_families.size(), 3u)
      << "fallback 没进到排版里 —— 主字体缺字形时会用别的字形, 宽度就不对了";
  EXPECT_EQ(text_style.font_families[0], "PingFang SC");
  EXPECT_EQ(text_style.font_families[1], "Helvetica");
  EXPECT_EQ(text_style.font_families[2], "Arial");
}

TEST(VenusFontFamilyChainTest, SingleNameReachesBothStyles) {
  txt::ParagraphStyle paragraph_style;
  txt::TextStyle text_style;
  venus_text_layout::ApplyFontFamilyChain("VenusMvpRoboto", &paragraph_style,
                                          &text_style);
  EXPECT_EQ(paragraph_style.font_family, "VenusMvpRoboto");
  ASSERT_EQ(text_style.font_families.size(), 1u);
  EXPECT_EQ(text_style.font_families[0], "VenusMvpRoboto");
}

TEST(VenusFontFamilyChainTest, EmptyInputGivesEmptyChain) {
  EXPECT_TRUE(venus_text_layout::SplitFontFamiliesForTesting("").empty());
}

// ---- 4. 多 worker 并发存 + 主线程认领 -----------------------------------

TEST_F(VenusParagraphStoreTest, ConcurrentDepositsAndClaims) {
  constexpr int kWorkers = 8;
  constexpr int kPerWorker = 20;
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (int w = 0; w < kWorkers; ++w) {
    workers.emplace_back([&, w] {
      std::shared_ptr<txt::FontCollection> fonts = CloneForWorker();
      for (int i = 0; i < kPerWorker; ++i) {
        const uint64_t token = static_cast<uint64_t>(w) * 1000 + i + 1u;
        ParagraphStore::Instance().Deposit(
            token, 150.0, 1u, kEngineId, kRegistrationGeneration,
            kSnapshotGenerationUnavailable, static_cast<uint32_t>(w),
            BuildAndLayout(fonts, "row " + std::to_string(token), 150.0));
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  // 每个 worker 交还自己的 slot —— 生产里这是 release_slot 触发的。
  for (int w = 0; w < kWorkers; ++w) {
    ParagraphStore::Instance().NotifySlotReleased(
        kEngineId, kRegistrationGeneration, static_cast<uint32_t>(w), true);
  }

  int claimed = 0;
  for (int w = 0; w < kWorkers; ++w) {
    for (int i = 0; i < kPerWorker; ++i) {
      const uint64_t token = static_cast<uint64_t>(w) * 1000 + i + 1u;
      if (ParagraphStore::Instance().Claim(token, 150.0, 1u) != nullptr) {
        claimed += 1;
      }
    }
  }
  EXPECT_EQ(claimed, kWorkers * kPerWorker);
  EXPECT_EQ(ParagraphStore::Instance().SizeForTesting(), 0u);
}

}  // namespace
}  // namespace testing
}  // namespace flutter

int main(int argc, char** argv) {
  fml::icu::InitializeICU("icudtl.dat");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
