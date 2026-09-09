#include "control/handlers/entity_ops.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
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
json AddDirectionalLight(cairns::Engine* e, int s, uint32_t ent,
                         const json& props) {
    cairns::headless::DirectionalLightParams p;
    p.dir_x = props.value("dirX", 0.0f);
    p.dir_y = props.value("dirY", -1.0f);
    p.dir_z = props.value("dirZ", 0.0f);
    p.color_r = props.value("colorR", 1.0f);
    p.color_g = props.value("colorG", 1.0f);
    p.color_b = props.value("colorB", 1.0f);
    p.intensity = props.value("intensity", 1.0f);
    p.ambient_r = props.value("ambientR", 0.05f);
    p.ambient_g = props.value("ambientG", 0.05f);
    p.ambient_b = props.value("ambientB", 0.05f);
    p.cast_shadows = props.value("castShadows", false);
    return {{"ok",
             cairns::headless::SetEntityDirectionalLight(e, s, ent, p)}};
}
json GetDirectionalLight(cairns::Engine* e, int s, uint32_t ent) {
    cairns::headless::DirectionalLightParams p;
    if (!cairns::headless::GetEntityDirectionalLight(e, s, ent, p)) {
        return {{"has", false}};
    }
    return {{"has", true},        {"dirX", p.dir_x},
            {"dirY", p.dir_y},    {"dirZ", p.dir_z},
            {"colorR", p.color_r}, {"colorG", p.color_g},
            {"colorB", p.color_b}, {"intensity", p.intensity},
            {"ambientR", p.ambient_r}, {"ambientG", p.ambient_g},
            {"ambientB", p.ambient_b}, {"castShadows", p.cast_shadows}};
}
json AddPostEffect(cairns::Engine* e, int s, uint32_t ent, const json& props) {
    cairns::headless::PostEffectParams p;
    p.type = props.value("type", 0u);
    p.order = props.value("order", 0u);
    if (props.contains("p0") && props["p0"].is_array()) {
        const auto& a = props["p0"];
        for (size_t i = 0; i < 4 && i < a.size(); ++i) {
            p.p0[i] = a[i].get<float>();
        }
    }
    if (props.contains("p1") && props["p1"].is_array()) {
        const auto& a = props["p1"];
        for (size_t i = 0; i < 4 && i < a.size(); ++i) {
            p.p1[i] = a[i].get<float>();
        }
    }
    return {{"ok", cairns::headless::SetEntityPostEffect(e, s, ent, p)}};
}
json GetPostEffect(cairns::Engine* e, int s, uint32_t ent) {
    cairns::headless::PostEffectParams p;
    if (!cairns::headless::GetEntityPostEffect(e, s, ent, p)) {
        return {{"has", false}};
    }
    return {{"has", true},
            {"type", p.type},
            {"order", p.order},
            {"p0", {p.p0[0], p.p0[1], p.p0[2], p.p0[3]}},
            {"p1", {p.p1[0], p.p1[1], p.p1[2], p.p1[3]}}};
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
    {"DirectionalLight", cairns::ComponentType::kDirectionalLight,
     AddDirectionalLight, GetDirectionalLight},
    {"Name", cairns::ComponentType::kName, AddName, GetName},
    {"ParticleEmitter", cairns::ComponentType::kParticleEmitter, AddEmitter,
     GetEmitter},
    {"PostEffect", cairns::ComponentType::kPostEffect, AddPostEffect,
     GetPostEffect},
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
        "cairns.entity.new",
        json{{"name", ""}, {"scene", -1}},
        "Create an EMPTY entity (no Transform/Renderable) as a component "
        "carrier -- lights, effect stacks. Returns {entity}.",
        [&engine](const json& args) -> json {
            const std::string name = args.value("name", "");
            const int scene = args.value("scene", -1);
            const uint32_t e = cairns::headless::CreateEmptyEntity(
                &engine, scene, name.c_str());
            if (e == UINT32_MAX) {
                throw std::runtime_error("bad scene index");
            }
            return {{"entity", e}};
        });

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
        "cairns.entity.setJointPose",
        json::object(),
        "Drive a skinned entity's joints directly, bypassing clip sampling. "
        "Args: {scene?, entity, joints:[{joint (uint, SKIN joint index -- see "
        "cairns.entity.jointNames), r:[x,y,z,w] quat OR axis:[x,y,z]+angle "
        "(radians), t:[x,y,z], s:[x,y,z]}]}. Every component is optional and "
        "an omitted one keeps its current value, which starts at the prefab's "
        "bind pose -- so a caller names only the DoFs it drives. The pose is "
        "static state, not a one-frame nudge: it holds until setJointPose "
        "changes it or clearJointPose hands the actor back to its clip, and it "
        "is time-independent, so a posed frame byte-gates. Set poses AFTER "
        "loading prefabs: a later cairns.prefab.load can force a full "
        "anim-table rebuild, which re-applies live poses but not ones set on "
        "an entity whose skin was recreated. Returns {ok, entity, joints}.",
        [&engine](const json& args) -> json {
            const uint32_t entity = args.value("entity", UINT32_MAX);
            std::vector<uint32_t> ids;
            std::vector<uint32_t> masks;
            std::vector<float> trs10;
            if (args.contains("joints") && args["joints"].is_array()) {
                const auto& arr = args["joints"];
                ids.reserve(arr.size());
                masks.reserve(arr.size());
                trs10.reserve(arr.size() * 10u);
                for (const json& j : arr) {
                    ids.push_back(j.value("joint", UINT32_MAX));
                    uint32_t mask = 0;
                    const std::array<float, 3> t =
                        ReadVec<3>(j, "t", {0.0f, 0.0f, 0.0f});
                    std::array<float, 4> r = {0.0f, 0.0f, 0.0f, 1.0f};
                    const std::array<float, 3> sc =
                        ReadVec<3>(j, "s", {1.0f, 1.0f, 1.0f});
                    if (j.contains("t")) { mask |= 1u; }
                    if (j.contains("s")) { mask |= 4u; }
                    if (j.contains("r")) {
                        r = ReadVec<4>(j, "r", r);
                        mask |= 2u;
                    } else if (j.contains("axis")) {
                        // Hinge form: the natural one for an MJCF-derived rig,
                        // where a joint node is one revolute DoF about a fixed
                        // axis. Normalised here so a caller can pass the raw
                        // MJCF axis.
                        const std::array<float, 3> ax =
                            ReadVec<3>(j, "axis", {0.0f, 1.0f, 0.0f});
                        const float angle = j.value("angle", 0.0f);
                        float len = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] +
                                              ax[2] * ax[2]);
                        if (len <= 0.0f) { len = 1.0f; }
                        const float half = angle * 0.5f;
                        const float sn = std::sin(half) / len;
                        r = {ax[0] * sn, ax[1] * sn, ax[2] * sn,
                             std::cos(half)};
                        mask |= 2u;
                    }
                    masks.push_back(mask);
                    trs10.insert(trs10.end(),
                                 {t[0], t[1], t[2], r[0], r[1], r[2], r[3],
                                  sc[0], sc[1], sc[2]});
                }
            }
            const bool ok = cairns::headless::SetEntityJointPose(
                &engine, SceneArg(args), entity, ids.data(), masks.data(),
                trs10.data(), static_cast<uint32_t>(ids.size()));
            return {{"ok", ok},
                    {"entity", entity},
                    {"joints", ids.size()}};
        });

    registry.Register(
        "cairns.entity.clearJointPose",
        json::object(),
        "Drop an entity's joint-pose override and hand the actor back to its "
        "animation clip. Args: {scene?, entity}. Returns {ok, entity}. An "
        "entity that was never posed is a no-op success.",
        [&engine](const json& args) -> json {
            const uint32_t entity = args.value("entity", UINT32_MAX);
            return {{"ok", cairns::headless::ClearEntityJointPose(
                               &engine, SceneArg(args), entity)},
                    {"entity", entity}};
        });

    registry.Register(
        "cairns.entity.jointNames",
        json::object(),
        "Skin joint index -> node name for a skinned entity, so a script can "
        "resolve a rig's joint by name instead of hardcoding an index. Args: "
        "{scene?, entity}. Returns {ok, joints:[{index, node, name}]}.",
        [&engine](const json& args) -> json {
            const uint32_t entity = args.value("entity", UINT32_MAX);
            std::vector<int32_t> nodes;
            std::vector<std::string> names;
            if (!cairns::headless::ListEntityJointNames(
                    &engine, SceneArg(args), entity, nodes, names)) {
                return {{"ok", false}};
            }
            json joints = json::array();
            for (size_t i = 0; i < nodes.size(); ++i) {
                joints.push_back({{"index", i},
                                  {"node", nodes[i]},
                                  {"name", names[i]}});
            }
            return {{"ok", true}, {"joints", std::move(joints)}};
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
