#include "control/handlers/selection_ops.hpp"

#include <stdexcept>
#include <vector>

#include "control/command_registry.hpp"
#include "engine_headless.hpp"
#include "scene/selection.hpp"
#include "util/json.hpp"

namespace cairns::control {

namespace {

cairns::SelectionType ParseType(const std::string& s) {
    if (s == "material") {
        return cairns::SelectionType::kMaterial;
    }
    if (s == "draw") {
        return cairns::SelectionType::kDraw;
    }
    return cairns::SelectionType::kEntity;
}

const char* TypeName(cairns::SelectionType t) {
    switch (t) {
        case cairns::SelectionType::kMaterial: return "material";
        case cairns::SelectionType::kDraw: return "draw";
        case cairns::SelectionType::kEntity:
        default: return "entity";
    }
}

cairns::SelectionTarget ParseTarget(const json& j) {
    cairns::SelectionTarget t;
    t.type = ParseType(j.value("type", std::string{"entity"}));
    t.id = static_cast<uint32_t>(j.value("id", uint64_t{0}));
    t.world = static_cast<uint32_t>(j.value("world", uint64_t{0}));
    return t;
}

std::vector<cairns::SelectionTarget> ParseTargets(const json& args) {
    std::vector<cairns::SelectionTarget> out;
    if (auto it = args.find("targets"); it != args.end() && it->is_array()) {
        out.reserve(it->size());
        for (const auto& tj : *it) {
            out.push_back(ParseTarget(tj));
        }
    } else if (args.contains("type") || args.contains("id")) {
        out.push_back(ParseTarget(args));
    }
    return out;
}

json EncodeTarget(const cairns::SelectionTarget& t) {
    return {{"type", TypeName(t.type)},
            {"id", t.id},
            {"world", t.world}};
}

json EncodeTargets(const std::vector<cairns::SelectionTarget>& v) {
    json arr = json::array();
    for (const auto& t : v) {
        arr.push_back(EncodeTarget(t));
    }
    return arr;
}

}  // namespace

void RegisterSelectionOps(CommandRegistry& registry, cairns::Engine* engine) {
    registry.Register(
        "cairns.selection.set",
        /*schema=*/json::object(),
        /*doc=*/"Replace the selection set. args.targets = [{type, id, world}, ...] "
                "or a single {type, id, world}. Type in {entity, material, draw}. "
                "Bumps the selection revision; subscribers to cairns.selection.changed "
                "see a new tick on the next emit.",
        [engine](const json& args) -> json {
            if (!engine) {
                throw std::runtime_error("engine not initialized");
            }
            cairns::headless::SetSelection(engine, ParseTargets(args));
            return {{"revision", cairns::headless::GetSelectionRevision(engine)}};
        });

    registry.Register(
        "cairns.selection.add",
        /*schema=*/json::object(),
        /*doc=*/"Add one target to the selection set (idempotent). args = "
                "{type, id, world}.",
        [engine](const json& args) -> json {
            if (!engine) {
                throw std::runtime_error("engine not initialized");
            }
            cairns::headless::AddSelection(engine, ParseTarget(args));
            return {{"revision", cairns::headless::GetSelectionRevision(engine)}};
        });

    registry.Register(
        "cairns.selection.remove",
        /*schema=*/json::object(),
        /*doc=*/"Remove one target from the selection set.",
        [engine](const json& args) -> json {
            if (!engine) {
                throw std::runtime_error("engine not initialized");
            }
            cairns::headless::RemoveSelection(engine, ParseTarget(args));
            return {{"revision", cairns::headless::GetSelectionRevision(engine)}};
        });

    registry.Register(
        "cairns.selection.clear",
        /*schema=*/json::object(),
        /*doc=*/"Empty the selection set.",
        [engine](const json&) -> json {
            if (!engine) {
                throw std::runtime_error("engine not initialized");
            }
            cairns::headless::ClearSelection(engine);
            return {{"revision", cairns::headless::GetSelectionRevision(engine)}};
        });

    registry.Register(
        "cairns.selection.get",
        /*schema=*/json::object(),
        /*doc=*/"Read the current selection set + its revision counter.",
        [engine](const json&) -> json {
            if (!engine) {
                throw std::runtime_error("engine not initialized");
            }
            return {{"targets", EncodeTargets(cairns::headless::GetSelection(engine))},
                    {"revision", cairns::headless::GetSelectionRevision(engine)}};
        });

    registry.Register(
        "cairns.highlight.set",
        /*schema=*/json::object(),
        /*doc=*/"Replace the highlight set -- the things that should glow "
                "in the next render. Independent from the selection set; "
                "the VLM agent may highlight everything matching a material "
                "without selecting any of them.",
        [engine](const json& args) -> json {
            if (!engine) {
                throw std::runtime_error("engine not initialized");
            }
            cairns::headless::SetHighlights(engine, ParseTargets(args));
            return json::object();
        });

    registry.Register(
        "cairns.highlight.clear",
        /*schema=*/json::object(),
        /*doc=*/"Empty the highlight set.",
        [engine](const json&) -> json {
            if (!engine) {
                throw std::runtime_error("engine not initialized");
            }
            cairns::headless::ClearHighlights(engine);
            return json::object();
        });

    registry.Register(
        "cairns.pick",
        /*schema=*/json::object(),
        /*doc=*/"Schedule a pick at (viewport, x, y) in viewport-local pixel "
                "coords. Records intent; the GPU ID-buffer + readback path is "
                "a follow-up. Today returns {pending:true}; once the GPU side "
                "lands, returns the resolved {type, id, world} of whatever was "
                "rendered at that texel.",
        [engine](const json& args) -> json {
            if (!engine) {
                throw std::runtime_error("engine not initialized");
            }
            const int vp = static_cast<int>(args.value("viewport", int64_t{0}));
            const uint32_t x = static_cast<uint32_t>(args.value("x", uint64_t{0}));
            const uint32_t y = static_cast<uint32_t>(args.value("y", uint64_t{0}));
            cairns::headless::RequestPick(engine, vp, x, y);
            return {{"pending", true},
                    {"viewport", vp},
                    {"x", x},
                    {"y", y}};
        });
}

}  // namespace cairns::control
