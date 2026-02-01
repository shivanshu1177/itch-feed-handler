#pragma once
#include "timestamp.hpp"
#include <cstdint>
#include <cstdio>
#include <thread>

#ifndef ITCH_LOG_LEVEL
#define ITCH_LOG_LEVEL 2 // INFO
#endif

namespace itch {

enum LogLevel : int { TRACE = 0, DEBUG = 1, INFO = 2, WARN = 3, ERROR = 4 };

inline const char* log_level_name(LogLevel level) {
    switch (level) {
    case TRACE:
        return "TRACE";
    case DEBUG:
        return "DEBUG";
    case INFO:
        return "INFO ";
    case WARN:
        return "WARN ";
    case ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

} // namespace itch

#define ITCH_LOG_CALL(level, fmt, ...)                                                             \
    do {                                                                                           \
        if (level >= ITCH_LOG_LEVEL) {                                                             \
            uint64_t ts = itch::steady_ns();                                                       \
            fprintf(stderr, "[%lu] [%s] " fmt "\n",                                               \
                    (unsigned long)ts,                                                             \
                    itch::log_level_name(level)                                                    \
                    __VA_OPT__(,) __VA_ARGS__);                                                    \
        }                                                                                         \
    } while (0)

#if ITCH_LOG_LEVEL <= 0
#define ITCH_LOG_TRACE(...) ITCH_LOG_CALL(itch::TRACE, __VA_ARGS__)
#else
#define ITCH_LOG_TRACE(...)
#endif

#if ITCH_LOG_LEVEL <= 1
#define ITCH_LOG_DEBUG(...) ITCH_LOG_CALL(itch::DEBUG, __VA_ARGS__)
#else
#define ITCH_LOG_DEBUG(...)
#endif

#if ITCH_LOG_LEVEL <= 2
#define ITCH_LOG_INFO(...) ITCH_LOG_CALL(itch::INFO, __VA_ARGS__)
#else
#define ITCH_LOG_INFO(...)
#endif

#if ITCH_LOG_LEVEL <= 3
#define ITCH_LOG_WARN(...) ITCH_LOG_CALL(itch::WARN, __VA_ARGS__)
#else
#define ITCH_LOG_WARN(...)
#endif

#define ITCH_LOG_ERROR(...) ITCH_LOG_CALL(itch::ERROR, __VA_ARGS__)
