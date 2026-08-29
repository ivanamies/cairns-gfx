// scene/selection.hpp
//
// Document-side selection + highlight state. NOT a Viewport concern: the
// selection set lives on the scene (the Document); each Viewport just shows
// it. Type is one of {entity, material, draw}; the GPU ID buffer reads back
// a packed `{type<<24 | id}` integer that maps 1:1 to SelectionTarget.

#pragma once

#include <cstdint>

namespace cairns {

enum class SelectionType : uint8_t {
    kEntity = 0,
    kMaterial = 1,
    kDraw = 2,
};

struct SelectionTarget {
    SelectionType type = SelectionType::kEntity;
    uint32_t id = 0;
    // SceneId.index, packed flat so selection/highlight ops over the protocol
    // can name a target without a typed handle. 0 today; non-zero once
    // multi-scene rendering goes past two scenes.
    uint32_t scene = 0;
};

inline bool operator==(const SelectionTarget& a, const SelectionTarget& b) {
    return a.type == b.type && a.id == b.id && a.scene == b.scene;
}

// Pack/unpack for the GPU ID buffer. type lives in the top byte so a draw's
// {type, id} can ride on a single R32U fragment output.
inline uint32_t PackSelectionId(SelectionType t, uint32_t id) {
    return (static_cast<uint32_t>(t) << 24) | (id & 0x00FFFFFFu);
}

inline SelectionTarget UnpackSelectionId(uint32_t packed, uint32_t scene = 0) {
    SelectionTarget t;
    t.type = static_cast<SelectionType>((packed >> 24) & 0xFFu);
    t.id = packed & 0x00FFFFFFu;
    t.scene = scene;
    return t;
}

}  // namespace cairns
