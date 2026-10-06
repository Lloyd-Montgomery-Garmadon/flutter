// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

precision mediump float;

#include <impeller/color.glsl>
#include <impeller/dithering.glsl>
#include <impeller/texture.glsl>
#include <impeller/types.glsl>

struct ColorPoint {
  vec4 color;
  float stop;
  float inverse_delta;
};

layout(std140) readonly buffer ColorData {
  ColorPoint colors[];
}
color_data;

uniform FragInfo {
  highp vec2 start_point;
  highp vec2 end_point;
  float alpha;
  float tile_mode;
  vec4 decal_border_color;
  int colors_length;
  highp vec2 start_to_end;
  float inverse_dot_start_to_end;
}
frag_info;

highp in vec2 v_position;

out vec4 frag_color;

void main() {
  highp vec2 start_to_position = v_position - frag_info.start_point;
  highp float projected_position =
      dot(start_to_position, frag_info.start_to_end);
  highp float gradient_length_squared =
      dot(frag_info.start_to_end, frag_info.start_to_end);
  highp float t = projected_position *
                  frag_info.inverse_dot_start_to_end;

  if ((t < 0.0 || t > 1.0) && frag_info.tile_mode == kTileModeDecal) {
    frag_color = frag_info.decal_border_color;
  } else {
    t = IPFloatTile(t, frag_info.tile_mode);
    // Compare clamp stops before normalization so rounding in the reciprocal
    // does not move a hard stop across a pixel on one triangle of a quad.
    bool compare_projected =
        frag_info.tile_mode == kTileModeClamp && gradient_length_squared != 0.0;
    highp float comparison_scale =
        compare_projected ? gradient_length_squared : 1.0;
    highp float comparison_position = compare_projected
                                          ? clamp(projected_position, 0.0,
                                                  gradient_length_squared)
                                          : t;

    for (int i = 1; i < frag_info.colors_length; i++) {
      ColorPoint prev_point = color_data.colors[i - 1];
      ColorPoint current_point = color_data.colors[i];
      if (comparison_position >= prev_point.stop * comparison_scale &&
          (comparison_position < current_point.stop * comparison_scale ||
           (i == frag_info.colors_length - 1 &&
            comparison_position <=
                current_point.stop * comparison_scale))) {
        if (prev_point.stop == current_point.stop ||
            current_point.inverse_delta > 1000.0) {
          frag_color = current_point.color;
        } else {
          float ratio = (t - prev_point.stop) * current_point.inverse_delta;
          frag_color = mix(prev_point.color, current_point.color, ratio);
        }
        break;
      }
    }
  }

  frag_color = IPPremultiply(frag_color) * frag_info.alpha;
  frag_color = IPOrderedDither8x8(frag_color, gl_FragCoord.xy);
}
