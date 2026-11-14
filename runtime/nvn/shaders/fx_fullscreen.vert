#version 450
// SPDX-License-Identifier: MIT
// Shared full-screen triangle for the Fx effects (nvn_fx.cpp). No vertex input.
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
