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
    t.scene = static_cast<uint32_t>(j.value("world", uint64_t{0}));
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
            {"world", t.scene}};
}

json EncodeTargets(const std::vector<cairns::SelectionTarget>& v) {
    json arr = json::array();
    for (const auto& t : v) {
        arr.push_back(EncodeTarget(t));
    }
    return arr;
}

void EmitSelectionChanged(CommandRegistry& registry, cairns::Engine* engine) {
    json data = {
        {"revision", cairns::headless::GetSelectionRevision(engine)},
        {"targets", EncodeTargets(cairns::headless::GetSelection(engine))}
    };
    registry.PublishEvent("cairns.selection.changed", std::move(data));
}

void EmitHighlightChanged(CommandRegistry& registry, cairns::Engine* engine) {
    json data = {
        {"targets", EncodeTargets(cairns::headless::GetHighlights(engine))}
    };
    registry.PublishEvent("cairns.highlight.changed", std::move(data));
}

}  // namespace

void RegisterSelectionOps(CommandRegistry& registry, cairns::Engine& engine) {
    registry.Register(
        "cairns.selection.set",
        /*schema=*/json::object(),
        /*doc=*/"Replace the selection set. args.targets = [{type, id, world}, ...] "
                "or a single {type, id, world}. Type in {entity, material, draw}. "
                "Bumps the selection revision; subscribers to cairns.selection.changed "
                "see a new tick on the next emit.",
        [engine = &engine, &registry](const json& args) -> json {
            cairns::headless::SetSelection(engine, ParseTargets(args));
            EmitSelectionChanged(registry, engine);
            return {{"revision", cairns::headless::GetSelectionRevision(engine)}};
        });

    registry.Register(
        "cairns.selection.add",
        /*schema=*/json::object(),
        /*doc=*/"Add one target to the selection set (idempotent). args = "
                "{type, id, world}.",
        [engine = &engine, &registry](const json& args) -> json {
            const uint32_t prev = cairns::headless::GetSelectionRevision(engine);
            cairns::headless::AddSelection(engine, ParseTarget(args));
            const uint32_t now = cairns::headless::GetSelectionRevision(engine);
            if (now != prev) {
                EmitSelectionChanged(registry, engine);
            }
            return {{"revision", now}};
        });

    registry.Register(
        "cairns.selection.remove",
        /*schema=*/json::object(),
        /*doc=*/"Remove one target from the selection set.",
        [engine = &engine, &registry](const json& args) -> json {
            const uint32_t prev = cairns::headless::GetSelectionRevision(engine);
            cairns::headless::RemoveSelection(engine, ParseTarget(args));
            const uint32_t now = cairns::headless::GetSelectionRevision(engine);
            if (now != prev) {
                EmitSelectionChanged(registry, engine);
            }
            return {{"revision", now}};
        });

    registry.Register(
        "cairns.selection.clear",
        /*schema=*/json::object(),
        /*doc=*/"Empty the selection set.",
        [engine = &engine, &registry](const json&) -> json {
            const uint32_t prev = cairns::headless::GetSelectionRevision(engine);
            cairns::headless::ClearSelection(engine);
            const uint32_t now = cairns::headless::GetSelectionRevision(engine);
            if (now != prev) {
                EmitSelectionChanged(registry, engine);
            }
            return {{"revision", now}};
        });

    registry.Register(
        "cairns.selection.get",
        /*schema=*/json::object(),
        /*doc=*/"Read the current selection set + its revision counter.",
        [engine = &engine](const json&) -> json {
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
        [engine = &engine, &registry](const json& args) -> json {
            cairns::headless::SetHighlights(engine, ParseTargets(args));
            EmitHighlightChanged(registry, engine);
            return json::object();
        });

    registry.Register(
        "cairns.highlight.clear",
        /*schema=*/json::object(),
        /*doc=*/"Empty the highlight set.",
        [engine = &engine, &registry](const json&) -> json {
            cairns::headless::ClearHighlights(engine);
            EmitHighlightChanged(registry, engine);
            return json::object();
        });

    registry.Register(
        "cairns.pick",
        /*schema=*/json::object(),
        /*doc=*/"Schedule a pick at (viewport, x, y) in viewport-local pixel "
                "coords. Records intent; the engine resolves it the next time "
                "draw() completes a frame after this request. Poll "
                "cairns.pick.consume to retrieve the resolved {type, id, raw}.",
        [engine = &engine](const json& args) -> json {
            const int vp = static_cast<int>(args.value("viewport", int64_t{0}));
            const uint32_t x = static_cast<uint32_t>(args.value("x", uint64_t{0}));
            const uint32_t y = static_cast<uint32_t>(args.value("y", uint64_t{0}));
            cairns::headless::RequestPick(engine, vp, x, y);
            return {{"pending", true},
                    {"viewport", vp},
                    {"x", x},
                    {"y", y}};
        });

    registry.Register(
        "cairns.pick.consume",
        /*schema=*/json::object(),
        /*doc=*/"Poll for the most recent resolved pick. {resolved:false} "
                "until the engine has run a frame post-request. On success: "
                "{resolved:true, viewport, x, y, type, id, raw}. raw is the "
                "stub source value (BGRA at the texel) until the R32U ID "
                "buffer lands; id swaps to the decoded value with #206.",
        [engine = &engine](const json&) -> json {
            cairns::headless::PickResultExport r =
                cairns::headless::ConsumePickResult(engine);
            if (!r.resolved) {
                return {{"resolved", false}};
            }
            return {{"resolved", true},
                    {"viewport", r.viewport},
                    {"x", r.x},
                    {"y", r.y},
                    {"type", TypeName(r.type)},
                    {"id", r.id},
                    {"raw", r.raw}};
        });
    registry.RegisterAlias("pick.consume", "cairns.pick.consume");
}

}  // namespace cairns::control
