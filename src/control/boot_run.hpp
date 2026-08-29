#pragma once

namespace cairns::control {

class CommandRegistry;

// Loads "run.js" from the bundled asset dir (resolved via SDL_GetBasePath
// on Apple, AAssetManager on Android, build/<config>/ on desktop dev),
// then dispatches it through cairns.script.eval. The JS body uses
// `cairns.dispatch(op, args)` to call registered ops.
//
// run.js is MANDATORY on every platform. If it isn't bundled or fails
// to eval, std::abort() -- a broken asset bundle is a build-system bug,
// not something to silently boot through.
void RunBootScript(CommandRegistry& registry);

}  // namespace cairns::control
