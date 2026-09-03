#include "TemporalFrameProvider.h"

#include "DebugLog.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>

namespace rifeofx {

namespace {

constexpr int kRGBAChannels = 4;
constexpr size_t kMaxCachedFrames = 16;

}  // namespace

TemporalFrameProvider::TemporalFrameProvider(
    const OfxPropertySuiteV1* propertySuite,
    const OfxImageEffectSuiteV1* imageEffectSuite,
    OfxImageClipHandle sourceClip,
    OfxImageEffectHandle effect,
    bool debug)
    : propertySuite_(propertySuite),
      imageEffectSuite_(imageEffectSuite),
      sourceClip_(sourceClip),
      effect_(effect),
      debug_(debug) {}

void TemporalFrameProvider::beginOutputFrame() { activeFrameRefs_.clear(); }

OfxStatus TemporalFrameProvider::getFrame(OfxTime time,
                                           const CachedFrame** frame) {
  if (!frame || !propertySuite_ || !imageEffectSuite_ || !sourceClip_) {
    return kOfxStatErrBadHandle;
  }

  // The time is the one TemporalMapping computed. It is passed to the host
  // untouched: no clamping to the clip's reported media-local frame range,
  // because Resolve may use a different origin for the effect instance.
  auto cached = cache_.find(time);
  if (cached != cache_.end()) {
    activeFrameRefs_.push_back(cached->second);
    *frame = cached->second.get();
    if (debug_) {
      std::ostringstream stream;
      stream << std::fixed << std::setprecision(6)
             << "clipGetImage requestedTime=" << time << " source=cache";
      debugLog(stream.str());
    }
    return kOfxStatOK;
  }
  return loadFrame(time, frame);
}

OfxStatus TemporalFrameProvider::loadFrame(OfxTime time,
                                            const CachedFrame** frame) {
  OfxPropertySetHandle image = nullptr;
  const OfxStatus status =
      imageEffectSuite_->clipGetImage(sourceClip_, time, nullptr, &image);
  if (status != kOfxStatOK || !image) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6)
           << "clipGetImage requestedTime=" << time << " status=" << status
           << " image=null";
    debugLog(stream.str());
    appendTemporalLog(stream.str());
    return status == kOfxStatOK ? kOfxStatFailed : status;
  }

  void* imageData = nullptr;
  int rowBytes = 0;
  OfxRectI bounds{};
  char* components = nullptr;
  char* depth = nullptr;
  const bool valid =
      propertySuite_->propGetPointer(image, kOfxImagePropData, 0, &imageData) == kOfxStatOK &&
      propertySuite_->propGetInt(image, kOfxImagePropRowBytes, 0, &rowBytes) == kOfxStatOK &&
      propertySuite_->propGetIntN(image, kOfxImagePropBounds, 4, &bounds.x1) == kOfxStatOK &&
      propertySuite_->propGetString(image, kOfxImageEffectPropComponents, 0,
                                    &components) == kOfxStatOK &&
      propertySuite_->propGetString(image, kOfxImageEffectPropPixelDepth, 0,
                                    &depth) == kOfxStatOK;

  const bool expectedFormat = valid && imageData && components && depth &&
                              std::strcmp(components, kOfxImageComponentRGBA) == 0 &&
                              std::strcmp(depth, kOfxBitDepthFloat) == 0 &&
                              bounds.x2 > bounds.x1 && bounds.y2 > bounds.y1;
  if (!expectedFormat) {
    imageEffectSuite_->clipReleaseImage(image);
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6)
           << "clipGetImage requestedTime=" << time
           << " rejected: components=" << (components ? components : "null")
           << " depth=" << (depth ? depth : "null");
    debugLog(stream.str());
    appendTemporalLog(stream.str());
    return kOfxStatErrImageFormat;
  }

  if (debug_) {
    // The host does not report which frame it decided to hand back, so the
    // requested time and the geometry are logged together with the caller's
    // signature to make a conformed repeat visible.
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6)
           << "clipGetImage requestedTime=" << time << " status=" << status
           << " bounds=[" << bounds.x1 << "," << bounds.y1 << "," << bounds.x2
           << "," << bounds.y2 << "] rowBytes=" << rowBytes;
    debugLog(stream.str());
    appendTemporalLog(stream.str());
  }

  CachedFrame loaded;
  loaded.time = time;
  loaded.bounds = bounds;
  loaded.rowBytes = (bounds.x2 - bounds.x1) * kRGBAChannels * static_cast<int>(sizeof(float));
  loaded.rgba.resize(static_cast<size_t>(bounds.y2 - bounds.y1) *
                     static_cast<size_t>(bounds.x2 - bounds.x1) * kRGBAChannels);

  const int width = bounds.x2 - bounds.x1;
  const size_t copyBytes = static_cast<size_t>(loaded.rowBytes);
  for (int y = bounds.y1; y < bounds.y2; ++y) {
    const auto* sourceRow = static_cast<const unsigned char*>(imageData) +
                            (y - bounds.y1) * rowBytes;
    auto* destinationRow = loaded.rgba.data() +
                           static_cast<size_t>(y - bounds.y1) * width * kRGBAChannels;
    std::memcpy(destinationRow, sourceRow, copyBytes);
  }

  imageEffectSuite_->clipReleaseImage(image);
  auto inserted = cache_.emplace(
      time, std::make_shared<CachedFrame>(std::move(loaded)));
  while (cache_.size() > kMaxCachedFrames) {
    auto eviction = cache_.begin();
    if (eviction == inserted.first) {
      ++eviction;
    }
    if (eviction == cache_.end()) {
      break;
    }
    cache_.erase(eviction);
  }
  activeFrameRefs_.push_back(inserted.first->second);
  *frame = inserted.first->second.get();
  return kOfxStatOK;
}

void TemporalFrameProvider::clear() {
  cache_.clear();
  activeFrameRefs_.clear();
}

}  // namespace rifeofx
