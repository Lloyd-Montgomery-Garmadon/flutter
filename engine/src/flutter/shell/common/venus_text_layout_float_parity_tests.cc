// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "flutter/display_list/dl_builder.h"
#include "flutter/fml/icu_util.h"
#include "flutter/lib/ui/text/font_collection.h"
#include "flutter/lib/ui/text/venus_text_layout_batch_oracle.h"
#include "flutter/lib/ui/text/venus_text_layout_paragraph_store.h"
#include "flutter/shell/common/venus_text_layout_registry.h"
#include "flutter/shell/common/venus_text_layout_service.h"
#include "flutter/txt/src/txt/asset_font_manager.h"
#include "flutter/txt/src/txt/paragraph.h"
#include "flutter/txt/src/txt/paragraph_builder.h"
#include "flutter/txt/src/txt/platform.h"
#include "gtest/gtest.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkTypeface.h"

#include "flutter/shell/common/venus_text_layout_parity_test_support.h"

namespace flutter {
namespace testing {
namespace {
namespace wire = venus_text_layout_batch;
using venus_parity::Bits;
using venus_parity::MakeFonts;
using venus_parity::MakeRobotoFonts;
using venus_parity::ParagraphWire;
}  // namespace

namespace {
struct FloatParagraphWire : ParagraphWire {
  std::vector<FlutterVenusTextLayoutLineMetricV2> lines{128};
  std::vector<FlutterVenusTextLayoutFragmentRectV2> fragments{128};

  explicit FloatParagraphWire(std::string contents) {
    text = std::move(contents);
    run.text_color_argb = 0xff000000u;
    input.text = reinterpret_cast<const uint8_t*>(text.data());
    input.text_length = run.text_length = static_cast<uint32_t>(text.size());
    input.line_metrics_out = lines.data();
    input.line_metric_capacity = static_cast<uint32_t>(lines.size());
    input.fragment_rects_out = fragments.data();
    input.fragment_rect_capacity = static_cast<uint32_t>(fragments.size());
  }
};

struct FloatProfileSession {
  const uint64_t engine;
  uint64_t generation = 0;
  FlutterVenusTextLayoutApiV2 api{};
  uint32_t slot = 0;
  uint64_t lease = 0;
  bool valid = false;

  explicit FloatProfileSession(
      uint64_t id,
      std::shared_ptr<FontCollection> fonts = MakeFonts())
      : engine(id) {
    if (!fonts) {
      return;
    }
    generation = VenusTextLayoutRegistry::Register(engine, std::move(fonts), false);
    if (generation != 0 &&
        VenusTextLayoutRegistry::GetApi(engine, sizeof(api), &api) ==
            kFlutterVenusV2Ok) {
      valid = api.acquire_slot(api.context, &slot, &lease) == kFlutterVenusV2Ok;
    }
  }
  ~FloatProfileSession() {
    if (lease != 0) {
      EXPECT_EQ(api.release_slot(api.context, slot, lease), kFlutterVenusV2Ok);
    }
    if (api.context != nullptr) {
      api.release_context(api.context);
    }
    VenusTextLayoutRegistry::Unregister(engine, generation);
  }
  int32_t Layout(FloatParagraphWire& paragraph,
                 const VenusParagraphExclusionsV1* profile) {
    paragraph.output.struct_size = sizeof(paragraph.output);
    return FlutterVenusTextLayoutParagraphWithExclusionsV1(
        engine, slot, lease, &paragraph.input, profile, &paragraph.output);
  }
};

VenusParagraphExclusionsV1 FloatProfile(
    const VenusFloatExclusionV1* rects = nullptr,
    uint32_t count = 0) {
  return {sizeof(VenusParagraphExclusionsV1), 1u, count, 0u, rects};
}
}  // namespace

TEST(VenusTextLayoutParityV2, FloatProfileV1TrustBoundaryAndEmptyParity) {
  FloatProfileSession session(0x5741564531460001ull);
  ASSERT_TRUE(session.valid);
  FloatParagraphWire paragraph(
      "one two three four five six seven eight nine ten");
  auto empty = FloatProfile();
  ASSERT_EQ(FlutterVenusTextLayoutParagraphV2(session.engine, session.slot,
                                              session.lease, &paragraph.input,
                                              &paragraph.output),
            kFlutterVenusV2Ok);
  const auto old_output = paragraph.output;
  const auto old_lines = paragraph.lines;
  const auto old_fragments = paragraph.fragments;
  ASSERT_EQ(session.Layout(paragraph, &empty), kFlutterVenusV2Ok);
  EXPECT_EQ(std::memcmp(&old_output, &paragraph.output, sizeof(old_output)), 0);
  EXPECT_EQ(std::memcmp(old_lines.data(), paragraph.lines.data(),
                        old_output.line_metric_count * sizeof(old_lines[0])),
            0);
  EXPECT_EQ(
      std::memcmp(old_fragments.data(), paragraph.fragments.data(),
                  old_output.fragment_rect_count * sizeof(old_fragments[0])),
      0);
  EXPECT_EQ(session.Layout(paragraph, nullptr), kFlutterVenusV2InvalidArgument);
  VenusFloatExclusionV1 rect{0, 40, 0, 80, kVenusFloatExclusionSideLeftV1, 0};
  auto profile = FloatProfile(&rect, 1);
  auto bad = profile;
  bad.struct_size -= 1;
  EXPECT_EQ(session.Layout(paragraph, &bad), kFlutterVenusV2AbiMismatch);
  bad = profile;
  bad.abi_version = 2;
  EXPECT_EQ(session.Layout(paragraph, &bad), kFlutterVenusV2AbiMismatch);
  bad = profile;
  bad.reserved = 1;
  EXPECT_EQ(session.Layout(paragraph, &bad), kFlutterVenusV2InvalidArgument);
  bad = profile;
  bad.exclusions = nullptr;
  EXPECT_EQ(session.Layout(paragraph, &bad), kFlutterVenusV2InvalidArgument);
  bad = profile;
  bad.count = 0;
  EXPECT_EQ(session.Layout(paragraph, &bad), kFlutterVenusV2InvalidArgument);
  const auto valid_rect = rect;
  for (int field = 0; field < 4; ++field) {
    rect = valid_rect;
    double* coordinates[] = {&rect.top, &rect.bottom, &rect.left, &rect.right};
    *coordinates[field] = std::numeric_limits<double>::infinity();
    EXPECT_EQ(session.Layout(paragraph, &profile),
              kFlutterVenusV2InvalidArgument);
  }
  rect = valid_rect;
  rect.top = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(session.Layout(paragraph, &profile),
            kFlutterVenusV2InvalidArgument);
  rect = valid_rect;
  rect.bottom = rect.top;
  EXPECT_EQ(session.Layout(paragraph, &profile),
            kFlutterVenusV2InvalidArgument);
  rect = valid_rect;
  rect.right = rect.left - 1;
  EXPECT_EQ(session.Layout(paragraph, &profile),
            kFlutterVenusV2InvalidArgument);
  rect = valid_rect;
  rect.side = 0;
  EXPECT_EQ(session.Layout(paragraph, &profile),
            kFlutterVenusV2InvalidArgument);
  rect = valid_rect;
  rect.reserved = 1;
  EXPECT_EQ(session.Layout(paragraph, &profile),
            kFlutterVenusV2InvalidArgument);
  rect = valid_rect;
  rect.top = -10;  // A float may begin in an earlier paragraph.
  ASSERT_EQ(session.Layout(paragraph, &profile), kFlutterVenusV2Ok);
  EXPECT_EQ(paragraph.lines.front().left, 80);
}

TEST(VenusTextLayoutParityV2, FloatProfileV1RestoresWidthBelowBothFloatSides) {
  FloatProfileSession session(0x5741564531460002ull);
  ASSERT_TRUE(session.valid);
  const std::string text =
      "one two three four five six seven eight nine ten eleven twelve "
      "one two three four five six seven eight nine ten eleven twelve";
  FloatParagraphWire plain(text);
  ASSERT_EQ(FlutterVenusTextLayoutParagraphV2(session.engine, session.slot,
                                              session.lease, &plain.input,
                                              &plain.output),
            kFlutterVenusV2Ok);
  const double bottom = plain.lines.front().height * 2;
  for (uint32_t side : {uint32_t(kVenusFloatExclusionSideLeftV1),
                        uint32_t(kVenusFloatExclusionSideRightV1)}) {
    SCOPED_TRACE(side);
    FloatParagraphWire paragraph(text);
    VenusFloatExclusionV1 rect{
        0, bottom, side == 1 ? 0.0 : 100.0, side == 1 ? 100.0 : 200.0, side, 0};
    auto profile = FloatProfile(&rect, 1);
    ASSERT_EQ(session.Layout(paragraph, &profile), kFlutterVenusV2Ok);
    ASSERT_EQ(paragraph.output.truncated, 0u);
    ASSERT_GT(paragraph.output.line_metric_count, 2u);
    EXPECT_DOUBLE_EQ(paragraph.lines[0].left, side == 1 ? 100 : 0);
    EXPECT_LE(paragraph.lines[0].width, 100);
    bool saw_full_width = false;
    for (uint32_t i = 2; i < paragraph.output.line_metric_count; ++i) {
      if (paragraph.lines[i].width > 100) {
        saw_full_width = true;
        EXPECT_DOUBLE_EQ(paragraph.lines[i].left, 0);
      }
    }
    EXPECT_TRUE(saw_full_width);
    // Same text/fonts/full width but a moved band must not reuse old geometry.
    rect.top = bottom;
    rect.bottom = bottom * 2;
    ASSERT_EQ(session.Layout(paragraph, &profile), kFlutterVenusV2Ok);
    EXPECT_DOUBLE_EQ(paragraph.lines.front().left, 0);
    EXPECT_GT(paragraph.lines.front().width, 100);
  }
}

TEST(VenusTextLayoutParityV2, FloatProfileV1ActualHeightAndClearance) {
  FloatProfileSession session(0x5741564531460003ull);
  ASSERT_TRUE(session.valid);
  FloatParagraphWire paragraph("x");
  FlutterVenusTextLayoutRunV2 runs[] = {paragraph.run, {}};
  runs[1].struct_size = sizeof(runs[1]);
  runs[1].kind = kFlutterVenusV2RunKindPlaceholder;
  runs[1].placeholder_width = 30;
  runs[1].placeholder_height = 70;
  runs[1].placeholder_baseline_offset = 70;
  paragraph.input.runs = runs;
  paragraph.input.run_count = 2;
  VenusFloatExclusionV1 rect{20, 90, 0, 100, kVenusFloatExclusionSideLeftV1, 0};
  auto profile = FloatProfile(&rect, 1);
  ASSERT_EQ(session.Layout(paragraph, &profile), kFlutterVenusV2Ok);
  ASSERT_EQ(paragraph.output.line_metric_count, 1u);
  EXPECT_GE(paragraph.lines.front().height, 70);
  EXPECT_DOUBLE_EQ(paragraph.lines.front().left, 100);

  // The first unbreakable word fits the full width, but not the one-pixel band.
  FloatParagraphWire clearance("MMMM");
  rect = {0, 60, 0, 199, kVenusFloatExclusionSideLeftV1, 0};
  ASSERT_EQ(session.Layout(clearance, &profile), kFlutterVenusV2Ok);
  ASSERT_EQ(clearance.output.line_metric_count, 1u);
  EXPECT_DOUBLE_EQ(clearance.lines.front().left, 0);
  EXPECT_GE(clearance.lines.front().baseline, 60);
  EXPECT_DOUBLE_EQ(clearance.output.first_baseline,
                   clearance.lines.front().baseline);

  // CSS normal/normal does not permit an emergency break inside a word.
  // Measure the real Roboto glyphs first; the corpus must exceed the full band.
  FloatProfileSession roboto(0x5741564531460092ull, MakeRobotoFonts());
  ASSERT_TRUE(roboto.valid);
  const std::string long_word =
      "SupercalifragilisticexpialidociousSupercalifragilisticexpialidocious";
  FloatParagraphWire unbounded(long_word);
  unbounded.input.max_width = 1e7;
  ASSERT_EQ(FlutterVenusTextLayoutParagraphV2(
                roboto.engine, roboto.slot, roboto.lease, &unbounded.input,
                &unbounded.output),
            kFlutterVenusV2Ok);
  ASSERT_EQ(unbounded.output.line_metric_count, 1u);
  const double word_width = unbounded.lines.front().width;
  ASSERT_GT(word_width, 200.0);

  // The default Flutter path retains its existing cluster wrapping.
  // This control must use Flutter's default builder. ParagraphV2 is the Venus
  // CSS entry and has deliberately opted into word-boundary wrapping.
  auto default_fonts = MakeRobotoFonts();
  ASSERT_NE(default_fonts, nullptr);
  txt::ParagraphStyle default_style;
  default_style.font_family = unbounded.family;
  default_style.font_size = unbounded.run.font_size;
  auto default_builder = txt::ParagraphBuilder::CreateSkiaBuilder(
      default_style, default_fonts->GetFontCollection(), false);
  ASSERT_NE(default_builder, nullptr);
  default_builder->AddText(reinterpret_cast<const uint8_t*>(long_word.data()),
                           long_word.size());
  auto default_wrapping = default_builder->Build();
  ASSERT_NE(default_wrapping, nullptr);
  default_wrapping->Layout(200.0);
  EXPECT_GT(default_wrapping->GetLineMetrics().size(), 1u);
  EXPECT_LE(default_wrapping->GetLineMetrics().front().width, 200.0);

  FloatParagraphWire overflowing(long_word);
  rect = {0, 50, 0, 50, kVenusFloatExclusionSideLeftV1, 0};
  ASSERT_EQ(roboto.Layout(overflowing, &profile), kFlutterVenusV2Ok);
  EXPECT_EQ(overflowing.output.truncated, 0u);
  ASSERT_EQ(overflowing.output.line_metric_count, 1u);
  EXPECT_DOUBLE_EQ(overflowing.lines.front().left, 0.0);
  EXPECT_GE(overflowing.lines.front().baseline - overflowing.lines.front().ascent,
            50.0);
  EXPECT_GT(overflowing.lines.front().width, 200.0);
  EXPECT_NEAR(overflowing.lines.front().width, word_width, 0.01);
  for (uint32_t i = 0; i < overflowing.output.fragment_rect_count; ++i) {
    EXPECT_EQ(overflowing.fragments[i].line_number, 0u);
  }

  // An earlier legal word stays beside the float; the long word clears intact.
  FloatParagraphWire preceded("x " + long_word);
  ASSERT_EQ(roboto.Layout(preceded, &profile), kFlutterVenusV2Ok);
  ASSERT_EQ(preceded.output.line_metric_count, 2u);
  EXPECT_DOUBLE_EQ(preceded.lines[0].left, 50.0);
  EXPECT_DOUBLE_EQ(preceded.lines[1].left, 0.0);
  EXPECT_GE(preceded.lines[1].baseline - preceded.lines[1].ascent, 50.0);
  EXPECT_NEAR(preceded.lines[1].width, word_width, 0.01);

  // Legal word/ZWSP boundaries continue to wrap beside the float.
  for (const std::string& text : {
           std::string("MMMMMMMM MMMMMMMM MMMMMMMM"),
           std::string("MMMMMMMM\xE2\x80\x8BMMMMMMMM\xE2\x80\x8BMMMMMMMM")}) {
    SCOPED_TRACE(text);
    FloatParagraphWire breakable(text);
    breakable.input.paragraph_height_multiple = 1.25;
    breakable.run.height = 1.25;
    ASSERT_EQ(roboto.Layout(breakable, &profile), kFlutterVenusV2Ok);
    EXPECT_EQ(breakable.output.truncated, 0u);
    EXPECT_GT(breakable.output.line_metric_count, 1u);
    EXPECT_DOUBLE_EQ(breakable.lines.front().left, 50.0);
    EXPECT_NEAR(breakable.lines.front().baseline - breakable.lines.front().ascent,
                0.0, 0.01);
    EXPECT_LE(breakable.lines.front().width, 150.0);
  }

  FloatParagraphWire hard_break("MMMM\nMMMM");
  hard_break.run.whitespace_mode = kFlutterVenusV2WhitespacePreserve;
  ASSERT_EQ(roboto.Layout(hard_break, &profile), kFlutterVenusV2Ok);
  EXPECT_EQ(hard_break.output.line_metric_count, 2u);
}

TEST(VenusTextLayoutParityV2, FloatProfileV1NoWrapKeepsSingleLine) {
  FloatProfileSession session(0x5741564531460004ull);
  ASSERT_TRUE(session.valid);
  FloatParagraphWire paragraph(
      "one two three four five six seven eight nine ten");
  paragraph.input.soft_wrap = 0;
  VenusFloatExclusionV1 rect{0, 50, 0, 100, kVenusFloatExclusionSideLeftV1, 0};
  auto profile = FloatProfile(&rect, 1);
  ASSERT_EQ(session.Layout(paragraph, &profile), kFlutterVenusV2Ok);
  EXPECT_EQ(paragraph.output.line_metric_count, 1u);
  EXPECT_GT(paragraph.lines.front().width, paragraph.input.max_width);

  // nowrap overflows from the available first-line origin instead of clearing.
  FloatProfileSession roboto(0x5741564531460093ull, MakeRobotoFonts());
  ASSERT_TRUE(roboto.valid);
  rect.right = 50;
  FloatParagraphWire nowrap(
      "mixed English words that should wrap around the float on the left side");
  nowrap.input.soft_wrap = 0;
  nowrap.input.paragraph_height_multiple = 1.25;
  nowrap.run.height = 1.25;
  ASSERT_EQ(roboto.Layout(nowrap, &profile), kFlutterVenusV2Ok);
  EXPECT_EQ(nowrap.output.truncated, 0u);
  ASSERT_EQ(nowrap.output.line_metric_count, 1u);
  EXPECT_DOUBLE_EQ(nowrap.lines.front().left, 50.0);
  EXPECT_NEAR(nowrap.lines.front().baseline - nowrap.lines.front().ascent,
              0.0, 0.01);
  EXPECT_GT(nowrap.lines.front().width, 200.0);

  // Both public encodings of pre preserve spaces/LF and suppress soft wrapping.
  for (bool paragraph_no_wrap : {false, true}) {
    SCOPED_TRACE(paragraph_no_wrap);
    FloatParagraphWire pre("mixed English words   keep spaces\n"
                           "second explicit line   keeps spaces");
    pre.input.soft_wrap = paragraph_no_wrap ? 0 : 1;
    pre.run.whitespace_mode = paragraph_no_wrap
                                  ? kFlutterVenusV2WhitespacePreserve
                                  : kFlutterVenusV2WhitespacePreserveNoSoftWrap;
    pre.input.paragraph_height_multiple = 1.25;
    pre.run.height = 1.25;
    rect.bottom = 10;
    ASSERT_EQ(roboto.Layout(pre, &profile), kFlutterVenusV2Ok);
    EXPECT_EQ(pre.output.truncated, 0u);
    ASSERT_EQ(pre.output.line_metric_count, 2u);
    EXPECT_DOUBLE_EQ(pre.lines[0].left, 50.0);
    EXPECT_NEAR(pre.lines[0].baseline - pre.lines[0].ascent, 0.0, 0.01);
    EXPECT_DOUBLE_EQ(pre.lines[1].left, 0.0);
    EXPECT_NEAR(pre.lines[1].baseline - pre.lines[1].ascent, 20.0, 0.01);
    EXPECT_DOUBLE_EQ(pre.lines[0].height, 20.0);
  }
}

TEST(VenusTextLayoutParityV2, FloatProfileV1FiniteDoubleScalarBoundary) {
  FloatProfileSession session(0x5741564531460006ull);
  ASSERT_TRUE(session.valid);
  const double huge = std::numeric_limits<double>::max();
  ASSERT_TRUE(std::isfinite(huge));

  // Huge coordinates that clip to a representable band remain valid. In
  // particular, a negative top is not a reason to reject an earlier float.
  FloatParagraphWire clipped("MMMM");
  VenusFloatExclusionV1 rect{
      -huge, 60, -huge, 80, kVenusFloatExclusionSideLeftV1, 0};
  auto profile = FloatProfile(&rect, 1);
  ASSERT_EQ(session.Layout(clipped, &profile), kFlutterVenusV2Ok);
  ASSERT_EQ(clipped.output.line_metric_count, 1u);
  EXPECT_DOUBLE_EQ(clipped.lines.front().left, 80);
  EXPECT_TRUE(std::isfinite(clipped.output.height));
  EXPECT_TRUE(std::isfinite(clipped.lines.front().baseline));

  // A fully blocked line must clear this float. Its finite double bottom cannot
  // be represented by SkScalar: report a layout failure rather than converting
  // to inf or silently laying text inside the exclusion at y=0.
  FloatParagraphWire blocked("MMMM");
  rect = {0, huge, 0, blocked.input.max_width, kVenusFloatExclusionSideLeftV1,
          0};
  EXPECT_EQ(session.Layout(blocked, &profile), kFlutterVenusV2ItemFailed);
  EXPECT_EQ(blocked.output.status, kFlutterVenusV2ItemFailed);
  EXPECT_EQ(blocked.output.line_metric_count, 0u);
  EXPECT_EQ(blocked.output.fragment_rect_count, 0u);
}

TEST(VenusTextLayoutParityV2, FloatProfileV1TextVerticalAlignAfterClearance) {
  FloatProfileSession session(0x5741564531460007ull);
  ASSERT_TRUE(session.valid);
  struct Laid {
    int32_t status;
    FlutterVenusTextLayoutParagraphOutputV2 output;
    FlutterVenusTextLayoutLineMetricV2 line;
    FlutterVenusTextLayoutFragmentRectV2 fragments[2];
  };
  const auto lay = [&](uint16_t align, double line_height,
                       const VenusParagraphExclusionsV1* profile) {
    FloatParagraphWire paragraph("Ab");
    FlutterVenusTextLayoutRunV2 runs[] = {paragraph.run, paragraph.run};
    runs[0].text_offset = 0;
    runs[0].text_length = 1;
    runs[0].font_size = 40;
    runs[1].text_offset = 1;
    runs[1].text_length = 1;
    runs[1].font_size = 16;
    runs[1].text_vertical_align = align;
    if (line_height > 0) {
      runs[0].height_mode = runs[1].height_mode = 2u;
      runs[0].height = runs[1].height = line_height;
    }
    paragraph.input.runs = runs;
    paragraph.input.run_count = 2;
    Laid result{};
    result.status = profile == nullptr
                        ? FlutterVenusTextLayoutParagraphV2(
                              session.engine, session.slot, session.lease,
                              &paragraph.input, &paragraph.output)
                        : session.Layout(paragraph, profile);
    result.output = paragraph.output;
    result.line = paragraph.lines.front();
    for (uint32_t i = 0; i < paragraph.output.fragment_rect_count; ++i) {
      if (paragraph.fragments[i].run_index < 2) {
        result.fragments[paragraph.fragments[i].run_index] =
            paragraph.fragments[i];
      }
    }
    return result;
  };
  constexpr double clearance = 60;
  VenusFloatExclusionV1 rect{
      0, clearance, 0, 199, kVenusFloatExclusionSideLeftV1, 0};
  auto profile = FloatProfile(&rect, 1);
  auto empty = FloatProfile();
  for (uint16_t align : {uint16_t(0), uint16_t(1), uint16_t(3)}) {
    for (double line_height : {0.0, 1.0}) {
      SCOPED_TRACE(align);
      SCOPED_TRACE(line_height);
      const Laid plain = lay(align, line_height, nullptr);
      const Laid empty_profile = lay(align, line_height, &empty);
      ASSERT_EQ(plain.status, kFlutterVenusV2Ok);
      ASSERT_EQ(empty_profile.status, kFlutterVenusV2Ok);
      ASSERT_EQ(plain.output.line_metric_count, 1u);
      ASSERT_EQ(plain.output.fragment_rect_count, 2u);
      EXPECT_EQ(std::memcmp(&plain.output, &empty_profile.output,
                            sizeof(plain.output)),
                0);
      EXPECT_EQ(
          std::memcmp(&plain.line, &empty_profile.line, sizeof(plain.line)), 0);
      EXPECT_EQ(std::memcmp(plain.fragments, empty_profile.fragments,
                            sizeof(plain.fragments)),
                0);

      // Clearing an unbreakable word only translates its native line origin.
      // top/bottom alignment and authored line-height stay local to that line.
      const Laid cleared = lay(align, line_height, &profile);
      ASSERT_EQ(cleared.status, kFlutterVenusV2Ok);
      ASSERT_EQ(cleared.output.line_metric_count, 1u);
      ASSERT_EQ(cleared.output.fragment_rect_count, 2u);
      EXPECT_DOUBLE_EQ(cleared.line.left, plain.line.left);
      EXPECT_DOUBLE_EQ(cleared.line.width, plain.line.width);
      EXPECT_DOUBLE_EQ(cleared.line.height, plain.line.height);
      EXPECT_NEAR(cleared.line.baseline, plain.line.baseline + clearance, 0.01);
      EXPECT_NEAR(cleared.output.first_baseline,
                  plain.output.first_baseline + clearance, 0.01);
      EXPECT_NEAR(cleared.output.height, plain.output.height + clearance, 0.01);
      for (size_t i = 0; i < 2; ++i) {
        EXPECT_DOUBLE_EQ(cleared.fragments[i].left, plain.fragments[i].left);
        EXPECT_DOUBLE_EQ(cleared.fragments[i].right, plain.fragments[i].right);
        EXPECT_NEAR(cleared.fragments[i].top,
                    plain.fragments[i].top + clearance, 0.01);
        EXPECT_NEAR(cleared.fragments[i].bottom,
                    plain.fragments[i].bottom + clearance, 0.01);
      }
    }
  }
}

TEST(VenusTextLayoutParityV2,
     FloatProfileV1ClaimOwnsGeometryForRelayoutPaintAndHit) {
  FloatProfileSession session(0x5741564531460005ull);
  ASSERT_TRUE(session.valid);
  FloatParagraphWire paragraph("MMMM");
  paragraph.input.paragraph_token = 0x5741564531461005ull;
  VenusFloatExclusionV1 rect{0, 80, 0, 80, kVenusFloatExclusionSideLeftV1, 0};
  auto profile = FloatProfile(&rect, 1);
  ASSERT_EQ(session.Layout(paragraph, &profile), kFlutterVenusV2Ok);
  FlutterVenusTextLayoutCountersV2 counters{};
  ASSERT_EQ(session.api.get_counters_v2(session.api.context, sizeof(counters),
                                        &counters),
            kFlutterVenusV2Ok);
  ASSERT_EQ(session.api.release_slot(session.api.context, session.slot,
                                     session.lease),
            kFlutterVenusV2Ok);
  session.lease = 0;
  // Borrowed geometry is invalid immediately after return; claim must retain a
  // copy.
  rect = {0, 0, 0, 0, 0, 0};
  auto claimed = venus_text_layout::ParagraphStore::Instance().Claim(
      paragraph.input.paragraph_token, paragraph.input.max_width,
      counters.font_epoch);
  ASSERT_NE(claimed, nullptr);
  ASSERT_EQ(claimed->GetLineMetrics().size(), 1u);
  EXPECT_DOUBLE_EQ(claimed->GetLineMetrics().front().left, 80);
  claimed->Layout(320);
  const auto& line = claimed->GetLineMetrics().front();
  EXPECT_DOUBLE_EQ(line.left, 80);
  const auto boxes =
      claimed->GetRectsForRange(0, 1, txt::Paragraph::RectHeightStyle::kTight,
                                txt::Paragraph::RectWidthStyle::kTight);
  ASSERT_FALSE(boxes.empty());
  EXPECT_GE(boxes.front().rect.left(), 80);
  EXPECT_EQ(
      claimed->GetGlyphPositionAtCoordinate(81, line.baseline / 2).position,
      0u);
  DisplayListBuilder builder;
  EXPECT_TRUE(claimed->Paint(&builder, 0, 0));
  const auto painted = builder.Build();
  ASSERT_NE(painted, nullptr);
  EXPECT_GT(painted->op_count(), 0u);
  EXPECT_GT(painted->GetBounds().GetLeft(), 50);
}

}  // namespace testing
}  // namespace flutter
