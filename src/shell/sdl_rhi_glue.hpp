// src/shell/sdl_rhi_glue.hpp
//
// Backend-neutral SDL <-> rhi::InitConfig glue. Lets main.cpp stay free of
// CAIRNS_METAL / CAIRNS_VULKAN switches: it asks for the backend's SDL window
// flag, populates the rhi::InitConfig, and later releases whatever
// backend-side shell handle (SDL_MetalView etc.) the attach step returned.

#pragma once

#include <SDL3/SDL.h>

namespace cairns::rhi {
struct InitConfig;
}

namespace cairns::shell {

// SDL window-creation flag for the active backend (SDL_WINDOW_METAL or
// SDL_WINDOW_VULKAN). Used by main.cpp at SDL_CreateWindow time.
SDL_WindowFlags BackendWindowFlag();

// Backend-specific window title shown in the title bar.
const char* BackendWindowTitle();

// Fill cfg.plat with the platform handles SDL surfaces for the active
// backend (metal: SDL_MetalView->CA::MetalLayer; vk: instance extensions +
// surface-create callback + window-size getter). Returns an opaque handle
// that must be passed back to DetachWindow() on shutdown. nullptr is a
// valid handle (vk: nothing to release; metal: a non-null SDL_MetalView).
[[nodiscard]] void* AttachWindow(SDL_Window* window, rhi::InitConfig& cfg);

// Inverse of AttachWindow. No-op on vk; SDL_Metal_DestroyView on metal.
void DetachWindow(void* shell_handle);

}  // namespace cairns::shell
