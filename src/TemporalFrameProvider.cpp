#include "TemporalFrameProvider.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

namespace rifeofx {

namespace {

constexpr int kRGBAChannels = 4;
constexpr size_t kMaxCachedFrames = 16;

void debugMessage(const std::string& message) {
  OutputDebugStringA(("[RifeOFX] " + message + "\n").c_str());
}

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

void TemporalFrameProvider::setFrameRange(OfxTime firstFrame,
                                          OfxTime lastFrame) {
  if (firstFrame > lastFrame) {
    return;
  }
  frameRangeAvailable_ = true;
  firstFrame_ = firstFrame;
  lastFrame_ = lastFrame;
}

void TemporalFrameProvider::debugLog(const char* operation, OfxTime time,
                                     OfxStatus status) const {
  if (!debug_) {
    return;
  }
  std::ostringstream stream;
  stream << operation << " time=" << time << " status=" << status;
  debugMessage(stream.str());
}

OfxStatus TemporalFrameProvider::getFrame(OfxTime time,
                                           const CachedFrame** frame) {
  if (!frame || !propertySuite_ || !imageEffectSuite_ || !sourceClip_) {
    return kOfxStatErrBadHandle;
  }

  const OfxTime requestedTime = time;
  if (frameRangeAvailable_) {
    time = std::clamp(time, firstFrame_, lastFrame_);
  }

  auto cached = cache_.find(time);
  if (cached != cache_.end()) {
    activeFrameRefs_.push_back(cached->second);
    *frame = cached->second.get();
    debugLog(requestedTime == time ? "getFrame(cache)" : "getFrame(cache, clamped)",
             time, kOfxStatOK);
    return kOfxStatOK;
  }
  const OfxStatus status = loadFrame(time, frame);
  if (requestedTime != time && status != kOfxStatOK) {
    debugLog("getFrame(clamped)", requestedTime, status);
  }
  return status;
}

OfxStatus TemporalFrameProvider::getFrameOffset(OfxTime time, int offset,
                                                 const CachedFrame** frame) {
  // OFX frame-based hosts conventionally express adjacent frames as +/-1 time
  // units (the official GetFramesNeeded example uses currentFrame +/- 1).
  return getFrame(time + static_cast<OfxTime>(offset), frame);
}

OfxStatus TemporalFrameProvider::getTemporalWindow(
    OfxTime time, int before, int after,
    std::vector<const CachedFrame*>& frames) {
  frames.clear();
  activeFrameRefs_.clear();
  if (before < 0 || after < 0) {
    return kOfxStatFailed;
  }

  const size_t frameCount = static_cast<size_t>(before + after + 1);
  frames.assign(frameCount, nullptr);

  // Load the central frame first. At a clip boundary the center can be valid
  // while the previous/next context frame is outside the source range.
  const size_t centerIndex = static_cast<size_t>(before);
  const CachedFrame* centerFrame = nullptr;
  const OfxStatus centerStatus = getFrame(time, &centerFrame);
  if (centerStatus != kOfxStatOK || !centerFrame) {
    frames.clear();
    return centerStatus;
  }
  frames[centerIndex] = centerFrame;

  for (int offset = -before; offset <= after; ++offset) {
    if (offset == 0) {
      continue;
    }
    const size_t index = static_cast<size_t>(offset + before);
    const CachedFrame* frame = nullptr;
    const OfxStatus status = getFrameOffset(time, offset, &frame);
    if (status == kOfxStatOK && frame) {
      frames[index] = frame;
      continue;
    }

    // Explicit edge policy: preserve the requested window shape by repeating
    // the central frame. This is only used for an unavailable temporal
    // neighbor; a missing center frame remains a hard render error.
    frames[index] = centerFrame;
    if (debug_) {
      std::ostringstream stream;
      stream << "temporal edge fallback requestedTime="
             << (time + static_cast<OfxTime>(offset))
             << " usingCenterTime=" << centerFrame->time
             << " status=" << status;
      debugMessage("[RifeOFX] " + stream.str());
    }
  }
  return kOfxStatOK;
}

OfxStatus TemporalFrameProvider::loadFrame(OfxTime time,
                                            const CachedFrame** frame) {
  OfxPropertySetHandle image = nullptr;
  OfxStatus status = imageEffectSuite_->clipGetImage(sourceClip_, time, nullptr, &image);
  debugLog("clipGetImage", time, status);
  if (status != kOfxStatOK || !image) {
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
    debugLog("clipGetImage(format rejected)", time, kOfxStatErrImageFormat);
    return kOfxStatErrImageFormat;
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
