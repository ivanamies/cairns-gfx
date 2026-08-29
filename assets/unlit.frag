#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 2) uniform sampler samplers[];

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) flat in uint inTexId;
layout(location = 2) flat in uint inSamplerId;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(
        sampler2D(textures[nonuniformEXT(inTexId)], samplers[nonuniformEXT(inSamplerId)]),
        inTexCoord);
}
