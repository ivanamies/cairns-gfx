#version 450

// A.3: solid red output for the L1 single-red-triangle path.
// No inputs, no textures, no UBO. The simplest possible fragment shader.

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(1.0, 0.0, 0.0, 1.0);
}
