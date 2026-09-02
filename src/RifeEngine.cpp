#include "RifeEngine.h"

#include "RifeNcnnBackend.h"

#include <windows.h>

namespace rifeofx {

namespace {

void debugLog(const std::string& message) {
  OutputDebugStringA(("[RifeOFX] " + message + "\n").c_str());
}

}  // namespace

RifeEngine::RifeEngine(int gpuId,
                       std::filesystem::path modelsRoot,
                       std::filesystem::path manifestPath)
    : gpuId_(gpuId), registry_(std::move(modelsRoot)) {
  std::string error;
  if (!registry_.loadManifest(manifestPath, &error)) {
    debugLog(error);
  }
  backend_ = std::make_unique<RifeNcnnBackend>();
}

RifeEngine::~RifeEngine() {
  unloadModel();
}

bool RifeEngine::initialized() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return backend_ && backend_->loaded();
}

OfxStatus RifeEngine::loadModel(const std::string& modelId) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!backend_) {
    backend_ = std::make_unique<RifeNcnnBackend>();
  }
  if (backend_->loaded() && activeModelId_ == modelId) {
    return kOfxStatOK;
  }

  const RifeModelDescriptor* descriptor = registry_.find(modelId);
  if (!descriptor) {
    debugLog("unknown RIFE model id: " + modelId);
    return kOfxStatErrValue;
  }
  if (descriptor->backend != "rife-ncnn") {
    debugLog("no NCNN backend registered for model: " + modelId);
    return kOfxStatErrMissingHostFeature;
  }

  backend_->unloadModel();
  activeModelId_.clear();
  const OfxStatus status = backend_->loadModel(*descriptor, gpuId_);
  if (status == kOfxStatOK) {
    activeModelId_ = modelId;
  }
  return status;
}

void RifeEngine::unloadModel() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backend_) {
    backend_->unloadModel();
  }
  activeModelId_.clear();
}

OfxStatus RifeEngine::interpolate(const CachedFrame& frameA,
                                  const CachedFrame& frameB,
                                  float timestep,
                                  std::vector<float>& outputRGBA,
                                  InferenceDiagnostics* diagnostics) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!backend_ || !backend_->loaded()) {
    debugLog("interpolate requested before a RIFE model was loaded");
    return kOfxStatFailed;
  }
  return backend_->interpolate(frameA, frameB, timestep, outputRGBA, diagnostics);
}

}  // namespace rifeofx
