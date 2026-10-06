// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/text/venus_text_layout_batch_oracle.h"

#include "flutter/lib/ui/text/venus_text_layout_node_core.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "flutter/lib/ui/text/font_collection.h"
#include "flutter/lib/ui/ui_dart_state.h"
#include "flutter/lib/ui/window/platform_configuration.h"
#include "flutter/txt/src/txt/font_style.h"
#include "flutter/txt/src/txt/paragraph_builder.h"
#include "flutter/txt/src/txt/paragraph_style.h"
#include "flutter/txt/src/txt/text_style.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_set.h"
#include "third_party/icu/source/common/unicode/ustring.h"

namespace flutter {
namespace {

namespace wire = venus_text_layout_batch;

struct BatchHeader {
  uint32_t item_count = 0u;
  uint32_t items_offset = 0u;
  uint32_t payload_offset = 0u;
  uint32_t payload_size = 0u;
  uint64_t batch_id = 0u;
};

struct BatchFailure {
  wire::BatchStatus status = wire::BatchStatus::kOk;
  wire::BatchErrorDetail detail = wire::BatchErrorDetail::kNone;
  uint32_t error_offset = wire::kNoErrorOffset;
  uint64_t batch_id = 0u;

  bool ok() const { return status == wire::BatchStatus::kOk; }
};

// Single source of truth lives in venus_text_layout_node_core.h; the oracle
// must not carry a second definition of either type.
using ItemFailure = venus_text_layout::ItemFailure;

using ValidatedItem = venus_text_layout::ParsedNodeView;

uint16_t ReadU16(const uint8_t* bytes, size_t offset) {
  return static_cast<uint16_t>(bytes[offset]) |
         static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1u]) << 8u);
}

uint32_t ReadU32(const uint8_t* bytes, size_t offset) {
  uint32_t value = 0u;
  for (size_t index = 0u; index < 4u; ++index) {
    value |= static_cast<uint32_t>(bytes[offset + index]) << (index * 8u);
  }
  return value;
}

uint64_t ReadU64(const uint8_t* bytes, size_t offset) {
  uint64_t value = 0u;
  for (size_t index = 0u; index < 8u; ++index) {
    value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8u);
  }
  return value;
}

double ReadDouble(const uint8_t* bytes, size_t offset) {
  const uint64_t bits = ReadU64(bytes, offset);
  double value = 0.0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void WriteU16(uint8_t* bytes, size_t offset, uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value & 0xffu);
  bytes[offset + 1u] = static_cast<uint8_t>((value >> 8u) & 0xffu);
}

void WriteU32(uint8_t* bytes, size_t offset, uint32_t value) {
  for (size_t index = 0u; index < 4u; ++index) {
    bytes[offset + index] =
        static_cast<uint8_t>((value >> (index * 8u)) & 0xffu);
  }
}

void WriteU64(uint8_t* bytes, size_t offset, uint64_t value) {
  for (size_t index = 0u; index < 8u; ++index) {
    bytes[offset + index] =
        static_cast<uint8_t>((value >> (index * 8u)) & 0xffu);
  }
}

void WriteDouble(uint8_t* bytes, size_t offset, double value) {
  uint64_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  WriteU64(bytes, offset, bits);
}

uint32_t SizeAsU32(size_t size) {
  return static_cast<uint32_t>(std::min(
      size, static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
}

BatchFailure FailBatch(wire::BatchStatus status,
                       wire::BatchErrorDetail detail,
                       uint32_t error_offset,
                       uint64_t batch_id) {
  return BatchFailure{status, detail, error_offset, batch_id};
}

ItemFailure FailItem(wire::ItemStatus status,
                     wire::ErrorFieldId field,
                     wire::ItemErrorDetail detail) {
  return ItemFailure{status, field, detail};
}

void InitializeResultHeader(uint8_t* output,
                            uint32_t byte_size,
                            wire::BatchStatus status,
                            uint32_t item_count,
                            uint32_t error_offset,
                            wire::BatchErrorDetail detail,
                            uint64_t batch_id) {
  std::fill(output, output + byte_size, 0u);
  std::copy(std::begin(wire::kResultMagic), std::end(wire::kResultMagic),
            output);
  WriteU16(output, wire::result_header::kSchemaVersion, wire::kSchemaVersion);
  WriteU16(output, wire::result_header::kHeaderSize,
           static_cast<uint16_t>(wire::kResultHeaderSize));
  WriteU32(output, wire::result_header::kByteSize, byte_size);
  WriteU32(output, wire::result_header::kBatchStatus,
           static_cast<uint32_t>(status));
  WriteU32(output, wire::result_header::kItemCount, item_count);
  WriteU32(output, wire::result_header::kItemStride, wire::kResultItemSize);
  WriteU32(output, wire::result_header::kItemsOffset, wire::kResultHeaderSize);
  WriteU32(output, wire::result_header::kFirstGlobalErrorOffset, error_offset);
  WriteU32(output, wire::result_header::kBatchErrorDetail,
           static_cast<uint32_t>(detail));
  WriteU64(output, wire::result_header::kBatchId, batch_id);
}

void EncodeGlobalFailure(const BatchFailure& failure,
                         std::vector<uint8_t>* output) {
  output->resize(wire::kResultHeaderSize);
  InitializeResultHeader(output->data(), wire::kResultHeaderSize,
                         failure.status, 0u, failure.error_offset,
                         failure.detail, failure.batch_id);
}

BatchFailure ValidateHeader(const uint8_t* input,
                            size_t input_size,
                            BatchHeader* header) {
  if (input == nullptr || input_size < wire::kRequestHeaderSize) {
    return FailBatch(wire::BatchStatus::kInvalidHeader,
                     wire::BatchErrorDetail::kTruncatedHeader,
                     SizeAsU32(input_size), 0u);
  }
  if (!std::equal(std::begin(wire::kRequestMagic),
                  std::end(wire::kRequestMagic), input)) {
    return FailBatch(wire::BatchStatus::kInvalidHeader,
                     wire::BatchErrorDetail::kBadMagic,
                     wire::request_header::kMagic, 0u);
  }
  if (ReadU16(input, wire::request_header::kSchemaVersion) !=
      wire::kSchemaVersion) {
    return FailBatch(wire::BatchStatus::kUnsupportedVersion,
                     wire::BatchErrorDetail::kBadVersion,
                     wire::request_header::kSchemaVersion, 0u);
  }
  if (ReadU16(input, wire::request_header::kHeaderSize) !=
      wire::kRequestHeaderSize) {
    return FailBatch(wire::BatchStatus::kInvalidHeader,
                     wire::BatchErrorDetail::kBadHeaderSize,
                     wire::request_header::kHeaderSize, 0u);
  }

  header->batch_id = ReadU64(input, wire::request_header::kBatchId);
  if (input_size > wire::kMaxInputBytes) {
    return FailBatch(wire::BatchStatus::kLimitExceeded,
                     wire::BatchErrorDetail::kOutputSizeOverflow,
                     wire::kNoErrorOffset, header->batch_id);
  }
  if (ReadU32(input, wire::request_header::kByteSize) != input_size) {
    return FailBatch(wire::BatchStatus::kInvalidHeader,
                     wire::BatchErrorDetail::kBadByteSize,
                     wire::request_header::kByteSize, header->batch_id);
  }
  if (ReadU32(input, wire::request_header::kBatchFlags) != 0u) {
    return FailBatch(wire::BatchStatus::kInvalidHeader,
                     wire::BatchErrorDetail::kNonzeroFlags,
                     wire::request_header::kBatchFlags, header->batch_id);
  }

  header->item_count = ReadU32(input, wire::request_header::kItemCount);
  if (header->item_count > wire::kMaxItemCount) {
    return FailBatch(wire::BatchStatus::kLimitExceeded,
                     wire::BatchErrorDetail::kItemCountLimit,
                     wire::kNoErrorOffset, header->batch_id);
  }
  if (ReadU32(input, wire::request_header::kItemStride) !=
      wire::kRequestItemSize) {
    return FailBatch(wire::BatchStatus::kInvalidTable,
                     wire::BatchErrorDetail::kBadItemStride,
                     wire::request_header::kItemStride, header->batch_id);
  }

  header->items_offset = ReadU32(input, wire::request_header::kItemsOffset);
  header->payload_offset = ReadU32(input, wire::request_header::kPayloadOffset);
  header->payload_size = ReadU32(input, wire::request_header::kPayloadSize);
  if ((header->items_offset & 7u) != 0u) {
    return FailBatch(wire::BatchStatus::kInvalidTable,
                     wire::BatchErrorDetail::kItemsOffsetAlignment,
                     wire::request_header::kItemsOffset, header->batch_id);
  }
  if ((header->payload_offset & 7u) != 0u) {
    return FailBatch(wire::BatchStatus::kInvalidTable,
                     wire::BatchErrorDetail::kPayloadOffsetAlignment,
                     wire::request_header::kPayloadOffset, header->batch_id);
  }
  if (ReadU32(input, wire::request_header::kReserved) != 0u) {
    return FailBatch(wire::BatchStatus::kInvalidHeader,
                     wire::BatchErrorDetail::kNonzeroReserved,
                     wire::request_header::kReserved, header->batch_id);
  }

  const uint64_t table_size =
      static_cast<uint64_t>(header->item_count) * wire::kRequestItemSize;
  const uint64_t table_end =
      static_cast<uint64_t>(header->items_offset) + table_size;
  if (header->items_offset != wire::kRequestHeaderSize ||
      table_end > input_size) {
    return FailBatch(wire::BatchStatus::kInvalidTable,
                     wire::BatchErrorDetail::kTableRange,
                     wire::request_header::kItemsOffset, header->batch_id);
  }
  if (header->payload_offset < table_end) {
    return FailBatch(wire::BatchStatus::kInvalidTable,
                     wire::BatchErrorDetail::kTablePayloadOverlap,
                     wire::request_header::kPayloadOffset, header->batch_id);
  }
  if (header->payload_offset != table_end) {
    return FailBatch(wire::BatchStatus::kInvalidTable,
                     wire::BatchErrorDetail::kTableRange,
                     wire::request_header::kPayloadOffset, header->batch_id);
  }
  if (header->payload_offset > input_size ||
      header->payload_size != input_size - header->payload_offset) {
    return FailBatch(wire::BatchStatus::kInvalidTable,
                     wire::BatchErrorDetail::kPayloadRange,
                     wire::request_header::kPayloadSize, header->batch_id);
  }

  const uint64_t result_size =
      static_cast<uint64_t>(wire::kResultHeaderSize) +
      static_cast<uint64_t>(header->item_count) * wire::kResultItemSize;
  if (result_size > std::numeric_limits<uint32_t>::max() ||
      result_size > std::numeric_limits<size_t>::max()) {
    return FailBatch(wire::BatchStatus::kSizeOverflow,
                     wire::BatchErrorDetail::kOutputSizeOverflow,
                     wire::kNoErrorOffset, header->batch_id);
  }
  return BatchFailure{};
}

wire::BlobRange ReadRange(const uint8_t* item, uint32_t offset) {
  return wire::BlobRange{ReadU32(item, offset), ReadU32(item, offset + 4u)};
}

ItemFailure ValidateRange(const wire::BlobRange& range,
                          const BatchHeader& header,
                          size_t input_size,
                          bool required,
                          uint32_t max_length,
                          wire::ErrorFieldId field) {
  if (range.length == 0u) {
    if (required) {
      return FailItem(wire::ItemStatus::kInvalidRange, field,
                      wire::ItemErrorDetail::kEmptyRequired);
    }
    if (range.offset != 0u) {
      return FailItem(wire::ItemStatus::kInvalidRange, field,
                      wire::ItemErrorDetail::kNoncanonicalInactiveValue);
    }
    return ItemFailure{};
  }
  if (range.length > max_length) {
    return FailItem(wire::ItemStatus::kInvalidRange, field,
                    wire::ItemErrorDetail::kAboveMax);
  }
  if (range.offset < header.payload_offset) {
    return FailItem(wire::ItemStatus::kInvalidRange, field,
                    wire::ItemErrorDetail::kIntersectsStructure);
  }
  if (range.offset > input_size || range.length > input_size - range.offset) {
    return FailItem(wire::ItemStatus::kInvalidRange, field,
                    wire::ItemErrorDetail::kOutsidePayload);
  }
  return ItemFailure{};
}

std::string_view ViewRange(const uint8_t* input, const wire::BlobRange& range) {
  return std::string_view(
      static_cast<const char*>(static_cast<const void*>(input + range.offset)),
      range.length);
}

bool IsValidUtf8(std::string_view value) {
  if (value.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    return false;
  }
  int32_t required = 0;
  UErrorCode error = U_ZERO_ERROR;
  u_strFromUTF8(nullptr, 0, &required, value.data(),
                static_cast<int32_t>(value.size()), &error);
  return error == U_BUFFER_OVERFLOW_ERROR || U_SUCCESS(error);
}

bool DecodeUtf8(std::string_view value, std::u16string* output) {
  if (value.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    return false;
  }
  int32_t required = 0;
  UErrorCode error = U_ZERO_ERROR;
  u_strFromUTF8(nullptr, 0, &required, value.data(),
                static_cast<int32_t>(value.size()), &error);
  if (error != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(error)) {
    return false;
  }
  error = U_ZERO_ERROR;
  std::vector<UChar> converted(static_cast<size_t>(required));
  int32_t written = 0;
  u_strFromUTF8(converted.data(), required, &written, value.data(),
                static_cast<int32_t>(value.size()), &error);
  if (U_FAILURE(error) || written != required) {
    return false;
  }
  output->clear();
  output->reserve(static_cast<size_t>(written));
  for (UChar code_unit : converted) {
    output->push_back(static_cast<char16_t>(code_unit));
  }
  return true;
}

bool IsCanonicalLocale(std::string_view locale) {
  if (locale.size() != 5u && locale.size() != 6u) {
    return false;
  }
  const size_t language_length = locale.size() - 3u;
  if (locale[language_length] != '_') {
    return false;
  }
  for (size_t index = 0u; index < language_length; ++index) {
    if (locale[index] < 'a' || locale[index] > 'z') {
      return false;
    }
  }
  return locale[language_length + 1u] >= 'A' &&
         locale[language_length + 1u] <= 'Z' &&
         locale[language_length + 2u] >= 'A' &&
         locale[language_length + 2u] <= 'Z';
}

wire::ItemErrorDetail EnumError(uint16_t value) {
  return value == 0u ? wire::ItemErrorDetail::kBelowMin
                     : wire::ItemErrorDetail::kAboveMax;
}

ItemFailure ValidateEnum(uint16_t value,
                         uint16_t max_value,
                         wire::ErrorFieldId field) {
  if (value < 1u || value > max_value) {
    return FailItem(wire::ItemStatus::kInvalidEnum, field, EnumError(value));
  }
  return ItemFailure{};
}

ItemFailure ValidateMetadataUtf8(std::string_view value,
                                 wire::ErrorFieldId field) {
  if (!IsValidUtf8(value)) {
    return FailItem(wire::ItemStatus::kInvalidUtf8, field,
                    wire::ItemErrorDetail::kMalformedUtf8);
  }
  if (value.find('\0') != std::string_view::npos) {
    return FailItem(wire::ItemStatus::kInvalidUtf8, field,
                    wire::ItemErrorDetail::kEmbeddedNul);
  }
  return ItemFailure{};
}

ItemFailure ValidateFiniteRange(double value,
                                double minimum,
                                double maximum,
                                wire::ItemStatus status,
                                wire::ErrorFieldId field) {
  if (!std::isfinite(value)) {
    return FailItem(status, field, wire::ItemErrorDetail::kNonFinite);
  }
  if (value < minimum) {
    return FailItem(status, field, wire::ItemErrorDetail::kBelowMin);
  }
  if (value > maximum) {
    return FailItem(status, field, wire::ItemErrorDetail::kAboveMax);
  }
  return ItemFailure{};
}

ItemFailure ValidateItem(const uint8_t* input,
                         size_t input_size,
                         const BatchHeader& header,
                         uint32_t index,
                         ValidatedItem* parsed) {
  const uint8_t* item = input + header.items_offset +
                        static_cast<size_t>(index) * wire::kRequestItemSize;
  parsed->view.item_id = ReadU64(item, wire::request_item::kItemId);

  const uint32_t struct_size = ReadU32(item, wire::request_item::kStructSize);
  if (struct_size != wire::kRequestItemSize) {
    return FailItem(wire::ItemStatus::kInvalidStructSize,
                    wire::ErrorFieldId::kStructSize,
                    struct_size < wire::kRequestItemSize
                        ? wire::ItemErrorDetail::kBelowMin
                        : wire::ItemErrorDetail::kAboveMax);
  }
  if (ReadU32(item, wire::request_item::kItemFlags) != 0u) {
    return FailItem(wire::ItemStatus::kInvalidRange,
                    wire::ErrorFieldId::kItemFlags,
                    wire::ItemErrorDetail::kNonzeroFlagsOrReserved);
  }
  if (ReadU32(item, wire::request_item::kReserved) != 0u) {
    return FailItem(wire::ItemStatus::kInvalidRange,
                    wire::ErrorFieldId::kReserved,
                    wire::ItemErrorDetail::kNonzeroFlagsOrReserved);
  }

  const wire::BlobRange text_range =
      ReadRange(item, wire::request_item::kTextOffset);
  const wire::BlobRange family_range =
      ReadRange(item, wire::request_item::kFontFamilyOffset);
  const wire::BlobRange locale_range =
      ReadRange(item, wire::request_item::kLocaleOffset);
  const wire::BlobRange ellipsis_range =
      ReadRange(item, wire::request_item::kEllipsisOffset);
  ItemFailure failure =
      ValidateRange(text_range, header, input_size, true, wire::kMaxTextBytes,
                    wire::ErrorFieldId::kTextRef);
  if (!failure.ok()) {
    return failure;
  }
  failure =
      ValidateRange(family_range, header, input_size, true,
                    wire::kMaxInputBytes, wire::ErrorFieldId::kFontFamilyRef);
  if (!failure.ok()) {
    return failure;
  }
  failure = ValidateRange(locale_range, header, input_size, true,
                          wire::kMaxInputBytes, wire::ErrorFieldId::kLocaleRef);
  if (!failure.ok()) {
    return failure;
  }

  parsed->view.text = ViewRange(input, text_range);
  parsed->view.font_family = ViewRange(input, family_range);
  parsed->view.locale = ViewRange(input, locale_range);
  if (!DecodeUtf8(parsed->view.text, &parsed->text)) {
    return FailItem(wire::ItemStatus::kInvalidUtf8,
                    wire::ErrorFieldId::kTextUtf8,
                    wire::ItemErrorDetail::kMalformedUtf8);
  }
  failure = ValidateMetadataUtf8(parsed->view.font_family,
                                 wire::ErrorFieldId::kFamilyUtf8);
  if (!failure.ok()) {
    return failure;
  }
  failure = ValidateMetadataUtf8(parsed->view.locale,
                                 wire::ErrorFieldId::kLocaleUtf8);
  if (!failure.ok()) {
    return failure;
  }
  if (!IsCanonicalLocale(parsed->view.locale)) {
    return FailItem(wire::ItemStatus::kUnsupportedSemantics,
                    wire::ErrorFieldId::kLocaleRef,
                    wire::ItemErrorDetail::kUnsupportedCombination);
  }

  parsed->view.min_width = ReadDouble(item, wire::request_item::kMinWidth);
  parsed->view.max_width = ReadDouble(item, wire::request_item::kMaxWidth);
  parsed->view.min_height = ReadDouble(item, wire::request_item::kMinHeight);
  parsed->view.max_height = ReadDouble(item, wire::request_item::kMaxHeight);
  parsed->view.font_size = ReadDouble(item, wire::request_item::kFontSize);
  parsed->view.letter_spacing =
      ReadDouble(item, wire::request_item::kLetterSpacing);
  parsed->view.word_spacing =
      ReadDouble(item, wire::request_item::kWordSpacing);
  parsed->view.height = ReadDouble(item, wire::request_item::kHeight);
  parsed->view.scaler_parameter =
      ReadDouble(item, wire::request_item::kScalerParameter);
  parsed->view.font_weight = ReadU32(item, wire::request_item::kFontWeight);
  parsed->view.max_lines = ReadU32(item, wire::request_item::kMaxLines);

  const std::pair<double, wire::ErrorFieldId> constraints[] = {
      {parsed->view.min_width, wire::ErrorFieldId::kMinWidth},
      {parsed->view.max_width, wire::ErrorFieldId::kMaxWidth},
      {parsed->view.min_height, wire::ErrorFieldId::kMinHeight},
      {parsed->view.max_height, wire::ErrorFieldId::kMaxHeight},
  };
  for (const auto& constraint : constraints) {
    failure = ValidateFiniteRange(
        constraint.first, 0.0, std::numeric_limits<double>::max(),
        wire::ItemStatus::kInvalidConstraints, constraint.second);
    if (!failure.ok()) {
      return failure;
    }
  }
  if (parsed->view.min_width > parsed->view.max_width) {
    return FailItem(wire::ItemStatus::kInvalidConstraints,
                    wire::ErrorFieldId::kMinWidth,
                    wire::ItemErrorDetail::kMinGreaterThanMax);
  }
  if (parsed->view.min_height > parsed->view.max_height) {
    return FailItem(wire::ItemStatus::kInvalidConstraints,
                    wire::ErrorFieldId::kMinHeight,
                    wire::ItemErrorDetail::kMinGreaterThanMax);
  }
  failure = ValidateFiniteRange(parsed->view.font_size, 0.0, 4096.0,
                                wire::ItemStatus::kInvalidRange,
                                wire::ErrorFieldId::kFontSize);
  if (!failure.ok()) {
    return failure;
  }
  const std::pair<double, wire::ErrorFieldId> spacings[] = {
      {parsed->view.letter_spacing, wire::ErrorFieldId::kLetterSpacing},
      {parsed->view.word_spacing, wire::ErrorFieldId::kWordSpacing},
  };
  for (const auto& spacing : spacings) {
    if (!std::isfinite(spacing.first)) {
      return FailItem(wire::ItemStatus::kInvalidRange, spacing.second,
                      wire::ItemErrorDetail::kNonFinite);
    }
    if (std::abs(spacing.first) > 4096.0) {
      return FailItem(wire::ItemStatus::kInvalidRange, spacing.second,
                      wire::ItemErrorDetail::kAboveMax);
    }
  }
  if (parsed->view.font_weight < 100u || parsed->view.font_weight > 900u) {
    return FailItem(
        wire::ItemStatus::kInvalidRange, wire::ErrorFieldId::kFontWeight,
        parsed->view.font_weight < 100u ? wire::ItemErrorDetail::kBelowMin
                                        : wire::ItemErrorDetail::kAboveMax);
  }
  if ((parsed->view.font_weight % 100u) != 0u) {
    return FailItem(wire::ItemStatus::kUnsupportedSemantics,
                    wire::ErrorFieldId::kFontWeight,
                    wire::ItemErrorDetail::kUnsupportedCombination);
  }

  const uint16_t text_encoding =
      ReadU16(item, wire::request_item::kTextEncoding);
  const uint16_t text_direction =
      ReadU16(item, wire::request_item::kTextDirection);
  const uint16_t text_align = ReadU16(item, wire::request_item::kTextAlign);
  const uint16_t font_style = ReadU16(item, wire::request_item::kFontStyle);
  const uint16_t leading_distribution =
      ReadU16(item, wire::request_item::kLeadingDistribution);
  const uint16_t text_width_basis =
      ReadU16(item, wire::request_item::kTextWidthBasis);
  const uint16_t text_scaler_kind =
      ReadU16(item, wire::request_item::kTextScalerKind);
  const uint16_t max_lines_mode =
      ReadU16(item, wire::request_item::kMaxLinesMode);
  const uint16_t overflow_mode =
      ReadU16(item, wire::request_item::kOverflowMode);
  const uint16_t soft_wrap = ReadU16(item, wire::request_item::kSoftWrap);
  const uint16_t first_ascent =
      ReadU16(item, wire::request_item::kApplyHeightToFirstAscent);
  const uint16_t last_descent =
      ReadU16(item, wire::request_item::kApplyHeightToLastDescent);
  const uint16_t height_mode = ReadU16(item, wire::request_item::kHeightMode);
  const uint16_t strut_mode = ReadU16(item, wire::request_item::kStrutMode);
  const std::pair<uint16_t, std::pair<uint16_t, wire::ErrorFieldId>> enums[] = {
      {text_encoding, {1u, wire::ErrorFieldId::kTextEncoding}},
      {text_direction, {2u, wire::ErrorFieldId::kTextDirection}},
      {text_align, {6u, wire::ErrorFieldId::kTextAlign}},
      {font_style, {2u, wire::ErrorFieldId::kFontStyle}},
      {leading_distribution, {2u, wire::ErrorFieldId::kLeadingDistribution}},
      {text_width_basis, {2u, wire::ErrorFieldId::kTextWidthBasis}},
      {text_scaler_kind, {2u, wire::ErrorFieldId::kTextScalerKind}},
      {max_lines_mode, {2u, wire::ErrorFieldId::kMaxLinesMode}},
      {overflow_mode, {2u, wire::ErrorFieldId::kOverflowMode}},
      {soft_wrap, {2u, wire::ErrorFieldId::kSoftWrap}},
      {first_ascent, {2u, wire::ErrorFieldId::kFirstAscent}},
      {last_descent, {2u, wire::ErrorFieldId::kLastDescent}},
      {height_mode, {2u, wire::ErrorFieldId::kHeightMode}},
      {strut_mode, {1u, wire::ErrorFieldId::kStrutMode}},
  };
  for (const auto& value : enums) {
    failure =
        ValidateEnum(value.first, value.second.first, value.second.second);
    if (!failure.ok()) {
      return failure;
    }
  }

  parsed->view.text_encoding = static_cast<wire::TextEncoding>(text_encoding);
  parsed->view.text_direction =
      static_cast<wire::TextDirection>(text_direction);
  parsed->view.text_align = static_cast<wire::TextAlign>(text_align);
  parsed->view.font_style = static_cast<wire::FontStyle>(font_style);
  parsed->view.leading_distribution =
      static_cast<wire::LeadingDistribution>(leading_distribution);
  parsed->view.text_width_basis =
      static_cast<wire::TextWidthBasis>(text_width_basis);
  parsed->view.text_scaler_kind =
      static_cast<wire::TextScalerKind>(text_scaler_kind);
  parsed->view.max_lines_mode = static_cast<wire::MaxLinesMode>(max_lines_mode);
  parsed->view.overflow_mode = static_cast<wire::OverflowMode>(overflow_mode);
  parsed->view.soft_wrap = static_cast<wire::SoftWrap>(soft_wrap);
  parsed->view.apply_height_to_first_ascent =
      static_cast<wire::ApplyHeightToFirstAscent>(first_ascent);
  parsed->view.apply_height_to_last_descent =
      static_cast<wire::ApplyHeightToLastDescent>(last_descent);
  parsed->view.height_mode = static_cast<wire::HeightMode>(height_mode);
  parsed->view.strut_mode = static_cast<wire::StrutMode>(strut_mode);

  if (parsed->view.max_lines_mode == wire::MaxLinesMode::kUnlimited) {
    if (parsed->view.max_lines != 0u) {
      return FailItem(wire::ItemStatus::kUnsupportedSemantics,
                      wire::ErrorFieldId::kMaxLines,
                      wire::ItemErrorDetail::kNoncanonicalInactiveValue);
    }
  } else if (parsed->view.max_lines < 1u || parsed->view.max_lines > 1024u) {
    return FailItem(
        wire::ItemStatus::kInvalidRange, wire::ErrorFieldId::kMaxLines,
        parsed->view.max_lines == 0u ? wire::ItemErrorDetail::kBelowMin
                                     : wire::ItemErrorDetail::kAboveMax);
  }
  if (parsed->view.soft_wrap == wire::SoftWrap::kFalse) {
    return FailItem(wire::ItemStatus::kUnsupportedSemantics,
                    wire::ErrorFieldId::kSemanticsCombination,
                    wire::ItemErrorDetail::kUnsupportedCombination);
  }

  if (parsed->view.overflow_mode == wire::OverflowMode::kClip) {
    failure =
        ValidateRange(ellipsis_range, header, input_size, false,
                      wire::kMaxInputBytes, wire::ErrorFieldId::kEllipsisRef);
    if (!failure.ok()) {
      return failure;
    }
    parsed->view.ellipsis = std::string_view();
    parsed->ellipsis.clear();
  } else {
    failure =
        ValidateRange(ellipsis_range, header, input_size, true,
                      wire::kMaxInputBytes, wire::ErrorFieldId::kEllipsisRef);
    if (!failure.ok()) {
      return failure;
    }
    parsed->view.ellipsis = ViewRange(input, ellipsis_range);
    failure = ValidateMetadataUtf8(parsed->view.ellipsis,
                                   wire::ErrorFieldId::kEllipsisUtf8);
    if (!failure.ok()) {
      return failure;
    }
    if (parsed->view.ellipsis != "\xe2\x80\xa6" ||
        !DecodeUtf8(parsed->view.ellipsis, &parsed->ellipsis)) {
      return FailItem(wire::ItemStatus::kUnsupportedSemantics,
                      wire::ErrorFieldId::kEllipsisRef,
                      wire::ItemErrorDetail::kUnsupportedCombination);
    }
    if (parsed->view.max_lines_mode == wire::MaxLinesMode::kUnlimited) {
      return FailItem(wire::ItemStatus::kUnsupportedSemantics,
                      wire::ErrorFieldId::kSemanticsCombination,
                      wire::ItemErrorDetail::kUnsupportedCombination);
    }
  }

  if (parsed->view.text_scaler_kind == wire::TextScalerKind::kNoScaling) {
    if (ReadU64(item, wire::request_item::kScalerParameter) !=
        0x3ff0000000000000u) {
      return FailItem(wire::ItemStatus::kUnsupportedSemantics,
                      wire::ErrorFieldId::kScalerParameter,
                      wire::ItemErrorDetail::kNoncanonicalInactiveValue);
    }
  } else {
    failure = ValidateFiniteRange(parsed->view.scaler_parameter, 0.0, 16.0,
                                  wire::ItemStatus::kInvalidRange,
                                  wire::ErrorFieldId::kScalerParameter);
    if (!failure.ok() || parsed->view.scaler_parameter == 0.0) {
      return failure.ok() ? FailItem(wire::ItemStatus::kInvalidRange,
                                     wire::ErrorFieldId::kScalerParameter,
                                     wire::ItemErrorDetail::kBelowMin)
                          : failure;
    }
    const double effective_size =
        parsed->view.font_size * parsed->view.scaler_parameter;
    if (!std::isfinite(effective_size) || effective_size > 4096.0) {
      return FailItem(wire::ItemStatus::kInvalidRange,
                      wire::ErrorFieldId::kScalerParameter,
                      wire::ItemErrorDetail::kAboveMax);
    }
  }

  if (parsed->view.height_mode == wire::HeightMode::kFontMetrics) {
    if (ReadU64(item, wire::request_item::kHeight) != 0u) {
      return FailItem(wire::ItemStatus::kUnsupportedSemantics,
                      wire::ErrorFieldId::kHeight,
                      wire::ItemErrorDetail::kNoncanonicalInactiveValue);
    }
  } else {
    failure = ValidateFiniteRange(parsed->view.height, 0.0, 16.0,
                                  wire::ItemStatus::kInvalidRange,
                                  wire::ErrorFieldId::kHeight);
    if (!failure.ok() || parsed->view.height == 0.0) {
      return failure.ok() ? FailItem(wire::ItemStatus::kInvalidRange,
                                     wire::ErrorFieldId::kHeight,
                                     wire::ItemErrorDetail::kBelowMin)
                          : failure;
    }
  }
  return ItemFailure{};
}


ItemFailure LayoutItem(const ValidatedItem& item,
                       const std::shared_ptr<txt::FontCollection>& fonts,
                       bool impeller_enabled,
                       uint8_t* result) {
  // Thin adapter over THE core. All shaping/line-breaking/ellipsis/constrain
  // semantics live in LayoutTextNodeCore; this function only serialises the
  // resulting metrics into the Phase 1 result blob.
  venus_text_layout::NodeMetrics metrics;
  const ItemFailure failure = venus_text_layout::LayoutTextNodeCore(
      item, fonts, impeller_enabled, &metrics);
  if (!failure.ok()) {
    return failure;
  }
  WriteDouble(result, wire::result_item::kPainterWidth, metrics.paragraph_width);
  WriteDouble(result, wire::result_item::kPainterHeight,
              metrics.paragraph_height);
  WriteDouble(result, wire::result_item::kBoxWidth, metrics.box_width);
  WriteDouble(result, wire::result_item::kBoxHeight, metrics.box_height);
  WriteDouble(result, wire::result_item::kMinIntrinsicWidth,
              metrics.min_intrinsic_width);
  WriteDouble(result, wire::result_item::kMaxIntrinsicWidth,
              metrics.max_intrinsic_width);
  WriteDouble(result, wire::result_item::kAlphabeticBaseline,
              metrics.alphabetic_baseline);
  WriteDouble(result, wire::result_item::kIdeographicBaseline,
              metrics.ideographic_baseline);
  WriteDouble(result, wire::result_item::kFirstLineLeft,
              metrics.first_line_left);
  WriteU32(result, wire::result_item::kLineCount, metrics.line_count);
  WriteU32(result, wire::result_item::kDidExceedMaxLines,
           metrics.did_exceed_max_lines ? 1u : 0u);
  return ItemFailure{};
}

void EncodeItemFailure(uint8_t* result, const ItemFailure& failure) {
  WriteU32(result, wire::result_item::kItemStatus,
           static_cast<uint32_t>(failure.status));
  WriteU32(result, wire::result_item::kErrorFieldId,
           static_cast<uint32_t>(failure.field));
  WriteU32(result, wire::result_item::kItemErrorDetail,
           static_cast<uint32_t>(failure.detail));
}

wire::ProcessingSummary ProcessBatch(
    const uint8_t* input,
    size_t input_size,
    const std::shared_ptr<txt::FontCollection>& fonts,
    bool impeller_enabled,
    std::vector<uint8_t>* output) {
  wire::ProcessingSummary summary;
  summary.stats.input_bytes = SizeAsU32(input_size);
  summary.stats.result_allocations = 1u;

  BatchHeader header;
  const BatchFailure batch_failure = ValidateHeader(input, input_size, &header);
  if (!batch_failure.ok()) {
    EncodeGlobalFailure(batch_failure, output);
    summary.wrote_output = true;
    summary.stats.output_bytes = wire::kResultHeaderSize;
    return summary;
  }

  const uint32_t output_size =
      wire::kResultHeaderSize + header.item_count * wire::kResultItemSize;
  output->resize(output_size);
  InitializeResultHeader(output->data(), output_size, wire::BatchStatus::kOk,
                         header.item_count, wire::kNoErrorOffset,
                         wire::BatchErrorDetail::kNone, header.batch_id);

  absl::flat_hash_set<uint64_t> item_ids;
  item_ids.reserve(header.item_count);
  for (uint32_t index = 0u; index < header.item_count; ++index) {
    ++summary.stats.item_visits;
    ++summary.stats.duplicate_id_probes;
    const uint8_t* request_item =
        input + header.items_offset +
        static_cast<size_t>(index) * wire::kRequestItemSize;
    const uint64_t item_id = ReadU64(request_item, wire::request_item::kItemId);
    uint8_t* result_item = output->data() + wire::kResultHeaderSize +
                           static_cast<size_t>(index) * wire::kResultItemSize;
    WriteU32(result_item, wire::result_item::kStructSize,
             wire::kResultItemSize);
    WriteU64(result_item, wire::result_item::kItemId, item_id);

    if (!item_ids.insert(item_id).second) {
      EncodeItemFailure(
          result_item,
          FailItem(wire::ItemStatus::kDuplicateItemId,
                   wire::ErrorFieldId::kDuplicateId,
                   wire::ItemErrorDetail::kDuplicateLaterOccurrence));
      continue;
    }

    ValidatedItem parsed;
    ItemFailure item_failure =
        ValidateItem(input, input_size, header, index, &parsed);
    if (item_failure.ok() && fonts) {
      item_failure = LayoutItem(parsed, fonts, impeller_enabled, result_item);
    }
    if (!item_failure.ok()) {
      EncodeItemFailure(result_item, item_failure);
    }
  }
  summary.wrote_output = true;
  summary.stats.output_bytes = output_size;
  return summary;
}

}  // namespace

tonic::Uint8List VenusTextLayoutBatchOracle::Measure(
    Dart_Handle request_handle) {
  // The dispatcher must not acquire TypedData before this root-isolate guard:
  // ThrowIfUIOperationsProhibited calls the Dart VM when it rejects a caller.
  UIDartState::ThrowIfUIOperationsProhibited();
  tonic::Uint8List request(request_handle);
  FontCollection& engine_fonts = UIDartState::Current()
                                     ->platform_configuration()
                                     ->client()
                                     ->GetFontCollection();
  const std::shared_ptr<txt::FontCollection> fonts =
      engine_fonts.GetFontCollection();
  const bool impeller_enabled = UIDartState::Current()->IsImpellerEnabled();

  std::vector<uint8_t> native_output;
  ProcessBatch(request.data(), static_cast<size_t>(request.num_elements()),
               fonts, impeller_enabled, &native_output);

  // Dart VM callbacks and allocation are prohibited while request TypedData is
  // acquired. All borrowed views are gone when ProcessBatch returns.
  request.Release();
  tonic::Uint8List result(Dart_NewTypedData(
      Dart_TypedData_kUint8, static_cast<intptr_t>(native_output.size())));
  std::memcpy(&result[0], native_output.data(), native_output.size());
  return result;
}

wire::ProcessingSummary VenusTextLayoutBatchOracle::ProcessForTesting(
    const uint8_t* input,
    size_t input_size,
    uint8_t* output,
    size_t output_capacity) {
  std::vector<uint8_t> native_output;
  wire::ProcessingSummary summary =
      ProcessBatch(input, input_size, nullptr, false, &native_output);
  if (output == nullptr || output_capacity < native_output.size()) {
    summary.wrote_output = false;
    return summary;
  }
  std::memcpy(output, native_output.data(), native_output.size());
  return summary;
}

wire::ProcessingSummary VenusTextLayoutBatchOracle::ValidateForAsync(
    const uint8_t* input,
    size_t input_size,
    std::vector<uint8_t>* output) {
  if (output == nullptr) {
    return {};
  }
  return ProcessBatch(input, input_size, nullptr, false, output);
}

wire::ProcessingSummary VenusTextLayoutBatchOracle::ProcessWithFontCollection(
    const uint8_t* input,
    size_t input_size,
    const std::shared_ptr<txt::FontCollection>& fonts,
    bool impeller_enabled,
    std::vector<uint8_t>* output) {
  if (output == nullptr) {
    return {};
  }
  return ProcessBatch(input, input_size, fonts, impeller_enabled, output);
}

wire::TextStyleMappingForTesting
VenusTextLayoutBatchOracle::ResolveTextStyleForTesting(
    const wire::ParsedItemView& item) {
  return venus_text_layout::ResolveTextStyleMapping(item);
}

}  // namespace flutter
