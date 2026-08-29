// shell/scenario_launcher.hpp -- the imgui scenario picker shared by the native
// SDL app (main.cpp) and the web app (web_main.cpp), so both windowed targets
// draw the SAME UI. Enumerates scripts/*.js next to the app, draws an imgui
// button list, and records the clicked index (pending) for the app to dispatch
// at its safe point (reset + script.eval). App-owned value, no singleton; the
// engine calls Draw() via its raw fn-ptr panel hook. Add a scenario = drop a
// .js in assets/scripts/.
#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"
#include "util/misc.hpp"  // GetBasePathSafe

namespace cairns {

struct ScenarioLauncher {
    struct Script {
        std::string label;
        std::string path;
    };
    std::vector<Script> scripts;
    int pending = -1;  // index clicked this frame; consumed by the app, then -1
    int current = -1;  // last-run index (button highlight)

    void Enumerate() {
        namespace fs = std::filesystem;
        const fs::path dir = fs::path(cairns::GetBasePathSafe()) / "scripts";
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            return;
        }
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() != ".js") {
                continue;
            }
            // Strip a leading "NN_" sort prefix for the button label.
            std::string label = entry.path().stem().string();
            const size_t us = label.find('_');
            if (us != std::string::npos &&
                label.find_first_not_of("0123456789") == us) {
                label = label.substr(us + 1);
            }
            scripts.push_back({label, entry.path().string()});
        }
        // Sort by path so the NN_ prefix orders the list, label stays clean.
        std::sort(scripts.begin(), scripts.end(),
                  [](const Script& a, const Script& b) { return a.path < b.path; });
    }

    // Drawn inside the engine's HUD imgui frame (panel hook).
    void Draw() {
        ImGui::SetNextWindowPos(ImVec2(20.0f, 200.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Scenarios");
        if (scripts.empty()) {
            ImGui::TextUnformatted("(no scripts/*.js found next to the app)");
        }
        for (int i = 0; i < static_cast<int>(scripts.size()); ++i) {
            const bool is_cur = (i == current);
            if (is_cur) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
            }
            if (ImGui::Button(scripts[i].label.c_str(), ImVec2(240.0f, 0.0f))) {
                pending = i;
            }
            if (is_cur) {
                ImGui::PopStyleColor();
            }
        }
        ImGui::End();
    }
};

// Raw fn-ptr trampoline for Engine::SetImguiPanel (no std::function alloc).
inline void DrawScenarioPanel(void* ctx) {
    static_cast<ScenarioLauncher*>(ctx)->Draw();
}

}  // namespace cairns
