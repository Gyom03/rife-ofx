#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rifeofx {

// Every diagnostic produced by the plugin goes through this layer so that a
// single DebugView filter on "[RifeOFX]" captures the whole temporal trace.
// Multi-line records are emitted with one OutputDebugStringA call so they stay
// contiguous when Resolve renders several frames on different threads.
void debugLog(const std::string& message);
void debugLogBlock(const std::string& message);

// Mirror of the debug stream into %TEMP%\RifeOFX-temporal.log. DebugView loses
// history quickly during a scrub; the file keeps the whole session.
void appendTemporalLog(const std::string& message);
const std::string& temporalLogPath();

// Diagnostic-only sampled signature, deliberately not a full hash: it must stay
// cheap enough to run on every fetched UHD frame. Its only job is to answer
// "did the host hand us two different images?".
std::uint64_t sampledSignature(const float* values, std::size_t count);
std::string toHex(std::uint64_t value);

}  // namespace rifeofx
