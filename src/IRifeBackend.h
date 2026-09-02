#pragma once

#include "ModelRegistry.h"
#include "TemporalFrameProvider.h"

#include <string>
#include <vector>

namespace rifeofx {

struct InferenceDiagnostics {
  std::string modelId;
  std::string backend;
  int gpuId = -1;
  int inputWidth = 0;
  int inputHeight = 0;
  int paddedWidth = 0;
  int paddedHeight = 0;
  float timestep = 0.0f;
  double inferenceMilliseconds = 0.0;
  std::size_t estimatedGpuMemoryMB = 0;
};

class IRifeBackend {
 public:
  virtual ~IRifeBackend() = default;

  virtual OfxStatus loadModel(const RifeModelDescriptor& descriptor,
                              int gpuId) = 0;
  virtual void unloadModel() = 0;
  virtual bool loaded() const = 0;
  virtual std::string backendName() const = 0;
  virtual OfxStatus interpolate(const CachedFrame& frameA,
                                const CachedFrame& frameB,
                                float timestep,
                                std::vector<float>& outputRGBA,
                                InferenceDiagnostics* diagnostics) = 0;
};

}  // namespace rifeofx
