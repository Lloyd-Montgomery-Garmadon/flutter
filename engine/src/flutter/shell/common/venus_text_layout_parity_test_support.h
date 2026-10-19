// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_VENUS_TEXT_LAYOUT_PARITY_TEST_SUPPORT_H_
#define FLUTTER_SHELL_COMMON_VENUS_TEXT_LAYOUT_PARITY_TEST_SUPPORT_H_

#include <cstring>
#include <memory>
#include <string>
#include "flutter/lib/ui/text/font_collection.h"
#include "flutter/shell/platform/common/public/flutter_venus_text_layout.h"
#include "flutter/txt/src/txt/asset_font_manager.h"
#include "flutter/txt/src/txt/platform.h"
#include "gtest/gtest.h"
#include "third_party/skia/include/core/SkTypeface.h"

namespace flutter {
namespace testing {
namespace venus_parity {

inline uint64_t Bits(double v) {
  uint64_t b = 0;
  std::memcpy(&b, &v, sizeof(b));
  return b;
}

inline std::shared_ptr<FontCollection> MakeFonts() {
  auto fonts = std::make_shared<FontCollection>();
  fonts->SetupDefaultFontManager(0u);
  fonts->RegisterTestFonts();
  return fonts;
}

inline std::shared_ptr<FontCollection> MakeRobotoFonts() {
  auto fonts = MakeFonts();
  // RegisterTestFonts maps unknown families to FlutterTest. Replace that test
  // manager with the SDK's actual Roboto face for these CSS geometry controls.
  auto typeface = txt::GetDefaultFontManager()->makeFromFile(
      "flutter/txt/third_party/fonts/Roboto-Regular.ttf");
  EXPECT_NE(typeface, nullptr);
  if (!typeface) {
    return nullptr;
  }
  SkString family;
  typeface->getFamilyName(&family);
  EXPECT_STREQ(family.c_str(), "Roboto");
  auto provider = std::make_unique<txt::TypefaceFontAssetProvider>();
  provider->RegisterTypeface(std::move(typeface), "Roboto");
  fonts->GetFontCollection()->SetTestFontManager(
      sk_make_sp<txt::AssetFontManager>(std::move(provider)));
  return fonts;
}

struct ParagraphWire {
  std::string text = "Gate A paragraph";
  std::string family = "Roboto";
  std::string ellipsis = "\xE2\x80\xA6";
  FlutterVenusTextLayoutRunV2 run{};
  FlutterVenusTextLayoutLineMetricV2 line{};
  FlutterVenusTextLayoutFragmentRectV2 fragment{};
  FlutterVenusTextLayoutParagraphInputV2 input{};
  FlutterVenusTextLayoutParagraphOutputV2 output{};

  ParagraphWire() {
    run.struct_size = sizeof(run);
    run.kind = kFlutterVenusV2RunKindText;
    run.text_length = static_cast<uint32_t>(text.size());
    run.font_family_length = static_cast<uint32_t>(family.size());
    run.font_weight = 400u;
    run.font_size = 16.0;
    input.struct_size = sizeof(input);
    input.abi_version = FLUTTER_VENUS_TEXT_LAYOUT_ABI_V2;
    input.run_count = 1u;
    input.text_length = static_cast<uint32_t>(text.size());
    input.font_families_length = static_cast<uint32_t>(family.size());
    input.ellipsis_length = static_cast<uint32_t>(ellipsis.size());
    input.text_direction = 1u;
    input.text_align = 5u;
    input.soft_wrap = 1u;
    input.line_metric_capacity = 1u;
    input.fragment_rect_capacity = 1u;
    input.runs = &run;
    input.text = reinterpret_cast<const uint8_t*>(text.data());
    input.font_families = reinterpret_cast<const uint8_t*>(family.data());
    input.line_metrics_out = &line;
    input.fragment_rects_out = &fragment;
    input.max_width = 200.0;
    input.ellipsis = reinterpret_cast<const uint8_t*>(ellipsis.data());
    output.struct_size = sizeof(output);
  }

  void EnableStrut(double font_size = 10.0, double height_multiple = 1.0) {
    input.strut_font_family_length = static_cast<uint32_t>(family.size());
    input.strut_font_weight = 400u;
    input.strut_enabled = 1u;
    input.strut_font_size = font_size;
    input.strut_height_multiple = height_multiple;
  }
};

}  // namespace venus_parity
}  // namespace testing
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_VENUS_TEXT_LAYOUT_PARITY_TEST_SUPPORT_H_
