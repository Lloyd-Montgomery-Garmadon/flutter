// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_VENUS_TEXT_LAYOUT_REGISTRY_H_
#define FLUTTER_SHELL_COMMON_VENUS_TEXT_LAYOUT_REGISTRY_H_

#include <cstdint>
#include <memory>

#include "flutter/lib/ui/text/font_collection.h"
#include "flutter/shell/platform/common/public/flutter_venus_text_layout.h"

namespace flutter {

class VenusTextLayoutRegistry final {
 public:
  static uint64_t Register(uint64_t engine_id,
                           const std::shared_ptr<FontCollection>& fonts,
                           bool impeller_enabled,
                           void (*paragraph_core_probe_for_testing)(void*, bool) =
                               nullptr,
                           void* paragraph_core_probe_context = nullptr);
  static void Unregister(uint64_t engine_id, uint64_t registration_generation);
  static int32_t GetApi(uint64_t engine_id,
                        uint32_t caller_struct_size,
                        FlutterVenusTextLayoutApiV2* out_api);

  /// 段落粒度 (一个 IFC 一次排完)。与 GetApi 走同一张注册表 ——
  /// slot/lease 校验落在 service 里, 与 layout_text_node_sync_v2 同一套。
  static int32_t LayoutParagraph(
      uint64_t engine_id,
      uint32_t slot,
      uint64_t lease_token,
      const FlutterVenusTextLayoutParagraphInputV2* input,
      FlutterVenusTextLayoutParagraphOutputV2* output);

  static int32_t BeginArtifactOwner(uint64_t engine_id,
                                    uint64_t registration_generation,
                                    uint64_t producer_generation,
                                    uint64_t* out_owner_id);
  static int32_t BindArtifactOwner(uint64_t engine_id,
                                   uint64_t owner_id,
                                   uint32_t slot,
                                   uint64_t lease_token);
  static int32_t SealArtifactOwner(uint64_t engine_id, uint64_t owner_id);
  static void ReleaseArtifactOwner(uint64_t engine_id, uint64_t owner_id);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_VENUS_TEXT_LAYOUT_REGISTRY_H_
