// #224 L3: loading-system instrumentation PODs.
//
// Populated by Engine::LoadPrefabBatch (per-stage timings + counts) and
// surfaced via NDJSON cairns.loader.{trace,counters}. The whole point
// of P0 loading is the instrument: you cannot debug or measure an
// engine you can't deterministically get content into. Timer values
// are wall-clock chrono::steady, so they vary run-to-run -- byte-gate
// goldens never reference them, and the L7 determinism check asserts
// identical counts and transforms but NOT identical trace timings.

#pragma once

#include <cstdint>

namespace cairns {

struct LoadStage {
    const char* name = "";
    double      ms = 0.0;
    uint64_t    bytes = 0;
    uint32_t    count = 0;
};

struct LoadTrace {
    static constexpr uint32_t kMaxStages = 16;
    LoadStage stages[kMaxStages] = {};
    uint8_t   stage_count = 0;
    double    total_ms = 0.0;
    uint64_t  bytes_uploaded = 0;
    uint32_t  prefabs_added = 0;
    uint32_t  meshes_added = 0;
    uint32_t  actors_instantiated = 0;

    void Add(const char* name, double ms, uint64_t bytes = 0,
              uint32_t count = 0) {
        if (stage_count >= kMaxStages) {
            return;
        }
        LoadStage& s = stages[stage_count++];
        s.name = name;
        s.ms = ms;
        s.bytes = bytes;
        s.count = count;
    }
};

enum class ValidationSeverity : uint8_t { kWarning = 0, kError = 1 };

struct ValidationIssue {
    ValidationSeverity sev = ValidationSeverity::kError;
    const char* what = "";
    uint32_t prefab_idx = UINT32_MAX;
};

struct ValidationReport {
    static constexpr uint32_t kMaxIssues = 32;
    bool ok = true;
    ValidationIssue issues[kMaxIssues] = {};
    uint8_t issue_count = 0;

    void Add(ValidationSeverity sev, const char* what,
              uint32_t prefab_idx = UINT32_MAX) {
        if (issue_count >= kMaxIssues) {
            return;
        }
        if (sev == ValidationSeverity::kError) {
            ok = false;
        }
        ValidationIssue& i = issues[issue_count++];
        i.sev = sev;
        i.what = what;
        i.prefab_idx = prefab_idx;
    }
};

struct LoaderCounters {
    uint64_t bytes_resident = 0;       // sum of all resident vertex/index/skin buffers
    uint32_t prefabs_resident = 0;
    uint32_t meshes_resident = 0;
    uint32_t textures_resident = 0;
    uint32_t actors_live = 0;
    uint64_t bytes_uploaded_total = 0;
    uint32_t batches_loaded = 0;
    double   last_batch_ms = 0.0;
    double   peak_batch_ms = 0.0;
};

}  // namespace cairns
