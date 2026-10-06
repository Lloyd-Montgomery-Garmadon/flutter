// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_LIB_UI_TEXT_VENUS_TEXT_LAYOUT_BATCH_ORACLE_H_
#define FLUTTER_LIB_UI_TEXT_VENUS_TEXT_LAYOUT_BATCH_ORACLE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "third_party/tonic/typed_data/typed_list.h"

namespace txt {
class FontCollection;
}

namespace flutter {

// Debug-only surface for the Venus text-layout Phase 1 oracle. This is a test
// wire, not a stable dart:ui API or a public engine ABI.
namespace venus_text_layout_batch {

constexpr uint8_t kRequestMagic[4] = {'V', 'T', 'L', 'B'};
constexpr uint8_t kResultMagic[4] = {'V', 'T', 'L', 'R'};
constexpr uint16_t kSchemaVersion = 1u;
constexpr uint32_t kNoErrorOffset = 0xffffffffu;

constexpr uint32_t kRequestHeaderSize = 48u;
constexpr uint32_t kRequestItemSize = 160u;
constexpr uint32_t kResultHeaderSize = 48u;
constexpr uint32_t kResultItemSize = 112u;
constexpr uint32_t kMaxInputBytes = 8u * 1024u * 1024u;
constexpr uint32_t kMaxItemCount = 1024u;
constexpr uint32_t kMaxTextBytes = 1024u * 1024u;

namespace request_header {
constexpr uint32_t kMagic = 0u;
constexpr uint32_t kSchemaVersion = 4u;
constexpr uint32_t kHeaderSize = 6u;
constexpr uint32_t kByteSize = 8u;
constexpr uint32_t kBatchFlags = 12u;
constexpr uint32_t kItemCount = 16u;
constexpr uint32_t kItemStride = 20u;
constexpr uint32_t kItemsOffset = 24u;
constexpr uint32_t kPayloadOffset = 28u;
constexpr uint32_t kPayloadSize = 32u;
constexpr uint32_t kReserved = 36u;
constexpr uint32_t kBatchId = 40u;
}  // namespace request_header

namespace request_item {
constexpr uint32_t kStructSize = 0u;
constexpr uint32_t kItemFlags = 4u;
constexpr uint32_t kItemId = 8u;
constexpr uint32_t kTextOffset = 16u;
constexpr uint32_t kTextLength = 20u;
constexpr uint32_t kFontFamilyOffset = 24u;
constexpr uint32_t kFontFamilyLength = 28u;
constexpr uint32_t kLocaleOffset = 32u;
constexpr uint32_t kLocaleLength = 36u;
constexpr uint32_t kEllipsisOffset = 40u;
constexpr uint32_t kEllipsisLength = 44u;
constexpr uint32_t kMinWidth = 48u;
constexpr uint32_t kMaxWidth = 56u;
constexpr uint32_t kMinHeight = 64u;
constexpr uint32_t kMaxHeight = 72u;
constexpr uint32_t kFontSize = 80u;
constexpr uint32_t kLetterSpacing = 88u;
constexpr uint32_t kWordSpacing = 96u;
constexpr uint32_t kHeight = 104u;
constexpr uint32_t kScalerParameter = 112u;
constexpr uint32_t kFontWeight = 120u;
constexpr uint32_t kMaxLines = 124u;
constexpr uint32_t kTextEncoding = 128u;
constexpr uint32_t kTextDirection = 130u;
constexpr uint32_t kTextAlign = 132u;
constexpr uint32_t kFontStyle = 134u;
constexpr uint32_t kLeadingDistribution = 136u;
constexpr uint32_t kTextWidthBasis = 138u;
constexpr uint32_t kTextScalerKind = 140u;
constexpr uint32_t kMaxLinesMode = 142u;
constexpr uint32_t kOverflowMode = 144u;
constexpr uint32_t kSoftWrap = 146u;
constexpr uint32_t kApplyHeightToFirstAscent = 148u;
constexpr uint32_t kApplyHeightToLastDescent = 150u;
constexpr uint32_t kHeightMode = 152u;
constexpr uint32_t kStrutMode = 154u;
constexpr uint32_t kReserved = 156u;
}  // namespace request_item

namespace result_header {
constexpr uint32_t kMagic = 0u;
constexpr uint32_t kSchemaVersion = 4u;
constexpr uint32_t kHeaderSize = 6u;
constexpr uint32_t kByteSize = 8u;
constexpr uint32_t kBatchStatus = 12u;
constexpr uint32_t kItemCount = 16u;
constexpr uint32_t kItemStride = 20u;
constexpr uint32_t kItemsOffset = 24u;
constexpr uint32_t kFirstGlobalErrorOffset = 28u;
constexpr uint32_t kBatchErrorDetail = 32u;
constexpr uint32_t kReserved = 36u;
constexpr uint32_t kBatchId = 40u;
}  // namespace result_header

namespace result_item {
constexpr uint32_t kStructSize = 0u;
constexpr uint32_t kItemStatus = 4u;
constexpr uint32_t kItemId = 8u;
constexpr uint32_t kPainterWidth = 16u;
constexpr uint32_t kPainterHeight = 24u;
constexpr uint32_t kBoxWidth = 32u;
constexpr uint32_t kBoxHeight = 40u;
constexpr uint32_t kMinIntrinsicWidth = 48u;
constexpr uint32_t kMaxIntrinsicWidth = 56u;
constexpr uint32_t kAlphabeticBaseline = 64u;
constexpr uint32_t kIdeographicBaseline = 72u;
constexpr uint32_t kFirstLineLeft = 80u;
constexpr uint32_t kLineCount = 88u;
constexpr uint32_t kDidExceedMaxLines = 92u;
constexpr uint32_t kResultFlags = 96u;
constexpr uint32_t kErrorFieldId = 100u;
constexpr uint32_t kItemErrorDetail = 104u;
constexpr uint32_t kReserved = 108u;
}  // namespace result_item

enum class TextEncoding : uint16_t { kUtf8 = 1u };
enum class TextDirection : uint16_t { kLtr = 1u, kRtl = 2u };
enum class TextAlign : uint16_t {
  kLeft = 1u,
  kRight = 2u,
  kCenter = 3u,
  kJustify = 4u,
  kStart = 5u,
  kEnd = 6u,
};
enum class FontStyle : uint16_t { kNormal = 1u, kItalic = 2u };
enum class LeadingDistribution : uint16_t {
  kProportional = 1u,
  kEven = 2u,
};
enum class TextWidthBasis : uint16_t { kParent = 1u, kLongestLine = 2u };
enum class TextScalerKind : uint16_t { kNoScaling = 1u, kLinear = 2u };
enum class MaxLinesMode : uint16_t { kUnlimited = 1u, kBounded = 2u };
enum class OverflowMode : uint16_t { kClip = 1u, kEllipsis = 2u };
enum class SoftWrap : uint16_t { kFalse = 1u, kTrue = 2u };
enum class ApplyHeightToFirstAscent : uint16_t {
  kFalse = 1u,
  kTrue = 2u,
};
enum class ApplyHeightToLastDescent : uint16_t {
  kFalse = 1u,
  kTrue = 2u,
};
enum class HeightMode : uint16_t { kFontMetrics = 1u, kMultiplier = 2u };
enum class StrutMode : uint16_t { kDisabled = 1u };

enum class BatchStatus : uint32_t {
  kOk = 0u,
  kInvalidHeader = 1u,
  kUnsupportedVersion = 2u,
  kInvalidTable = 3u,
  kSizeOverflow = 4u,
  kLimitExceeded = 5u,
  kNotImplemented = 6u,
  kInternalError = 7u,
};

enum class BatchErrorDetail : uint32_t {
  kNone = 0u,
  kTruncatedHeader = 1u,
  kBadMagic = 2u,
  kBadVersion = 3u,
  kBadHeaderSize = 4u,
  kBadByteSize = 5u,
  kNonzeroFlags = 6u,
  kItemCountLimit = 7u,
  kBadItemStride = 8u,
  kItemsOffsetAlignment = 9u,
  kPayloadOffsetAlignment = 10u,
  kTableRange = 11u,
  kTablePayloadOverlap = 12u,
  kPayloadRange = 13u,
  kArithmeticOverflow = 14u,
  kNonzeroReserved = 15u,
  kOutputSizeOverflow = 16u,
  kStubNotImplemented = 17u,
  kUnexpectedInternalFailure = 18u,
};

enum class ItemStatus : uint32_t {
  kOk = 0u,
  kInvalidStructSize = 1u,
  kInvalidEnum = 2u,
  kInvalidRange = 3u,
  kInvalidUtf8 = 4u,
  kInvalidConstraints = 5u,
  kUnsupportedSemantics = 6u,
  kDuplicateItemId = 7u,
  kParagraphBuildFailed = 8u,
};

enum class ErrorFieldId : uint32_t {
  kNone = 0u,
  kStructSize = 1u,
  kItemFlags = 2u,
  kItemId = 3u,
  kTextRef = 4u,
  kFontFamilyRef = 5u,
  kLocaleRef = 6u,
  kEllipsisRef = 7u,
  kMinWidth = 8u,
  kMaxWidth = 9u,
  kMinHeight = 10u,
  kMaxHeight = 11u,
  kFontSize = 12u,
  kLetterSpacing = 13u,
  kWordSpacing = 14u,
  kHeight = 15u,
  kScalerParameter = 16u,
  kFontWeight = 17u,
  kMaxLines = 18u,
  kTextEncoding = 19u,
  kTextDirection = 20u,
  kTextAlign = 21u,
  kFontStyle = 22u,
  kLeadingDistribution = 23u,
  kTextWidthBasis = 24u,
  kTextScalerKind = 25u,
  kMaxLinesMode = 26u,
  kOverflowMode = 27u,
  kSoftWrap = 28u,
  kFirstAscent = 29u,
  kLastDescent = 30u,
  kHeightMode = 31u,
  kStrutMode = 32u,
  kReserved = 33u,
  kDuplicateId = 34u,
  kTextUtf8 = 35u,
  kFamilyUtf8 = 36u,
  kLocaleUtf8 = 37u,
  kEllipsisUtf8 = 38u,
  kSemanticsCombination = 39u,
};

enum class ItemErrorDetail : uint32_t {
  kNone = 0u,
  kBelowMin = 1u,
  kAboveMax = 2u,
  kNonFinite = 3u,
  kMinGreaterThanMax = 4u,
  kOutsidePayload = 5u,
  kIntersectsStructure = 6u,
  kMalformedUtf8 = 7u,
  kEmbeddedNul = 8u,
  kEmptyRequired = 9u,
  kDuplicateLaterOccurrence = 10u,
  kUnsupportedCombination = 11u,
  kParagraphNull = 12u,
  kNonzeroFlagsOrReserved = 13u,
  kNoncanonicalInactiveValue = 14u,
};

struct BlobRange {
  uint32_t offset = 0u;
  uint32_t length = 0u;
};

// Borrowed strings are valid only for the duration of the synchronous call.
struct ParsedItemView {
  uint64_t item_id = 0u;
  // Phase 3 采用路径的预排 token。由调用方(Dart)定义, 对 Engine 不透明 ——
  // Engine 只把它当交付表的键。0 = 不参与采用路径。
  // blob 路(Phase 1 oracle)不填, 恒 0。
  uint64_t prelayout_token = 0u;
  // 文字颜色 ARGB8888。0 = 未指定（沿用 txt 默认）。
  // 线材必须带它：交出去的是整个 paragraph，而 paragraph 同时是绘制产物。
  uint32_t text_color_argb = 0u;
  std::string_view text;
  std::string_view font_family;
  std::string_view locale;
  std::string_view ellipsis;
  double min_width = 0.0;
  double max_width = 0.0;
  double min_height = 0.0;
  double max_height = 0.0;
  double font_size = 0.0;
  double letter_spacing = 0.0;
  double word_spacing = 0.0;
  double height = 0.0;
  double scaler_parameter = 0.0;
  // IFC 续排: 这段文字在行内从第几 px 开始 (见公共头同名字段)。0 = 行首。
  double leading_placeholder_width = 0.0;
  uint32_t font_weight = 0u;
  uint32_t max_lines = 0u;
  TextEncoding text_encoding = TextEncoding::kUtf8;
  TextDirection text_direction = TextDirection::kLtr;
  TextAlign text_align = TextAlign::kStart;
  FontStyle font_style = FontStyle::kNormal;
  LeadingDistribution leading_distribution = LeadingDistribution::kProportional;
  TextWidthBasis text_width_basis = TextWidthBasis::kParent;
  TextScalerKind text_scaler_kind = TextScalerKind::kNoScaling;
  MaxLinesMode max_lines_mode = MaxLinesMode::kUnlimited;
  OverflowMode overflow_mode = OverflowMode::kClip;
  SoftWrap soft_wrap = SoftWrap::kTrue;
  ApplyHeightToFirstAscent apply_height_to_first_ascent =
      ApplyHeightToFirstAscent::kTrue;
  ApplyHeightToLastDescent apply_height_to_last_descent =
      ApplyHeightToLastDescent::kTrue;
  HeightMode height_mode = HeightMode::kFontMetrics;
  StrutMode strut_mode = StrutMode::kDisabled;
};

struct ProcessingStats {
  uint32_t input_bytes = 0u;
  uint32_t output_bytes = 0u;
  uint32_t item_visits = 0u;
  uint32_t duplicate_id_probes = 0u;
  uint32_t result_allocations = 0u;
  uint32_t whole_input_copies = 0u;
};

struct ProcessingSummary {
  bool wrote_output = false;
  ProcessingStats stats;
};

// Test observation of the production parsed-view-to-style mapping. It keeps
// tests independent of txt types while proving that semantic fields reach the
// actual values consumed by LayoutItem.
struct TextStyleMappingForTesting {
  double effective_font_size = 0.0;
  double height = 0.0;
  std::string_view paragraph_font_family;
  std::string_view run_font_family;
  std::string_view paragraph_locale;
  std::string_view run_locale;
  uint32_t paragraph_font_weight = 0u;
  uint32_t run_font_weight = 0u;
  FontStyle paragraph_font_style = FontStyle::kNormal;
  FontStyle run_font_style = FontStyle::kNormal;
  double run_letter_spacing = 0.0;
  double run_word_spacing = 0.0;
  bool paragraph_has_height_override = false;
  bool run_has_height_override = false;
  bool half_leading = false;
  bool disable_first_ascent = false;
  bool disable_last_descent = false;
};

}  // namespace venus_text_layout_batch

class VenusTextLayoutBatchOracle final {
 public:
  static tonic::Uint8List Measure(Dart_Handle request_handle);

  // Test seam for the production parser/validator. `output` is caller-owned
  // and must have room for at least kResultHeaderSize bytes.
  static venus_text_layout_batch::ProcessingSummary ProcessForTesting(
      const uint8_t* input,
      size_t input_size,
      uint8_t* output,
      size_t output_capacity);

  // Validates a production request and initializes the ordered result slots
  // without constructing or laying out a paragraph. This is the asynchronous
  // service's single-pass codec seam; every valid item is shaped later by the
  // worker executor.
  static venus_text_layout_batch::ProcessingSummary ValidateForAsync(
      const uint8_t* input,
      size_t input_size,
      std::vector<uint8_t>* output);

  // Production C++ seam. The caller owns both buffers; no Dart object is
  // touched and all borrowed request views die before this call returns.
  static venus_text_layout_batch::ProcessingSummary ProcessWithFontCollection(
      const uint8_t* input,
      size_t input_size,
      const std::shared_ptr<txt::FontCollection>& fonts,
      bool impeller_enabled,
      std::vector<uint8_t>* output);

  static venus_text_layout_batch::TextStyleMappingForTesting
  ResolveTextStyleForTesting(
      const venus_text_layout_batch::ParsedItemView& item);
};

}  // namespace flutter

#endif  // FLUTTER_LIB_UI_TEXT_VENUS_TEXT_LAYOUT_BATCH_ORACLE_H_
