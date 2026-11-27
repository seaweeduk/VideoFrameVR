#version 450
#extension GL_GOOGLE_include_directive : require

// The seek preview's frame: the unit quad (4 vertices) or, curved, kColumns
// columns between pairs of vertices (a triangle strip).

#include "preview.glsl"

layout(location = 0) out vec2 vPos;  // across the quad, 0..1, row 0 at the top

const float kColumns = 16.0;  // kPreviewColumns in scene_renderer.cpp

void main() {
  vec4 v = pc.draw.view;
  bool curved = v.x > 0.0;
  vec2 aPos = curved ? vec2(-0.5 + float(gl_VertexIndex >> 1) / kColumns,
                            (gl_VertexIndex & 1) != 0 ? 0.5 : -0.5)
                     : vec2((gl_VertexIndex & 1) != 0 ? 0.5 : -0.5,
                            (gl_VertexIndex & 2) != 0 ? 0.5 : -0.5);
  vPos = vec2(aPos.x + 0.5, 0.5 - aPos.y);
  vec4 q = vec4(aPos, 0.0, 1.0);
  if (curved) {
    float angle = aPos.x * 2.0 * v.y;
    q = vec4(v.x * sin(angle), aPos.y, -v.x * cos(angle), 1.0);
  }
  gl_Position = eyeClip(currentEye(), vec3(dot(pc.draw.model0, q), dot(pc.draw.model1, q),
                                           dot(pc.draw.model2, q)));
}
