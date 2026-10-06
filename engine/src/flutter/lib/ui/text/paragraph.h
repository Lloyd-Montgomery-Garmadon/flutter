// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_LIB_UI_TEXT_PARAGRAPH_H_
#define FLUTTER_LIB_UI_TEXT_PARAGRAPH_H_

#include "flutter/fml/message_loop.h"
#include "flutter/lib/ui/dart_wrapper.h"
#include "flutter/lib/ui/painting/canvas.h"
#include "flutter/txt/src/txt/paragraph.h"

namespace flutter {

class Paragraph : public RefCountedDartWrappable<Paragraph> {
  DEFINE_WRAPPERTYPEINFO();
  FML_FRIEND_MAKE_REF_COUNTED(Paragraph);

 public:
  static void Create(Dart_Handle paragraph_handle,
                     std::unique_ptr<txt::Paragraph> txt_paragraph) {
    auto paragraph = fml::MakeRefCounted<Paragraph>(std::move(txt_paragraph));
    paragraph->AssociateWithDartWrapper(paragraph_handle);
  }

  ~Paragraph() override;

  double width();
  double height();
  double longestLine();
  double minIntrinsicWidth();
  double maxIntrinsicWidth();
  double alphabeticBaseline();
  double ideographicBaseline();
  bool didExceedMaxLines();

  void layout(double width);

  // Phase 3 采用路径：给这个 Paragraph 挂一个预排 token。
  //
  // 挂上之后，下一次 layout(width) 会先去交付表里认领 —— 命中就直接换上 worker
  // 线程提前排好的那份，**跳过 UI 线程上的 shaping**；未命中就照常自己排。
  //
  // 对象仍然是 Dart 造的、由 Dart GC 管的这一个；换掉的只是它内部那份
  // txt::Paragraph。所以没有任何对象跨 Venus 的 FFI 边界，
  // §5 那套"Engine 不缓存调用方地址"的冻结规则一条不动。
  //
  // token = 0 表示不参与采用路径（默认值），行为与 Phase 2 完全一致。
  void setVenusPrelayoutToken(uint64_t token, uint64_t font_epoch);

  // **环境 token**：挂在当前线程上，由**下一次宽度匹配的** layout() 一次性消费。
  //
  // 为什么需要它而不是只用上面那个对象方法：Venus 的文本节点走 Flutter 的
  // TextPainter，而 TextPainter 内部自己造 ui.Paragraph —— 那个对象在私有类
  // _TextPainterLayoutCacheWithOffset 上，Venus 够不到，正式 SDK 又钉死禁改。
  // 于是只能"挂在环境里，让它自己来取"。
  //
  // 安全性靠三条，缺一不可：
  //  1. 键含 width，且**一次性消费**；
  //  2. 调用方只在 `width` 有限时挂 —— TextPainter 内部那个 layoutTemplate
  //     （内容是一个空格）恒以 `width: infinity` 排版，宽度有限就永远偷不走；
  //  3. token 本身是**内容指纹**（Dart 侧 _computeLayoutInputFingerprint），
  //     所以即便被同宽度的另一个 paragraph 取走，内容也必然一致。
  //
  // 失败模式因此是"未命中并回退"，不是"画错内容"。
  // `width_bits` 是宽度的**位模式**(double 按位重解释成 uint64), 不是 double。
  // 全程整数比对: 没有 == 的 -0.0 判等问题, 没有 NaN 不等于自己的问题,
  // 也不存在"浮点比较写错了却看着像宽度在变"这类误导性计数。
  //
  // `note_version` 是这张条子的版本。调用方每贴一张就 +1, 排版调用结束后
  // **无论命中与否**都要清掉(见 Dart 侧的 finally)。
  // 存在的理由: 贴了条子但那次排版被"输入没变"的守卫跳过时, 条子会留在线程上
  // 没人撕 —— 下一个文本节点宽度一旦撞上就会捡走它。版本号让过期条子作废。
  static void SetVenusAmbientPrelayoutToken(uint64_t token,
                                            uint64_t font_epoch,
                                            uint64_t width_bits,
                                            uint64_t note_version);

  // 当前线程条子的版本号；调用方用它做"贴 -> 排版 -> 撕"的配对。
  static uint64_t VenusAmbientNoteVersion();
  void paint(Canvas* canvas, double x, double y);

  tonic::Float32List getRectsForRange(unsigned start,
                                      unsigned end,
                                      unsigned boxHeightStyle,
                                      unsigned boxWidthStyle);
  tonic::Float32List getRectsForPlaceholders();
  Dart_Handle getPositionForOffset(double dx, double dy);
  Dart_Handle getGlyphInfoAt(unsigned utf16Offset,
                             Dart_Handle constructor) const;
  Dart_Handle getClosestGlyphInfo(double dx,
                                  double dy,
                                  Dart_Handle constructor) const;
  Dart_Handle getWordBoundary(unsigned offset);
  Dart_Handle getLineBoundary(unsigned offset);
  tonic::Float64List computeLineMetrics() const;
  Dart_Handle getLineMetricsAt(int lineNumber, Dart_Handle constructor) const;
  size_t getNumberOfLines() const;
  int getLineNumberAt(size_t utf16Offset) const;

  void dispose();

 private:
  std::unique_ptr<txt::Paragraph> m_paragraph_;
  uint64_t venus_prelayout_token_ = 0u;
  uint64_t venus_font_epoch_ = 0u;

  explicit Paragraph(std::unique_ptr<txt::Paragraph> paragraph);
};

}  // namespace flutter

#endif  // FLUTTER_LIB_UI_TEXT_PARAGRAPH_H_
