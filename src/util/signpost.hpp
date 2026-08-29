// util/signpost.hpp
//
// Xcode Instruments os_signpost wrapper. Drop-in for Time Profiler +
// (per Apple's API surface) the Memory profiler's "Points of Interest"
// instrument. On non-Apple platforms the macros expand to nothing.
//
// Usage:
//   {
//     CAIRNS_SIGNPOST_INTERVAL_SCOPED("load_prefab", "aatrox.glb");
//     ... heavy work ...
//   }
//
// Or for one-shot points:
//   CAIRNS_SIGNPOST_EVENT("reload_done", "%s %ums", path, ms);
//
// In Instruments: choose "Time Profiler" -> "Points of Interest" lane.
// Each named signpost shows as a flag (event) or band (interval); the
// 2nd arg is a fmt-string that becomes the signpost's descriptive
// payload on hover.

#pragma once

#include "util/define.hpp"

#if CAIRNS_APPLE

#include <os/signpost.h>

namespace cairns {

// One shared signpost log "cairns.gfx" / category "cairns" so the
// instrument lane groups them under a single subsystem. Not a static
// (per the engine's no-statics rule) -- the os_log_t is a process-
// wide singleton owned by libsystem; we just hand the handle out.
inline os_log_t SignpostLog() {
    return os_log_create("cairns.gfx", "cairns");
}

class SignpostInterval {
public:
    SignpostInterval(const char* name, const char* arg)
        : log_(SignpostLog()),
          id_(os_signpost_id_generate(log_)),
          name_(name) {
        os_signpost_interval_begin(log_, id_, "interval",
                                   "%{public}s | %{public}s", name, arg);
    }
    ~SignpostInterval() {
        os_signpost_interval_end(log_, id_, "interval", "%{public}s",
                                 name_);
    }
    SignpostInterval(const SignpostInterval&) = delete;
    SignpostInterval& operator=(const SignpostInterval&) = delete;

private:
    os_log_t log_;
    os_signpost_id_t id_;
    const char* name_;
};

inline void SignpostEvent(const char* name, const char* msg) {
    os_signpost_event_emit(SignpostLog(), OS_SIGNPOST_ID_EXCLUSIVE,
                           "event", "%{public}s | %{public}s", name, msg);
}

}  // namespace cairns

#define CAIRNS_SIGNPOST_INTERVAL_SCOPED(NAME, ARG) \
    ::cairns::SignpostInterval _cairns_sp_##__LINE__((NAME), (ARG))

#define CAIRNS_SIGNPOST_EVENT(NAME, ARG) \
    ::cairns::SignpostEvent((NAME), (ARG))

#else  // non-Apple

#define CAIRNS_SIGNPOST_INTERVAL_SCOPED(NAME, ARG) ((void)0)
#define CAIRNS_SIGNPOST_EVENT(NAME, ARG)           ((void)0)

#endif  // CAIRNS_APPLE
