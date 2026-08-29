#pragma once

#include "util/define.hpp"

#if CAIRNS_APPLE
#include <TargetConditionals.h>
#endif

#if CAIRNS_ANDROID
#include <android/log.h>
#define CAIRNS_PRINT(...) __android_log_print(ANDROID_LOG_INFO, "cairns", __VA_ARGS__)
#elif CAIRNS_APPLE && TARGET_OS_IPHONE
#include <SDL3/SDL_log.h>
#define CAIRNS_PRINT(...) SDL_Log(__VA_ARGS__)
#else
#include <cstdio>
// Desktop path writes to stderr (not stdout). stdout is reserved as the
// NDJSON control-plane channel for cairns_serve -- any stray write to
// stdout corrupts the JSON stream. Match the existing fprintf(stderr) callers
// ([FLAKE], [FCLK], [RG]) so logs go through a single sink.
#define CAIRNS_PRINT(...) std::fprintf(stderr, __VA_ARGS__)
#endif
