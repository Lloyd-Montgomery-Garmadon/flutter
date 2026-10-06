// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_LIB_UI_TEXT_VENUS_TEXT_LAYOUT_NODE_CORE_H_
#define FLUTTER_LIB_UI_TEXT_VENUS_TEXT_LAYOUT_NODE_CORE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <string_view>

#include "flutter/lib/ui/text/venus_text_layout_batch_oracle.h"
#include "txt/paragraph.h"

namespace txt {
class FontCollection;
class ParagraphStyle;
class TextStyle;
}  // namespace txt

namespace flutter {
namespace venus_text_layout {

namespace wire = venus_text_layout_batch;

// Item-level failure triple. Mirrors the Phase 1 wire vocabulary so the debug
// batch oracle and both V2 transports report identical status/field/detail.
struct ItemFailure {
  wire::ItemStatus status = wire::ItemStatus::kOk;
  wire::ErrorFieldId field = wire::ErrorFieldId::kNone;
  wire::ItemErrorDetail detail = wire::ItemErrorDetail::kNone;

  bool ok() const { return status == wire::ItemStatus::kOk; }
};

// The core's sole input. This is the Phase 1 `ValidatedItem` shape: a parsed
// read-only view plus the already-decoded UTF-16 text and ellipsis.
//
// UTF-8 -> UTF-16 decoding stays in the adapters (the debug oracle's parser,
// the sync adapter's validation step, the async adapter's deep-copy step) so
// that all three feed the core an identical input type. That identity is the
// premise of the blob-path vs native-path bit-for-bit parity guard.
struct ParsedNodeView {
  wire::ParsedItemView view;
  std::u16string text;
  std::u16string ellipsis;
};

// The core's sole output. Field-for-field the geometry half of
// FlutterVenusTextLayoutOutputV2; the adapters copy it out, they never
// recompute any of it.
struct NodeMetrics {
  double paragraph_width = 0.0;
  double paragraph_height = 0.0;
  double box_width = 0.0;
  double box_height = 0.0;
  double min_intrinsic_width = 0.0;
  double max_intrinsic_width = 0.0;
  double alphabetic_baseline = 0.0;
  double ideographic_baseline = 0.0;
  double first_line_left = 0.0;
  /// 最后一行的宽度。多行时它通常小于 paragraph_width —— 调用方要在同一行里
  /// 接着排后续内联内容, 靠的就是这个数。
  double last_line_width = 0.0;
  /// 首行内容区高度 = ascent + descent (不含行距)。
  /// `display:inline` 元素的盒高用它, 不是行高 —— 见公共头同名字段。
  double content_height = 0.0;
  uint32_t line_count = 0u;
  bool did_exceed_max_lines = false;
};

// THE single-node layout core. Pure with respect to engine state: it touches no
// global, keeps nothing alive past the call, and holds no reference other than
// `fonts` for the duration. Safe to call from any thread that owns an exclusive
// slot, which is what lets the sync adapter, the async workers and the debug
// oracle all share exactly one implementation of field mapping, shaping, line
// breaking, ellipsis, metrics and box constrain.
//
// On kOk `out_metrics` is fully populated; otherwise it is left untouched and
// the caller writes canonical zeros.
// The frozen wire -> ParagraphStyle/TextStyle field mapping. Exposed because the
// debug oracle's test seam must observe exactly the mapping the core consumes;
// there is no second copy of it anywhere.
// 把线材上的 `font_family` 段拆成一条 CSS font-family 链。
//
// 那一段承载的是**整条链**，用 US(0x1F) 连接，首项是主字体。理由见 .cc:
// 实测 venus example 里 96% 的 `font-family` 声明是多族的，线材只带一个 family
// 会让 Dart 侧的准入闸挡掉绝大多数文本节点。
//
// 老调用方送单个字体名（没有 US）时拆出来就是单元素 —— 向后兼容。
std::vector<std::string> SplitFontFamiliesForTesting(std::string_view joined);

// 把一条 font-family 链落到 ParagraphStyle(取链首作默认字体)与
// TextStyle(整条链参与字形回落)上。
//
// **抽成具名函数是为了能测到交付物那一层。** 只测 SplitFontFamilies 的话,
// 变异「只取链首、fallback 全丢」会让全部用例照绿 —— 而那正是这次改动的核心
// 行为(实测踩过)。核心与用例走同一个入口, 漏不掉。
void ApplyFontFamilyChain(std::string_view joined,
                          txt::ParagraphStyle* paragraph_style,
                          txt::TextStyle* text_style);

wire::TextStyleMappingForTesting ResolveTextStyleMapping(
    const wire::ParsedItemView& item);

// `out_paragraph` 非空且排版成功时，把**排好版的** paragraph 交出来而不是析构 ——
// Phase 3 的采用路径靠它。传 nullptr 则行为与 Phase 2 逐字节一致（用完即弃）。
ItemFailure LayoutTextNodeCore(
    const ParsedNodeView& node,
    const std::shared_ptr<txt::FontCollection>& fonts,
    bool impeller_enabled,
    NodeMetrics* out_metrics,
    std::unique_ptr<txt::Paragraph>* out_paragraph = nullptr);


// ── 段落粒度 (一个 IFC 一次排完) ─────────────────────────
//
// 与上面那套单节点的区别只有一条, 但很要紧: **它交出片段矩形。**
// CSS 里内联元素的矩形是各行片段的并集, 单节点那套一个 run 只能给一个外接矩形,
// 表达不了 —— 调用方 (venus 布局内核) 因此只能整页 fail-closed。
//
// 底下就是 txt::Paragraph 的 PushStyle/Pop/AddPlaceholder +
// GetLineMetrics/GetRectsForRange, **不是新写一个 IFC**。
struct ParagraphRunView {
  bool is_placeholder = false;
  /// 硬换行 (`<br>`)。段落里写一个 U+000A —— **由这里写, 不由调用方混进文本**,
  /// 理由见公共头 kFlutterVenusV2RunKindLineBreak 的说明。
  bool is_line_break = false;
  std::u16string text;          ///< is_placeholder 时为空
  std::string font_family;      ///< US(0x1f) 分隔的整条链, 与单节点那套同口径
  double font_size = 0.0;
  int font_weight = 400;
  bool italic = false;
  double letter_spacing = 0.0;
  double word_spacing = 0.0;
  double height_multiple = 0.0;  ///< 0 = 不覆盖 (除非 height_declared)
  /// 行高是作者声明的值 —— **0 也算** (CSS `line-height: 0`, 零高行内盒)。
  /// false 时沿用旧口径: 0 = 字体自然行高。
  bool height_declared = false;
  uint32_t color_argb = 0u;
  double placeholder_width = 0.0;
  double placeholder_height = 0.0;
  double placeholder_baseline_offset = 0.0;
  /// 占位盒的垂直对齐 (CSS vertical-align)。口径 = venus 内核 VerticalAlign:
  /// 0=baseline 1=top 2=middle 3=bottom 4=text-top 5=text-bottom 6=<length>。
  /// **0 是既有行为** —— 这个字段以前恒为 0(硬编码 kBaseline), 所以新增它对
  /// 老调用方是无变化的。
  uint16_t placeholder_alignment = 0;
  /// 文本 run 所属非替换 inline 包装盒的 CSS vertical-align (公共头
  /// `text_vertical_align`): 0=baseline 1=top 3=bottom。`LayoutParagraphCore`
  /// 把它换算成下面的 `baseline_shift` 后清零, 再排正式那一趟。
  uint16_t text_vertical_align = 0;
  /// 以下两个只在引擎内部两趟排版之间传递, 不上线材。
  /// 相对行基线的竖直位移 (向下为正), 转给 Skia `setBaselineShift`。
  double baseline_shift = 0.0;
  /// 探针趟: 行高按比例缩成 0 (上/下沿都收到基线上), 不参与行盒。
  bool collapse_to_baseline = false;
};

struct ParagraphInputView {
  std::vector<ParagraphRunView> runs;
  std::string locale;
  double max_width = 0.0;
  double height_multiple = 0.0;
  bool strut_enabled = false;
  std::string strut_font_family;
  double strut_font_size = 0.0;
  int strut_font_weight = 0;
  bool strut_italic = false;
  double strut_height_multiple = 0.0;
  wire::TextAlign text_align = wire::TextAlign::kStart;
  wire::TextDirection text_direction = wire::TextDirection::kLtr;
  bool soft_wrap = true;
  uint32_t max_lines = 0u;  ///< 0 = 不限
};

struct ParagraphLineOut {
  uint32_t line_number = 0u;
  double left = 0.0, width = 0.0, height = 0.0, baseline = 0.0;
  double ascent = 0.0, descent = 0.0;
};

struct ParagraphFragmentOut {
  uint32_t run_index = 0u;
  uint32_t line_number = 0u;
  double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
};

struct ParagraphMetrics {
  std::vector<ParagraphLineOut> lines;
  std::vector<ParagraphFragmentOut> fragments;
  double width = 0.0;
  double height = 0.0;
  double first_baseline = 0.0;
  double min_intrinsic_width = 0.0;
  double max_intrinsic_width = 0.0;
};

ItemFailure LayoutParagraphCore(const ParagraphInputView& input,
                                const std::shared_ptr<txt::FontCollection>& fonts,
                                bool impeller_enabled,
                                ParagraphMetrics* out_metrics,
                                std::unique_ptr<txt::Paragraph>* out_paragraph = nullptr);

}  // namespace venus_text_layout
}  // namespace flutter

#endif  // FLUTTER_LIB_UI_TEXT_VENUS_TEXT_LAYOUT_NODE_CORE_H_
