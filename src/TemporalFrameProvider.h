#pragma once

#include <ofxCore.h>
#include <ofxImageEffect.h>

#include <cstdint>
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

// The host renders the same time at different render scales: a proxy or
// thumbnail pass and then the full resolution one. Those return images of
// different sizes, so the scale is part of a frame's identity. Keying the cache
// on time alone hands a proxy-sized image back to a full resolution render.
struct FrameKey {
  OfxTime time = 0.0;
  double renderScaleX = 1.0;
  double renderScaleY = 1.0;

  bool operator<(const FrameKey& other) const {
    if (time != other.time) return time < other.time;
    if (renderScaleX != other.renderScaleX) return renderScaleX < other.renderScaleX;
    return renderScaleY < other.renderScaleY;
  }
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

  // Releases the references held for the previous output frame, and records the
  // render scale the host asked for. Call once per render action before the
  // first getFrame.
  void beginOutputFrame(double renderScaleX, double renderScaleY);
  OfxStatus getFrame(OfxTime time, const CachedFrame** frame);

  void setDebug(bool debug) { debug_ = debug; }
  void clear();

 private:
  OfxStatus loadFrame(OfxTime time, const CachedFrame** frame);
  // Shared image-property validation and row copy used by both fetch paths.
  OfxStatus readImage(OfxTime time, CachedFrame* frame, bool logOnSuccess);

  const OfxPropertySuiteV1* propertySuite_ = nullptr;
  const OfxImageEffectSuiteV1* imageEffectSuite_ = nullptr;
  OfxImageClipHandle sourceClip_ = nullptr;
  OfxImageEffectHandle effect_ = nullptr;
  bool debug_ = false;
  double renderScaleX_ = 1.0;
  double renderScaleY_ = 1.0;
  std::map<FrameKey, std::shared_ptr<CachedFrame>> cache_;
  // Keeps pointers returned for the current output frame alive even when the
  // bounded cache evicts an older timestamp during the same request.
  std::vector<std::shared_ptr<CachedFrame>> activeFrameRefs_;
};

}  // namespace rifeofx
