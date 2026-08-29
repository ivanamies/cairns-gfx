#include "control/handlers/entity_ops.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "control/command_registry.hpp"
#include "engine_headless.hpp"
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
}

}  // namespace cairns::control
