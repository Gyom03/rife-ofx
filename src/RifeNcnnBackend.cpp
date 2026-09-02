#include "RifeNcnnBackend.h"

#include "rife.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <sstream>

#include "gpu.h"
#include "mat.h"

namespace rifeofx {

namespace {

void debugMessage(const std::string& message) {
  OutputDebugStringA(("[RifeOFX] " + message + "\n").c_str());
}

}  // namespace

RifeNcnnBackend::RifeNcnnBackend() = default;

RifeNcnnBackend::~RifeNcnnBackend() {
  unloadModel();
}

void RifeNcnnBackend::debugLog(const std::string& message) const {
  debugMessage(message);
}

unsigned char RifeNcnnBackend::toByte(float value) {
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  return static_cast<unsigned char>(std::lround(clamped * 255.0f));
}

int RifeNcnnBackend::paddedDimension(int value, int alignment) {
  if (alignment <= 1) {
    return value;
  }
  return ((value + alignment - 1) / alignment) * alignment;
}

OfxStatus RifeNcnnBackend::loadModel(const RifeModelDescriptor& descriptor,
                                     int gpuId) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Called only after the engine has serialized model switches.
  rife_.reset();
  initialized_ = false;

  if (descriptor.backend != "rife-ncnn") {
    debugLog("model requests unsupported backend: " + descriptor.backend);
    return kOfxStatErrMissingHostFeature;
  }
  // The multi-model UI is intentionally ahead of the validated adapters.
  // Keep unvalidated variants explicit until their exact NCNN weights and
  // padding/model behavior have been tested independently.
  if (descriptor.id != "rife-v4.6" && descriptor.id != "rife-v4.25" &&
      descriptor.id != "rife-v4.25-lite" &&
      descriptor.id != "rife-v4.22-lite" &&
      descriptor.id != "rife-v4.26" &&
      descriptor.id != "rife-v4.26-large") {
    debugLog("model adapter not validated yet: " + descriptor.id);
    return kOfxStatErrMissingHostFeature;
  }
  if (!descriptor.filesPresent()) {
    debugLog("RIFE model files not found for " + descriptor.id +
             " at " + descriptor.modelPath.string());
    return kOfxStatFailed;
  }

  const int gpuCount = ncnn::get_gpu_count();
  if (gpuCount <= 0 || gpuId < 0 || gpuId >= gpuCount) {
    std::ostringstream stream;
    stream << "No usable Vulkan GPU for RIFE: requested=" << gpuId
           << " available=" << gpuCount;
    debugLog(stream.str());
    return kOfxStatErrMissingHostFeature;
  }

  auto candidate = std::make_unique<RIFE>(gpuId, false, false, false, 1, false, true);
  const int status = candidate->load(descriptor.modelPath.wstring());
  if (status != 0) {
    debugLog("RIFE model load failed for " + descriptor.id);
    return kOfxStatFailed;
  }

  rife_ = std::move(candidate);
  descriptor_ = descriptor;
  gpuId_ = gpuId;
  initialized_ = true;
  debugLog("RIFE model loaded: " + descriptor.id + " with Vulkan");
  return kOfxStatOK;
}

void RifeNcnnBackend::unloadModel() {
  std::lock_guard<std::mutex> lock(mutex_);
  rife_.reset();
  initialized_ = false;
  descriptor_ = {};
  gpuId_ = -1;
  inputA_.clear();
  inputB_.clear();
  outputBGR_.clear();
}

OfxStatus RifeNcnnBackend::convertInput(
    const CachedFrame& frame,
    int paddedWidth,
    int paddedHeight,
    std::vector<unsigned char>& output) const {
  const int width = frame.bounds.x2 - frame.bounds.x1;
  const int height = frame.bounds.y2 - frame.bounds.y1;
  if (width <= 0 || height <= 0 || paddedWidth < width ||
      paddedHeight < height ||
      frame.rgba.size() != static_cast<size_t>(width) * height * 4) {
    return kOfxStatErrImageFormat;
  }

  output.resize(static_cast<size_t>(paddedWidth) * paddedHeight * 3);
  for (int y = 0; y < paddedHeight; ++y) {
    const int sourceY = std::min(y, height - 1);
    for (int x = 0; x < paddedWidth; ++x) {
      const int sourceX = std::min(x, width - 1);
      const size_t rgbaIndex =
          (static_cast<size_t>(sourceY) * width + sourceX) * 4;
      const size_t bgrIndex =
          (static_cast<size_t>(y) * paddedWidth + x) * 3;
      output[bgrIndex + 0] = toByte(frame.rgba[rgbaIndex + 2]);
      output[bgrIndex + 1] = toByte(frame.rgba[rgbaIndex + 1]);
      output[bgrIndex + 2] = toByte(frame.rgba[rgbaIndex + 0]);
    }
  }
  return kOfxStatOK;
}

OfxStatus RifeNcnnBackend::interpolate(const CachedFrame& frameA,
                                       const CachedFrame& frameB,
                                       float timestep,
                                       std::vector<float>& outputRGBA,
                                       InferenceDiagnostics* diagnostics) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_ || !rife_) {
    return kOfxStatFailed;
  }
  if (frameA.bounds.x1 != frameB.bounds.x1 || frameA.bounds.y1 != frameB.bounds.y1 ||
      frameA.bounds.x2 != frameB.bounds.x2 || frameA.bounds.y2 != frameB.bounds.y2) {
    return kOfxStatErrImageFormat;
  }

  const int width = frameA.bounds.x2 - frameA.bounds.x1;
  const int height = frameA.bounds.y2 - frameA.bounds.y1;
  if (width <= 0 || height <= 0 || timestep <= 0.0f || timestep >= 1.0f) {
    return kOfxStatErrImageFormat;
  }

  const int paddedWidth = paddedDimension(width, descriptor_.alignment);
  const int paddedHeight = paddedDimension(height, descriptor_.alignment);

  OfxStatus status = convertInput(frameA, paddedWidth, paddedHeight, inputA_);
  if (status != kOfxStatOK) {
    return status;
  }
  status = convertInput(frameB, paddedWidth, paddedHeight, inputB_);
  if (status != kOfxStatOK) {
    return status;
  }

  outputBGR_.resize(static_cast<size_t>(paddedWidth) * paddedHeight * 3);
  ncnn::Mat inputAMat(paddedWidth, paddedHeight, inputA_.data(), 3u, 1);
  ncnn::Mat inputBMat(paddedWidth, paddedHeight, inputB_.data(), 3u, 1);
  ncnn::Mat outputMat(paddedWidth, paddedHeight, outputBGR_.data(), 3u, 1);

  const auto start = std::chrono::steady_clock::now();
  const int rifeStatus = rife_->process(inputAMat, inputBMat, timestep, outputMat);
  const auto end = std::chrono::steady_clock::now();
  if (rifeStatus != 0) {
    debugLog("RIFE inference failed for " + descriptor_.id);
    return kOfxStatFailed;
  }

  outputRGBA.resize(static_cast<size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t bgrIndex =
          (static_cast<size_t>(y) * paddedWidth + x) * 3;
      const size_t rgbaIndex = (static_cast<size_t>(y) * width + x) * 4;
      outputRGBA[rgbaIndex + 0] = outputBGR_[bgrIndex + 2] / 255.0f;
      outputRGBA[rgbaIndex + 1] = outputBGR_[bgrIndex + 1] / 255.0f;
      outputRGBA[rgbaIndex + 2] = outputBGR_[bgrIndex + 0] / 255.0f;
      outputRGBA[rgbaIndex + 3] = frameA.rgba[rgbaIndex + 3] * (1.0f - timestep) +
                                  frameB.rgba[rgbaIndex + 3] * timestep;
    }
  }

  if (diagnostics) {
    diagnostics->modelId = descriptor_.id;
    diagnostics->backend = backendName();
    diagnostics->gpuId = gpuId_;
    diagnostics->inputWidth = width;
    diagnostics->inputHeight = height;
    diagnostics->paddedWidth = paddedWidth;
    diagnostics->paddedHeight = paddedHeight;
    diagnostics->timestep = timestep;
    diagnostics->inferenceMilliseconds =
        std::chrono::duration<double, std::milli>(end - start).count();
    diagnostics->estimatedGpuMemoryMB = descriptor_.estimatedGpuMemoryMB;
  }
  return kOfxStatOK;
}

}  // namespace rifeofx
