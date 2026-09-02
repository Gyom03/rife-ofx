#pragma once

#include <ofxCore.h>
#include <ofxImageEffect.h>

#include <map>
#include <memory>
#include <vector>

namespace rifeofx {

struct CachedFrame {
  OfxTime time = 0.0;
  OfxRectI bounds{};
  int rowBytes = 0;
  std::vector<float> rgba;
};

class TemporalFrameProvider {
 public:
  TemporalFrameProvider(const OfxPropertySuiteV1* propertySuite,
                        const OfxImageEffectSuiteV1* imageEffectSuite,
                        OfxImageClipHandle sourceClip,
                        OfxImageEffectHandle effect,
                        bool debug);

  OfxStatus getFrame(OfxTime time, const CachedFrame** frame);
  OfxStatus getFrameOffset(OfxTime time, int offset, const CachedFrame** frame);
  OfxStatus getTemporalWindow(OfxTime time, int before, int after,
                              std::vector<const CachedFrame*>& frames);

  void setDebug(bool debug) { debug_ = debug; }
  void setFrameRange(OfxTime firstFrame, OfxTime lastFrame);
  void clear();

 private:
  OfxStatus loadFrame(OfxTime time, const CachedFrame** frame);
  void debugLog(const char* operation, OfxTime time, OfxStatus status) const;

  const OfxPropertySuiteV1* propertySuite_ = nullptr;
  const OfxImageEffectSuiteV1* imageEffectSuite_ = nullptr;
  OfxImageClipHandle sourceClip_ = nullptr;
  OfxImageEffectHandle effect_ = nullptr;
  bool debug_ = false;
  bool frameRangeAvailable_ = false;
  OfxTime firstFrame_ = 0.0;
  OfxTime lastFrame_ = 0.0;
  std::map<OfxTime, std::shared_ptr<CachedFrame>> cache_;
  // Keeps pointers returned for the current temporal window alive even when
  // the bounded cache evicts an older timestamp during the same request.
  std::vector<std::shared_ptr<CachedFrame>> activeFrameRefs_;
};

}  // namespace rifeofx
