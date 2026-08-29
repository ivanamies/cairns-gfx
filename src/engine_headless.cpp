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

}  // namespace cairns::headless
