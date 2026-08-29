#include "control/handlers/entity_ops.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "control/command_registry.hpp"
#include "engine_headless.hpp"
#include "scene/component_type.hpp"
#include "util/json.hpp"

namespace cairns::control {

namespace {

// Read a fixed-length float array from json (`key`), falling back to `def`.
template <size_t N>
std::array<float, N> ReadVec(const json& args, const char* key,
                             const std::array<float, N>& def) {
    std::array<float, N> out = def;
    if (args.contains(key) && args[key].is_array()) {
        const auto& a = args[key];
        for (size_t i = 0; i < N && i < a.size(); ++i) {
            out[i] = a[i].get<float>();
        }
    }
    return out;
}

int SceneArg(const json& args) { return args.value("scene", -1); }

// Component-type table. Each row binds a type name to typed headless::
// accessors; add/get parse json props here (control side), remove uses the
// generic ComponentType switch. Sorted-by-name array, binary-searched (no
// maps). Adding a CV-tool/effect/diffusion component = one row + typed
// accessors; tools.list advertises it automatically.
json AddName(cairns::Engine* e, int s, uint32_t ent, const json& props) {
    const std::string n = props.value("name", std::string{});
    return {{"ok", cairns::headless::SetEntityName(e, s, ent, n)}};
}
json GetName(cairns::Engine* e, int s, uint32_t ent) {
    std::string out;
    if (!cairns::headless::GetEntityName(e, s, ent, out)) {
        return {{"has", false}};
    }
    return {{"has", true}, {"name", out}};
}
json AddCamera(cairns::Engine* e, int s, uint32_t ent, const json& props) {
    const float fov = props.value("fovYRad", 1.5707964f);  // 90 deg
    const float nz = props.value("nearZ", 0.1f);
    const float fz = props.value("farZ", 100.0f);
    const bool main = props.value("isMain", false);
    return {{"ok", cairns::headless::SetEntityCamera(e, s, ent, fov, nz, fz,
                                                     main)}};
}
json GetCamera(cairns::Engine* e, int s, uint32_t ent) {
    float fov = 0.0f;
    float nz = 0.0f;
    float fz = 0.0f;
    bool main = false;
    if (!cairns::headless::GetEntityCamera(e, s, ent, fov, nz, fz, main)) {
        return {{"has", false}};
    }
    return {{"has", true},
            {"fovYRad", fov},
            {"nearZ", nz},
            {"farZ", fz},
            {"isMain", main}};
}
json AddEmitter(cairns::Engine* e, int s, uint32_t ent, const json&) {
    return {{"ok", cairns::headless::AddParticleEmitter(e, s, ent)}};
}
json GetEmitter(cairns::Engine* e, int s, uint32_t ent) {
    return {{"has", cairns::headless::HasComponent(
                 e, s, ent, cairns::ComponentType::kParticleEmitter)}};
}
json AddRenderable(cairns::Engine* e, int s, uint32_t ent, const json& props) {
    const uint32_t lm = props.value("layerMask", 0xFFFFFFFFu);
    const uint32_t fl = props.value("flags", 0u);
    return {{"ok", cairns::headless::SetEntityRenderable(e, s, ent, lm, fl)}};
}
json GetRenderable(cairns::Engine* e, int s, uint32_t ent) {
    uint32_t lm = 0;
    uint32_t fl = 0;
    if (!cairns::headless::GetEntityRenderable(e, s, ent, lm, fl)) {
        return {{"has", false}};
    }
    return {{"has", true}, {"layerMask", lm}, {"flags", fl}};
}

struct ComponentRow {
    const char* name;
    cairns::ComponentType type;
    json (*add)(cairns::Engine*, int, uint32_t, const json&);
    json (*get)(cairns::Engine*, int, uint32_t);
};

// MUST stay sorted by name (binary search below).
const ComponentRow kComponentRows[] = {
    {"Camera", cairns::ComponentType::kCamera, AddCamera, GetCamera},
    {"Name", cairns::ComponentType::kName, AddName, GetName},
    {"ParticleEmitter", cairns::ComponentType::kParticleEmitter, AddEmitter,
     GetEmitter},
    {"Renderable", cairns::ComponentType::kRenderable, AddRenderable,
     GetRenderable},
};

const ComponentRow* FindComponentRow(const std::string& name) {
    int lo = 0;
    int hi = static_cast<int>(std::size(kComponentRows)) - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int cmp = name.compare(kComponentRows[mid].name);
        if (cmp == 0) {
            return &kComponentRows[mid];
        }
        if (cmp < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return nullptr;
}

}  // namespace

void RegisterEntityOps(CommandRegistry& registry, cairns::Engine& engine) {
    registry.Register(
        "cairns.entity.destroy",
        json::object(),
        "Destroy an entity in a scene (and scrub it from selection/highlight). "
        "Args: {scene? (0/1, default active), entity (uint)}. Returns {ok}.",
        [&engine](const json& args) -> json {
            const uint32_t entity = args.value("entity", UINT32_MAX);
            const bool ok = cairns::headless::DestroyEntity(
                &engine, SceneArg(args), entity);
            return {{"ok", ok}};
        });

    registry.Register(
        "cairns.entity.setTRS",
        json::object(),
        "Author an entity's local transform. Args: {scene?, entity, "
        "t:[x,y,z], r:[x,y,z,w] (quat), s:[x,y,z]}. Missing components default "
        "to identity (t=0, r=identity, s=1). Returns {ok}.",
        [&engine](const json& args) -> json {
            const uint32_t entity = args.value("entity", UINT32_MAX);
            const std::array<float, 3> t =
                ReadVec<3>(args, "t", {0.0f, 0.0f, 0.0f});
            const std::array<float, 4> r =
                ReadVec<4>(args, "r", {0.0f, 0.0f, 0.0f, 1.0f});
            const std::array<float, 3> s =
                ReadVec<3>(args, "s", {1.0f, 1.0f, 1.0f});
            const bool ok = cairns::headless::SetEntityTRS(
                &engine, SceneArg(args), entity, t.data(), r.data(), s.data());
            return {{"ok", ok}};
        });

    registry.Register(
        "cairns.entity.getTRS",
        json::object(),
        "Read an entity's local transform. Args: {scene?, entity}. Returns "
        "{ok, t:[x,y,z], r:[x,y,z,w], s:[x,y,z]}.",
        [&engine](const json& args) -> json {
            const uint32_t entity = args.value("entity", UINT32_MAX);
            float t[3];
            float r[4];
            float s[3];
            const bool ok = cairns::headless::GetEntityTRS(
                &engine, SceneArg(args), entity, t, r, s);
            if (!ok) {
                return {{"ok", false}};
            }
            return {{"ok", true},
                    {"t", {t[0], t[1], t[2]}},
                    {"r", {r[0], r[1], r[2], r[3]}},
                    {"s", {s[0], s[1], s[2]}}};
        });

    registry.Register(
        "cairns.entity.setParent",
        json::object(),
        "Re-parent an entity (writes a Parent component; cycle-guarded). "
        "Args: {scene?, entity, parent (uint) | null to unparent}. "
        "Returns {ok}.",
        [&engine](const json& args) -> json {
            const uint32_t entity = args.value("entity", UINT32_MAX);
            const bool clear =
                !args.contains("parent") || args["parent"].is_null();
            const uint32_t parent = clear ? 0u : args.value("parent", 0u);
            const bool ok = cairns::headless::SetEntityParent(
                &engine, SceneArg(args), entity, parent, clear);
            return {{"ok", ok}};
        });

    registry.Register(
        "cairns.entity.find",
        json::object(),
        "Find the first entity with a matching Name. Args: {scene?, name}. "
        "Returns {found, entity}.",
        [&engine](const json& args) -> json {
            const std::string name = args.value("name", std::string{});
            const uint32_t e = cairns::headless::FindEntityByName(
                &engine, SceneArg(args), name);
            return {{"found", e != UINT32_MAX}, {"entity", e}};
        });

    registry.Register(
        "cairns.entity.setName",
        json::object(),
        "Set an entity's Name (editor-side). Args: {scene?, entity, name}. "
        "Returns {ok}.",
        [&engine](const json& args) -> json {
            const uint32_t entity = args.value("entity", UINT32_MAX);
            const std::string name = args.value("name", std::string{});
            const bool ok = cairns::headless::SetEntityName(
                &engine, SceneArg(args), entity, name);
            return {{"ok", ok}};
        });

    // Generic component ops -- the extension seam. type is a table row name
    // (cairns.entity.componentTypes lists them); props are type-specific.
    registry.Register(
        "cairns.entity.addComponent",
        json::object(),
        "Add/replace a component by type name. Args: {scene?, entity, type, "
        "props?}. type in componentTypes; props are type-specific (Camera: "
        "{fovYRad,nearZ,farZ,isMain}; Name: {name}; Renderable: {layerMask,"
        "flags}; ParticleEmitter: {}). Returns {ok} or {ok:false, error}.",
        [&engine](const json& args) -> json {
            const ComponentRow* row =
                FindComponentRow(args.value("type", std::string{}));
            if (!row) {
                return {{"ok", false}, {"error", "unknown component type"}};
            }
            const uint32_t entity = args.value("entity", UINT32_MAX);
            const json props = args.value("props", json::object());
            return row->add(&engine, SceneArg(args), entity, props);
        });

    registry.Register(
        "cairns.entity.getComponent",
        json::object(),
        "Read a component by type name. Args: {scene?, entity, type}. Returns "
        "{has, ...type-specific fields} or {has:false, error} for unknown type.",
        [&engine](const json& args) -> json {
            const ComponentRow* row =
                FindComponentRow(args.value("type", std::string{}));
            if (!row) {
                return {{"has", false}, {"error", "unknown component type"}};
            }
            const uint32_t entity = args.value("entity", UINT32_MAX);
            return row->get(&engine, SceneArg(args), entity);
        });

    registry.Register(
        "cairns.entity.removeComponent",
        json::object(),
        "Remove a component by type name. Args: {scene?, entity, type}. "
        "Returns {ok} (false for unknown type or absent component).",
        [&engine](const json& args) -> json {
            const ComponentRow* row =
                FindComponentRow(args.value("type", std::string{}));
            if (!row) {
                return {{"ok", false}, {"error", "unknown component type"}};
            }
            const uint32_t entity = args.value("entity", UINT32_MAX);
            const bool ok = cairns::headless::RemoveComponent(
                &engine, SceneArg(args), entity, row->type);
            return {{"ok", ok}};
        });

    registry.Register(
        "cairns.entity.componentTypes",
        json::object(),
        "List the component type names addComponent/getComponent understand. "
        "Returns {types:[...]}.",
        [](const json&) -> json {
            json types = json::array();
            for (const ComponentRow& row : kComponentRows) {
                types.push_back(row.name);
            }
            return {{"types", types}};
        });
}

}  // namespace cairns::control
