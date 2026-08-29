#pragma once

#include "util/define.hpp"

#if CAIRNS_ANDROID
#include <android/log.h>
#define CAIRNS_PRINT(...) __android_log_print(ANDROID_LOG_INFO, "cairns", __VA_ARGS__)
#else // CAIRNS_ANDROID
#include <cstdio>
#define CAIRNS_PRINT(...) std::printf(__VA_ARGS__)
#endif // CAIRNS_ANDROID
