// shell/scenario_launcher.hpp -- the imgui scenario picker shared by the native
// and web builds of the SDL shell (one main.cpp), so both windowed targets
// draw the SAME UI. Enumerates scripts/*.js next to the app, draws an imgui
// button list, and records the clicked index (pending) for the app to dispatch
// at its safe point (reset + script.eval). App-owned value, no singleton; the
// engine calls Draw() via its raw fn-ptr panel hook. Add a scenario = drop a
// .js in assets/scripts/.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "imgui.h"
#include "util/misc.hpp"  // GetBasePathSafe
// SDL_EnumerateDirectory routes to the Android AssetManager (posix SDL_sysfsops
// falls back to AAssetDir), plain dirs on desktop, and MEMFS on the web build
// (posix opendir) -- one path enumerates APK assets, desktop, and the browser's
// preloaded FS alike; std::filesystem can't see APK assets.
#include <SDL3/SDL_filesystem.h>

namespace cairns {

struct ScenarioLauncher {
    struct Script {
        std::string label;
        std::string path;
    };
    std::vector<Script> scripts;
    int pending = -1;  // index clicked this frame; consumed by the app, then -1
    int current = -1;  // last-run index (button highlight)

    // Strip a leading "NN_" sort prefix for the button label; keep the full
    // (asset-relative on Android) path so the app can ReadAsset it.
    void AddOne(const std::string& stem, const std::string& path) {
        std::string label = stem;
        const size_t us = label.find('_');
        if (us != std::string::npos &&
            label.find_first_not_of("0123456789") == us) {
            label = label.substr(us + 1);
        }
        scripts.push_back({label, path});
    }

    static SDL_EnumerationResult SDLCALL EnumCb(void* userdata,
                                                const char* dirname,
                                                const char* fname) {
        const std::string name = fname ? fname : "";
        if (name.size() > 3 && name.compare(name.size() - 3, 3, ".js") == 0) {
            std::string dir = dirname ? dirname : "";
            if (!dir.empty() && dir.back() != '/') {
                dir.push_back('/');
            }
            static_cast<ScenarioLauncher*>(userdata)->AddOne(
                name.substr(0, name.size() - 3), dir + name);
        }
        return SDL_ENUM_CONTINUE;
    }

    void Enumerate() {
        scripts.clear();
        // SDL_EnumerateDirectory finds APK assets via the AssetManager (Android),
        // plain dirs on desktop, and the preloaded FS on web. (SDL strips any
        // trailing '/' internally, so the callback dirname may or may not carry
        // one -- EnumCb normalizes it.)
        const std::string dir = cairns::GetBasePathSafe() + "scripts";
        SDL_EnumerateDirectory(dir.c_str(), &EnumCb, this);
        // Sort by path so the NN_ prefix orders the list, label stays clean.
        std::sort(scripts.begin(), scripts.end(),
                  [](const Script& a, const Script& b) { return a.path < b.path; });
    }

    // Drawn inside the engine's HUD imgui frame (panel hook).
    void Draw() {
        // Top-right, clear of the HUD (top-left). Pivot (1,0) anchors the
        // window's top-right corner so it never clips regardless of window size.
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(disp.x - 16.0f, 16.0f),
                                ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
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
