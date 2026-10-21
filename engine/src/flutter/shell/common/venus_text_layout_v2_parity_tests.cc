// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// §8 P3 的「core 唯一」守卫：同一批输入分别走
//   blob 路   —— Phase 1 debug oracle 的 C++ seam (ProcessWithFontCollection)
//   native 路 —— V2 同步 adapter (layout_text_node_sync_v2)
// 两边的每一个 double 必须逐 bit 相同。它证明的不是「算得准」，而是**两条路真的
// 在调用同一个 LayoutTextNodeCore** —— 一旦有人在任一侧偷偷复制一份字段映射或
// shaping，这里立刻红。
//
// 放在 C++ 而不是 Dart：oracle 的 Dart 入口只存在于定制 Engine 的 dart:ui 里，
// 从 Venus 侧引用它会让 `flutter test`（用官方 SDK 的 dart:ui）直接编不过。

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

// 一个 corpus 项。字段集与 §8 P3 的矩阵一一对应；每个 case 都必须满足 Phase 1
// 冻结的组合真值表（locale 非空、soft_wrap=true、noScaling 时 scaler 恰为 1.0、
// fontMetrics 时 height 恰为 0、bounded 时 max_lines ∈ [1,1024]）。
struct Case {
  const char* name;
  std::string text;
  double min_width = 0.0;
  double max_width = 137.75;
  double min_height = 0.0;
  double max_height = 90.25;
  double font_size = 17.0;
  double letter_spacing = 0.3;
  double word_spacing = 0.0;
  double height = 1.4;
  double scaler = 1.0;
  uint32_t font_weight = 400;
  uint32_t max_lines = 3;
  uint16_t direction = 1;      // ltr
  uint16_t align = 5;          // start
  uint16_t font_style = 1;     // normal
  uint16_t leading = 2;        // even
  uint16_t width_basis = 2;    // longestLine
  uint16_t scaler_kind = 1;    // noScaling
  uint16_t max_lines_mode = 2; // bounded
  uint16_t overflow = 2;       // ellipsis
  uint16_t first_ascent = 2;   // true
  uint16_t last_descent = 2;   // true
  uint16_t height_mode = 2;    // multiplier
  std::string locale = "en_US";
};

std::vector<Case> BuildMatrix() {
  std::vector<Case> cases;
  Case base;

  Case wrapped = base;
  wrapped.name = "wrapped loose box";
  wrapped.text = "Venus drives Flutter text shaping across several wrapped words.";
  cases.push_back(wrapped);

  Case clamped = base;
  clamped.name = "height clamped by box";
  clamped.text = "Three visible lines are taller than this deliberately short box.";
  clamped.min_width = 20.25;
  clamped.min_height = 18.5;
  clamped.max_height = 42.25;
  cases.push_back(clamped);

  Case raised = base;
  raised.name = "width raised by box minimum";
  raised.text = "Hi";
  raised.min_width = 80.125;
  raised.min_height = 18.5;
  cases.push_back(raised);

  Case newlines = base;
  newlines.name = "newline and trailing spaces";
  newlines.text = "first line   \nsecond line with trailing spaces   ";
  cases.push_back(newlines);

  Case long_word = base;
  long_word.name = "long unbreakable word";
  long_word.text = "Supercalifragilisticexpialidociousandthensome";
  cases.push_back(long_word);

  Case clipped = base;
  clipped.name = "clip overflow unlimited lines";
  clipped.text = "Clip mode keeps every shaped line without an ellipsis run.";
  clipped.overflow = 1;         // clip
  clipped.max_lines_mode = 1;   // unlimited
  clipped.max_lines = 0;
  cases.push_back(clipped);

  Case rtl = base;
  rtl.name = "rtl direction latin";
  rtl.text = "Latin glyphs laid out in a right to left paragraph.";
  rtl.direction = 2;
  cases.push_back(rtl);

  Case combining = base;
  combining.name = "combining diacritics";
  combining.text = "cafe\xCC\x81 nai\xCC\x88ve re\xCC\x81sume\xCC\x81 combining";
  cases.push_back(combining);

  Case scaled = base;
  scaled.name = "text scaler linear";
  scaled.text = "Linear text scaler multiplies the effective font size.";
  scaled.scaler_kind = 2;
  scaled.scaler = 1.25;
  cases.push_back(scaled);

  Case font_metrics = base;
  font_metrics.name = "font metrics height mode";
  font_metrics.text = "Font metrics height mode ignores the multiplier field.";
  font_metrics.height_mode = 1;
  font_metrics.height = 0.0;
  cases.push_back(font_metrics);

  Case zero_font = font_metrics;
  zero_font.name = "zero font size";
  zero_font.text = "x";
  zero_font.font_size = 0.0;
  cases.push_back(zero_font);

  Case proportional = base;
  proportional.name = "leading proportional";
  proportional.text = "Proportional leading distributes half leading differently.";
  proportional.leading = 1;
  cases.push_back(proportional);

  Case spacing = base;
  spacing.name = "letter and word spacing";
  spacing.text = "Letter and word spacing widen every shaped run.";
  spacing.letter_spacing = 1.75;
  spacing.word_spacing = 2.5;
  cases.push_back(spacing);

  Case parent_basis = base;
  parent_basis.name = "text width basis parent";
  parent_basis.text = "Parent width basis reports the full constraint width.";
  parent_basis.width_basis = 1;
  cases.push_back(parent_basis);

  Case justified = base;
  justified.name = "justify align";
  justified.text = "Justified text stretches interword space across full lines here.";
  justified.align = 4;
  cases.push_back(justified);

  Case italic_bold = base;
  italic_bold.name = "italic bold";
  italic_bold.text = "Italic bold run exercises the style resolution path.";
  italic_bold.font_style = 2;
  italic_bold.font_weight = 700;
  cases.push_back(italic_bold);

  Case height_flags = base;
  height_flags.name = "height flags off";
  height_flags.text = "First ascent and last descent height flags are both off.";
  height_flags.first_ascent = 1;
  height_flags.last_descent = 1;
  cases.push_back(height_flags);

  Case locale_gb = base;
  locale_gb.name = "locale en_GB";
  locale_gb.text = "Locale only changes the shaping locale, not the glyphs.";
  locale_gb.locale = "en_GB";
  cases.push_back(locale_gb);

  Case intrinsics = base;
  intrinsics.name = "wide box intrinsics";
  intrinsics.text = "A wide box makes min and max intrinsic width differ a lot.";
  intrinsics.max_width = 900.5;
  intrinsics.max_height = 400.25;
  cases.push_back(intrinsics);

  Case one_line = base;
  one_line.name = "maxLines one with ellipsis";
  one_line.text = "One shaped line then the ellipsis run takes over completely.";
  one_line.max_lines = 1;
  cases.push_back(one_line);

  // 依赖字体 fallback 的三类。它们在这里是**安全的**：blob 路与 native 路在同一
  // 进程、同一 FontCollection 上跑，fallback 行为两侧完全一致，对拍照样成立。
  // 但按 §8 P3，在 TSan 与压力门证明同一 FontMgr snapshot 并发安全之前，这三类
  // **不得进入设备侧的多 slot 通过项**，只留在单 slot lane。
  Case cjk = base;
  cjk.name = "cjk mixed (fallback)";
  cjk.text = "\xE4\xB8\xAD\xE6\x96\x87\xE6\x8E\x92\xE7\x89\x88 mixed with latin";
  cases.push_back(cjk);

  Case emoji = base;
  emoji.name = "emoji zwj (fallback)";
  emoji.text =
      "family \xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
      "\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6 zwj sequence";
  cases.push_back(emoji);

  Case arabic = base;
  arabic.name = "rtl arabic (fallback)";
  arabic.text = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85";
  arabic.direction = 2;
  cases.push_back(arabic);

  return cases;
}

void PutU16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
  b[off] = static_cast<uint8_t>(v & 0xffu);
  b[off + 1] = static_cast<uint8_t>((v >> 8) & 0xffu);
}
void PutU32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  for (size_t i = 0; i < 4; ++i) {
    b[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xffu);
  }
}
void PutU64(std::vector<uint8_t>& b, size_t off, uint64_t v) {
  std::memcpy(b.data() + off, &v, sizeof(v));
}
void PutF64(std::vector<uint8_t>& b, size_t off, double v) {
  std::memcpy(b.data() + off, &v, sizeof(v));
}

// 把 case 列表编成一份 VTLB，并把每项字符串在 payload 区的位置回填。
std::vector<uint8_t> EncodeVtlb(const std::vector<Case>& cases,
                                const std::string& family,
                                const std::string& ellipsis) {
  const uint32_t count = static_cast<uint32_t>(cases.size());
  const uint32_t table_end = wire::kRequestHeaderSize + count * wire::kRequestItemSize;
  std::string payload;
  struct Ref {
    uint32_t off;
    uint32_t len;
  };
  auto put = [&](const std::string& s) {
    const Ref r{static_cast<uint32_t>(table_end + payload.size()),
                static_cast<uint32_t>(s.size())};
    payload += s;
    return r;
  };
  std::vector<Ref> texts, families, locales, ellipses;
  for (const Case& c : cases) {
    texts.push_back(put(c.text));
    families.push_back(put(family));
    locales.push_back(put(c.locale));
    ellipses.push_back(c.overflow == 2 ? put(ellipsis) : Ref{0u, 0u});
  }
  const uint32_t byte_size = table_end + static_cast<uint32_t>(payload.size());
  std::vector<uint8_t> b(byte_size, 0);
  std::memcpy(b.data(), wire::kRequestMagic, 4);
  PutU16(b, 4, wire::kSchemaVersion);
  PutU16(b, 6, static_cast<uint16_t>(wire::kRequestHeaderSize));
  PutU32(b, 8, byte_size);
  PutU32(b, 16, count);
  PutU32(b, 20, wire::kRequestItemSize);
  PutU32(b, 24, wire::kRequestHeaderSize);
  PutU32(b, 28, table_end);
  PutU32(b, 32, static_cast<uint32_t>(payload.size()));
  PutU64(b, 40, 0x56544C0000000001ull);
  std::memcpy(b.data() + table_end, payload.data(), payload.size());

  for (uint32_t i = 0; i < count; ++i) {
    const Case& c = cases[i];
    const size_t o = wire::kRequestHeaderSize + i * wire::kRequestItemSize;
    PutU32(b, o + wire::request_item::kStructSize, wire::kRequestItemSize);
    PutU64(b, o + wire::request_item::kItemId, 0x1000u + i);
    PutU32(b, o + wire::request_item::kTextOffset, texts[i].off);
    PutU32(b, o + wire::request_item::kTextLength, texts[i].len);
    PutU32(b, o + wire::request_item::kFontFamilyOffset, families[i].off);
    PutU32(b, o + wire::request_item::kFontFamilyLength, families[i].len);
    PutU32(b, o + wire::request_item::kLocaleOffset, locales[i].off);
    PutU32(b, o + wire::request_item::kLocaleLength, locales[i].len);
    PutU32(b, o + wire::request_item::kEllipsisOffset, ellipses[i].off);
    PutU32(b, o + wire::request_item::kEllipsisLength, ellipses[i].len);
    PutF64(b, o + wire::request_item::kMinWidth, c.min_width);
    PutF64(b, o + wire::request_item::kMaxWidth, c.max_width);
    PutF64(b, o + wire::request_item::kMinHeight, c.min_height);
    PutF64(b, o + wire::request_item::kMaxHeight, c.max_height);
    PutF64(b, o + wire::request_item::kFontSize, c.font_size);
    PutF64(b, o + wire::request_item::kLetterSpacing, c.letter_spacing);
    PutF64(b, o + wire::request_item::kWordSpacing, c.word_spacing);
    PutF64(b, o + wire::request_item::kHeight, c.height);
    PutF64(b, o + wire::request_item::kScalerParameter, c.scaler);
    PutU32(b, o + wire::request_item::kFontWeight, c.font_weight);
    PutU32(b, o + wire::request_item::kMaxLines, c.max_lines);
    PutU16(b, o + wire::request_item::kTextEncoding, 1u);
    PutU16(b, o + wire::request_item::kTextDirection, c.direction);
    PutU16(b, o + wire::request_item::kTextAlign, c.align);
    PutU16(b, o + wire::request_item::kFontStyle, c.font_style);
    PutU16(b, o + wire::request_item::kLeadingDistribution, c.leading);
    PutU16(b, o + wire::request_item::kTextWidthBasis, c.width_basis);
    PutU16(b, o + wire::request_item::kTextScalerKind, c.scaler_kind);
    PutU16(b, o + wire::request_item::kMaxLinesMode, c.max_lines_mode);
    PutU16(b, o + wire::request_item::kOverflowMode, c.overflow);
    PutU16(b, o + wire::request_item::kSoftWrap, 2u);
    PutU16(b, o + wire::request_item::kApplyHeightToFirstAscent, c.first_ascent);
    PutU16(b, o + wire::request_item::kApplyHeightToLastDescent, c.last_descent);
    PutU16(b, o + wire::request_item::kHeightMode, c.height_mode);
    PutU16(b, o + wire::request_item::kStrutMode, 1u);
  }
  return b;
}

FlutterVenusTextLayoutInputV2 NativeInput(const Case& c,
                                          const std::string& family,
                                          const std::string& ellipsis) {
  FlutterVenusTextLayoutInputV2 in{};
  in.struct_size = sizeof(in);
  in.abi_version = FLUTTER_VENUS_TEXT_LAYOUT_ABI_V2;
  in.text_length = static_cast<uint32_t>(c.text.size());
  in.font_family_length = static_cast<uint32_t>(family.size());
  in.locale_length = static_cast<uint32_t>(c.locale.size());
  in.ellipsis_length = c.overflow == 2 ? static_cast<uint32_t>(ellipsis.size()) : 0u;
  in.font_weight = c.font_weight;
  in.max_lines = c.max_lines;
  in.text_encoding = 1u;
  in.text_direction = c.direction;
  in.text_align = c.align;
  in.font_style = c.font_style;
  in.leading_distribution = c.leading;
  in.text_width_basis = c.width_basis;
  in.text_scaler_kind = c.scaler_kind;
  in.max_lines_mode = c.max_lines_mode;
  in.overflow_mode = c.overflow;
  in.soft_wrap = 2u;
  in.apply_height_to_first_ascent = c.first_ascent;
  in.apply_height_to_last_descent = c.last_descent;
  in.height_mode = c.height_mode;
  in.strut_mode = 1u;
  in.text = reinterpret_cast<const uint8_t*>(c.text.data());
  in.font_family = reinterpret_cast<const uint8_t*>(family.data());
  in.locale = reinterpret_cast<const uint8_t*>(c.locale.data());
  in.ellipsis = in.ellipsis_length == 0u
                    ? nullptr
                    : reinterpret_cast<const uint8_t*>(ellipsis.data());
  in.min_width = c.min_width;
  in.max_width = c.max_width;
  in.min_height = c.min_height;
  in.max_height = c.max_height;
  in.font_size = c.font_size;
  in.letter_spacing = c.letter_spacing;
  in.word_spacing = c.word_spacing;
  in.height = c.height;
  in.scaler_parameter = c.scaler;
  return in;
}

uint64_t ReadU64(const std::vector<uint8_t>& b, size_t off) {
  uint64_t v = 0;
  std::memcpy(&v, b.data() + off, sizeof(v));
  return v;
}

uint32_t ReadU32(const std::vector<uint8_t>& b, size_t off) {
  return static_cast<uint32_t>(b[off]) |
         static_cast<uint32_t>(b[off + 1]) << 8 |
         static_cast<uint32_t>(b[off + 2]) << 16 |
         static_cast<uint32_t>(b[off + 3]) << 24;
}

}  // namespace

TEST(VenusTextLayoutParityV2, BlobPathAndNativePathAreBitIdentical) {
  const std::vector<Case> cases = BuildMatrix();
  const std::string family = "Roboto";
  const std::string ellipsis = "\xE2\x80\xA6";

  auto fonts = MakeFonts();
  const std::vector<uint8_t> request = EncodeVtlb(cases, family, ellipsis);
  std::vector<uint8_t> blob_result;
  const auto summary = VenusTextLayoutBatchOracle::ProcessWithFontCollection(
      request.data(), request.size(), fonts->GetFontCollection(), false,
      &blob_result);
  ASSERT_TRUE(summary.wrote_output);
  ASSERT_EQ(ReadU32(blob_result, wire::result_header::kBatchStatus), 0u)
      << "blob 路整批失败，说明 corpus 违反了冻结的组合真值表";
  ASSERT_EQ(ReadU32(blob_result, wire::result_header::kItemCount), cases.size());

  auto* service = new VenusTextLayoutService(1u, 1u, fonts, false);
  FlutterVenusTextLayoutApiV2 api{};
  service->PopulateApi(&api);
  uint32_t slot = 0;
  uint64_t token = 0;
  ASSERT_EQ(api.acquire_slot(api.context, &slot, &token), kFlutterVenusV2Ok);

  // blob 结果里的九个几何 double，顺序与 V2 output 的几何半区一一对应。
  const uint32_t kGeometryOffsets[9] = {
      wire::result_item::kPainterWidth,       wire::result_item::kPainterHeight,
      wire::result_item::kBoxWidth,           wire::result_item::kBoxHeight,
      wire::result_item::kMinIntrinsicWidth,  wire::result_item::kMaxIntrinsicWidth,
      wire::result_item::kAlphabeticBaseline, wire::result_item::kIdeographicBaseline,
      wire::result_item::kFirstLineLeft};

  for (size_t i = 0; i < cases.size(); ++i) {
    const Case& c = cases[i];
    FlutterVenusTextLayoutInputV2 in = NativeInput(c, family, ellipsis);
    FlutterVenusTextLayoutOutputV2 out{};
    ASSERT_EQ(api.layout_text_node_sync_v2(api.context, slot, token, &in, &out),
              kFlutterVenusV2Ok)
        << c.name;
    ASSERT_EQ(out.status, kFlutterVenusV2Ok)
        << c.name << " field=" << out.error_field_id
        << " detail=" << out.error_detail;

    const size_t base = wire::kResultHeaderSize + i * wire::kResultItemSize;
    ASSERT_EQ(ReadU32(blob_result, base + wire::result_item::kItemStatus), 0u)
        << c.name << " blob 路逐项失败";

    const double native[9] = {
        out.paragraph_width,      out.paragraph_height,
        out.box_width,            out.box_height,
        out.min_intrinsic_width,  out.max_intrinsic_width,
        out.alphabetic_baseline,  out.ideographic_baseline,
        out.first_line_left};
    for (int m = 0; m < 9; ++m) {
      const uint64_t blob_bits = ReadU64(blob_result, base + kGeometryOffsets[m]);
      EXPECT_EQ(blob_bits, Bits(native[m]))
          << c.name << " metric " << m
          << " blob 路与 native 路不是逐 bit 相同 —— 说明两条路没有共用同一个 core";
    }
    EXPECT_EQ(ReadU32(blob_result, base + wire::result_item::kLineCount),
              out.line_count)
        << c.name;
    const uint32_t blob_exceeded =
        ReadU32(blob_result, base + wire::result_item::kDidExceedMaxLines);
    const uint32_t native_exceeded =
        (out.flags &
         static_cast<uint32_t>(kFlutterVenusV2OutputFlagDidExceedMaxLines))
            ? 1u
            : 0u;
    EXPECT_EQ(blob_exceeded, native_exceeded) << c.name;
  }

  EXPECT_EQ(api.release_slot(api.context, slot, token), kFlutterVenusV2Ok);
  service->Shutdown();
  api.release_context(api.context);
}

// 逐项拒绝：非法输入必须 fail closed，且**整批仍被接受**（守恒式不破）。
TEST(VenusTextLayoutParityV2, MalformedItemsFailClosedWithCanonicalOutput) {
  auto fonts = MakeFonts();
  auto* service = new VenusTextLayoutService(1u, 1u, fonts, false);
  FlutterVenusTextLayoutApiV2 api{};
  service->PopulateApi(&api);
  uint32_t slot = 0;
  uint64_t token = 0;
  ASSERT_EQ(api.acquire_slot(api.context, &slot, &token), kFlutterVenusV2Ok);

  const std::string family = "Roboto";
  const std::string ellipsis = "\xE2\x80\xA6";
  Case base;
  base.name = "base";
  base.text = "fail closed corpus";

  struct Rejection {
    const char* name;
    void (*mutate)(FlutterVenusTextLayoutInputV2*);
    wire::ErrorFieldId field;
  };
  const Rejection rejections[] = {
      {"empty text",
       [](FlutterVenusTextLayoutInputV2* in) {
         in->text = nullptr;
         in->text_length = 0u;
       },
       wire::ErrorFieldId::kTextRef},
      {"empty locale",
       [](FlutterVenusTextLayoutInputV2* in) {
         in->locale = nullptr;
         in->locale_length = 0u;
       },
       wire::ErrorFieldId::kLocaleRef},
      {"soft wrap false",
       [](FlutterVenusTextLayoutInputV2* in) { in->soft_wrap = 1u; },
       wire::ErrorFieldId::kSemanticsCombination},
      {"unlimited with max lines",
       [](FlutterVenusTextLayoutInputV2* in) {
         in->max_lines_mode = 1u;
         in->overflow_mode = 1u;
         in->ellipsis = nullptr;
         in->ellipsis_length = 0u;
       },
       wire::ErrorFieldId::kMaxLines},
      {"out of range direction",
       [](FlutterVenusTextLayoutInputV2* in) { in->text_direction = 4242u; },
       wire::ErrorFieldId::kTextDirection},
      {"noncanonical scaler for noScaling",
       [](FlutterVenusTextLayoutInputV2* in) { in->scaler_parameter = 0.0; },
       wire::ErrorFieldId::kScalerParameter},
      {"nonzero height for font metrics",
       [](FlutterVenusTextLayoutInputV2* in) {
         in->height_mode = 1u;
         in->height = 1.4;
       },
       wire::ErrorFieldId::kHeight},
      {"non finite max width",
       [](FlutterVenusTextLayoutInputV2* in) {
         in->max_width = std::nan("");
       },
       wire::ErrorFieldId::kMinWidth},
      {"min greater than max",
       [](FlutterVenusTextLayoutInputV2* in) { in->min_width = 500.0; },
       wire::ErrorFieldId::kMinWidth},
      {"font weight out of range",
       [](FlutterVenusTextLayoutInputV2* in) { in->font_weight = 4242u; },
       wire::ErrorFieldId::kFontWeight},
      // CSS Fonts 4 §3.1: font-size 计算值为 0 是合法值, 不是错误 (见下方
      // ItemV2ZeroFontSizeIsAuthoredNotInvalid 的正向用例)。负值仍不是合法
      // CSS 长度, 必须继续拒绝 —— 这条防的是"放宽阈值时手滑放过了负值"。
      {"font size below zero",
       [](FlutterVenusTextLayoutInputV2* in) { in->font_size = -1.0; },
       wire::ErrorFieldId::kFontSize},
  };

  for (const Rejection& r : rejections) {
    FlutterVenusTextLayoutInputV2 in = NativeInput(base, family, ellipsis);
    r.mutate(&in);
    FlutterVenusTextLayoutOutputV2 out{};
    // 逐项失败**不影响调用级返回码** —— 这正是只断言返回码的用例会空绿的原因。
    ASSERT_EQ(api.layout_text_node_sync_v2(api.context, slot, token, &in, &out),
              kFlutterVenusV2Ok)
        << r.name;
    EXPECT_EQ(out.status, kFlutterVenusV2ItemFailed) << r.name;
    EXPECT_EQ(out.error_field_id, static_cast<uint32_t>(r.field)) << r.name;
    EXPECT_NE(out.error_detail, 0u) << r.name;
    // canonical 失败 output：九个几何 double 全是 +0.0，计数字段清零。
    const double zeros[9] = {out.paragraph_width,     out.paragraph_height,
                             out.box_width,           out.box_height,
                             out.min_intrinsic_width, out.max_intrinsic_width,
                             out.alphabetic_baseline, out.ideographic_baseline,
                             out.first_line_left};
    for (int m = 0; m < 9; ++m) {
      EXPECT_EQ(Bits(zeros[m]), Bits(0.0)) << r.name << " metric " << m;
    }
    EXPECT_EQ(out.line_count, 0u) << r.name;
    EXPECT_EQ(out.flags, 0u) << r.name;
  }

  FlutterVenusTextLayoutCountersV2 counters{};
  ASSERT_EQ(api.get_counters_v2(api.context, sizeof(counters), &counters),
            kFlutterVenusV2Ok);
  EXPECT_EQ(counters.accepted_total, counters.completed_total)
      << "逐项拒绝不得破坏唯一守恒式";

  EXPECT_EQ(api.release_slot(api.context, slot, token), kFlutterVenusV2Ok);
  service->Shutdown();
  api.release_context(api.context);
}

// CSS Fonts 4 §3.1: `font-size` 的计算值可以是 0, 那是合法值不是错误
// (常见于清除 inline-block 间隙)。判据页 WPT css-grid/subgrid
// standalone-axis-size-005/006 的 `<div style="font-size: 0">` 包住两个
// inline-block, 中间的折叠空白文本节点继承零字号送来测量 —— 此前 ABI 校验器
// 把 `font_size <= 0.0` 一并当非法参数拒绝, 单节点这一路因此把整页发布判 -7
// kTextMeasureFailed。
TEST(VenusTextLayoutParityV2, ItemV2ZeroFontSizeIsAuthoredNotInvalid) {
  auto fonts = MakeFonts();
  auto* service = new VenusTextLayoutService(1u, 1u, fonts, false);
  FlutterVenusTextLayoutApiV2 api{};
  service->PopulateApi(&api);
  uint32_t slot = 0;
  uint64_t token = 0;
  ASSERT_EQ(api.acquire_slot(api.context, &slot, &token), kFlutterVenusV2Ok);

  const std::string family = "Roboto";
  const std::string ellipsis = "\xE2\x80\xA6";
  Case zero;
  zero.name = "zero font size";
  zero.text = " ";  // 折叠空白节点, 与 inline-block 间隙同形。
  zero.font_size = 0.0;
  // letter-spacing 是独立声明的附加量, 不随字号清零 —— 清零它才能让下面的
  // box_width/box_height 断言只反映 font-size:0 本身贡献的度量。
  zero.letter_spacing = 0.0;
  zero.word_spacing = 0.0;

  FlutterVenusTextLayoutInputV2 in = NativeInput(zero, family, ellipsis);
  FlutterVenusTextLayoutOutputV2 out{};
  ASSERT_EQ(api.layout_text_node_sync_v2(api.context, slot, token, &in, &out),
            kFlutterVenusV2Ok);
  EXPECT_EQ(out.status, kFlutterVenusV2Ok)
      << "font-size:0 是合法声明, 不该被判 InvalidRange/ItemFailed";
  EXPECT_EQ(out.box_width, 0.0) << "零字号不产生可见宽度";
  EXPECT_EQ(out.box_height, 0.0) << "零字号不产生可见高度";

  // 合成 strut 使用 "x" + normal 字体度量；空格的盒高为零不能替它作证。
  zero.text = "x";
  zero.height_mode = 1u;  // font metrics, 即 line-height: normal。
  zero.height = 0.0;
  zero.max_height = 1000.0;
  FlutterVenusTextLayoutInputV2 strut_in = NativeInput(zero, family, ellipsis);
  FlutterVenusTextLayoutOutputV2 strut_out{};
  ASSERT_EQ(api.layout_text_node_sync_v2(api.context, slot, token, &strut_in,
                                         &strut_out),
            kFlutterVenusV2Ok);
  ASSERT_EQ(strut_out.status, kFlutterVenusV2Ok);
  EXPECT_EQ(strut_out.box_width, 0.0);
  EXPECT_EQ(strut_out.box_height, 0.0);
  EXPECT_EQ(strut_out.content_height, 0.0);
  EXPECT_EQ(strut_out.alphabetic_baseline, 0.0);

  // 零字号不等于零宽：作者声明的字距和盒最小高度仍须保留。
  zero.text = "xx";
  zero.letter_spacing = 5.0;
  zero.min_height = 7.0;
  FlutterVenusTextLayoutInputV2 spaced_in = NativeInput(zero, family, ellipsis);
  FlutterVenusTextLayoutOutputV2 spaced_out{};
  ASSERT_EQ(api.layout_text_node_sync_v2(api.context, slot, token, &spaced_in,
                                         &spaced_out),
            kFlutterVenusV2Ok);
  ASSERT_EQ(spaced_out.status, kFlutterVenusV2Ok);
  EXPECT_GT(spaced_out.box_width, 0.0);
  EXPECT_EQ(spaced_out.paragraph_height, 0.0);
  EXPECT_EQ(spaced_out.box_height, 7.0);

  // 显式倍数与零字号相乘仍为零，正字号则不能被守卫误伤。
  zero.text = "x";
  zero.letter_spacing = 0.0;
  zero.min_height = 0.0;
  zero.height_mode = 2u;
  zero.height = 2.0;
  FlutterVenusTextLayoutInputV2 explicit_in = NativeInput(zero, family, ellipsis);
  FlutterVenusTextLayoutOutputV2 explicit_out{};
  ASSERT_EQ(api.layout_text_node_sync_v2(api.context, slot, token, &explicit_in,
                                         &explicit_out),
            kFlutterVenusV2Ok);
  ASSERT_EQ(explicit_out.status, kFlutterVenusV2Ok);
  EXPECT_EQ(explicit_out.box_height, 0.0);

  zero.font_size = 1.0;
  zero.height_mode = 1u;
  zero.height = 0.0;
  FlutterVenusTextLayoutInputV2 one_in = NativeInput(zero, family, ellipsis);
  FlutterVenusTextLayoutOutputV2 one_out{};
  ASSERT_EQ(api.layout_text_node_sync_v2(api.context, slot, token, &one_in,
                                         &one_out),
            kFlutterVenusV2Ok);
  ASSERT_EQ(one_out.status, kFlutterVenusV2Ok);
  EXPECT_GT(one_out.content_height, 0.0);

  EXPECT_EQ(api.release_slot(api.context, slot, token), kFlutterVenusV2Ok);
  service->Shutdown();
  api.release_context(api.context);
}

TEST(VenusTextLayoutParityV2, ParagraphV2TrustBoundary) {
  constexpr uint64_t kEngineId = 0x5741564531410001ull;
  ParagraphWire base;

  EXPECT_EQ(FlutterVenusTextLayoutParagraphV2(kEngineId, 0u, 0u,
                                              &base.input, nullptr),
            kFlutterVenusV2InvalidArgument);

  FlutterVenusTextLayoutParagraphOutputV2 short_output;
  std::memset(&short_output, 0xA5, sizeof(short_output));
  short_output.struct_size = sizeof(short_output) - 1u;
  const FlutterVenusTextLayoutParagraphOutputV2 untouched = short_output;
  EXPECT_EQ(FlutterVenusTextLayoutParagraphV2(kEngineId, 0u, 0u,
                                              &base.input, &short_output),
            kFlutterVenusV2AbiMismatch);
  EXPECT_EQ(std::memcmp(&short_output, &untouched, sizeof(short_output)), 0);

  FlutterVenusTextLayoutParagraphOutputV2 output{};
  output.struct_size = sizeof(output);
  EXPECT_EQ(FlutterVenusTextLayoutParagraphV2(kEngineId, 0u, 0u, nullptr,
                                              &output),
            kFlutterVenusV2InvalidArgument);
  EXPECT_EQ(output.status, kFlutterVenusV2InvalidArgument);
  output.struct_size = sizeof(output);
  EXPECT_EQ(FlutterVenusTextLayoutParagraphV2(kEngineId, 0u, 0u, &base.input,
                                              &output),
            kFlutterVenusV2Unavailable);
  EXPECT_EQ(output.status, kFlutterVenusV2Unavailable);

  const auto fonts = MakeFonts();
  const uint64_t generation =
      VenusTextLayoutRegistry::Register(kEngineId, fonts, false);
  ASSERT_NE(generation, 0u);
  FlutterVenusTextLayoutApiV2 api{};
  ASSERT_EQ(VenusTextLayoutRegistry::GetApi(kEngineId, sizeof(api) - 1u, &api),
            kFlutterVenusV2AbiMismatch);
  ASSERT_EQ(VenusTextLayoutRegistry::GetApi(kEngineId, sizeof(api), &api),
            kFlutterVenusV2Ok);
  uint32_t slot = 0u;
  uint64_t lease = 0u;
  ASSERT_EQ(api.acquire_slot(api.context, &slot, &lease),
            kFlutterVenusV2Ok);

  struct Rejection {
    const char* name;
    int32_t expected;
    void (*mutate)(ParagraphWire*);
    uint32_t error_field = 0u;
    uint32_t error_detail = 0u;
  };
  const Rejection rejections[] = {
      {"input size", kFlutterVenusV2AbiMismatch,
       [](ParagraphWire* w) { --w->input.struct_size; }},
      {"input abi", kFlutterVenusV2AbiMismatch,
       [](ParagraphWire* w) { --w->input.abi_version; }},
      {"run size", kFlutterVenusV2AbiMismatch,
       [](ParagraphWire* w) { --w->run.struct_size; }},
      {"reserved", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->input.reserved1 = 1u; }},
      {"run count cap", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) {
         w->input.run_count = kFlutterVenusV2MaxParagraphRuns + 1u;
       }},
      {"blob cap", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) {
         w->input.text_length = kFlutterVenusV2MaxTextBytes + 1u;
       }},
      {"output capacity", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) {
         w->input.fragment_rect_capacity =
             kFlutterVenusV2MaxFragmentRects + 1u;
       }},
      {"line pointer without capacity", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->input.line_metric_capacity = 0u; }},
      {"fragment pointer without capacity", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->input.fragment_rect_capacity = 0u; }},
      {"pointer count", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->input.text = nullptr; }},
      {"run range", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->run.text_offset = w->input.text_length; }},
      {"input enum", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->input.text_direction = 0u; }},
      {"whitespace mode enum", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->run.whitespace_mode = 3u; }},
      {"nontext whitespace mode", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) {
         w->run.kind = kFlutterVenusV2RunKindLineBreak;
         w->run.text_length = 0u;
         w->run.font_family_length = 0u;
         w->run.whitespace_mode = kFlutterVenusV2WhitespacePreserve;
       }},
      {"run enum", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->run.kind = 3u; }},
      {"malformed utf8", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) {
         w->text[0] = static_cast<char>(0x80);
         w->input.text = reinterpret_cast<const uint8_t*>(w->text.data());
       }},
      {"non finite", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->run.font_size = std::nan(""); }},
      // 0 已合法 (CSS Fonts 4 §3.1 的零字号声明, 见下方
      // ParagraphV2ZeroFontSizeRunIsAuthoredNotInvalid 的正向用例), 负值仍拒。
      {"run font size below zero", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->run.font_size = -1.0; }},
      {"disabled strut payload", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->input.strut_font_size = 10.0; }},
      {"strut enabled enum", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->input.strut_enabled = 2u; }},
      {"strut family range", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) {
         w->EnableStrut();
         w->input.strut_font_family_offset = w->input.font_families_length;
       }},
      {"strut weight", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) {
         w->EnableStrut();
         w->input.strut_font_weight = 0u;
       }},
      {"strut style", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) {
         w->EnableStrut();
         w->input.strut_font_style = 2u;
       }},
      // Zero is now the positive regression above; the invalid-size contract
      // retains the original rejection assertions using a genuinely negative size.
      {"strut size", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) {
         w->EnableStrut();
         w->input.strut_font_size = -1.0;
       }, 12u, 1u},
      {"strut size NaN", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->EnableStrut(std::nan(""), 0.0); }, 12u, 3u},
      {"strut size positive infinity", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->EnableStrut(std::numeric_limits<double>::infinity(), 0.0); }, 12u, 3u},
      {"strut size negative infinity", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->EnableStrut(-std::numeric_limits<double>::infinity(), 0.0); }, 12u, 3u},
      {"strut size above maximum", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->EnableStrut(4097.0, 0.0); }, 12u, 2u},
      {"strut height NaN", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->EnableStrut(20.0, std::nan("")); }, 15u, 3u},
      {"strut height infinity", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->EnableStrut(20.0, std::numeric_limits<double>::infinity()); }, 15u, 3u},
      {"strut height above maximum", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->EnableStrut(20.0, 17.0); }, 15u, 2u},
      // 0 已合法 (CSS `line-height: 0` 的零高 strut), 负值仍拒。
      {"strut height", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) {
         w->EnableStrut();
         w->input.strut_height_multiple = -1.0;
       }},
      {"run height mode enum", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) { w->run.height_mode = 3u; }},
      {"font metrics run with height", kFlutterVenusV2InvalidArgument,
       [](ParagraphWire* w) {
         w->run.height_mode = 1u;
         w->run.height = 1.0;
       }},
  };
  for (const Rejection& rejection : rejections) {
    SCOPED_TRACE(rejection.name);
    ParagraphWire wire;
    rejection.mutate(&wire);
    const int32_t status = FlutterVenusTextLayoutParagraphV2(
        kEngineId, slot, lease, &wire.input, &wire.output);
    EXPECT_EQ(status, rejection.expected) << rejection.name;
    EXPECT_EQ(wire.output.status, status) << rejection.name;
    EXPECT_EQ(wire.output.width, 0.0) << rejection.name;
    EXPECT_EQ(wire.output.line_metric_count, 0u) << rejection.name;
    if (rejection.error_field != 0u) {
      EXPECT_EQ(wire.output.error_field_id, rejection.error_field);
      EXPECT_EQ(wire.output.error_detail, rejection.error_detail);
    }
  }

  // Exact RunV2 shape is required before reading its tail or advancing the stride.
  for (const uint32_t wrong_size : {uint32_t(sizeof(base.run) - 8u),
                                    uint32_t(sizeof(base.run) + 8u)}) {
    for (const uint32_t count : {1u, 2u}) {
      ParagraphWire wire;
      FlutterVenusTextLayoutRunV2 runs[2] = {wire.run, wire.run};
      runs[0].struct_size = wrong_size;
      wire.input.runs = runs;
      wire.input.run_count = count;
      EXPECT_EQ(FlutterVenusTextLayoutParagraphV2(
                    kEngineId, slot, lease, &wire.input, &wire.output),
                kFlutterVenusV2AbiMismatch);
    }
  }
  // An actual old-stride allocation contains no new scalar tail for run zero.
  alignas(FlutterVenusTextLayoutRunV2) uint8_t old_runs[2 * (sizeof(base.run) - 8u)]{};
  const uint32_t old_size = sizeof(base.run) - 8u;
  std::memcpy(old_runs, &old_size, sizeof(old_size));
  ParagraphWire old_wire;
  old_wire.input.runs = reinterpret_cast<const FlutterVenusTextLayoutRunV2*>(old_runs);
  old_wire.input.run_count = 2u;
  EXPECT_EQ(FlutterVenusTextLayoutParagraphV2(
                kEngineId, slot, lease, &old_wire.input, &old_wire.output),
            kFlutterVenusV2AbiMismatch);

  const double invalid_lengths[] = {1.0, -0.0, std::nan(""),
                                    std::numeric_limits<double>::infinity(),
                                    -std::numeric_limits<double>::infinity()};
  for (const uint16_t kind : {uint16_t(kFlutterVenusV2RunKindText),
                             uint16_t(kFlutterVenusV2RunKindPlaceholder),
                             uint16_t(kFlutterVenusV2RunKindLineBreak)}) {
    for (const double length : invalid_lengths) {
      ParagraphWire wire;
      wire.run.kind = kind;
      if (kind != kFlutterVenusV2RunKindText) {
        wire.run.text_offset = wire.run.text_length = 0u;
        wire.run.font_family_offset = wire.run.font_family_length = 0u;
      }
      wire.run.text_vertical_align_length = length;
      EXPECT_EQ(FlutterVenusTextLayoutParagraphV2(
                    kEngineId, slot, lease, &wire.input, &wire.output),
                kFlutterVenusV2InvalidArgument);
    }
  }
  for (const double length : {std::nan(""),
                              std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity()}) {
    ParagraphWire wire;
    wire.run.text_vertical_align = 6u;
    wire.run.text_vertical_align_length = length;
    EXPECT_EQ(FlutterVenusTextLayoutParagraphV2(
                  kEngineId, slot, lease, &wire.input, &wire.output),
              kFlutterVenusV2InvalidArgument);
  }

  ParagraphWire valid;
  valid.input.paragraph_token = 0u;
  valid.input.line_metric_capacity = 0u;
  valid.input.fragment_rect_capacity = 0u;
  valid.input.line_metrics_out = nullptr;
  valid.input.fragment_rects_out = nullptr;
  const int32_t valid_status = FlutterVenusTextLayoutParagraphV2(
      kEngineId, slot, lease, &valid.input, &valid.output);
  EXPECT_EQ(valid_status, kFlutterVenusV2Ok);
  EXPECT_EQ(valid.output.status, valid_status);
  EXPECT_EQ(valid.output.truncated, 1u);
  EXPECT_NE(api.capabilities & kFlutterVenusV2CapabilityPreservedWhitespace, 0u);
  EXPECT_NE(api.capabilities & kFlutterVenusV2CapabilityTextVerticalAlignLength, 0u);
  EXPECT_NE(api.capabilities & kFlutterVenusV2CapabilityZeroFontStrut, 0u);
  valid.run.whitespace_mode = kFlutterVenusV2WhitespacePreserve;
  EXPECT_EQ(FlutterVenusTextLayoutParagraphV2(
                kEngineId, slot, lease, &valid.input, &valid.output),
            kFlutterVenusV2Ok);

  // Adjacent protected runs span a font/color boundary; Normal around them still wraps.
  ParagraphWire mixed; mixed.text = "AA AA AA XX  YY  ZZ BB BB BB";
  mixed.input.text = reinterpret_cast<const uint8_t*>(mixed.text.data()); mixed.input.text_length = mixed.text.size();
  FlutterVenusTextLayoutRunV2 runs[4] = {mixed.run, mixed.run, mixed.run, mixed.run};
  const uint32_t offsets[] = {0, 9, 13, 19}, lengths[] = {9, 4, 6, 9};
  for (uint32_t i = 0; i < 4; ++i) { runs[i].text_offset = offsets[i]; runs[i].text_length = lengths[i]; }
  runs[1].text_color_argb = 0xffff0000; runs[2].text_color_argb = 0xff0000ff; runs[2].font_size = 20;
  FlutterVenusTextLayoutLineMetricV2 lines[32]{}; FlutterVenusTextLayoutFragmentRectV2 rects[32]{};
  mixed.input.runs = runs; mixed.input.run_count = 4; mixed.input.max_width = 32;
  mixed.input.line_metrics_out = lines; mixed.input.line_metric_capacity = 32;
  mixed.input.fragment_rects_out = rects; mixed.input.fragment_rect_capacity = 32;
  uint32_t normal_lines = 0;
  for (const uint16_t mode : {uint16_t(0), uint16_t(2), uint16_t(1), uint16_t(2), uint16_t(0)}) {
    SCOPED_TRACE(mode); runs[1].whitespace_mode = runs[2].whitespace_mode = mode;
    const auto status = FlutterVenusTextLayoutParagraphV2(kEngineId, slot, lease, &mixed.input, &mixed.output);
    EXPECT_EQ(status, kFlutterVenusV2Ok); if (status != kFlutterVenusV2Ok) continue;
    EXPECT_EQ(mixed.output.truncated, 0u); uint32_t counts[4] = {}; uint32_t protected_line = UINT32_MAX;
    for (uint32_t i = 0; i < mixed.output.fragment_rect_count; ++i) {
      ASSERT_LT(rects[i].run_index, 4u); ++counts[rects[i].run_index];
      if (mode == uint16_t(2) && (rects[i].run_index == 1 || rects[i].run_index == 2)) {
        if (protected_line == UINT32_MAX) protected_line = rects[i].line_number;
        EXPECT_EQ(rects[i].line_number, protected_line);
      }
    }
    EXPECT_GT(counts[0], 1u); EXPECT_GT(counts[3], 1u);
    if (mode == uint16_t(2)) { EXPECT_EQ(counts[1], 1u); EXPECT_EQ(counts[2], 1u); }
    if (mode == 0) { if (normal_lines == 0) normal_lines = mixed.output.line_metric_count;
      EXPECT_EQ(mixed.output.line_metric_count, normal_lines); }
  }

  EXPECT_EQ(api.release_slot(api.context, slot, lease), kFlutterVenusV2Ok);
  api.release_context(api.context);
  VenusTextLayoutRegistry::Unregister(kEngineId, generation);
}

TEST(VenusTextLayoutParityV2, ParagraphV2StrutIsMinimumNotForcedRunHeight) {
  constexpr uint64_t kEngineId = 0x5741564531410002ull;
  const auto fonts = MakeFonts();
  const uint64_t generation =
      VenusTextLayoutRegistry::Register(kEngineId, fonts, false);
  ASSERT_NE(generation, 0u);
  FlutterVenusTextLayoutApiV2 api{};
  ASSERT_EQ(VenusTextLayoutRegistry::GetApi(kEngineId, sizeof(api), &api),
            kFlutterVenusV2Ok);
  uint32_t slot = 0u;
  uint64_t lease = 0u;
  ASSERT_EQ(api.acquire_slot(api.context, &slot, &lease), kFlutterVenusV2Ok);

  ParagraphWire short_run;
  short_run.text = "x";
  short_run.run.text_length = 1u;
  short_run.run.font_size = 4.0;
  short_run.input.text_length = 1u;
  short_run.input.text =
      reinterpret_cast<const uint8_t*>(short_run.text.data());
  short_run.EnableStrut();
  short_run.input.strut_font_family_offset =
      short_run.input.font_families_length;
  short_run.input.strut_font_family_length = 0u;
  ASSERT_EQ(FlutterVenusTextLayoutParagraphV2(
                kEngineId, slot, lease, &short_run.input, &short_run.output),
            kFlutterVenusV2Ok);
  ASSERT_EQ(short_run.output.line_metric_count, 1u);
  EXPECT_GE(short_run.line.height, 10.0);

  ParagraphWire tall_without_strut;
  tall_without_strut.text = "X";
  tall_without_strut.run.text_length = 1u;
  tall_without_strut.run.font_size = 20.0;
  tall_without_strut.input.text_length = 1u;
  tall_without_strut.input.text =
      reinterpret_cast<const uint8_t*>(tall_without_strut.text.data());
  ASSERT_EQ(FlutterVenusTextLayoutParagraphV2(kEngineId, slot, lease,
                                              &tall_without_strut.input,
                                              &tall_without_strut.output),
            kFlutterVenusV2Ok);
  ASSERT_EQ(tall_without_strut.output.line_metric_count, 1u);

  ParagraphWire tall_with_strut;
  tall_with_strut.text = "X";
  tall_with_strut.run.text_length = 1u;
  tall_with_strut.run.font_size = 20.0;
  tall_with_strut.input.text_length = 1u;
  tall_with_strut.input.text =
      reinterpret_cast<const uint8_t*>(tall_with_strut.text.data());
  tall_with_strut.EnableStrut();
  ASSERT_EQ(FlutterVenusTextLayoutParagraphV2(kEngineId, slot, lease,
                                              &tall_with_strut.input,
                                              &tall_with_strut.output),
            kFlutterVenusV2Ok);
  ASSERT_EQ(tall_with_strut.output.line_metric_count, 1u);
  EXPECT_GE(tall_with_strut.line.height, tall_without_strut.line.height);

  EXPECT_EQ(api.release_slot(api.context, slot, lease), kFlutterVenusV2Ok);
  api.release_context(api.context);
  VenusTextLayoutRegistry::Unregister(kEngineId, generation);
}

// CSS `line-height: 0` 过公共 ABI: 零高 strut 合法, 文本 run 以 height_mode=Multiplier
// + height 0 声明零行高, 同行 270 高 baseline 占位独自决定行盒。height_mode 0
// (旧调用方) 保持"0 = 字体自然行高"。
TEST(VenusTextLayoutParityV2, ParagraphV2ZeroLineHeightIsAuthoredNotUnset) {
  constexpr uint64_t kEngineId = 0x5741564531410003ull;
  const auto fonts = MakeFonts();
  const uint64_t generation =
      VenusTextLayoutRegistry::Register(kEngineId, fonts, false);
  ASSERT_NE(generation, 0u);
  FlutterVenusTextLayoutApiV2 api{};
  ASSERT_EQ(VenusTextLayoutRegistry::GetApi(kEngineId, sizeof(api), &api),
            kFlutterVenusV2Ok);
  uint32_t slot = 0u;
  uint64_t lease = 0u;
  ASSERT_EQ(api.acquire_slot(api.context, &slot, &lease), kFlutterVenusV2Ok);

  const auto line_height = [&](uint16_t height_mode) {
    ParagraphWire w;
    w.text = " x";
    w.run.text_length = 2u;
    w.run.font_size = 10.0;
    w.run.height = 0.0;
    w.run.height_mode = height_mode;
    w.input.text_length = 2u;
    w.input.text = reinterpret_cast<const uint8_t*>(w.text.data());
    w.EnableStrut(10.0, 0.0);
    FlutterVenusTextLayoutRunV2 atom{};
    atom.struct_size = sizeof(atom);
    atom.kind = kFlutterVenusV2RunKindPlaceholder;
    atom.placeholder_width = 10.0;
    atom.placeholder_height = 270.0;
    atom.placeholder_baseline_offset = 270.0;
    FlutterVenusTextLayoutRunV2 runs[2] = {atom, w.run};
    FlutterVenusTextLayoutFragmentRectV2 fragments[2]{};
    w.input.runs = runs;
    w.input.run_count = 2u;
    w.input.fragment_rects_out = fragments;
    w.input.fragment_rect_capacity = 2u;
    EXPECT_EQ(FlutterVenusTextLayoutParagraphV2(kEngineId, slot, lease,
                                                &w.input, &w.output),
              kFlutterVenusV2Ok)
        << "height_mode=" << height_mode;
    EXPECT_EQ(w.output.line_metric_count, 1u) << "height_mode=" << height_mode;
    return w.line.height;
  };
  EXPECT_DOUBLE_EQ(line_height(2u), 270.0) << "声明的零行高: 文本不抬行盒";
  EXPECT_GT(line_height(0u), 270.0) << "对照组: 旧调用方 0 = 字体自然行高";

  EXPECT_EQ(api.release_slot(api.context, slot, lease), kFlutterVenusV2Ok);
  api.release_context(api.context);
  VenusTextLayoutRegistry::Unregister(kEngineId, generation);
}

// CSS Fonts 4 §3.1: `font-size: 0` 的计算值就是 0, 是合法值不是错误。段落路
// (LayoutParagraph/ValidateParagraphInput) 与单节点路是两条独立的校验门,
// 必须分别验证同一条 CSS 语义。判据页同上 (WPT css-grid/subgrid
// standalone-axis-size-005/006)。
TEST(VenusTextLayoutParityV2, ParagraphV2ZeroFontSizeRunIsAuthoredNotInvalid) {
  constexpr uint64_t kEngineId = 0x5741564531410005ull;
  const auto fonts = MakeFonts();
  const uint64_t generation =
      VenusTextLayoutRegistry::Register(kEngineId, fonts, false);
  ASSERT_NE(generation, 0u);
  FlutterVenusTextLayoutApiV2 api{};
  ASSERT_EQ(VenusTextLayoutRegistry::GetApi(kEngineId, sizeof(api), &api),
            kFlutterVenusV2Ok);
  uint32_t slot = 0u;
  uint64_t lease = 0u;
  ASSERT_EQ(api.acquire_slot(api.context, &slot, &lease), kFlutterVenusV2Ok);

  struct ZeroFontCase {
    const char* name;
    uint32_t run_count;
    uint16_t height_mode;
    bool enabled_strut;
    double strut_size;
    double strut_height;
  };
  const ZeroFontCase cases[] = {
      {"original zero-font run", 1u, 0u, false, 0.0, 0.0},
      {"zero strut single natural run", 1u, 1u, true, 0.0, 0.0},
      {"zero strut single declared-zero run", 1u, 2u, true, 0.0, 0.0},
      {"zero strut mixed natural runs", 3u, 1u, true, 0.0, 0.0},
      {"zero strut mixed declared-zero runs", 3u, 2u, true, 0.0, 0.0},
      {"zero strut positive multiplier", 3u, 1u, true, 0.0, 1.5},
      {"positive strut zero multiplier", 3u, 1u, true, 20.0, 0.0},
      {"maximum finite strut size", 3u, 1u, true, 4096.0, 0.0},
  };
  for (const ZeroFontCase& c : cases) {
    SCOPED_TRACE(c.name);
    ParagraphWire w;
    w.run.font_size = 0.0;
    std::vector<FlutterVenusTextLayoutRunV2> runs;
    std::vector<FlutterVenusTextLayoutFragmentRectV2> fragments(8);
    if (c.enabled_strut) {
      w.text = "TESTINLINEVISIBLE";
      w.input.text = reinterpret_cast<const uint8_t*>(w.text.data());
      w.input.text_length = static_cast<uint32_t>(w.text.size());
      for (uint32_t i = 0; i < c.run_count; ++i) {
        auto run = w.run;
        run.text_offset = c.run_count == 1u ? 0u : (i == 0u ? 0u : (i == 1u ? 4u : 10u));
        run.text_length = c.run_count == 1u ? 17u : (i == 0u ? 4u : (i == 1u ? 6u : 7u));
        run.font_size = c.run_count == 1u || i == 2u ? 20.0 : 0.0;
        run.height_mode = c.height_mode;
        runs.push_back(run);
      }
      w.input.runs = runs.data();
      w.input.run_count = static_cast<uint32_t>(runs.size());
      w.input.fragment_rects_out = fragments.data();
      w.input.fragment_rect_capacity = static_cast<uint32_t>(fragments.size());
      w.EnableStrut(c.strut_size, c.strut_height);
    }
    const int32_t status = FlutterVenusTextLayoutParagraphV2(
        kEngineId, slot, lease, &w.input, &w.output);
    EXPECT_EQ(status, kFlutterVenusV2Ok)
        << "font-size:0 是合法声明, 段落路不该判 InvalidArgument";
    EXPECT_EQ(w.output.status, status);
    if (status != kFlutterVenusV2Ok || !c.enabled_strut) continue;
    EXPECT_EQ(w.output.truncated, 0u);
    EXPECT_EQ(w.output.line_metric_count, 1u);
    if (c.strut_size != 0.0) continue;

    // Geometry probe v2: an enabled zero-size strut retains its baseline point.
    // The old disabled-equivalence prediction is archived verbatim with its
    // failure; it was invalid when a zero-height run has a negative descent.
    FlutterVenusTextLayoutParagraphInputV2 control = w.input;
    control.strut_enabled = 0u;
    control.strut_font_family_offset = 0u;
    control.strut_font_family_length = 0u;
    control.strut_font_weight = 0u;
    control.strut_font_style = 0u;
    control.strut_font_size = 0.0;
    control.strut_height_multiple = 0.0;
    FlutterVenusTextLayoutLineMetricV2 control_line{};
    std::vector<FlutterVenusTextLayoutFragmentRectV2> control_fragments(8);
    control.line_metrics_out = &control_line;
    control.fragment_rects_out = control_fragments.data();
    FlutterVenusTextLayoutParagraphOutputV2 control_output{};
    control_output.struct_size = sizeof(control_output);
    ASSERT_EQ(FlutterVenusTextLayoutParagraphV2(
                  kEngineId, slot, lease, &control, &control_output),
              kFlutterVenusV2Ok);
    EXPECT_DOUBLE_EQ(w.output.width, control_output.width);
    SCOPED_TRACE("zero strut baseline-envelope probe v3");
    const double envelope_ascent = std::max(0.0, control_line.ascent);
    const double envelope_descent = std::max(0.0, control_line.descent);
    const double envelope_height = std::round(envelope_ascent + envelope_descent);
    EXPECT_DOUBLE_EQ(w.output.height, envelope_height);
    EXPECT_DOUBLE_EQ(w.output.first_baseline, control_output.first_baseline);
    EXPECT_DOUBLE_EQ(w.output.min_intrinsic_width, control_output.min_intrinsic_width);
    EXPECT_DOUBLE_EQ(w.output.max_intrinsic_width, control_output.max_intrinsic_width);
    EXPECT_EQ(w.output.fragment_rect_count, control_output.fragment_rect_count);
    EXPECT_DOUBLE_EQ(w.line.height, envelope_height);
    // Public ascent/descent intentionally report fMaxRunMetrics (before strut)
    // in TextLine::getMetrics; line-box height reports the merged envelope.
    EXPECT_DOUBLE_EQ(w.line.ascent, control_line.ascent);
    EXPECT_DOUBLE_EQ(w.line.descent, control_line.descent);
    if (control_line.descent < 0.0) {
      EXPECT_GT(w.line.height, control_line.height);
    }
    EXPECT_DOUBLE_EQ(w.line.baseline, control_line.baseline);
    for (uint32_t i = 0; i < w.output.fragment_rect_count; ++i) {
      EXPECT_EQ(fragments[i].run_index, control_fragments[i].run_index);
      EXPECT_EQ(fragments[i].line_number, control_fragments[i].line_number);
      EXPECT_DOUBLE_EQ(fragments[i].left, control_fragments[i].left);
      EXPECT_DOUBLE_EQ(fragments[i].top, control_fragments[i].top);
      EXPECT_DOUBLE_EQ(fragments[i].right, control_fragments[i].right);
      EXPECT_DOUBLE_EQ(fragments[i].bottom, control_fragments[i].bottom);
    }
  }

  EXPECT_EQ(api.release_slot(api.context, slot, lease), kFlutterVenusV2Ok);
  api.release_context(api.context);
  VenusTextLayoutRegistry::Unregister(kEngineId, generation);
}

// 非替换 inline 包装盒 (`<span style="vertical-align:top">`) 摊成的文本 run:
// CSS 2.1 §10.8.1 的 top / bottom 对齐**行盒**上 / 下沿, 行盒由其余内容决定。
// 判据页 venus WPT css-grid/grid-model__display-inline-grid。
TEST(VenusTextLayoutParityV2, ParagraphV2TextRunVerticalAlignUsesLineBox) {
  constexpr uint64_t kEngineId = 0x5741564531410004ull;
  const auto fonts = MakeFonts();
  const uint64_t generation =
      VenusTextLayoutRegistry::Register(kEngineId, fonts, false);
  ASSERT_NE(generation, 0u);
  FlutterVenusTextLayoutApiV2 api{};
  ASSERT_EQ(VenusTextLayoutRegistry::GetApi(kEngineId, sizeof(api), &api),
            kFlutterVenusV2Ok);
  uint32_t slot = 0u;
  uint64_t lease = 0u;
  ASSERT_EQ(api.acquire_slot(api.context, &slot, &lease), kFlutterVenusV2Ok);

  struct Laid {
    int32_t status = 0;
    FlutterVenusTextLayoutLineMetricV2 line{};
    FlutterVenusTextLayoutFragmentRectV2 first{};   ///< run 0 的片段
    FlutterVenusTextLayoutFragmentRectV2 second{};  ///< run 1 的片段
  };
  // run 0 = "A" (size0), run 1 = "b" (size1, 对齐档 align1); size0 <= 0 = 只有 run 1。
  // line_height > 0: 两个 run 都声明行高倍数 (CSS `line-height: <number>`, height_mode=Multiplier)。
  const auto lay = [&](double size0, double size1, uint16_t align1,
                       const std::string& text1 = "b",
                       double max_width = 200.0, double line_height = 0.0,
                       double length = 0.0, double parent_size = -1.0,
                       const std::string& parent_family = "Roboto") {
    ParagraphWire w;
    const bool two = size0 > 0.0;
    w.text = (two ? std::string("A") : std::string()) + text1;
    FlutterVenusTextLayoutRunV2 r0 = w.run;
    r0.text_offset = 0u;
    r0.text_length = 1u;
    r0.font_size = size0;
    FlutterVenusTextLayoutRunV2 r1 = w.run;
    r1.text_offset = two ? 1u : 0u;
    r1.text_length = static_cast<uint32_t>(text1.size());
    r1.font_size = size1;
    r1.text_vertical_align = align1;
    r1.text_vertical_align_length = length;
    if (line_height > 0.0) {
      r0.height_mode = r1.height_mode = 2u;
      r0.height = r1.height = line_height;
    }
    FlutterVenusTextLayoutRunV2 runs[2] = {r0, r1};
    FlutterVenusTextLayoutLineMetricV2 lines[4]{};
    FlutterVenusTextLayoutFragmentRectV2 fragments[8]{};
    w.input.runs = two ? runs : &runs[1];
    w.input.run_count = two ? 2u : 1u;
    w.input.text = reinterpret_cast<const uint8_t*>(w.text.data());
    w.input.text_length = static_cast<uint32_t>(w.text.size());
    w.input.line_metrics_out = lines;
    w.input.line_metric_capacity = 4u;
    w.input.fragment_rects_out = fragments;
    w.input.fragment_rect_capacity = 8u;
    w.input.max_width = max_width;
    Laid out;
    if (parent_size >= 0.0) {
      const uint32_t offset = static_cast<uint32_t>(w.family.size());
      w.family += parent_family;
      w.input.font_families = reinterpret_cast<const uint8_t*>(w.family.data());
      w.input.font_families_length = static_cast<uint32_t>(w.family.size());
      VenusTextParentFontV1 font{two ? 1u : 0u, offset,
          static_cast<uint32_t>(parent_family.size()), 400u, 0u, 0u, parent_size};
      VenusParagraphParentFontsV1 parents{sizeof(parents), 1u, 1u, 0u, &font};
      out.status = FlutterVenusTextLayoutParagraphWithParentFontsV1(
          kEngineId, slot, lease, &w.input, nullptr, &parents, &w.output);
    } else {
      out.status = FlutterVenusTextLayoutParagraphV2(kEngineId, slot, lease,
                                                   &w.input, &w.output);
    }
    out.line = lines[0];
    for (uint32_t i = 0; i < w.output.fragment_rect_count; ++i) {
      const uint32_t which = two ? fragments[i].run_index : 1u;
      (which == 0u ? out.first : out.second) = fragments[i];
    }
    return out;
  };

  const Laid base = lay(40.0, 16.0, 0u);
  ASSERT_EQ(base.status, kFlutterVenusV2Ok);
  ASSERT_GT(base.second.top, 5.0) << "对照组: baseline 对齐的小字顶在行顶下方";

  const Laid top = lay(40.0, 16.0, 1u);
  ASSERT_EQ(top.status, kFlutterVenusV2Ok) << "top 文本 run 应被接受";
  EXPECT_NEAR(top.second.top, 0.0, 0.5) << "top: 小字盒顶贴行盒上沿";
  EXPECT_DOUBLE_EQ(top.line.height, base.line.height) << "行盒仍由大字决定";
  EXPECT_DOUBLE_EQ(top.first.top, base.first.top) << "其余内容不动";

  const Laid bottom = lay(40.0, 16.0, 3u);
  ASSERT_EQ(bottom.status, kFlutterVenusV2Ok) << "bottom 文本 run 应被接受";
  EXPECT_NEAR(bottom.second.bottom, bottom.line.height, 0.5)
      << "bottom: 小字盒底贴行盒下沿";
  EXPECT_DOUBLE_EQ(bottom.line.height, base.line.height);

  for (const double length : {-1.0, 0.0, 1.0}) {
    const Laid shifted = lay(40.0, 16.0, 6u, "b", 200.0, 0.0, length);
    ASSERT_EQ(shifted.status, kFlutterVenusV2Ok);
    EXPECT_NEAR(shifted.second.top, base.second.top - length, 0.0001);
    EXPECT_NEAR(shifted.second.bottom, base.second.bottom - length, 0.0001);
    EXPECT_DOUBLE_EQ(shifted.first.top, base.first.top);
    EXPECT_DOUBLE_EQ(shifted.line.height, base.line.height);
  }

  // top 盒比其余内容高: 行盒向下长, 其余内容 (基线组) 留在行顶。
  const Laid tall_base = lay(16.0, 40.0, 0u);
  const Laid tall_top = lay(16.0, 40.0, 1u);
  ASSERT_EQ(tall_top.status, kFlutterVenusV2Ok);
  EXPECT_NEAR(tall_top.second.top, 0.0, 0.5);
  EXPECT_NEAR(tall_top.first.top, 0.0, 0.5) << "基线组贴行顶";
  EXPECT_GT(tall_base.first.top, 5.0) << "对照组: baseline 时小字被大字压低";
  EXPECT_NEAR(tall_top.line.height,
              tall_base.second.bottom - tall_base.second.top, 0.5)
      << "行高 = 高盒自身高 (基线组整个装在里面)";

  // 反判据: 行里只有它自己 -> 与 baseline 逐位相同。
  const Laid alone_base = lay(0.0, 16.0, 0u);
  const Laid alone_top = lay(0.0, 16.0, 1u);
  ASSERT_EQ(alone_top.status, kFlutterVenusV2Ok);
  EXPECT_DOUBLE_EQ(alone_top.second.top, alone_base.second.top);
  EXPECT_DOUBLE_EQ(alone_top.line.height, alone_base.line.height);

  // 声明行高 (CSS `line-height: 1`): Skia 走行高覆盖分支。真机对照页 (venus
  // scratchpad valign/anticase.html) 修前行盒 61 高、Chrome 40 —— 位移在行度量里算了两遍。
  // 判据: 行盒仍由大字决定; 小字盒顶 / 底到行顶 / 行底的距离与它独占一行时相同。
  const Laid lh_base = lay(40.0, 16.0, 0u, "b", 200.0, 1.0);
  const Laid lh_top = lay(40.0, 16.0, 1u, "b", 200.0, 1.0);
  const Laid lh_bottom = lay(40.0, 16.0, 3u, "b", 200.0, 1.0);
  const Laid lh_alone = lay(0.0, 16.0, 0u, "b", 200.0, 1.0);
  ASSERT_EQ(lh_top.status, kFlutterVenusV2Ok);
  ASSERT_EQ(lh_bottom.status, kFlutterVenusV2Ok);
  EXPECT_DOUBLE_EQ(lh_top.line.height, lh_base.line.height)
      << "声明行高的 top: 行盒不许被位移撑高";
  EXPECT_DOUBLE_EQ(lh_bottom.line.height, lh_base.line.height)
      << "声明行高的 bottom: 行盒不许被位移撑高";
  EXPECT_NEAR(lh_top.second.top, lh_alone.second.top, 0.5)
      << "top: 盒顶贴行顶 (紧矩形顶相对行顶与独占一行时同)";
  EXPECT_NEAR(lh_bottom.line.height - lh_bottom.second.bottom,
              lh_alone.line.height - lh_alone.second.bottom, 0.5)
      << "bottom: 盒底贴行底";

  // 跨行的对齐 run 表达不了 (每行各自对齐要按行切 run): 必须失败, 不许静默按 baseline 排。
  const Laid split = lay(40.0, 16.0, 1u, "bb bb bb bb bb bb", 60.0);
  EXPECT_NE(split.status, kFlutterVenusV2Ok);

  // 其余档位 (middle 等) 与非文本 run 上的非 0 值仍是非法参数。
  EXPECT_EQ(lay(40.0, 16.0, 2u).status, kFlutterVenusV2InvalidArgument);

  // The first sibling remains 60px while the actual parent varies. Using
  // that first run, child font size, or half an em must fail these relations.
  for (double parent_size : {16.0, 24.0, 32.0}) {
    for (const std::string& family : {std::string("Roboto"), std::string("MissingParent\x1fRoboto")}) {
      SCOPED_TRACE(parent_size);
      SCOPED_TRACE(family);
      const auto baseline = lay(60.0, 28.0, 0u);
      const auto middle = lay(60.0, 28.0, 2u, "b", 200.0, 0.0, 0.0, parent_size, family);
      ASSERT_EQ(middle.status, kFlutterVenusV2Ok);
      venus_text_layout::ParagraphInputView parent;
      venus_text_layout::ParagraphRunView font;
      font.text = u"x";
      font.font_family = family;
      font.font_size = parent_size;
      parent.runs.push_back(font);
      parent.max_width = 200.0;
      venus_text_layout::ParagraphMetrics metrics;
      std::unique_ptr<txt::Paragraph> paragraph;
      ASSERT_TRUE(venus_text_layout::LayoutParagraphCore(
          parent, fonts->GetFontCollection(), false, &metrics, &paragraph).ok());
      const double x_height = paragraph->GetLineMetrics().front().run_metrics.begin()->second.font_metrics.fXHeight;
      const auto own = lay(0.0, 28.0, 0u);
      const double shift = own.line.baseline - own.line.height / 2.0 - x_height / 2.0;
      EXPECT_NEAR(middle.second.top - baseline.second.top, shift, 0.001);
      EXPECT_NEAR(middle.second.bottom - baseline.second.bottom, shift, 0.001);
      EXPECT_DOUBLE_EQ(middle.first.top, baseline.first.top);
    }
  }
  {
    ParagraphWire w;
    w.run.text_vertical_align = 2u;
    VenusTextParentFontV1 parent{0u, 0u, static_cast<uint32_t>(w.family.size()), 400u, 0u, 0u, 16.0};
    VenusParagraphParentFontsV1 parents{sizeof(parents), 1u, 1u, 0u, &parent};
    struct Bad { const char* name; void (*mutate)(VenusTextParentFontV1&, VenusParagraphParentFontsV1&); };
    const Bad cases[] = {
      {"index", [](auto& p, auto&) { p.run_index = 1u; }},
      {"family range", [](auto& p, auto&) { p.font_family_offset = UINT32_MAX; }},
      {"nonfinite", [](auto& p, auto&) { p.font_size = std::nan(""); }},
      {"negative", [](auto& p, auto&) { p.font_size = -1.0; }},
      {"size limit", [](auto& p, auto&) { p.font_size = 4097.0; }},
      {"weight", [](auto& p, auto&) { p.font_weight = 1001u; }},
      {"style", [](auto& p, auto&) { p.font_style = 2u; }},
      {"reserved", [](auto& p, auto&) { p.reserved = 1u; }},
      {"missing", [](auto&, auto& p) { p.count = 0u; p.fonts = nullptr; }},
      {"extra", [](auto&, auto& p) { p.count = 2u; }},
    };
    for (const auto& bad : cases) {
      SCOPED_TRACE(bad.name);
      auto font = parent; auto sidecar = parents; sidecar.fonts = &font;
      bad.mutate(font, sidecar);
      w.output.struct_size = sizeof(w.output);
      EXPECT_EQ(FlutterVenusTextLayoutParagraphWithParentFontsV1(
          kEngineId, slot, lease, &w.input, nullptr, &sidecar, &w.output), kFlutterVenusV2InvalidArgument);
    }
    EXPECT_EQ(FlutterVenusTextLayoutParagraphWithParentFontsV1(
        kEngineId, slot, lease, &w.input, nullptr, nullptr, &w.output), kFlutterVenusV2InvalidArgument);
  }

  EXPECT_EQ(api.release_slot(api.context, slot, lease), kFlutterVenusV2Ok);
  api.release_context(api.context);
  VenusTextLayoutRegistry::Unregister(kEngineId, generation);
}

}  // namespace testing
}  // namespace flutter

int main(int argc, char** argv) {
  fml::icu::InitializeICU("icudtl.dat");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
