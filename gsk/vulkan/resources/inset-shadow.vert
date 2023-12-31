#version 450

#include "clip.vert.glsl"

layout(location = 0) in mat3x4 inOutline;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec2 inOffset;
layout(location = 5) in float inSpread;
layout(location = 6) in float inBlurRadius;

layout(location = 0) out vec2 outPos;
layout(location = 1) out flat vec4 outOutline;
layout(location = 2) out flat vec4 outOutlineCornerWidths;
layout(location = 3) out flat vec4 outOutlineCornerHeights;
layout(location = 4) out flat vec4 outColor;
layout(location = 5) out flat vec2 outOffset;
layout(location = 6) out flat float outSpread;

vec2 offsets[6] = { vec2(0.0, 0.0),
                    vec2(1.0, 0.0),
                    vec2(0.0, 1.0),
                    vec2(0.0, 1.0),
                    vec2(1.0, 0.0),
                    vec2(1.0, 1.0) };

void main() {
  vec4 rect = clip (inOutline[0]);

  vec2 pos = rect.xy + rect.zw * offsets[gl_VertexIndex];
  gl_Position = push.mvp * vec4 (push.scale * pos, 0.0, 1.0);
  outPos = pos;

  outOutline = inOutline[0];
  outOutlineCornerWidths = inOutline[1];
  outOutlineCornerHeights = inOutline[2];
  outColor = inColor;
  outOffset = inOffset;
  outSpread = inSpread;
}
