#include "DebugLog.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace rifeofx {

namespace {

std::string resolveTemporalLogPath() {
  wchar_t tempPath[MAX_PATH] = {};
  const DWORD length = GetTempPathW(MAX_PATH, tempPath);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  const std::filesystem::path path =
      std::filesystem::path(tempPath) / L"RifeOFX-temporal.log";
  return path.string();
}

std::mutex& logFileMutex() {
  static std::mutex mutex;
  return mutex;
}

}  // namespace

void debugLog(const std::string& message) {
  OutputDebugStringA(("[RifeOFX] " + message + "\n").c_str());
}

void debugLogBlock(const std::string& message) {
  // The caller already formatted the "[RifeOFX]" header and the line breaks.
  OutputDebugStringA((message + "\n").c_str());
}

const std::string& temporalLogPath() {
  static const std::string path = resolveTemporalLogPath();
  return path;
}

void appendTemporalLog(const std::string& message) {
  const std::string& path = temporalLogPath();
  if (path.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(logFileMutex());
  std::ofstream stream(path, std::ios::out | std::ios::app);
  if (stream) {
    stream << message << '\n';
  }
}

std::uint64_t sampledSignature(const float* values, std::size_t count) {
  constexpr std::uint64_t kOffset = 1469598103934665603ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  constexpr std::size_t kMaximumSamples = 4096;
  static_assert(sizeof(std::uint32_t) == sizeof(float),
                "float signature requires 32-bit floats");

  std::uint64_t hash = kOffset;
  if (!values || count == 0) {
    return hash;
  }
  const std::size_t stride = std::max<std::size_t>(1, count / kMaximumSamples);
  for (std::size_t index = 0; index < count; index += stride) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &values[index], sizeof(bits));
    hash ^= bits;
    hash *= kPrime;
  }
  return hash;
}

std::string toHex(std::uint64_t value) {
  static const char kDigits[] = "0123456789abcdef";
  std::string hex(16, '0');
  for (int index = 15; index >= 0; --index) {
    hex[static_cast<std::size_t>(index)] = kDigits[value & 0xFULL];
    value >>= 4;
  }
  return "0x" + hex;
}

}  // namespace rifeofx
