#pragma once

#include "IRifeBackend.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rifeofx {

class RifeEngine {
 public:
  RifeEngine(int gpuId,
             std::filesystem::path modelsRoot,
             std::filesystem::path manifestPath);
  ~RifeEngine();

  OfxStatus loadModel(const std::string& modelId);
  void unloadModel();

  OfxStatus interpolate(const CachedFrame& frameA,
                        const CachedFrame& frameB,
                        float timestep,
                        std::vector<float>& outputRGBA,
                        InferenceDiagnostics* diagnostics = nullptr);

  bool initialized() const;
  const std::string& activeModelId() const { return activeModelId_; }
  const ModelRegistry& registry() const { return registry_; }

 private:
  int gpuId_ = 0;
  ModelRegistry registry_;
  std::unique_ptr<IRifeBackend> backend_;
  std::string activeModelId_;
  mutable std::mutex mutex_;
};

}  // namespace rifeofx
