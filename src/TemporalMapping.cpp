#include "TemporalMapping.h"

namespace rifeofx {

const char* describeBlendPolicy(BlendPolicy policy) {
  switch (policy) {
    case BlendPolicy::kHoldA:
      return "holdA";
    case BlendPolicy::kHoldB:
      return "holdB";
    case BlendPolicy::kInterpolate:
      return "interpolate";
  }
  return "unknown";
}

}  // namespace rifeofx
