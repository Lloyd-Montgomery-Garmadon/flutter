// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_COMMON_PUBLIC_FLUTTER_VENUS_TEXT_LAYOUT_H_
#define FLUTTER_SHELL_PLATFORM_COMMON_PUBLIC_FLUTTER_VENUS_TEXT_LAYOUT_H_

#include <stdint.h>

// This contract is copied byte-for-byte into Venus, whose C/C++ formatting
// policy differs from Flutter's. Keep the ABI authority stable in both trees.
// clang-format off

#ifndef FLUTTER_EXPORT
#ifdef _WIN32
#define FLUTTER_EXPORT __declspec(dllexport)
#else
#define FLUTTER_EXPORT __attribute__((visibility("default")))
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define FLUTTER_VENUS_TEXT_LAYOUT_ABI_V2 2u
#define FLUTTER_VENUS_TEXT_LAYOUT_ENGINE_REVISION_BYTES 41u

/* P7 收敛：sync 获胜，异步 transport 已整条删除。批次容量与 resident 上限都只服务
   异步队列，因此一并移除；slot 与文本尺寸上限继续有效。 */
enum {
  kFlutterVenusV2MaxSlots           = 8,
  kFlutterVenusV2MaxTextBytes       = 1048576,
  kFlutterVenusV2MaxFontFamilyBytes = 4096,
  kFlutterVenusV2MaxLocaleBytes     = 64,
  kFlutterVenusV2MaxEllipsisBytes   = 64,
  kFlutterVenusV2MaxParagraphRuns   = 65536,
  kFlutterVenusV2MaxLineMetrics     = 1048576,
  kFlutterVenusV2MaxFragmentRects   = 1048576
};

/* Artifact owner V1 admission limits. `InputChargeBytes` is the exact wire
   input retained per deposited artifact; it is deliberately not advertised as
   a bound on the native Paragraph heap. Owned artifacts are never capacity-
   evicted: an over-limit deposit fails the whole owner instead. */
enum {
  kFlutterVenusTextLayoutArtifactOwnerV1MaxConcurrentOwners = 64,
  kFlutterVenusTextLayoutArtifactOwnerV1MaxEntriesPerOwner = 8192,
  kFlutterVenusTextLayoutArtifactOwnerV1MaxInputChargeBytesPerOwner =
      67108864,
  kFlutterVenusTextLayoutArtifactOwnerV1MaxEntriesPerProcess = 16384,
  kFlutterVenusTextLayoutArtifactOwnerV1MaxInputChargeBytesPerProcess =
      134217728
};

typedef enum FlutterVenusTextLayoutStatusV2 {
  kFlutterVenusV2Ok              = 0,
  kFlutterVenusV2Unavailable     = 1,
  kFlutterVenusV2InvalidArgument = 2,
  kFlutterVenusV2AbiMismatch     = 3,
  kFlutterVenusV2Backpressure    = 4,
  kFlutterVenusV2NotReady        = 5,
  kFlutterVenusV2ShuttingDown    = 6,
  kFlutterVenusV2NotFound        = 7,
  kFlutterVenusV2AlreadyTaken    = 8,
  kFlutterVenusV2BufferTooSmall  = 9,
  kFlutterVenusV2SlotBusy        = 10,
  kFlutterVenusV2SlotNotOwned    = 11,
  kFlutterVenusV2ItemFailed      = 12,
} FlutterVenusTextLayoutStatusV2;

/* bit1 曾是 Async，P7 之后**永久退役**：位次保留不复用，免得将来有人换个语义
   悄悄占回去，而旧二进制仍按 Async 解读。 */
typedef enum FlutterVenusTextLayoutCapabilityV2 {
  kFlutterVenusV2CapabilityNone            = 0,
  kFlutterVenusV2CapabilitySync            = 1u << 0,
  kFlutterVenusV2CapabilityItemTiming      = 1u << 2,
  kFlutterVenusV2CapabilityFontFingerprint = 1u << 3,
  kFlutterVenusV2CapabilityKnownMask       = 0x0du,
  // §8 P7 冻结的 RequiredSyncWinner: Sync | FontFingerprint。
  // ItemTiming 是 benchmark/证据用的可选能力, 收敛之后不再是生产必需项 ——
  // 它仍在 KnownMask 里, 带着它来注册照常接受。
  kFlutterVenusV2CapabilityRequired        = 0x09u,
} FlutterVenusTextLayoutCapabilityV2;

typedef enum FlutterVenusTextLayoutInputFlagV2 {
  kFlutterVenusV2InputFlagNone       = 0,
  kFlutterVenusV2InputFlagItemTiming = 1u << 0,
  kFlutterVenusV2InputFlagKnownMask  = 0x01u,
} FlutterVenusTextLayoutInputFlagV2;

typedef enum FlutterVenusTextLayoutOutputFlagV2 {
  kFlutterVenusV2OutputFlagNone              = 0,
  kFlutterVenusV2OutputFlagDidExceedMaxLines = 1u << 0,
  kFlutterVenusV2OutputFlagKnownMask         = 0x01u,
} FlutterVenusTextLayoutOutputFlagV2;

typedef struct FlutterVenusTextLayoutInputV2 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t flags;
  uint32_t worker_slot;
  uint32_t text_length;
  uint32_t font_family_length;
  uint32_t locale_length;
  uint32_t ellipsis_length;
  uint32_t font_weight;
  uint32_t max_lines;
  uint16_t text_encoding;
  uint16_t text_direction;
  uint16_t text_align;
  uint16_t font_style;
  uint16_t leading_distribution;
  uint16_t text_width_basis;
  uint16_t text_scaler_kind;
  uint16_t max_lines_mode;
  uint16_t overflow_mode;
  uint16_t soft_wrap;
  uint16_t apply_height_to_first_ascent;
  uint16_t apply_height_to_last_descent;
  uint16_t height_mode;
  uint16_t strut_mode;
  // 这个字段原名 reserved0（此前校验必须为 0），偏移与宽度一字节未变 —— 与
  // reserved1 -> prelayout_token 同一处置，**这就是预留字段存在的意义**。
  //
  // ## 为什么线材必须带颜色
  //
  // Phase 3 交给 UI 线程的不是几何量，是**整个 txt::Paragraph**，而它同时是
  // 绘制产物。`txt::TextStyle::color` 默认 SK_ColorWHITE —— 不带颜色的话，
  // 认领之后画出来一律是白字。真机上白底页面因此全白，而**几何量逐项全对**
  // （12200 次认领 0 处不符），没有任何几何对拍能发现它。
  //
  // 0 = 未指定，沿用 txt 默认值（保持 Phase 2 corpus 的既有行为，它们从不绘制）。
  // 非 0 = ARGB8888，直接作为 `txt::TextStyle::color`。真实文字的 alpha 恒非 0，
  // 所以"0 表示未指定"不会和任何可见颜色相撞。
  uint32_t text_color_argb;
  const uint8_t* text;
  const uint8_t* font_family;
  const uint8_t* locale;
  const uint8_t* ellipsis;
  double min_width;
  double max_width;
  double min_height;
  double max_height;
  double font_size;
  double letter_spacing;
  double word_spacing;
  double height;
  double scaler_parameter;
  // Phase 3 采用路径的预排 token。**由调用方(Dart)定义, 对 Engine 不透明** ——
  // Engine 只把它当键用, 不解释它的含义。这样 Dart 那边的
  // _computeLayoutInputFingerprint() 是唯一真相源, C++ 不需要复现同一套 hash
  // (两份实现必然漂移)。
  //
  // 非 0 时: 这一项排好之后存进交付表, 供后续 ui.Paragraph.layout() 认领。
  // 0 时: 行为与 Phase 2 逐字节一致, 不参与采用路径。
  //
  // 这个字段原名 reserved1, 偏移(176)、宽度(8)与结构体大小(184)一字节未变 ——
  // 预留字段存在的意义就是这种扩展。ABI 等价, 不需要重新冻结 offset 表。
  uint64_t prelayout_token;
  // 这一段文字在**行内**从第几 px 开始排 (IFC 续排)。0 = 从行首开始。
  //
  // ## 为什么必须有它
  //
  // `<div>中文说明 <code>data-measure</code> 后续文字...</div>` 里, `<code>`
  // 之后那段文字的首行只剩 (可用宽 - 前面已占宽) 可用, 后续行才是整宽。
  // 本结构体此前只有 min/max_width 一对**整宽**约束, 表达不了这件事 ——
  // 调用方算好了偏移也送不进来, 于是首行被当成宽了 X px, **行数少算**。
  //
  // 实测 venus mvp3_plain 的 .notice: 浏览器/Dart 排 3 行 82px, 而按整宽排
  // 只有 2 行 62px —— 少一行会把它下面的所有内容往上拉 20px。整页短 20px,
  // 而所有状态码都是干净的。
  //
  // ## 实现
  //
  // 非 0 时在文本之前插一个等宽、零高的 `txt::PlaceholderRun` —— Flutter 的
  // 内联 widget 本来就是这么占位的。占位只影响断行, 不参与任何对外宽度:
  // 核心在算 paragraph_width / last_line_width 时会把首行里的这一段扣掉,
  // 否则调用方拿到的宽度会平白多出 X px。
  //
  // 只在 max_width 有限时生效 (量 max-content 那条路不断行, 占位没有意义)。
  double leading_placeholder_width;
} FlutterVenusTextLayoutInputV2;

typedef struct FlutterVenusTextLayoutOutputV2 {
  uint32_t struct_size;
  int32_t status;
  uint32_t error_field_id;
  uint32_t error_detail;
  uint32_t line_count;
  uint32_t flags;
  uint32_t worker_slot;
  // 这个字段原名 reserved0（此前恒写 0、无消费方、无"必须为 0"校验），偏移与
  // 宽度一字节未变 —— 与输入侧 reserved0 -> text_color_argb 同一处置。
  //
  // ## 为什么必须有它
  //
  // 调用方要在同一行里接着排这段文字后面的内联内容，就必须知道**最后一行**
  // 有多宽。整段的宽度回答不了这个问题：多行文本的最后一行通常比整段窄。
  // 没有它，调用方只能猜，而猜错的表现是同行后续内容整体偏移，不报错。
  //
  // float 的位模式（不是 double）：uint32_t 的宽度只装得下 float，而排版宽度
  // 本来就是 SkScalar(float)，转成 double 再截回来没有额外信息。
  // 用 memcpy 取回，不要 reinterpret_cast —— 后者是严格别名 UB。
  uint32_t last_line_width_bits;
  double paragraph_width;
  double paragraph_height;
  double box_width;
  double box_height;
  double min_intrinsic_width;
  double max_intrinsic_width;
  double alphabetic_baseline;
  double ideographic_baseline;
  double first_line_left;
  uint64_t thread_id_hash;
  uint64_t enqueue_ns;
  uint64_t begin_ns;
  uint64_t end_ns;
  // **首行的内容区高度** = `ascent + descent` (不含行距/half-leading)。
  //
  // ## 为什么需要它
  //
  // CSS 里 `display:inline` 的非替换元素, 它自己的盒高是**字体内容区**,
  // 不是行高 —— 行盒可以比它高 (line-height > 1 时), 但那部分不属于这个
  // 元素的边框盒。`getBoundingClientRect()` 返回的就是内容区。
  //
  // 调用方 (venus C++ 布局内核) 此前拿不到这个数, 只能退而用行高, 于是
  // 内联元素的高度一律偏大。实测 mvp3 的 `<code>` (字号 13 / 行高 20.15):
  // Dart 报 15.23 (内容区), C++ 报 20.00 (行高)。
  //
  // 从 `paragraph_height` 与 `alphabetic_baseline` **推不出来**:
  // half-leading 把内容区居中放进行盒, 只有这两个数解不出 ascent 与 descent。
  // 而 `txt::LineMetrics` 里本来就有这两项 —— 缺的只是导出。
  //
  // 多行时取**首行**: 调用方要的是"这个内联盒有多高", 各行内容区在同一段
  // 文本里相同 (段内字号统一时精确; 段内混排不同字号是上界近似, 与
  // per_line_height 那处同一口径)。
  double content_height;
} FlutterVenusTextLayoutOutputV2;

typedef struct FlutterVenusTextLayoutCountersV2 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t active_slot_calls;
  uint32_t leased_slots;
  uint64_t accepted_total;
  uint64_t completed_total;
  uint64_t rejected_total;
  uint64_t font_epoch;
  uint8_t font_sha256[32];
  uint32_t accepting;
  uint32_t reserved0;
} FlutterVenusTextLayoutCountersV2;

typedef struct FlutterVenusTextLayoutApiV2 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t engine_id;
  uint64_t registration_generation;
  uint32_t capabilities;
  uint32_t max_slots;
  char engine_revision[FLUTTER_VENUS_TEXT_LAYOUT_ENGINE_REVISION_BYTES];
  uint8_t reserved[7];
  void* context;
  void (*release_context)(void* context);
  int32_t (*acquire_slot)(void* context,
                          uint32_t* out_slot,
                          uint64_t* out_lease_token);
  int32_t (*release_slot)(void* context,
                          uint32_t slot,
                          uint64_t lease_token);
  int32_t (*layout_text_node_sync_v2)(
      void* context,
      uint32_t slot,
      uint64_t lease_token,
      const FlutterVenusTextLayoutInputV2* input,
      FlutterVenusTextLayoutOutputV2* output);
  int32_t (*get_counters_v2)(void* context,
                             uint32_t caller_struct_size,
                             FlutterVenusTextLayoutCountersV2* out_counters);
} FlutterVenusTextLayoutApiV2;

// 预排条子（ambient prelayout note）。
//
// 为什么是**独立导出符号**而不是 ApiV2 里的函数指针：ApiV2 的大小与字段偏移在
// P7 已冻结并有 static_assert 守着，加一个函数指针就是 ABI 破坏。这两个入口与
// wave 派发是两条不相干的路（一条在 worker 侧存，一条在 UI 线程上取），本来也
// 不必挤进同一张表。
//
// 为什么调用方不直接用 dart:ui：Venus 应用代码对着**正式 SDK** 做分析，而正式
// SDK 禁改。走 C 符号，应用侧那份 dart:ui 一个字不用动。
//
// 调用契约：贴 -> 排版 -> **无论命不命中都要撕**。不撕的后果不是少省一点时间，
// 而是条子留在这条线程上，下一个宽度撞上的文本节点会捡走它，画出上一个节点的
// 内容。撕只撕自己贴的那张（版本号对不上说明已被别人换掉，不动）。
//
// `width` 必须有限：TextPainter 内部有个只含一个空格的 layoutTemplate 恒以
// infinity 排版，宽度无限会让它取走这张条子。传 infinity 时本函数**不贴**并返回
// 0；返回 0 表示没贴，Clear(0) 是空操作。
//
// 线程：条子只在调用它的那条线程上有效（thread_local）。
FLUTTER_EXPORT uint64_t
FlutterVenusTextLayoutPostPrelayoutNoteV2(uint64_t token,
                                          uint64_t font_epoch,
                                          double width);

FLUTTER_EXPORT void FlutterVenusTextLayoutClearPrelayoutNoteV2(
    uint64_t note_version);

// 交付表计数。用**定长数组**而不是结构体：结构体一旦出现在这个公共头上就多了一
// 张要冻结的 ABI 表, 而这只是诊断读数。写入个数由返回值给出, 调用方按下标取。
//
// 顺序（追加式，只在末尾加新项，不重排、不删除）：
//   [0] deposited            存入总数
//   [1] claimed              认领成功
//   [2] miss_not_prewarmed   这个 token 从没被存过
//   [3] miss_width_mismatch  token 在, 但排的是别的宽度
//   [4] miss_font_epoch      token 在, 但字体已换代
//   [5] miss_slot_busy       排它的 slot 还没交还
//   [6] released_by_slot     因 slot 交还而转为可认领的条数
//   [7] evicted_font_epoch   字体换代时整表作废的条数
//   [8] evicted_capacity     超容量被挤掉的条数
//   [9] dropped_on_deposit   存入时同 token 已有旧值, 旧值被丢弃
//
// 返回实际写入的个数; capacity 不足时写满前 capacity 个并返回 capacity。
// out_counters 为空或 capacity 为 0 时返回**可用总数**, 供调用方分配。
FLUTTER_EXPORT uint32_t FlutterVenusTextLayoutCopyPrelayoutStoreCountersV2(
    uint64_t* out_counters,
    uint32_t capacity);

// ── 段落入口 (一个 IFC 一次排完) ─────────────────────────
//
// ## 为什么是**独立结构体 + 独立导出符号**, 不是把 InputV2 扩宽
//
// 两条理由, 一条 ABI 一条语义:
//
// 1. `FlutterVenusTextLayoutApiV2` 的大小与偏移在 P7 已冻结并有 static_assert
//    守着 —— 加函数指针就是 ABI 破坏。上面 PrelayoutNote 那两个符号就是同一个
//    处置的先例, 这里照办。
// 2. InputV2 描述的是**一个文本节点一次测量**; 段落描述的是**一个 IFC 一次排完**。
//    挤进同一个结构体意味着每个字段的有效性都要看一个模式位 —— 那正是
//    "两种东西共用一张表" 的坏味道。
//
// ## 它交出来的是什么
//
// 行盒 + **逐 run 的片段矩形**。后者是 venus 今天缺的那一半: 发布通道只有一个
// 矩形一个元素, 而 CSS 里内联元素的矩形是各行片段的**并集** —— 表达不了, 于是
// 只能整页 fail-closed (见 venus 侧 layout_publish.cpp 的 ifc_adoption_domain)。
//
// 底下就是 `txt::Paragraph` (实现 = Skia skparagraph) 的
// GetLineMetrics / GetRectsForRange —— **不是新写一个 IFC**, 是把已有的那个
// 按正确粒度调一次。

typedef enum FlutterVenusTextLayoutRunKindV2 {
  kFlutterVenusV2RunKindText        = 0,
  /// 原子内联盒 (inline-block / 替换元素): 宽高由调用方算好, 只占位。
  kFlutterVenusV2RunKindPlaceholder = 1,
  /// 硬换行 (`<br>`)。段落里就是一个 U+000A, 由**引擎**写进去。
  ///
  /// **为什么是独立 kind, 而不是让调用方在文本里塞一个 `\n`。**
  ///
  /// 边界上有一条不变量: 声称"折叠过空白"的文本里不该再有裸控制符
  /// (见 venus 侧 text_measure_bridge 的 raw_control_char)。那条不变量存在的
  /// 理由是: 引擎收到的只是字节, 它分不出 `\n` 是"作者要求硬换行"还是
  /// "源码缩进" —— **数据被当成了控制**, 与 SQL 注入同一个形状。
  ///
  /// 所以硬换行必须以**结构**的形式过边界, 不能混进内容里。修法与当初那条
  /// 一致: 把"这是一个换行"这件事声明出来, 由边界自己去写那个字符。
  kFlutterVenusV2RunKindLineBreak   = 2,
} FlutterVenusTextLayoutRunKindV2;

typedef struct FlutterVenusTextLayoutRunV2 {
  uint32_t struct_size;
  uint32_t kind;  ///< FlutterVenusTextLayoutRunKindV2
  /// kind=Text: 这段文字在 `FlutterVenusTextLayoutParagraphInputV2::text`
  /// 里的字节区间。区间而不是各自的指针: 段落本来就是一整串, 分开传会让
  /// "run 边界" 与 "字节边界" 变成两份可以互相漂移的事实。
  uint32_t text_offset;
  uint32_t text_length;
  uint32_t font_family_offset;  ///< 在 `font_families` 里的区间 (同上)
  uint32_t font_family_length;
  uint32_t font_weight;
  uint32_t text_color_argb;
  uint16_t font_style;
  /// 原名 reserved0 (校验必须为 0), 偏移与宽度未变。kind=Text 时是 `height` 的解释方式,
  /// 取值与 InputV2.height_mode 同一枚举, 另留 0:
  ///   0 = 未指明 (旧调用方): height > 0 按倍数, 0 = 字体自然行高;
  ///   1 = FontMetrics: 字体自然行高, height 必须逐位为 0;
  ///   2 = Multiplier: height 是作者声明的倍数, **0 也算** —— CSS `line-height: 0`
  ///       的零高行内盒 (CSS 2.1 §10.8.1), 不是"未设置"。
  /// 非文本 run 必须为 0。
  uint16_t height_mode;
  double font_size;
  double letter_spacing;
  double word_spacing;
  /// 行高**倍数** (不是 px); 0 的含义由 `height_mode` 决定。
  double height;
  /// kind=Placeholder 专用。
  double placeholder_width;
  double placeholder_height;
  double placeholder_baseline_offset;
  uint16_t placeholder_alignment;
  uint16_t placeholder_baseline;
  /// 原 `uint32_t reserved1` 的前半 (偏移 100, 宽 2; 与 reserved0 -> height_mode 同一处置)。
  /// kind=Text 专用: 摊成这个 run 的**非替换 inline 包装盒**的 CSS vertical-align,
  /// 口径同 `placeholder_alignment`, 只收 0=baseline / 1=top / 3=bottom
  /// (对齐**行盒**上/下沿, CSS 2.1 §10.8.1); 其余值与非文本 run 上的非 0 均为非法参数。
  uint16_t text_vertical_align;
  uint16_t reserved1;  ///< 必须为 0
} FlutterVenusTextLayoutRunV2;

typedef struct FlutterVenusTextLayoutLineMetricV2 {
  uint32_t struct_size;
  uint32_t line_number;
  double left;
  double width;
  double height;
  double baseline;  ///< 距该行顶
  double ascent;
  double descent;
} FlutterVenusTextLayoutLineMetricV2;

/// 一个 run 落在某一行上的片段矩形 (相对段落原点)。**跨行的 run 有多条** ——
/// 这正是单矩形通道表达不了的那件事。
typedef struct FlutterVenusTextLayoutFragmentRectV2 {
  uint32_t struct_size;
  uint32_t run_index;
  uint32_t line_number;
  uint32_t reserved0;
  double left;
  double top;
  double right;
  double bottom;
} FlutterVenusTextLayoutFragmentRectV2;

typedef struct FlutterVenusTextLayoutParagraphInputV2 {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t run_count;
  uint32_t text_length;
  uint32_t font_families_length;
  uint32_t locale_length;
  uint16_t text_direction;
  uint16_t text_align;
  uint16_t soft_wrap;
  uint16_t reserved0;
  uint32_t max_lines;  ///< 0 = 不限
  /// 调用方分配的回填缓冲上限。引擎按上限填, 实际条数写回 Output;
  /// 超出上限时置 Output.truncated —— **截断必须可见**, 悄悄少给几条片段
  /// 与"这个 run 只有一行"长得一模一样。
  uint32_t line_metric_capacity;
  uint32_t fragment_rect_capacity;
  const FlutterVenusTextLayoutRunV2* runs;
  const uint8_t* text;           ///< UTF-8, 所有 run 拼在一起
  const uint8_t* font_families;  ///< 所有 run 的字体族名拼在一起
  const uint8_t* locale;
  FlutterVenusTextLayoutLineMetricV2* line_metrics_out;
  FlutterVenusTextLayoutFragmentRectV2* fragment_rects_out;
  double max_width;
  /// 容器的 line-height **倍数** (0 = 各 run 用自己的)。
  double paragraph_height_multiple;
  /// 整个 IFC 的 artifact identity。0 = 不存、不认领。
  uint64_t paragraph_token;
  /// Wave 1 只校验并纳入 identity；真正截断在 TODO 7。
  const uint8_t* ellipsis;
  uint32_t ellipsis_length;
  uint32_t reserved1;
  /// 容器 strut 的 font-family 在 `font_families` 里的字节区间。
  uint32_t strut_font_family_offset;
  uint32_t strut_font_family_length;
  uint32_t strut_font_weight;
  uint16_t strut_font_style;
  /// 0 = 关闭，且其余 strut 字段必须全部为 0。
  uint16_t strut_enabled;
  double strut_font_size;
  /// 容器 line-height 倍数；enabled 时始终是 explicit override。
  double strut_height_multiple;
} FlutterVenusTextLayoutParagraphInputV2;

typedef struct FlutterVenusTextLayoutParagraphOutputV2 {
  uint32_t struct_size;
  int32_t status;  ///< FlutterVenusTextLayoutStatusV2
  uint32_t error_field_id;
  uint32_t error_detail;
  uint32_t line_metric_count;
  uint32_t fragment_rect_count;
  uint32_t truncated;  ///< 非 0 = 缓冲不够, 上面两个数是被截断的
  uint32_t reserved0;
  double width;
  double height;
  /// 首行基线距段落顶 —— 容器要用它跟同行的兄弟对基线。
  double first_baseline;
  double min_intrinsic_width;
  double max_intrinsic_width;
} FlutterVenusTextLayoutParagraphOutputV2;

/// 排一个段落。符号不存在 = 引擎不支持段落粒度, 调用方退回单节点那套。
FLUTTER_EXPORT int32_t FlutterVenusTextLayoutParagraphV2(
    uint64_t engine_id,
    uint32_t slot,
    uint64_t lease_token,
    const FlutterVenusTextLayoutParagraphInputV2* input,
    FlutterVenusTextLayoutParagraphOutputV2* output);

/* Version-scoped ownership for prelaid Paragraph artifacts. Begin must happen
   before the first owned deposit. Bind attaches an acquired slot lease to the
   owner; Seal succeeds only after every bound lease has been returned. Claim
   remains token-based but cannot consume an owned artifact before Seal. */
FLUTTER_EXPORT int32_t FlutterVenusTextLayoutArtifactOwnerBeginV1(
    uint64_t engine_id,
    uint64_t registration_generation,
    uint64_t producer_generation,
    uint64_t* out_owner_id);

FLUTTER_EXPORT int32_t FlutterVenusTextLayoutArtifactOwnerBindSlotV1(
    uint64_t engine_id,
    uint64_t owner_id,
    uint32_t slot,
    uint64_t lease_token);

FLUTTER_EXPORT int32_t FlutterVenusTextLayoutArtifactOwnerSealV1(
    uint64_t engine_id,
    uint64_t owner_id);

FLUTTER_EXPORT void FlutterVenusTextLayoutArtifactOwnerReleaseV1(
    uint64_t engine_id,
    uint64_t owner_id);

FLUTTER_EXPORT int32_t
FlutterVenusTextLayoutGetApiV2(uint64_t engine_id,
                               uint32_t caller_struct_size,
                               FlutterVenusTextLayoutApiV2* out_api);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FLUTTER_SHELL_PLATFORM_COMMON_PUBLIC_FLUTTER_VENUS_TEXT_LAYOUT_H_

// clang-format on
