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

// Thin wrapper over OfxImageEffectSuiteV1::clipGetImage that performs the
// random temporal reads for one effect instance. It never derives a time of its
// own: callers pass the exact OfxTime computed by TemporalMapping, and every
// request is logged with the time that was actually asked for.
class TemporalFrameProvider {
 public:
  TemporalFrameProvider(const OfxPropertySuiteV1* propertySuite,
                        const OfxImageEffectSuiteV1* imageEffectSuite,
                        OfxImageClipHandle sourceClip,
                        OfxImageEffectHandle effect,
                        bool debug);

  // Releases the references held for the previous output frame. Call once per
  // render action before the first getFrame.
  void beginOutputFrame();
  OfxStatus getFrame(OfxTime time, const CachedFrame** frame);

  void setDebug(bool debug) { debug_ = debug; }
  void clear();

 private:
  OfxStatus loadFrame(OfxTime time, const CachedFrame** frame);

  const OfxPropertySuiteV1* propertySuite_ = nullptr;
  const OfxImageEffectSuiteV1* imageEffectSuite_ = nullptr;
  OfxImageClipHandle sourceClip_ = nullptr;
  OfxImageEffectHandle effect_ = nullptr;
  bool debug_ = false;
  std::map<OfxTime, std::shared_ptr<CachedFrame>> cache_;
  // Keeps pointers returned for the current output frame alive even when the
  // bounded cache evicts an older timestamp during the same request.
  std::vector<std::shared_ptr<CachedFrame>> activeFrameRefs_;
};

}  // namespace rifeofx
