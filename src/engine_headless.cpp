#include "engine_headless.hpp"

#include "engine.hpp"

namespace cairns::headless {

bool RenderFrame(Engine* engine) {
    if (!engine) {
        return false;
    }
    return engine->RenderHeadlessFrame();
}

bool DumpFinalTarget(Engine* engine, const std::filesystem::path& path) {
    if (!engine) {
        return false;
    }
    return engine->DumpFinalTarget(path);
}

void SetRandomSeed(Engine* engine, uint32_t seed) {
    if (!engine) {
        return;
    }
    engine->SetRandomSeed(seed);
}

bool ResizeFinalTarget(Engine* engine, uint32_t w, uint32_t h) {
    if (!engine) {
        return false;
    }
    return engine->ResizeFinalTarget(w, h);
}

uint32_t GetFinalTargetWidth(Engine* engine) {
    if (!engine) {
        return 0;
    }
    return engine->GetFinalTargetWidth();
}

uint32_t GetFinalTargetHeight(Engine* engine) {
    if (!engine) {
        return 0;
    }
    return engine->GetFinalTargetHeight();
}

bool RequestWindowDump(Engine* engine, const std::filesystem::path& path) {
    if (!engine) {
        return false;
    }
    return engine->RequestViewportDump(path);
}

std::vector<SelectionTarget> GetSelection(Engine* engine) {
    if (!engine) {
        return {};
    }
    return engine->Selection();
}

std::vector<SelectionTarget> GetHighlights(Engine* engine) {
    if (!engine) {
        return {};
    }
    return engine->Highlights();
}

uint32_t GetSelectionRevision(Engine* engine) {
    if (!engine) {
        return 0;
    }
    return engine->SelectionRevision();
}

void SetSelection(Engine* engine, std::vector<SelectionTarget>&& targets) {
    if (!engine) {
        return;
    }
    engine->SetSelection(std::move(targets));
}

void AddSelection(Engine* engine, const SelectionTarget& target) {
    if (!engine) {
        return;
    }
    engine->AddSelection(target);
}

void RemoveSelection(Engine* engine, const SelectionTarget& target) {
    if (!engine) {
        return;
    }
    engine->RemoveSelection(target);
}

void ClearSelection(Engine* engine) {
    if (!engine) {
        return;
    }
    engine->ClearSelection();
}

void SetHighlights(Engine* engine, std::vector<SelectionTarget>&& targets) {
    if (!engine) {
        return;
    }
    engine->SetHighlights(std::move(targets));
}

void ClearHighlights(Engine* engine) {
    if (!engine) {
        return;
    }
    engine->ClearHighlights();
}

void RequestPick(Engine* engine, int viewport, uint32_t x, uint32_t y) {
    if (!engine) {
        return;
    }
    engine->RequestPick(viewport, x, y);
}

int OpenViewport(Engine* engine) {
    if (!engine) {
        return -1;
    }
    const uint32_t name = engine->OpenViewport();
    return name == UINT32_MAX ? -1 : static_cast<int>(name);
}

bool CloseViewport(Engine* engine) {
    return engine && engine->CloseViewport();
}

bool SetViewportLayout(Engine* engine, int viewport,
                        float x, float y, float w, float h) {
    return engine && engine->SetViewportLayout(viewport,
                                                glm::vec4(x, y, w, h));
}

bool SetViewportLayoutByName(Engine* engine, uint32_t name_counter,
                              float x, float y, float w, float h) {
    return engine && engine->SetViewportLayoutByName(name_counter,
                                                       glm::vec4(x, y, w, h));
}

int ActiveViewportCount(Engine* engine) {
    return engine ? engine->ActiveViewportCount() : 0;
}

PickResultExport ConsumePickResult(Engine* engine) {
    PickResultExport out;
    if (!engine || !engine->PickResolved()) {
        return out;
    }
    Engine::PickResult r = engine->ConsumePickResult();
    out.resolved = true;
    out.viewport = r.viewport;
    out.x = r.x;
    out.y = r.y;
    out.type = r.type;
    out.id = r.id;
    out.raw = r.raw;
    return out;
}

uint32_t InstantiatePrefab(Engine* engine, uint32_t scene_idx,
                    float x, float y, float z, float scale,
                    float time_phase) {
    if (!engine) {
        return 0;
    }
    glm::mat4 m(1.0f);
    m = glm::translate(m, glm::vec3(x, y, z));
    m = glm::scale(m, glm::vec3(scale));
    return engine->InstantiatePrefab(scene_idx, m, time_phase);
}

uint32_t NumPrefabs(Engine* engine) {
    return engine ? engine->NumPrefabs() : 0;
}

float PrefabExtentMax(Engine* engine, uint32_t scene_idx) {
    return engine ? engine->PrefabExtentMax(scene_idx) : 0.0f;
}

std::vector<uint32_t> ListActiveSceneEntities(Engine* engine) {
    if (!engine) {
        return {};
    }
    return engine->ListActiveSceneEntities();
}

bool SetEntityTransform(Engine* engine, uint32_t entity_int,
                         float x, float y, float z, float scale) {
    if (!engine) {
        return false;
    }
    glm::mat4 m(1.0f);
    m = glm::translate(m, glm::vec3(x, y, z));
    m = glm::scale(m, glm::vec3(scale));
    return engine->SetEntityTransform(entity_int, m);
}

uint32_t ClearActiveScene(Engine* engine) {
    return engine ? engine->ClearActiveScene() : 0;
}

LoadTrace LastLoadTrace(Engine* engine) {
    return engine ? engine->LastLoadTrace() : LoadTrace{};
}

LoaderCounters Counters(Engine* engine) {
    return engine ? engine->Counters() : LoaderCounters{};
}

ValidationReport LastValidationReport(Engine* engine) {
    return engine ? engine->LastValidationReport() : ValidationReport{};
}

LoadBatchExport RuntimeLoadGlbs(Engine* engine,
                                  uint32_t cursor, uint32_t count) {
    LoadBatchExport out{};
    if (!engine) {
        return out;
    }
    std::vector<std::filesystem::path> paths =
        engine->ResolveDebugGlbPaths(cursor, count);
    Engine::LoadPrefabBatchResult r =
        engine->RuntimeLoadBatch(std::span<const std::filesystem::path>(
            paths.data(), paths.size()));
    out.first_prefab_idx = r.first_prefab_idx;
    out.count = r.count;
    return out;
}

std::vector<uint32_t> InstantiateGridFitted(Engine* engine,
                                              uint32_t first_prefab_idx,
                                              uint32_t prefab_count) {
    std::vector<uint32_t> new_entities;
    if (!engine || prefab_count == 0) {
        return new_entities;
    }
    // existing entities + new prefabs together = total grid size.
    std::vector<uint32_t> existing = engine->ListActiveSceneEntities();
    const uint32_t n_total =
        static_cast<uint32_t>(existing.size()) + prefab_count;

    // per-actor extents: existing actors first (we don't have their
    // source prefab handy; treat as fallback 100), then new prefabs.
    std::vector<float> extents;
    extents.reserve(n_total);
    for (size_t i = 0; i < existing.size(); ++i) {
        extents.push_back(0.0f);  // existing: fall back to small default
    }
    std::vector<float> new_extents =
        engine->PrefabExtentSnapshot(first_prefab_idx, prefab_count);
    for (float e : new_extents) {
        extents.push_back(e);
    }

    std::vector<glm::mat4> fitted = engine->FitGridToViewport(
        n_total,
        std::span<const float>(extents.data(), extents.size()));

    // relayout existing actors (the no-flash slide).
    for (size_t i = 0; i < existing.size() && i < fitted.size(); ++i) {
        engine->SetEntityTransform(existing[i], fitted[i]);
    }
    // instantiate the new prefabs at their fitted cells.
    new_entities.reserve(prefab_count);
    for (uint32_t i = 0; i < prefab_count; ++i) {
        const size_t fit_idx = existing.size() + i;
        if (fit_idx >= fitted.size()) {
            break;
        }
        const float time_phase =
            0.137f * static_cast<float>(existing.size() + i);
        const uint32_t eid = engine->InstantiatePrefab(
            first_prefab_idx + i, fitted[fit_idx], time_phase);
        new_entities.push_back(eid);
    }
    return new_entities;
}

}  // namespace cairns::headless
