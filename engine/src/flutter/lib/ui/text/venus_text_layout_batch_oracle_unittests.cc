// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/text/venus_text_layout_batch_oracle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "flutter/testing/testing.h"

namespace flutter {
namespace testing {
namespace {

namespace wire = venus_text_layout_batch;

void WriteU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value & 0xffu);
  bytes[offset + 1u] = static_cast<uint8_t>((value >> 8u) & 0xffu);
}

void WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (size_t index = 0u; index < 4u; ++index) {
    bytes[offset + index] =
        static_cast<uint8_t>((value >> (index * 8u)) & 0xffu);
  }
}

void WriteU64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
  for (size_t index = 0u; index < 8u; ++index) {
    bytes[offset + index] =
        static_cast<uint8_t>((value >> (index * 8u)) & 0xffu);
  }
}

void WriteDouble(std::vector<uint8_t>& bytes, size_t offset, double value) {
  uint64_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  WriteU64(bytes, offset, bits);
}

uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset) {
  uint32_t value = 0u;
  for (size_t index = 0u; index < 4u; ++index) {
    value |= static_cast<uint32_t>(bytes[offset + index]) << (index * 8u);
  }
  return value;
}

uint64_t ReadU64(const std::vector<uint8_t>& bytes, size_t offset) {
  uint64_t value = 0u;
  for (size_t index = 0u; index < 8u; ++index) {
    value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8u);
  }
  return value;
}

void Append(std::vector<uint8_t>& bytes, std::string_view value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}

std::vector<uint8_t> MakeCanonicalRequest(uint32_t item_count) {
  const uint32_t payload_offset =
      wire::kRequestHeaderSize + item_count * wire::kRequestItemSize;
  const std::string_view text = "abc";
  const std::string_view family = "Roboto";
  const std::string_view locale = "en_US";
  const uint32_t text_offset = payload_offset;
  const uint32_t family_offset =
      text_offset + static_cast<uint32_t>(text.size());
  const uint32_t locale_offset =
      family_offset + static_cast<uint32_t>(family.size());

  std::vector<uint8_t> request(payload_offset, 0u);
  Append(request, text);
  Append(request, family);
  Append(request, locale);

  std::copy(std::begin(wire::kRequestMagic), std::end(wire::kRequestMagic),
            request.begin());
  WriteU16(request, wire::request_header::kSchemaVersion, wire::kSchemaVersion);
  WriteU16(request, wire::request_header::kHeaderSize,
           static_cast<uint16_t>(wire::kRequestHeaderSize));
  WriteU32(request, wire::request_header::kByteSize,
           static_cast<uint32_t>(request.size()));
  WriteU32(request, wire::request_header::kBatchFlags, 0u);
  WriteU32(request, wire::request_header::kItemCount, item_count);
  WriteU32(request, wire::request_header::kItemStride, wire::kRequestItemSize);
  WriteU32(request, wire::request_header::kItemsOffset,
           wire::kRequestHeaderSize);
  WriteU32(request, wire::request_header::kPayloadOffset, payload_offset);
  WriteU32(request, wire::request_header::kPayloadSize,
           static_cast<uint32_t>(request.size()) - payload_offset);
  WriteU32(request, wire::request_header::kReserved, 0u);
  WriteU64(request, wire::request_header::kBatchId, 0x1020304050607080u);

  for (uint32_t index = 0u; index < item_count; ++index) {
    const size_t base = wire::kRequestHeaderSize +
                        static_cast<size_t>(index) * wire::kRequestItemSize;
    WriteU32(request, base + wire::request_item::kStructSize,
             wire::kRequestItemSize);
    WriteU32(request, base + wire::request_item::kItemFlags, 0u);
    WriteU64(request, base + wire::request_item::kItemId,
             static_cast<uint64_t>(index) + 1u);
    WriteU32(request, base + wire::request_item::kTextOffset, text_offset);
    WriteU32(request, base + wire::request_item::kTextLength,
             static_cast<uint32_t>(text.size()));
    WriteU32(request, base + wire::request_item::kFontFamilyOffset,
             family_offset);
    WriteU32(request, base + wire::request_item::kFontFamilyLength,
             static_cast<uint32_t>(family.size()));
    WriteU32(request, base + wire::request_item::kLocaleOffset, locale_offset);
    WriteU32(request, base + wire::request_item::kLocaleLength,
             static_cast<uint32_t>(locale.size()));
    WriteU32(request, base + wire::request_item::kEllipsisOffset, 0u);
    WriteU32(request, base + wire::request_item::kEllipsisLength, 0u);
    WriteDouble(request, base + wire::request_item::kMinWidth, 0.0);
    WriteDouble(request, base + wire::request_item::kMaxWidth, 200.0);
    WriteDouble(request, base + wire::request_item::kMinHeight, 0.0);
    WriteDouble(request, base + wire::request_item::kMaxHeight, 200.0);
    WriteDouble(request, base + wire::request_item::kFontSize, 17.0);
    WriteDouble(request, base + wire::request_item::kLetterSpacing, 0.0);
    WriteDouble(request, base + wire::request_item::kWordSpacing, 0.0);
    WriteDouble(request, base + wire::request_item::kHeight, 0.0);
    WriteDouble(request, base + wire::request_item::kScalerParameter, 1.0);
    WriteU32(request, base + wire::request_item::kFontWeight, 400u);
    WriteU32(request, base + wire::request_item::kMaxLines, 0u);
    WriteU16(request, base + wire::request_item::kTextEncoding,
             static_cast<uint16_t>(wire::TextEncoding::kUtf8));
    WriteU16(request, base + wire::request_item::kTextDirection,
             static_cast<uint16_t>(wire::TextDirection::kLtr));
    WriteU16(request, base + wire::request_item::kTextAlign,
             static_cast<uint16_t>(wire::TextAlign::kStart));
    WriteU16(request, base + wire::request_item::kFontStyle,
             static_cast<uint16_t>(wire::FontStyle::kNormal));
    WriteU16(request, base + wire::request_item::kLeadingDistribution,
             static_cast<uint16_t>(wire::LeadingDistribution::kProportional));
    WriteU16(request, base + wire::request_item::kTextWidthBasis,
             static_cast<uint16_t>(wire::TextWidthBasis::kParent));
    WriteU16(request, base + wire::request_item::kTextScalerKind,
             static_cast<uint16_t>(wire::TextScalerKind::kNoScaling));
    WriteU16(request, base + wire::request_item::kMaxLinesMode,
             static_cast<uint16_t>(wire::MaxLinesMode::kUnlimited));
    WriteU16(request, base + wire::request_item::kOverflowMode,
             static_cast<uint16_t>(wire::OverflowMode::kClip));
    WriteU16(request, base + wire::request_item::kSoftWrap,
             static_cast<uint16_t>(wire::SoftWrap::kTrue));
    WriteU16(request, base + wire::request_item::kApplyHeightToFirstAscent,
             static_cast<uint16_t>(wire::ApplyHeightToFirstAscent::kTrue));
    WriteU16(request, base + wire::request_item::kApplyHeightToLastDescent,
             static_cast<uint16_t>(wire::ApplyHeightToLastDescent::kTrue));
    WriteU16(request, base + wire::request_item::kHeightMode,
             static_cast<uint16_t>(wire::HeightMode::kFontMetrics));
    WriteU16(request, base + wire::request_item::kStrutMode,
             static_cast<uint16_t>(wire::StrutMode::kDisabled));
    WriteU32(request, base + wire::request_item::kReserved, 0u);
  }
  return request;
}

struct OracleOutput {
  std::vector<uint8_t> bytes;
  wire::ProcessingSummary summary;
};

OracleOutput RunOracle(const std::vector<uint8_t>& request,
                       uint32_t expected_item_capacity = 3u) {
  OracleOutput output;
  output.bytes.resize(
      wire::kResultHeaderSize +
          static_cast<size_t>(expected_item_capacity) * wire::kResultItemSize,
      0u);
  output.summary = VenusTextLayoutBatchOracle::ProcessForTesting(
      request.data(), request.size(), output.bytes.data(), output.bytes.size());
  if (output.summary.wrote_output &&
      output.summary.stats.output_bytes <= output.bytes.size()) {
    output.bytes.resize(output.summary.stats.output_bytes);
  }
  return output;
}

void ExpectGlobalFailure(const OracleOutput& output,
                         wire::BatchStatus status,
                         wire::BatchErrorDetail detail,
                         uint32_t error_offset,
                         uint64_t batch_id) {
  ASSERT_TRUE(output.summary.wrote_output);
  ASSERT_EQ(output.bytes.size(), wire::kResultHeaderSize);
  EXPECT_TRUE(std::equal(std::begin(wire::kResultMagic),
                         std::end(wire::kResultMagic), output.bytes.begin()));
  EXPECT_EQ(ReadU32(output.bytes, wire::result_header::kByteSize),
            wire::kResultHeaderSize);
  EXPECT_EQ(ReadU32(output.bytes, wire::result_header::kBatchStatus),
            static_cast<uint32_t>(status));
  EXPECT_EQ(ReadU32(output.bytes, wire::result_header::kItemCount), 0u);
  EXPECT_EQ(ReadU32(output.bytes, wire::result_header::kItemStride),
            wire::kResultItemSize);
  EXPECT_EQ(ReadU32(output.bytes, wire::result_header::kItemsOffset),
            wire::kResultHeaderSize);
  EXPECT_EQ(ReadU32(output.bytes, wire::result_header::kFirstGlobalErrorOffset),
            error_offset);
  EXPECT_EQ(ReadU32(output.bytes, wire::result_header::kBatchErrorDetail),
            static_cast<uint32_t>(detail));
  EXPECT_EQ(ReadU32(output.bytes, wire::result_header::kReserved), 0u);
  EXPECT_EQ(ReadU64(output.bytes, wire::result_header::kBatchId), batch_id);
}

void ExpectItemFailure(const OracleOutput& output,
                       uint32_t index,
                       wire::ItemStatus status,
                       wire::ErrorFieldId field,
                       wire::ItemErrorDetail detail) {
  EXPECT_EQ(ReadU32(output.bytes, wire::result_header::kBatchStatus),
            static_cast<uint32_t>(wire::BatchStatus::kOk));
  const size_t required =
      wire::kResultHeaderSize +
      (static_cast<size_t>(index) + 1u) * wire::kResultItemSize;
  EXPECT_GE(output.bytes.size(), required);
  if (output.bytes.size() < required) {
    return;
  }
  const size_t base = wire::kResultHeaderSize +
                      static_cast<size_t>(index) * wire::kResultItemSize;
  EXPECT_EQ(ReadU32(output.bytes, base + wire::result_item::kItemStatus),
            static_cast<uint32_t>(status));
  EXPECT_EQ(ReadU32(output.bytes, base + wire::result_item::kErrorFieldId),
            static_cast<uint32_t>(field));
  EXPECT_EQ(ReadU32(output.bytes, base + wire::result_item::kItemErrorDetail),
            static_cast<uint32_t>(detail));
  for (uint32_t offset :
       {wire::result_item::kPainterWidth, wire::result_item::kPainterHeight,
        wire::result_item::kBoxWidth, wire::result_item::kBoxHeight,
        wire::result_item::kMinIntrinsicWidth,
        wire::result_item::kMaxIntrinsicWidth,
        wire::result_item::kAlphabeticBaseline,
        wire::result_item::kIdeographicBaseline,
        wire::result_item::kFirstLineLeft}) {
    EXPECT_EQ(ReadU64(output.bytes, base + offset), 0u);
  }
}

TEST(VenusTextLayoutBatchOracleTest, WireSchemaArithmeticAndDiscriminants) {
  EXPECT_EQ(wire::request_header::kBatchId + sizeof(uint64_t),
            wire::kRequestHeaderSize);
  EXPECT_EQ(wire::request_item::kReserved + sizeof(uint32_t),
            wire::kRequestItemSize);
  EXPECT_EQ(wire::result_header::kBatchId + sizeof(uint64_t),
            wire::kResultHeaderSize);
  EXPECT_EQ(wire::result_item::kReserved + sizeof(uint32_t),
            wire::kResultItemSize);
  EXPECT_EQ(static_cast<uint32_t>(wire::BatchStatus::kNotImplemented), 6u);
  EXPECT_EQ(static_cast<uint32_t>(wire::BatchErrorDetail::kStubNotImplemented),
            17u);
  EXPECT_EQ(static_cast<uint16_t>(wire::TextAlign::kEnd), 6u);
  EXPECT_EQ(static_cast<uint16_t>(wire::TextScalerKind::kLinear), 2u);
  EXPECT_EQ(static_cast<uint16_t>(wire::StrutMode::kDisabled), 1u);
  EXPECT_EQ(static_cast<uint32_t>(wire::ItemStatus::kParagraphBuildFailed), 8u);
  EXPECT_EQ(static_cast<uint32_t>(wire::ErrorFieldId::kSemanticsCombination),
            39u);
  EXPECT_EQ(
      static_cast<uint32_t>(wire::ItemErrorDetail::kNoncanonicalInactiveValue),
      14u);
}

TEST(VenusTextLayoutBatchOracleTest, CanonicalSuccessEnvelope) {
  const std::vector<uint8_t> request = MakeCanonicalRequest(1u);
  const OracleOutput output = RunOracle(request);
  ASSERT_EQ(output.bytes.size(),
            wire::kResultHeaderSize + wire::kResultItemSize);
  EXPECT_EQ(ReadU32(output.bytes, wire::result_header::kBatchStatus),
            static_cast<uint32_t>(wire::BatchStatus::kOk));
  EXPECT_EQ(ReadU32(output.bytes, wire::result_header::kItemCount), 1u);
  EXPECT_EQ(ReadU32(output.bytes, wire::result_header::kBatchErrorDetail),
            static_cast<uint32_t>(wire::BatchErrorDetail::kNone));
  EXPECT_EQ(ReadU64(output.bytes, wire::result_header::kBatchId),
            0x1020304050607080u);
  EXPECT_EQ(ReadU32(output.bytes,
                    wire::kResultHeaderSize + wire::result_item::kItemStatus),
            static_cast<uint32_t>(wire::ItemStatus::kOk));
  EXPECT_EQ(output.summary.stats.input_bytes, request.size());
  EXPECT_EQ(output.summary.stats.output_bytes,
            wire::kResultHeaderSize + wire::kResultItemSize);
  EXPECT_EQ(output.summary.stats.item_visits, 1u);
  EXPECT_EQ(output.summary.stats.duplicate_id_probes, 1u);
  EXPECT_EQ(output.summary.stats.result_allocations, 1u);
  EXPECT_EQ(output.summary.stats.whole_input_copies, 0u);
}

TEST(VenusTextLayoutBatchOracleTest, BatchIdEchoRequiresSafePrefix) {
  std::vector<uint8_t> request = MakeCanonicalRequest(0u);
  WriteU32(request, wire::request_header::kByteSize,
           static_cast<uint32_t>(request.size()) + 1u);
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidHeader,
                      wire::BatchErrorDetail::kBadByteSize,
                      wire::request_header::kByteSize, 0x1020304050607080u);

  request = MakeCanonicalRequest(0u);
  request[wire::request_header::kMagic] = 'X';
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidHeader,
                      wire::BatchErrorDetail::kBadMagic,
                      wire::request_header::kMagic, 0u);
  request = MakeCanonicalRequest(0u);
  WriteU16(request, wire::request_header::kSchemaVersion, 2u);
  ExpectGlobalFailure(RunOracle(request),
                      wire::BatchStatus::kUnsupportedVersion,
                      wire::BatchErrorDetail::kBadVersion,
                      wire::request_header::kSchemaVersion, 0u);
  request = MakeCanonicalRequest(0u);
  WriteU16(request, wire::request_header::kHeaderSize, 40u);
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidHeader,
                      wire::BatchErrorDetail::kBadHeaderSize,
                      wire::request_header::kHeaderSize, 0u);
}

TEST(VenusTextLayoutBatchOracleTest, CanonicalSingleItemParses) {
  const OracleOutput output = RunOracle(MakeCanonicalRequest(1u));
  EXPECT_EQ(ReadU32(output.bytes, wire::result_header::kBatchStatus),
            static_cast<uint32_t>(wire::BatchStatus::kOk));
  EXPECT_EQ(output.summary.stats.output_bytes,
            wire::kResultHeaderSize + wire::kResultItemSize);
  EXPECT_EQ(output.summary.stats.item_visits, 1u);
  EXPECT_EQ(output.summary.stats.duplicate_id_probes, 1u);
}

TEST(VenusTextLayoutBatchOracleTest, ParsedViewMapsToTextStyleSemantics) {
  wire::ParsedItemView view;
  view.font_family = "VenusMvpRoboto";
  view.locale = "en_US";
  view.font_weight = 700u;
  view.font_style = wire::FontStyle::kItalic;
  view.font_size = 20.0;
  view.letter_spacing = 0.3;
  view.word_spacing = 0.7;
  view.scaler_parameter = 1.0;
  view.text_scaler_kind = wire::TextScalerKind::kNoScaling;
  view.height = 0.0;
  view.height_mode = wire::HeightMode::kFontMetrics;
  view.leading_distribution = wire::LeadingDistribution::kProportional;
  view.apply_height_to_first_ascent = wire::ApplyHeightToFirstAscent::kTrue;
  view.apply_height_to_last_descent = wire::ApplyHeightToLastDescent::kTrue;

  wire::TextStyleMappingForTesting mapping =
      VenusTextLayoutBatchOracle::ResolveTextStyleForTesting(view);
  EXPECT_DOUBLE_EQ(mapping.effective_font_size, 20.0);
  EXPECT_DOUBLE_EQ(mapping.height, 0.0);
  EXPECT_EQ(mapping.paragraph_font_family, "VenusMvpRoboto");
  EXPECT_EQ(mapping.run_font_family, "VenusMvpRoboto");
  EXPECT_EQ(mapping.paragraph_locale, "en_US");
  EXPECT_EQ(mapping.run_locale, "en_US");
  EXPECT_EQ(mapping.paragraph_font_weight, 700u);
  EXPECT_EQ(mapping.run_font_weight, 700u);
  EXPECT_EQ(mapping.paragraph_font_style, wire::FontStyle::kItalic);
  EXPECT_EQ(mapping.run_font_style, wire::FontStyle::kItalic);
  EXPECT_DOUBLE_EQ(mapping.run_letter_spacing, 0.3);
  EXPECT_DOUBLE_EQ(mapping.run_word_spacing, 0.7);
  EXPECT_FALSE(mapping.paragraph_has_height_override);
  EXPECT_FALSE(mapping.run_has_height_override);
  EXPECT_FALSE(mapping.half_leading);
  EXPECT_FALSE(mapping.disable_first_ascent);
  EXPECT_FALSE(mapping.disable_last_descent);

  view.text_scaler_kind = wire::TextScalerKind::kLinear;
  view.scaler_parameter = 1.25;
  view.height = 0.1;
  view.height_mode = wire::HeightMode::kMultiplier;
  view.leading_distribution = wire::LeadingDistribution::kEven;
  view.apply_height_to_first_ascent = wire::ApplyHeightToFirstAscent::kFalse;
  view.apply_height_to_last_descent = wire::ApplyHeightToLastDescent::kFalse;
  mapping = VenusTextLayoutBatchOracle::ResolveTextStyleForTesting(view);
  EXPECT_DOUBLE_EQ(mapping.effective_font_size, 25.0);
  EXPECT_DOUBLE_EQ(mapping.height, 0.1);
  EXPECT_TRUE(mapping.paragraph_has_height_override);
  EXPECT_TRUE(mapping.run_has_height_override);
  EXPECT_TRUE(mapping.half_leading);
  EXPECT_TRUE(mapping.disable_first_ascent);
  EXPECT_TRUE(mapping.disable_last_descent);
}

TEST(VenusTextLayoutBatchOracleTest, MalformedTruncatedHeader) {
  std::vector<uint8_t> request = MakeCanonicalRequest(0u);
  request.resize(wire::kRequestHeaderSize - 1u);
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidHeader,
                      wire::BatchErrorDetail::kTruncatedHeader,
                      wire::kRequestHeaderSize - 1u, 0u);
}

TEST(VenusTextLayoutBatchOracleTest, MalformedMagicVersionAndHeaderSize) {
  std::vector<uint8_t> request = MakeCanonicalRequest(0u);
  request[wire::request_header::kMagic] = 'X';
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidHeader,
                      wire::BatchErrorDetail::kBadMagic,
                      wire::request_header::kMagic, 0u);

  request = MakeCanonicalRequest(0u);
  WriteU16(request, wire::request_header::kSchemaVersion, 2u);
  ExpectGlobalFailure(RunOracle(request),
                      wire::BatchStatus::kUnsupportedVersion,
                      wire::BatchErrorDetail::kBadVersion,
                      wire::request_header::kSchemaVersion, 0u);

  request = MakeCanonicalRequest(0u);
  WriteU16(request, wire::request_header::kHeaderSize, 40u);
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidHeader,
                      wire::BatchErrorDetail::kBadHeaderSize,
                      wire::request_header::kHeaderSize, 0u);
}

TEST(VenusTextLayoutBatchOracleTest, MalformedByteSizeFlagsAndReserved) {
  std::vector<uint8_t> request = MakeCanonicalRequest(0u);
  WriteU32(request, wire::request_header::kByteSize,
           static_cast<uint32_t>(request.size()) + 1u);
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidHeader,
                      wire::BatchErrorDetail::kBadByteSize,
                      wire::request_header::kByteSize, 0x1020304050607080u);

  request = MakeCanonicalRequest(0u);
  WriteU32(request, wire::request_header::kBatchFlags, 1u);
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidHeader,
                      wire::BatchErrorDetail::kNonzeroFlags,
                      wire::request_header::kBatchFlags, 0x1020304050607080u);

  request = MakeCanonicalRequest(0u);
  WriteU32(request, wire::request_header::kReserved, 1u);
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidHeader,
                      wire::BatchErrorDetail::kNonzeroReserved,
                      wire::request_header::kReserved, 0x1020304050607080u);
}

TEST(VenusTextLayoutBatchOracleTest, MalformedLimitAndStride) {
  std::vector<uint8_t> request = MakeCanonicalRequest(0u);
  WriteU32(request, wire::request_header::kItemCount, wire::kMaxItemCount + 1u);
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kLimitExceeded,
                      wire::BatchErrorDetail::kItemCountLimit,
                      wire::kNoErrorOffset, 0x1020304050607080u);

  request = MakeCanonicalRequest(0u);
  WriteU32(request, wire::request_header::kItemStride,
           wire::kRequestItemSize - 8u);
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidTable,
                      wire::BatchErrorDetail::kBadItemStride,
                      wire::request_header::kItemStride, 0x1020304050607080u);
}

TEST(VenusTextLayoutBatchOracleTest, MalformedTableOffsetsAndRanges) {
  std::vector<uint8_t> request = MakeCanonicalRequest(1u);
  WriteU32(request, wire::request_header::kItemsOffset,
           wire::kRequestHeaderSize + 1u);
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidTable,
                      wire::BatchErrorDetail::kItemsOffsetAlignment,
                      wire::request_header::kItemsOffset, 0x1020304050607080u);

  request = MakeCanonicalRequest(1u);
  WriteU32(request, wire::request_header::kPayloadOffset,
           wire::kRequestHeaderSize + wire::kRequestItemSize + 1u);
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidTable,
                      wire::BatchErrorDetail::kPayloadOffsetAlignment,
                      wire::request_header::kPayloadOffset,
                      0x1020304050607080u);

  request = MakeCanonicalRequest(1u);
  request.resize(100u);
  WriteU32(request, wire::request_header::kByteSize,
           static_cast<uint32_t>(request.size()));
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidTable,
                      wire::BatchErrorDetail::kTableRange,
                      wire::request_header::kItemsOffset, 0x1020304050607080u);

  request = MakeCanonicalRequest(1u);
  WriteU32(request, wire::request_header::kPayloadOffset,
           wire::kRequestHeaderSize);
  WriteU32(request, wire::request_header::kPayloadSize,
           static_cast<uint32_t>(request.size()) - wire::kRequestHeaderSize);
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidTable,
                      wire::BatchErrorDetail::kTablePayloadOverlap,
                      wire::request_header::kPayloadOffset,
                      0x1020304050607080u);

  request = MakeCanonicalRequest(1u);
  WriteU32(request, wire::request_header::kPayloadSize,
           ReadU32(request, wire::request_header::kPayloadSize) + 1u);
  ExpectGlobalFailure(RunOracle(request), wire::BatchStatus::kInvalidTable,
                      wire::BatchErrorDetail::kPayloadRange,
                      wire::request_header::kPayloadSize, 0x1020304050607080u);
}

TEST(VenusTextLayoutBatchOracleTest, InvalidItemStructAndEnum) {
  std::vector<uint8_t> request = MakeCanonicalRequest(1u);
  WriteU32(request, wire::kRequestHeaderSize + wire::request_item::kStructSize,
           wire::kRequestItemSize - 8u);
  ExpectItemFailure(
      RunOracle(request), 0u, wire::ItemStatus::kInvalidStructSize,
      wire::ErrorFieldId::kStructSize, wire::ItemErrorDetail::kBelowMin);

  request = MakeCanonicalRequest(1u);
  WriteU16(request,
           wire::kRequestHeaderSize + wire::request_item::kTextDirection, 99u);
  ExpectItemFailure(RunOracle(request), 0u, wire::ItemStatus::kInvalidEnum,
                    wire::ErrorFieldId::kTextDirection,
                    wire::ItemErrorDetail::kAboveMax);
}

TEST(VenusTextLayoutBatchOracleTest, InvalidItemPayloadRangesAndUtf8) {
  std::vector<uint8_t> request = MakeCanonicalRequest(1u);
  WriteU32(request, wire::kRequestHeaderSize + wire::request_item::kTextOffset,
           0u);
  ExpectItemFailure(RunOracle(request), 0u, wire::ItemStatus::kInvalidRange,
                    wire::ErrorFieldId::kTextRef,
                    wire::ItemErrorDetail::kIntersectsStructure);

  request = MakeCanonicalRequest(1u);
  const uint32_t byte_size = ReadU32(request, wire::request_header::kByteSize);
  WriteU32(request, wire::kRequestHeaderSize + wire::request_item::kTextOffset,
           byte_size);
  WriteU32(request, wire::kRequestHeaderSize + wire::request_item::kTextLength,
           1u);
  ExpectItemFailure(RunOracle(request), 0u, wire::ItemStatus::kInvalidRange,
                    wire::ErrorFieldId::kTextRef,
                    wire::ItemErrorDetail::kOutsidePayload);

  request = MakeCanonicalRequest(1u);
  const uint32_t text_offset = ReadU32(
      request, wire::kRequestHeaderSize + wire::request_item::kTextOffset);
  request[text_offset] = 0xc0u;
  request[text_offset + 1u] = 0xafu;
  WriteU32(request, wire::kRequestHeaderSize + wire::request_item::kTextLength,
           2u);
  ExpectItemFailure(RunOracle(request), 0u, wire::ItemStatus::kInvalidUtf8,
                    wire::ErrorFieldId::kTextUtf8,
                    wire::ItemErrorDetail::kMalformedUtf8);
}

TEST(VenusTextLayoutBatchOracleTest, InvalidItemConstraintsAndSemantics) {
  std::vector<uint8_t> request = MakeCanonicalRequest(1u);
  WriteDouble(request, wire::kRequestHeaderSize + wire::request_item::kMaxWidth,
              std::numeric_limits<double>::quiet_NaN());
  ExpectItemFailure(
      RunOracle(request), 0u, wire::ItemStatus::kInvalidConstraints,
      wire::ErrorFieldId::kMaxWidth, wire::ItemErrorDetail::kNonFinite);

  request = MakeCanonicalRequest(1u);
  WriteDouble(request, wire::kRequestHeaderSize + wire::request_item::kMinWidth,
              201.0);
  ExpectItemFailure(
      RunOracle(request), 0u, wire::ItemStatus::kInvalidConstraints,
      wire::ErrorFieldId::kMinWidth, wire::ItemErrorDetail::kMinGreaterThanMax);

  request = MakeCanonicalRequest(1u);
  WriteU16(request, wire::kRequestHeaderSize + wire::request_item::kSoftWrap,
           static_cast<uint16_t>(wire::SoftWrap::kFalse));
  ExpectItemFailure(RunOracle(request), 0u,
                    wire::ItemStatus::kUnsupportedSemantics,
                    wire::ErrorFieldId::kSemanticsCombination,
                    wire::ItemErrorDetail::kUnsupportedCombination);

  request = MakeCanonicalRequest(1u);
  WriteDouble(request,
              wire::kRequestHeaderSize + wire::request_item::kScalerParameter,
              2.0);
  ExpectItemFailure(RunOracle(request), 0u,
                    wire::ItemStatus::kUnsupportedSemantics,
                    wire::ErrorFieldId::kScalerParameter,
                    wire::ItemErrorDetail::kNoncanonicalInactiveValue);
}

TEST(VenusTextLayoutBatchOracleTest, GoodBadGoodItemsAreIsolated) {
  std::vector<uint8_t> request = MakeCanonicalRequest(3u);
  const size_t second_item = wire::kRequestHeaderSize + wire::kRequestItemSize;
  WriteU16(request, second_item + wire::request_item::kTextAlign, 99u);
  const OracleOutput output = RunOracle(request, 3u);
  EXPECT_EQ(output.summary.stats.output_bytes,
            wire::kResultHeaderSize + 3u * wire::kResultItemSize);
  if (output.bytes.size() <
      wire::kResultHeaderSize + 3u * wire::kResultItemSize) {
    ADD_FAILURE() << "Oracle did not return all item records";
    return;
  }
  EXPECT_EQ(ReadU32(output.bytes,
                    wire::kResultHeaderSize + wire::result_item::kItemStatus),
            static_cast<uint32_t>(wire::ItemStatus::kOk));
  ExpectItemFailure(output, 1u, wire::ItemStatus::kInvalidEnum,
                    wire::ErrorFieldId::kTextAlign,
                    wire::ItemErrorDetail::kAboveMax);
  EXPECT_EQ(ReadU32(output.bytes, wire::kResultHeaderSize +
                                      2u * wire::kResultItemSize +
                                      wire::result_item::kItemStatus),
            static_cast<uint32_t>(wire::ItemStatus::kOk));
}

TEST(VenusTextLayoutBatchOracleTest, DuplicateItemIdIsFirstWins) {
  std::vector<uint8_t> request = MakeCanonicalRequest(3u);
  const size_t third_item =
      wire::kRequestHeaderSize + 2u * wire::kRequestItemSize;
  WriteU64(request, third_item + wire::request_item::kItemId, 1u);
  const OracleOutput output = RunOracle(request, 3u);
  ExpectItemFailure(output, 2u, wire::ItemStatus::kDuplicateItemId,
                    wire::ErrorFieldId::kDuplicateId,
                    wire::ItemErrorDetail::kDuplicateLaterOccurrence);
}

TEST(VenusTextLayoutBatchOracleTest, SharedMetadataPayloadRangesAreAccepted) {
  const OracleOutput output = RunOracle(MakeCanonicalRequest(3u), 3u);
  EXPECT_EQ(ReadU32(output.bytes, wire::result_header::kBatchStatus),
            static_cast<uint32_t>(wire::BatchStatus::kOk));
  EXPECT_EQ(output.summary.stats.item_visits, 3u);
}

TEST(VenusTextLayoutBatchOracleTest, BatchCardinalityBudget) {
  for (uint32_t item_count : std::array<uint32_t, 3u>{1u, 16u, 100u}) {
    const std::vector<uint8_t> request = MakeCanonicalRequest(item_count);
    const OracleOutput output = RunOracle(request, item_count);
    RecordProperty("request_bytes_" + std::to_string(item_count),
                   static_cast<int>(request.size()));
    RecordProperty("response_bytes_" + std::to_string(item_count),
                   output.summary.stats.output_bytes);
    RecordProperty("item_visits_" + std::to_string(item_count),
                   output.summary.stats.item_visits);
    RecordProperty("duplicate_probes_" + std::to_string(item_count),
                   output.summary.stats.duplicate_id_probes);
    RecordProperty("result_allocations_" + std::to_string(item_count),
                   output.summary.stats.result_allocations);
    RecordProperty("whole_input_copies_" + std::to_string(item_count),
                   output.summary.stats.whole_input_copies);
    EXPECT_EQ(output.summary.stats.output_bytes,
              wire::kResultHeaderSize + item_count * wire::kResultItemSize);
    EXPECT_EQ(output.summary.stats.item_visits, item_count);
    EXPECT_LE(output.summary.stats.duplicate_id_probes, item_count);
    EXPECT_EQ(output.summary.stats.result_allocations, 1u);
    EXPECT_EQ(output.summary.stats.whole_input_copies, 0u);
  }
}

}  // namespace
}  // namespace testing
}  // namespace flutter
