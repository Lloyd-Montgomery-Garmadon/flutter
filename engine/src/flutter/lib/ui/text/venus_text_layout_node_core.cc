// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/text/venus_text_layout_node_core.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "flutter/txt/src/txt/paragraph.h"
#include "flutter/txt/src/txt/paragraph_builder.h"
#include "flutter/txt/src/txt/paragraph_style.h"
#include "flutter/txt/src/txt/placeholder_run.h"

namespace flutter {

namespace {

// `font_family` 这一段 payload 承载的是**一整条 CSS font-family 链**，
// 用 US(0x1F) 连接，第一个是主字体，其余是 fallback。
//
// ## 为什么是这样，而不是"只带一个 family"
//
// 实测：venus 的 example 里 446 处 `font-family` 声明，**430 处是多族的（96%）**。
// 而 `FontFamilyResolver` 连 `font-family: X, sans-serif` 这种也会展开成两项。
// 线材只带一个 family 的话，Dart 侧的准入闸（fallback 非空即拒 —— 拒是对的，
// 主字体缺字时用哪个补直接决定字形宽度）会挡掉 96% 的文本节点，
// 这条路在真实页面上就是**接近全灭**。
//
// 而 `txt::TextStyle::font_families` 本来就是 `std::vector<std::string>` ——
// 线材一个字节都不用动，把这段字符串按 US 拆开就行。
//
// **向后兼容**：老调用方送的是单个字体名，里面没有 US，拆出来就是单元素。
constexpr char kFontFamilySeparator = '\x1f';

std::vector<std::string> SplitFontFamilies(std::string_view joined) {
  std::vector<std::string> out;
  size_t begin = 0;
  while (begin <= joined.size()) {
    const size_t end = joined.find(kFontFamilySeparator, begin);
    const std::string_view piece = end == std::string_view::npos
                                       ? joined.substr(begin)
                                       : joined.substr(begin, end - begin);
    if (!piece.empty()) {
      out.emplace_back(piece);
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return out;
}

}  // namespace

namespace venus_text_layout {

namespace {

/// venus 内核 VerticalAlign -> txt::PlaceholderAlignment。
///
/// 0=baseline 1=top 2=middle 3=bottom 4=text-top 5=text-bottom 6=<length>。
/// 未覆盖的档位一律回落 kBaseline —— **那是既有行为**, 不是新的近似;
/// 要正确支持它们得先在 txt 侧有对应语义 (见 D20 的记录)。
txt::PlaceholderAlignment VenusVerticalAlignToPlaceholder(uint16_t v) {
  switch (v) {
    case 1: return txt::PlaceholderAlignment::kTop;
    case 2: return txt::PlaceholderAlignment::kMiddle;
    case 3: return txt::PlaceholderAlignment::kBottom;
    default: return txt::PlaceholderAlignment::kBaseline;
  }
}

}  // namespace


std::vector<std::string> SplitFontFamiliesForTesting(std::string_view joined) {
  return SplitFontFamilies(joined);
}

void ApplyFontFamilyChain(std::string_view joined,
                          txt::ParagraphStyle* paragraph_style,
                          txt::TextStyle* text_style) {
  std::vector<std::string> families = SplitFontFamilies(joined);
  if (paragraph_style != nullptr) {
    // ParagraphStyle 只收一个默认字体 —— 取链首(主字体)。
    paragraph_style->font_family =
        families.empty() ? std::string() : families.front();
  }
  if (text_style != nullptr) {
    // 整条链都给它: 主字体缺字形时按顺序回落, 这正是 CSS font-family 的语义,
    // 也是 Dart 侧 fontFamily + fontFamilyFallback 的合并结果。
    text_style->font_families = std::move(families);
  }
}

}  // namespace venus_text_layout

namespace venus_text_layout {
namespace {

ItemFailure FailItem(wire::ItemStatus status,
                     wire::ErrorFieldId field,
                     wire::ItemErrorDetail detail) {
  return ItemFailure{status, field, detail};
}

txt::TextDirection ToTxtDirection(wire::TextDirection direction) {
  switch (direction) {
    case wire::TextDirection::kLtr:
      return txt::TextDirection::ltr;
    case wire::TextDirection::kRtl:
      return txt::TextDirection::rtl;
  }
  return txt::TextDirection::ltr;
}

txt::TextAlign ToTxtAlign(wire::TextAlign align) {
  switch (align) {
    case wire::TextAlign::kLeft:
      return txt::TextAlign::left;
    case wire::TextAlign::kRight:
      return txt::TextAlign::right;
    case wire::TextAlign::kCenter:
      return txt::TextAlign::center;
    case wire::TextAlign::kJustify:
      return txt::TextAlign::justify;
    case wire::TextAlign::kStart:
      return txt::TextAlign::start;
    case wire::TextAlign::kEnd:
      return txt::TextAlign::end;
  }
  return txt::TextAlign::start;
}

txt::FontStyle ToTxtFontStyle(wire::FontStyle style) {
  switch (style) {
    case wire::FontStyle::kNormal:
      return txt::FontStyle::normal;
    case wire::FontStyle::kItalic:
      return txt::FontStyle::italic;
  }
  return txt::FontStyle::normal;
}

double PaintOffsetFraction(wire::TextAlign align,
                           wire::TextDirection direction) {
  switch (align) {
    case wire::TextAlign::kLeft:
      return 0.0;
    case wire::TextAlign::kRight:
      return 1.0;
    case wire::TextAlign::kCenter:
      return 0.5;
    case wire::TextAlign::kStart:
    case wire::TextAlign::kJustify:
      return direction == wire::TextDirection::kLtr ? 0.0 : 1.0;
    case wire::TextAlign::kEnd:
      return direction == wire::TextDirection::kLtr ? 1.0 : 0.0;
  }
  return 0.0;
}

}  // namespace

wire::TextStyleMappingForTesting ResolveTextStyleMapping(
    const wire::ParsedItemView& item) {
  wire::TextStyleMappingForTesting mapping;
  mapping.effective_font_size =
      item.text_scaler_kind == wire::TextScalerKind::kLinear
          ? item.font_size * item.scaler_parameter
          : item.font_size;
  mapping.height = item.height;
  mapping.paragraph_font_family = item.font_family;
  mapping.run_font_family = item.font_family;
  mapping.paragraph_locale = item.locale;
  mapping.run_locale = item.locale;
  mapping.paragraph_font_weight = item.font_weight;
  mapping.run_font_weight = item.font_weight;
  mapping.paragraph_font_style = item.font_style;
  mapping.run_font_style = item.font_style;
  mapping.run_letter_spacing = item.letter_spacing;
  mapping.run_word_spacing = item.word_spacing;
  mapping.paragraph_has_height_override =
      item.height_mode == wire::HeightMode::kMultiplier;
  mapping.run_has_height_override = mapping.paragraph_has_height_override;
  mapping.half_leading =
      item.leading_distribution == wire::LeadingDistribution::kEven;
  mapping.disable_first_ascent = item.apply_height_to_first_ascent ==
                                 wire::ApplyHeightToFirstAscent::kFalse;
  mapping.disable_last_descent = item.apply_height_to_last_descent ==
                                 wire::ApplyHeightToLastDescent::kFalse;
  return mapping;
}

ItemFailure LayoutTextNodeCore(const ParsedNodeView& item,
                               const std::shared_ptr<txt::FontCollection>& fonts,
                               bool impeller_enabled,
                               NodeMetrics* out_metrics,
                               std::unique_ptr<txt::Paragraph>* out_paragraph) {
  const wire::TextStyleMappingForTesting mapping =
      ResolveTextStyleMapping(item.view);
  txt::ParagraphStyle paragraph_style;
  paragraph_style.font_weight = static_cast<int>(mapping.paragraph_font_weight);
  paragraph_style.font_style = ToTxtFontStyle(mapping.paragraph_font_style);
  // font-family 链在下面 ApplyFontFamilyChain 里一次落到两个 style 上。
  paragraph_style.font_size = mapping.effective_font_size;
  paragraph_style.text_align = ToTxtAlign(item.view.text_align);
  paragraph_style.text_direction = ToTxtDirection(item.view.text_direction);
  paragraph_style.max_lines =
      item.view.max_lines_mode == wire::MaxLinesMode::kUnlimited
          ? std::numeric_limits<size_t>::max()
          : static_cast<size_t>(item.view.max_lines);
  paragraph_style.ellipsis = item.ellipsis;
  paragraph_style.locale = std::string(mapping.paragraph_locale);
  paragraph_style.text_height_behavior = txt::TextHeightBehavior::kAll;
  if (mapping.disable_first_ascent) {
    paragraph_style.text_height_behavior |=
        txt::TextHeightBehavior::kDisableFirstAscent;
  }
  if (mapping.disable_last_descent) {
    paragraph_style.text_height_behavior |=
        txt::TextHeightBehavior::kDisableLastDescent;
  }
  if (mapping.paragraph_has_height_override) {
    paragraph_style.height = mapping.height;
    paragraph_style.has_height_override = mapping.paragraph_has_height_override;
  }

  txt::TextStyle text_style = paragraph_style.GetTextStyle();
  text_style.font_size = mapping.effective_font_size;
  text_style.font_weight = static_cast<int>(mapping.run_font_weight);
  text_style.font_style = ToTxtFontStyle(mapping.run_font_style);
  venus_text_layout::ApplyFontFamilyChain(mapping.run_font_family,
                                          &paragraph_style, &text_style);
  // 颜色一律照搬。**不能再把 0 当"未指定"**: `color: transparent` 的 ARGB 正是
  // 0, 跳过赋色会让 txt 保留默认的 SK_ColorWHITE, 而这个段落同时是绘制产物 ——
  // 于是"按规范不可见"的文字被画成不透明白字, 把底下的东西擦掉
  // (判据页 WPT CSS2/abspos/static-inside-inline-002)。
  // 生产方那侧已经保证 0 只可能是作者声明的透明: venus 的 TextSource::color_argb
  // 缺省就是 CSS 初值不透明黑, 按 presence 覆盖; Dart 预排路送的是
  // TextStyle.color 的 ARGB。Phase 2 corpus 从不绘制, 拿到黑还是白都不影响它。
  text_style.color = static_cast<SkColor>(item.view.text_color_argb);
  text_style.locale = std::string(mapping.run_locale);
  text_style.letter_spacing = mapping.run_letter_spacing;
  text_style.word_spacing = mapping.run_word_spacing;
  text_style.half_leading = mapping.half_leading;
  if (mapping.run_has_height_override) {
    text_style.height = mapping.height;
    text_style.has_height_override = mapping.run_has_height_override;
  } else {
    text_style.has_height_override = false;
  }

  std::unique_ptr<txt::ParagraphBuilder> builder =
      txt::ParagraphBuilder::CreateSkiaBuilder(paragraph_style, fonts,
                                               impeller_enabled);
  if (!builder) {
    return FailItem(wire::ItemStatus::kParagraphBuildFailed,
                    wire::ErrorFieldId::kSemanticsCombination,
                    wire::ItemErrorDetail::kParagraphNull);
  }
  // ── IFC 续排的行内起始偏移 ──
  //
  // 这一段文字不一定从行首开始: `<div>说明 <code>x</code> 后续文字</div>` 里,
  // 后半段的**首行**只剩 (max_width - 前面已占宽) 可用, 后续行才是整宽。
  // 用一个等宽、零高的占位表达它 —— Flutter 的内联 widget 本来就是这么占位的。
  //
  // 只在会断行时插: soft_wrap 关掉、或 max_width 不是有限值 (量 max-content)
  // 时都不断行, 插了只会白白污染宽度。
  const bool has_leading_gap =
      item.view.leading_placeholder_width > 0.0 &&
      std::isfinite(item.view.max_width) &&
      item.view.soft_wrap == wire::SoftWrap::kTrue;
  if (has_leading_gap) {
    // 零高 + kBaseline + baseline_offset=0: 占位不参与行高。
    // 非零高会把首行撑高, 而调用方要的只是"首行少这么多可用宽"。
    txt::PlaceholderRun gap(item.view.leading_placeholder_width, 0.0,
                            txt::PlaceholderAlignment::kBaseline,
                            txt::TextBaseline::kAlphabetic, 0.0);
    builder->AddPlaceholder(gap);
  }
  builder->PushStyle(text_style);
  builder->AddText(item.text);
  builder->Pop();
  std::unique_ptr<txt::Paragraph> paragraph = builder->Build();
  if (!paragraph) {
    return FailItem(wire::ItemStatus::kParagraphBuildFailed,
                    wire::ErrorFieldId::kSemanticsCombination,
                    wire::ItemErrorDetail::kParagraphNull);
  }
  paragraph->Layout(item.view.max_width);
  const std::vector<txt::LineMetrics>& lines = paragraph->GetLineMetrics();
  if (lines.empty()) {
    return FailItem(wire::ItemStatus::kParagraphBuildFailed,
                    wire::ErrorFieldId::kSemanticsCombination,
                    wire::ItemErrorDetail::kParagraphNull);
  }

  // **占位只影响断行, 不参与任何对外宽度。**
  //
  // 不扣的话调用方拿到的宽度平白多出 leading_placeholder_width —— 而它本来
  // 就是调用方自己算出来送进来的, 再原样还回去等于把同一段宽度算了两次。
  // 首行以外的行不含占位, 所以只扣首行。
  const double gap = has_leading_gap ? item.view.leading_placeholder_width : 0.0;
  // **没有占位时, 一个字节都不改道。**
  //
  // 下面那条按 LineMetrics 逐行取最大的路是为了扣掉首行里的占位。它与
  // `GetLongestLine()` 在"不含行尾空白"上口径相同, 所以**大概率**等价 ——
  // 但"大概率等价"正是本项目已经栽过两次的那种理由 (见 AGENTS.md 第一性原理
  // 条: 碰巧一致不是一致)。Dart 自己的渲染也走这个函数, 拿它去赌等于把整条
  // Dart 基线押在一个没验过的假设上。
  //
  // 所以 gap == 0 时原样用 GetLongestLine(), 与改动前逐字节相同; 新路只在
  // 真有占位时启用 —— 那条路本来就没有旧行为可比。
  double longest_line = 0.0;
  if (has_leading_gap) {
    for (size_t i = 0; i < lines.size(); ++i) {
      const double w = lines[i].width - (i == 0 ? gap : 0.0);
      longest_line = std::max(longest_line, w > 0.0 ? w : 0.0);
    }
  } else {
    longest_line = paragraph->GetLongestLine();
  }
  const double raw_width =
      item.view.text_width_basis == wire::TextWidthBasis::kParent
          ? paragraph->GetMaxIntrinsicWidth() - gap
          : longest_line;
  const double painter_width =
      std::clamp(raw_width, item.view.min_width, item.view.max_width);
  const double painter_height = paragraph->GetHeight();
  const double paint_offset =
      PaintOffsetFraction(item.view.text_align, item.view.text_direction) *
      (painter_width - paragraph->GetMaxWidth());

  out_metrics->paragraph_width = painter_width;
  out_metrics->paragraph_height = painter_height;
  out_metrics->box_width =
      std::clamp(painter_width, item.view.min_width, item.view.max_width);
  out_metrics->box_height =
      std::clamp(painter_height, item.view.min_height, item.view.max_height);
  // 内在宽度同样要扣: 占位在 Skia 眼里是一个不可断片段, 不扣的话
  // min-intrinsic 会被它顶成"至少这么宽", max-intrinsic 会整体多一段。
  // 有占位时本来就不该拿这两个值去算内在尺寸 (那条路不断行、不送占位),
  // 但**留着一个被污染的值比留一个空洞更危险** —— 扣干净。
  // 同上: gap == 0 时不绕道, 与改动前逐字节相同。
  out_metrics->min_intrinsic_width =
      has_leading_gap ? std::max(0.0, paragraph->GetMinIntrinsicWidth() - gap)
                      : paragraph->GetMinIntrinsicWidth();
  out_metrics->max_intrinsic_width =
      has_leading_gap ? std::max(0.0, paragraph->GetMaxIntrinsicWidth() - gap)
                      : paragraph->GetMaxIntrinsicWidth();
  out_metrics->alphabetic_baseline = paragraph->GetAlphabeticBaseline();
  out_metrics->ideographic_baseline = paragraph->GetIdeographicBaseline();
  out_metrics->first_line_left = lines.front().left + paint_offset;
  // 末行宽度: LineMetrics::width 是这一行的排版宽度 (不含行尾空白的 ghost)。
  // 单行时它与整段等价; 多行时它是唯一能让调用方接着排同一行的数。
  out_metrics->last_line_width =
      lines.back().width - (has_leading_gap && lines.size() == 1u ? gap : 0.0);
  // ── 内容区高度: 必须取**字体度量**, 不是行的 ascent/descent ──
  //
  // `LineMetrics::ascent/descent` 是**排完版之后**的值 —— 注释原文:
  // "can be impacted by the strut, height, scaling"。设了 line-height
  // (height override) 时 Skia 会把它们缩放到填满行高, 于是 ascent+descent
  // 恒等于行高, 拿它当内容区等于什么都没做。
  //
  // 实测栽过: 初版用 lines.front().ascent+descent, mvp3 的 <code> 从 20.00
  // 变成 20.15 (= 13 x 1.55 的行高), 离目标的 15.23 更远了。
  //
  // `run_metrics` 才是要的那个 —— 注释原文: "The metrics here are before
  // layout and are the base values we calculate from"。SkFontMetrics 的
  // fAscent 是负数 (向上), fDescent 为正, 内容区 = -fAscent + fDescent。
  //
  // 取首个 run: 段内字号统一时精确; 混排不同字号是近似, 与 per_line_height
  // 那处同一口径。run_metrics 为空 (理论上不该发生) 时留 0, 调用方退回行高。
  {
    const auto& runs = lines.front().run_metrics;
    if (!runs.empty()) {
      const SkFontMetrics& fm = runs.begin()->second.font_metrics;
      out_metrics->content_height =
          static_cast<double>(-fm.fAscent + fm.fDescent);
    }
  }
  out_metrics->line_count = static_cast<uint32_t>(lines.size());
  out_metrics->did_exceed_max_lines = paragraph->DidExceedMaxLines();
  // Phase 3：调用方要的话就把排好版的 paragraph 交出去，而不是在这里析构掉。
  // Phase 2 一直在白白扔掉它，然后 Flutter 在 UI 线程上把同一件事再做一遍。
  // 传 nullptr 时行为与 Phase 2 逐字节一致。
  if (out_paragraph != nullptr) {
    *out_paragraph = std::move(paragraph);
  }
  return ItemFailure{};
}


// ── 段落粒度实现 ────────────────────────────────────────
//
// 与单节点那套共用全部 helper (ToTxtAlign / ToTxtDirection / ApplyFontFamilyChain)
// —— 这不是"另一条实现", 是同一个 txt::Paragraph 换了个调用粒度。分成两个入口
// 只因为语义不同: 一次一个文本节点 vs 一次一个 IFC。
ItemFailure LayoutParagraphCore(const ParagraphInputView& input,
                                const std::shared_ptr<txt::FontCollection>& fonts,
                                bool impeller_enabled,
                                ParagraphMetrics* out_metrics,
                                std::unique_ptr<txt::Paragraph>* out_paragraph) {
  if (out_metrics == nullptr || input.runs.empty()) {
    return FailItem(wire::ItemStatus::kParagraphBuildFailed,
                    wire::ErrorFieldId::kSemanticsCombination,
                    wire::ItemErrorDetail::kParagraphNull);
  }

  // 非替换 inline 包装盒的 `vertical-align: top / bottom` (公共头 text_vertical_align):
  // 它的盒对齐**最终行盒**上/下沿 (CSS 2.1 §10.8.1), 行盒由其余内容决定。三步:
  //   1. 探针趟: 对齐 run 按比例缩成 0 高 (上/下沿都落在基线上, 不抬行盒), 得每行
  //      其余内容的行升部 Ag / 行降部 Dg, 并记下对齐 run 落在哪一行;
  //   2. 该 run 自己的盒升部 A_r / 降部 D_r: 用它自己的样式单独排一个一 run 段落,
  //      读 Skia 的行度量 —— 不在这里抄 Run::calculateMetrics 的算式;
  //   3. 正式趟: top 位移 A_r − Ag, bottom 位移 Dg − D_r (y 向下为正)。Skia
  //      Run::ascent()/descent() 含位移, 行盒与紧矩形随之走, 下游零改动。
  // 盒比其余内容高时行盒向下 (top) / 向上 (bottom) 长, 其余内容留在行顶 / 行底。
  const auto text_aligned_to_line_box = [](const ParagraphRunView& run) {
    return !run.is_placeholder && !run.is_line_break &&
           (run.text_vertical_align == 1u || run.text_vertical_align == 3u);
  };
  if (std::any_of(input.runs.begin(), input.runs.end(),
                  text_aligned_to_line_box)) {
    const auto unsupported = [] {
      return FailItem(wire::ItemStatus::kParagraphBuildFailed,
                      wire::ErrorFieldId::kSemanticsCombination,
                      wire::ItemErrorDetail::kUnsupportedCombination);
    };
    ParagraphInputView probe_input = input;
    for (ParagraphRunView& run : probe_input.runs) {
      if (text_aligned_to_line_box(run)) {
        run.text_vertical_align = 0u;
        run.collapse_to_baseline = true;
      }
    }
    ParagraphMetrics probe;
    const ItemFailure probe_failure = LayoutParagraphCore(
        probe_input, fonts, impeller_enabled, &probe, nullptr);
    if (!probe_failure.ok()) {
      return probe_failure;
    }

    const size_t no_line = std::numeric_limits<size_t>::max();
    std::vector<size_t> run_lines(input.runs.size(), no_line);
    for (const ParagraphFragmentOut& fragment : probe.fragments) {
      const size_t i = fragment.run_index;
      if (i >= input.runs.size() || !text_aligned_to_line_box(input.runs[i])) {
        continue;
      }
      // ponytail: 跨行的对齐 run 要每行各自对齐 (按行切 run 分别求位移);
      // 线材一 run 一个位移表达不了, 失败而不是静默按首行排。
      if (run_lines[i] != no_line && run_lines[i] != fragment.line_number) {
        return unsupported();
      }
      run_lines[i] = fragment.line_number;
    }

    std::vector<double> line_ascents;
    std::vector<double> line_descents;
    line_ascents.reserve(probe.lines.size());
    line_descents.reserve(probe.lines.size());
    double line_top_y = 0.0;
    for (const ParagraphLineOut& line : probe.lines) {
      const double baseline_from_line_top = line.baseline - line_top_y;
      line_ascents.push_back(baseline_from_line_top);
      line_descents.push_back(line.height - baseline_from_line_top);
      line_top_y += line.height;
    }

    ParagraphInputView final_input = input;
    for (size_t i = 0; i < final_input.runs.size(); ++i) {
      ParagraphRunView& run = final_input.runs[i];
      if (!text_aligned_to_line_box(run)) {
        continue;
      }
      const uint16_t align = run.text_vertical_align;
      run.text_vertical_align = 0u;
      if (run.text.empty()) {
        continue;  // 空串 run 不画, 没有盒可对齐
      }
      const size_t line = run_lines[i];
      if (line == no_line || line >= line_ascents.size()) {
        return unsupported();
      }
      ParagraphInputView solo;
      solo.runs.push_back(run);
      solo.locale = input.locale;
      solo.text_align = input.text_align;
      solo.text_direction = input.text_direction;
      solo.soft_wrap = false;
      solo.max_width = std::numeric_limits<double>::infinity();
      ParagraphMetrics own;
      const ItemFailure own_failure =
          LayoutParagraphCore(solo, fonts, impeller_enabled, &own, nullptr);
      if (!own_failure.ok()) {
        return own_failure;
      }
      if (own.lines.size() != 1u) {
        return unsupported();
      }
      const double own_ascent = own.lines.front().baseline;
      const double own_descent = own.lines.front().height - own_ascent;
      run.baseline_shift = align == 1u ? own_ascent - line_ascents[line]
                                       : line_descents[line] - own_descent;
    }
    return LayoutParagraphCore(final_input, fonts, impeller_enabled,
                               out_metrics, out_paragraph);
  }

  // SkParagraph 的 top / bottom 是“字体上/下沿”，CSS top / bottom 是“最终行盒
  // 上/下沿” (CSS 2.1 §10.8.1)。两者只在行高恰为字体高时重合: 行高大于 normal 时差
  // 一个 half-leading, `line-height: 0` 时字体上沿比零高点低 (A-D)/2。所以这两档
  // 先换成零高 baseline 占位排一遍，拿到所属行的真实上沿/下沿，再换算成已有的
  // baseline offset；没有这两档的段落仍只排一遍。
  const auto aligned_to_line_box = [](const ParagraphRunView& run) {
    return run.is_placeholder &&
           (run.placeholder_alignment == 1u || run.placeholder_alignment == 3u);
  };
  const bool has_line_box_alignment =
      std::any_of(input.runs.begin(), input.runs.end(), aligned_to_line_box);
  if (has_line_box_alignment) {
    ParagraphInputView adjusted = input;
    for (ParagraphRunView& run : adjusted.runs) {
      if (aligned_to_line_box(run)) {
        run.placeholder_height = 0.0;
        run.placeholder_baseline_offset = 0.0;
        run.placeholder_alignment = 0u;
      }
    }

    ParagraphMetrics probe;
    const ItemFailure probe_failure = LayoutParagraphCore(
        adjusted, fonts, impeller_enabled, &probe, nullptr);
    if (!probe_failure.ok()) {
      return probe_failure;
    }

    const size_t no_line = std::numeric_limits<size_t>::max();
    std::vector<size_t> run_lines(input.runs.size(), no_line);
    for (const ParagraphFragmentOut& fragment : probe.fragments) {
      if (fragment.run_index < input.runs.size() &&
          aligned_to_line_box(input.runs[fragment.run_index])) {
        run_lines[fragment.run_index] = fragment.line_number;
      }
    }

    // 行盒上沿到基线 (升部) 与基线到行盒下沿 (降部)。
    std::vector<double> line_ascents;
    std::vector<double> line_descents;
    line_ascents.reserve(probe.lines.size());
    line_descents.reserve(probe.lines.size());
    double line_top_y = 0.0;
    for (const ParagraphLineOut& line : probe.lines) {
      const double baseline_from_line_top = line.baseline - line_top_y;
      line_ascents.push_back(baseline_from_line_top);
      line_descents.push_back(line.height - baseline_from_line_top);
      line_top_y += line.height;
    }
    for (size_t i = 0; i < adjusted.runs.size(); ++i) {
      ParagraphRunView& run = adjusted.runs[i];
      const ParagraphRunView& original = input.runs[i];
      if (!aligned_to_line_box(original)) {
        continue;
      }
      const size_t line = run_lines[i];
      if (line == no_line || line >= line_ascents.size()) {
        return FailItem(wire::ItemStatus::kParagraphBuildFailed,
                        wire::ErrorFieldId::kSemanticsCombination,
                        wire::ItemErrorDetail::kUnsupportedCombination);
      }
      run.placeholder_height = original.placeholder_height;
      // top: 占位顶贴行盒上沿 = 基线上方一个行升部;
      // bottom: 占位底贴行盒下沿 = 基线下方一个行降部。
      run.placeholder_baseline_offset =
          original.placeholder_alignment == 1u
              ? line_ascents[line]
              : original.placeholder_height - line_descents[line];
      run.placeholder_alignment = 0u;
    }
    return LayoutParagraphCore(adjusted, fonts, impeller_enabled, out_metrics,
                               out_paragraph);
  }

  txt::ParagraphStyle paragraph_style;
  paragraph_style.text_align = ToTxtAlign(input.text_align);
  paragraph_style.text_direction = ToTxtDirection(input.text_direction);
  paragraph_style.max_lines = input.max_lines == 0u
                                  ? std::numeric_limits<size_t>::max()
                                  : static_cast<size_t>(input.max_lines);
  paragraph_style.locale = input.locale;
  paragraph_style.text_height_behavior = txt::TextHeightBehavior::kAll;
  // CSS 段落: 声明的零行高是零高行内盒 (CSS 2.1 §10.8.1), 不是"未设置"。
  // 只有 height_declared 的 run 会带"覆盖 + 0", 其余 run 与旧口径逐位相同。
  paragraph_style.honor_zero_height_override = true;
  // 无容器 strut 的兼容路径以首个真实文本 run 作为换行默认样式。LineBreak
  // 本身没有字体，不能拿它的零值污染 paragraph default。
  for (const ParagraphRunView& r : input.runs) {
    if (!r.is_placeholder && !r.is_line_break) {
      ApplyFontFamilyChain(r.font_family, &paragraph_style, nullptr);
      paragraph_style.font_size = r.font_size;
      paragraph_style.font_weight = r.font_weight;
      paragraph_style.font_style =
          r.italic ? txt::FontStyle::italic : txt::FontStyle::normal;
      if (r.height_multiple > 0.0) {
        paragraph_style.height = r.height_multiple;
        paragraph_style.has_height_override = true;
        paragraph_style.half_leading = true;
      }
      break;
    }
  }
  if (input.height_multiple > 0.0) {
    paragraph_style.height = input.height_multiple;
    paragraph_style.has_height_override = true;
    paragraph_style.half_leading = true;
  }
  if (input.strut_enabled) {
    std::vector<std::string> strut_families =
        SplitFontFamilies(input.strut_font_family);
    paragraph_style.font_family =
        strut_families.empty() ? std::string() : strut_families.front();
    paragraph_style.font_size = input.strut_font_size;
    paragraph_style.font_weight = input.strut_font_weight;
    paragraph_style.font_style =
        input.strut_italic ? txt::FontStyle::italic : txt::FontStyle::normal;
    if (input.height_multiple <= 0.0) {
      paragraph_style.height = input.strut_height_multiple;
      paragraph_style.has_height_override = true;
    }
    paragraph_style.half_leading = true;
    paragraph_style.strut_enabled = true;
    paragraph_style.strut_font_families = std::move(strut_families);
    paragraph_style.strut_font_size = input.strut_font_size;
    paragraph_style.strut_font_weight = input.strut_font_weight;
    paragraph_style.strut_font_style =
        input.strut_italic ? txt::FontStyle::italic : txt::FontStyle::normal;
    paragraph_style.strut_height = input.strut_height_multiple;
    paragraph_style.strut_has_height_override = true;
    paragraph_style.strut_half_leading = true;
    paragraph_style.force_strut_height = false;
    // 每个真实文本 run 都会 PushStyle 自己的字号；paragraph default 只负责
    // 空行基准。让它也带一份字体度量会先造出负 leading，随后零高 placeholder
    // 把 leading 清零，10px CSS 行盒便会膨胀成 12px。空行只交给 strut 定义。
    paragraph_style.font_size = 0.0;
  }

  std::unique_ptr<txt::ParagraphBuilder> builder =
      txt::ParagraphBuilder::CreateSkiaBuilder(paragraph_style, fonts,
                                               impeller_enabled);
  if (!builder) {
    return FailItem(wire::ItemStatus::kParagraphBuildFailed,
                    wire::ErrorFieldId::kSemanticsCombination,
                    wire::ItemErrorDetail::kParagraphNull);
  }

  // 逐 run 记下它在段落里的 UTF-16 区间 —— GetRectsForRange 用的是 UTF-16
  // 码元下标 (与 dart:ui 同口径), 不是字节下标。两者混用是静默错位的经典来源。
  struct RunSpan {
    size_t begin = 0;
    size_t end = 0;
    bool placeholder = false;
    size_t placeholder_index = 0;
  };
  std::vector<RunSpan> spans;
  spans.reserve(input.runs.size());
  size_t cursor = 0;
  size_t placeholder_count = 0;
  for (const ParagraphRunView& r : input.runs) {
    RunSpan span;
    span.begin = cursor;
    if (r.is_line_break) {
      // 硬换行用当前段落样式写一个 U+000A。不 PushStyle: <br> 自己没有字形,
      // 换行点的度量由它所在的行盒决定。
      builder->AddText(std::u16string(1, u'\n'));
      cursor += 1;
    } else if (r.is_placeholder) {
      span.placeholder = true;
      span.placeholder_index = placeholder_count++;
      // **别再硬编码 kBaseline。** 以前这里恒 kBaseline, 于是 CSS 的
      // vertical-align: top/middle/bottom 对原子内联盒(inline-block 等)全部
      // 失效 —— 四档落在同一个位置 (venus 侧实测 40.584 x4, Chrome 是
      // 1.000/47.625/41.000/41.000)。见 venus docs/issue/ D20。
      //
      // 口径注意: txt 的 kTop/kBottom 文档写的是"对齐**字体**上/下沿",
      // 语义更接近 CSS 的 text-top/text-bottom; 而 CSS 的 top/bottom 是对齐
      // **行盒**上/下沿。所以这里的映射是**按实测校准的**, 不是按名字对。
      txt::PlaceholderRun ph(r.placeholder_width, r.placeholder_height,
                             VenusVerticalAlignToPlaceholder(
                                 r.placeholder_alignment),
                             txt::TextBaseline::kAlphabetic,
                             r.placeholder_baseline_offset);
      builder->AddPlaceholder(ph);
      // 占位在段落里占一个 U+FFFC (object replacement character)。
      cursor += 1;
    } else {
      txt::TextStyle text_style = paragraph_style.GetTextStyle();
      text_style.font_size = r.font_size;
      text_style.font_weight = r.font_weight;
      text_style.font_style =
          r.italic ? txt::FontStyle::italic : txt::FontStyle::normal;
      ApplyFontFamilyChain(r.font_family, &paragraph_style, &text_style);
      // 与单 run 路同一条规则: 0 是 `color: transparent`, 不是"未指定"。
      text_style.color = static_cast<SkColor>(r.color_argb);
      text_style.locale = input.locale;
      text_style.letter_spacing = r.letter_spacing;
      text_style.word_spacing = r.word_spacing;
      // 声明过的行高 (含 CSS `line-height: 0`) 一律覆盖; 段落已打开
      // honor_zero_height_override, Skia 不再把倍数 0 当"未设置"。
      if (r.height_multiple > 0.0 || r.height_declared) {
        text_style.height = r.height_multiple;
        text_style.has_height_override = true;
        text_style.half_leading = true;
      } else {
        text_style.has_height_override = false;
      }
      if (r.collapse_to_baseline) {
        // 按比例缩成 0 高 (不用 half-leading: 那会收成 (A+D)/2 那一点而不在基线上)。
        text_style.height = 0.0;
        text_style.has_height_override = true;
        text_style.half_leading = false;
      }
      text_style.baseline_shift = r.baseline_shift;
      builder->PushStyle(text_style);
      builder->AddText(r.text);
      builder->Pop();
      cursor += r.text.size();
    }
    span.end = cursor;
    spans.push_back(span);
  }

  std::unique_ptr<txt::Paragraph> paragraph = builder->Build();
  if (!paragraph) {
    return FailItem(wire::ItemStatus::kParagraphBuildFailed,
                    wire::ErrorFieldId::kSemanticsCombination,
                    wire::ItemErrorDetail::kParagraphNull);
  }
  const double layout_width =
      (input.soft_wrap && std::isfinite(input.max_width))
          ? input.max_width
          : std::numeric_limits<double>::infinity();
  paragraph->Layout(layout_width);

  out_metrics->width = paragraph->GetLongestLine();
  out_metrics->height = paragraph->GetHeight();
  out_metrics->min_intrinsic_width = paragraph->GetMinIntrinsicWidth();
  out_metrics->max_intrinsic_width = paragraph->GetMaxIntrinsicWidth();
  out_metrics->first_baseline = paragraph->GetAlphabeticBaseline();

  std::vector<txt::LineMetrics>& lines = paragraph->GetLineMetrics();
  out_metrics->lines.reserve(lines.size());
  for (const txt::LineMetrics& lm : lines) {
    ParagraphLineOut out_line;
    out_line.line_number = static_cast<uint32_t>(lm.line_number);
    out_line.left = lm.left;
    out_line.width = lm.width;
    out_line.height = lm.height;
    out_line.baseline = lm.baseline;
    out_line.ascent = lm.ascent;
    out_line.descent = lm.descent;
    out_metrics->lines.push_back(out_line);
  }

  // 逐 run 取片段矩形。**跨行的 run 会拿到多条** —— 这正是单矩形通道给不出的。
  //
  // 必须先按 LineMetrics 的 UTF-16 区间切 run，再对每行的可见区间取矩形。
  // TextBox 不带行号，用它的 y 中点去猜会在高低字号/占位混排时把矩形
  // 归到相邻行。GetLineNumberAt 以同一 Paragraph 的文本位置回答，没有
  // 几何容差。end_excluding_whitespace 同时在 Paragraph 这个唯一事实源上
  // 摘掉行尾可折叠空白，下游不再修改 artifact 的片段矩形。
  const std::vector<txt::Paragraph::TextBox> placeholders =
      paragraph->GetRectsForPlaceholders();
  for (size_t i = 0; i < spans.size(); ++i) {
    const RunSpan& span = spans[i];
    // A hard break shapes the line box but paints no glyph or placeholder.
    if (input.runs[i].is_line_break) {
      continue;
    }
    if (span.placeholder) {
      if (span.placeholder_index >= placeholders.size()) {
        return FailItem(wire::ItemStatus::kParagraphBuildFailed,
                        wire::ErrorFieldId::kSemanticsCombination,
                        wire::ItemErrorDetail::kUnsupportedCombination);
      }
      const txt::Paragraph::TextBox& box = placeholders[span.placeholder_index];
      const int line_number = paragraph->GetLineNumberAt(span.begin);
      if (line_number < 0 || static_cast<size_t>(line_number) >= lines.size()) {
        return FailItem(wire::ItemStatus::kParagraphBuildFailed,
                        wire::ErrorFieldId::kSemanticsCombination,
                        wire::ItemErrorDetail::kUnsupportedCombination);
      }
      ParagraphFragmentOut fragment;
      fragment.run_index = static_cast<uint32_t>(i);
      fragment.line_number = static_cast<uint32_t>(line_number);
      fragment.left = box.rect.fLeft;
      fragment.top = box.rect.fTop;
      fragment.right = box.rect.fRight;
      fragment.bottom = box.rect.fBottom;
      out_metrics->fragments.push_back(fragment);
    } else if (span.end > span.begin) {
      for (const txt::LineMetrics& line : lines) {
        const size_t slice_begin = std::max(span.begin, line.start_index);
        const size_t slice_end =
            std::min(span.end, line.end_excluding_whitespace);
        if (slice_end <= slice_begin) {
          continue;
        }
        const int line_number = paragraph->GetLineNumberAt(slice_begin);
        if (line_number < 0 ||
            static_cast<size_t>(line_number) != line.line_number) {
          return FailItem(wire::ItemStatus::kParagraphBuildFailed,
                          wire::ErrorFieldId::kSemanticsCombination,
                          wire::ItemErrorDetail::kUnsupportedCombination);
        }
        const std::vector<txt::Paragraph::TextBox> boxes =
            paragraph->GetRectsForRange(slice_begin, slice_end,
                                        txt::Paragraph::RectHeightStyle::kTight,
                                        txt::Paragraph::RectWidthStyle::kTight);
        for (const txt::Paragraph::TextBox& box : boxes) {
          ParagraphFragmentOut fragment;
          fragment.run_index = static_cast<uint32_t>(i);
          fragment.line_number = static_cast<uint32_t>(line_number);
          fragment.left = box.rect.fLeft;
          fragment.top = box.rect.fTop;
          fragment.right = box.rect.fRight;
          fragment.bottom = box.rect.fBottom;
          out_metrics->fragments.push_back(fragment);
        }
      }
    }
  }
  // Metrics/fragments and the paint artifact must describe this exact layout.
  // Moving this already-laid-out object avoids a second shaping/layout result.
  if (out_paragraph != nullptr) {
    *out_paragraph = std::move(paragraph);
  }
  return ItemFailure{};
}
}  // namespace venus_text_layout
}  // namespace flutter
