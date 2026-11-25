#pragma once
/*
 * The two things every source file in this plugin wants: the log prefix and
 * a monotonic clock.
 */

#include <chrono>
#include <cstdint>

namespace tmc {

constexpr const char* LOGPFX = "StepperMotion";

// The plugin's directory name, which is also its repoName. Load-bearing (see
// TmcPlugin.cpp): the plugin object's name, the channel output's type in
// co-other.json, the plugindata folder, and the owner tag on its HTTP
// requests. The only copy in the C++; the page and scripts take it from the
// directory they run from.
constexpr const char* PLUGIN_NAME = "fpp-StepperMotion";

inline int64_t monotonicMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline double monotonicSeconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

} // namespace tmc
