// util/primitives.hpp -- procedural primitive mesh generators (Unity's
// GameObject.CreatePrimitive analog). Pure CPU math: each returns model-space
// positions/normals/uvs/indices that the engine packs into its mesh vertex
// layout (pos stream = vec4 w=1, attr stream = VertexAttribute, u32 indices)
// and uploads through the SAME static-mesh path as die.glb -- so a primitive
// is a normal vbo mesh, not a special-case draw. Unit-sized; the entity's
// scale turns a sphere into an ellipsoid, a disk into an ellipse.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace cairns {

enum class PrimitiveKind {
    kTriangle,
    kPyramid,
    kCylinder,
    kEllipse,    // filled unit disk (XY); scale for an ellipse
    kEllipsoid,  // unit sphere; scale for an ellipsoid
};

struct PrimitiveMesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t> indices;

    void PushVert(const glm::vec3& p, const glm::vec3& n, const glm::vec2& uv) {
        positions.push_back(p);
        normals.push_back(n);
        uvs.push_back(uv);
    }
    // Emit a triangle by absolute vertex indices (already pushed).
    void PushTri(uint32_t a, uint32_t b, uint32_t c) {
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
    }
};

// A single flat triangle in the XY plane, double-sided (unlit ignores the
// normal; both windings so it shows regardless of cull/camera side).
inline PrimitiveMesh MakeTriangle() {
    PrimitiveMesh m;
    m.PushVert({-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f});
    m.PushVert({ 1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f});
    m.PushVert({ 0.0f,  1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.5f, 1.0f});
    m.PushTri(0, 1, 2);
    m.PushTri(0, 2, 1);
    return m;
}

// Square-base pyramid, apex up. Per-face flat normals => duplicated verts.
inline PrimitiveMesh MakePyramid() {
    PrimitiveMesh m;
    const glm::vec3 apex{0.0f, 1.0f, 0.0f};
    const glm::vec3 base[4] = {
        {-1.0f, -1.0f, -1.0f}, { 1.0f, -1.0f, -1.0f},
        { 1.0f, -1.0f,  1.0f}, {-1.0f, -1.0f,  1.0f}};
    // 4 side faces (CCW seen from outside).
    for (int i = 0; i < 4; ++i) {
        const glm::vec3& a = base[i];
        const glm::vec3& b = base[(i + 1) % 4];
        const glm::vec3 n = glm::normalize(glm::cross(b - a, apex - a));
        const uint32_t v = static_cast<uint32_t>(m.positions.size());
        m.PushVert(a, n, {0.0f, 0.0f});
        m.PushVert(b, n, {1.0f, 0.0f});
        m.PushVert(apex, n, {0.5f, 1.0f});
        m.PushTri(v, v + 1, v + 2);
    }
    // Base quad (facing down), two triangles.
    const glm::vec3 dn{0.0f, -1.0f, 0.0f};
    const uint32_t b0 = static_cast<uint32_t>(m.positions.size());
    for (int i = 0; i < 4; ++i) {
        m.PushVert(base[i], dn, {base[i].x * 0.5f + 0.5f, base[i].z * 0.5f + 0.5f});
    }
    m.PushTri(b0, b0 + 2, b0 + 1);
    m.PushTri(b0, b0 + 3, b0 + 2);
    return m;
}

// Cylinder: radius 1, height 2 (y in [-1,1]), radial side + two caps.
inline PrimitiveMesh MakeCylinder() {
    PrimitiveMesh m;
    constexpr uint32_t kSeg = 24;
    constexpr float kPi = 3.14159265358979323846f;
    // Side wall: duplicated ring per y so side normals are radial.
    const uint32_t side_base = static_cast<uint32_t>(m.positions.size());
    for (uint32_t i = 0; i <= kSeg; ++i) {
        const float a = (static_cast<float>(i) / kSeg) * 2.0f * kPi;
        const glm::vec3 dir{std::cos(a), 0.0f, std::sin(a)};
        const float u = static_cast<float>(i) / kSeg;
        m.PushVert({dir.x, 1.0f, dir.z}, dir, {u, 1.0f});
        m.PushVert({dir.x, -1.0f, dir.z}, dir, {u, 0.0f});
    }
    for (uint32_t i = 0; i < kSeg; ++i) {
        const uint32_t t = side_base + i * 2;
        m.PushTri(t, t + 1, t + 2);
        m.PushTri(t + 1, t + 3, t + 2);
    }
    // Caps: center + rim fan, correct normals.
    for (int cap = 0; cap < 2; ++cap) {
        const float y = cap == 0 ? 1.0f : -1.0f;
        const glm::vec3 n{0.0f, y, 0.0f};
        const uint32_t c = static_cast<uint32_t>(m.positions.size());
        m.PushVert({0.0f, y, 0.0f}, n, {0.5f, 0.5f});
        for (uint32_t i = 0; i <= kSeg; ++i) {
            const float a = (static_cast<float>(i) / kSeg) * 2.0f * kPi;
            m.PushVert({std::cos(a), y, std::sin(a)}, n,
                       {std::cos(a) * 0.5f + 0.5f, std::sin(a) * 0.5f + 0.5f});
        }
        for (uint32_t i = 0; i < kSeg; ++i) {
            if (cap == 0) {
                m.PushTri(c, c + 1 + i, c + 2 + i);
            } else {
                m.PushTri(c, c + 2 + i, c + 1 + i);  // flip for downward cap
            }
        }
    }
    return m;
}

// Filled unit disk in the XY plane, double-sided (triangle fan).
inline PrimitiveMesh MakeEllipse() {
    PrimitiveMesh m;
    constexpr uint32_t kSeg = 48;
    constexpr float kPi = 3.14159265358979323846f;
    const glm::vec3 fn{0.0f, 0.0f, 1.0f};
    const uint32_t c = static_cast<uint32_t>(m.positions.size());
    m.PushVert({0.0f, 0.0f, 0.0f}, fn, {0.5f, 0.5f});
    for (uint32_t i = 0; i <= kSeg; ++i) {
        const float a = (static_cast<float>(i) / kSeg) * 2.0f * kPi;
        m.PushVert({std::cos(a), std::sin(a), 0.0f}, fn,
                   {std::cos(a) * 0.5f + 0.5f, std::sin(a) * 0.5f + 0.5f});
    }
    for (uint32_t i = 0; i < kSeg; ++i) {
        m.PushTri(c, c + 1 + i, c + 2 + i);       // front
        m.PushTri(c, c + 2 + i, c + 1 + i);       // back (double-sided)
    }
    return m;
}

// Unit UV sphere; per-vertex normal = position.
inline PrimitiveMesh MakeEllipsoid() {
    PrimitiveMesh m;
    constexpr uint32_t kStacks = 16;
    constexpr uint32_t kSlices = 24;
    constexpr float kPi = 3.14159265358979323846f;
    for (uint32_t s = 0; s <= kStacks; ++s) {
        const float phi = (static_cast<float>(s) / kStacks) * kPi;  // 0..pi
        for (uint32_t j = 0; j <= kSlices; ++j) {
            const float theta = (static_cast<float>(j) / kSlices) * 2.0f * kPi;
            const glm::vec3 p{std::sin(phi) * std::cos(theta), std::cos(phi),
                              std::sin(phi) * std::sin(theta)};
            m.PushVert(p, p,
                       {static_cast<float>(j) / kSlices,
                        1.0f - static_cast<float>(s) / kStacks});
        }
    }
    const uint32_t row = kSlices + 1;
    for (uint32_t s = 0; s < kStacks; ++s) {
        for (uint32_t j = 0; j < kSlices; ++j) {
            const uint32_t a = s * row + j;
            const uint32_t b = a + row;
            m.PushTri(a, b, a + 1);
            m.PushTri(a + 1, b, b + 1);
        }
    }
    return m;
}

inline PrimitiveMesh MakePrimitive(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::kPyramid:   return MakePyramid();
        case PrimitiveKind::kCylinder:  return MakeCylinder();
        case PrimitiveKind::kEllipse:   return MakeEllipse();
        case PrimitiveKind::kEllipsoid: return MakeEllipsoid();
        case PrimitiveKind::kTriangle:
        default:                        return MakeTriangle();
    }
}

}  // namespace cairns
