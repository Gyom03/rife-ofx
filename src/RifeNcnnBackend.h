#pragma once

#include "IRifeBackend.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

class RIFE;

namespace rifeofx {

class RifeNcnnBackend final : public IRifeBackend {
 public:
  RifeNcnnBackend();
  ~RifeNcnnBackend() override;

  OfxStatus loadModel(const RifeModelDescriptor& descriptor,
                      int gpuId) override;
  void unloadModel() override;
  bool loaded() const override { return initialized_; }
  std::string backendName() const override { return "NCNN/Vulkan"; }

  OfxStatus interpolate(const CachedFrame& frameA,
                        const CachedFrame& frameB,
                        float timestep,
                        std::vector<float>& outputRGBA,
                        InferenceDiagnostics* diagnostics) override;

 private:
  static unsigned char toByte(float value);
  static int paddedDimension(int value, int alignment);
  OfxStatus convertInput(const CachedFrame& frame,
                         int paddedWidth,
                         int paddedHeight,
                         std::vector<unsigned char>& output) const;
  void debugLog(const std::string& message) const;

  int gpuId_ = -1;
  RifeModelDescriptor descriptor_;
  std::unique_ptr<RIFE> rife_;
  bool initialized_ = false;
  mutable std::mutex mutex_;
  std::vector<unsigned char> inputA_;
  std::vector<unsigned char> inputB_;
  std::vector<unsigned char> outputBGR_;
};

}  // namespace rifeofx
