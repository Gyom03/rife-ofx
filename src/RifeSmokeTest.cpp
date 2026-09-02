#include "RifeEngine.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

rifeofx::CachedFrame makeTestFrame(int squareX) {
  // Deliberately not aligned to 64: this exercises the model descriptor's
  // explicit model-specific padding path. At 130x66, standard 4.25 becomes
  // 192x128 while 4.25 Lite becomes 256x128 because Lite declares alignment
  // 128.
  constexpr int width = 130;
  constexpr int height = 66;
  rifeofx::CachedFrame frame;
  frame.bounds = {0, 0, width, height};
  frame.rowBytes = width * 4 * static_cast<int>(sizeof(float));
  frame.rgba.resize(static_cast<size_t>(width) * height * 4, 0.0f);

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const bool square = x >= squareX && x < squareX + 16 && y >= 24 && y < 40;
      const size_t index = (static_cast<size_t>(y) * width + x) * 4;
      frame.rgba[index + 0] = square ? 1.0f : 0.05f;
      frame.rgba[index + 1] = square ? 0.2f : 0.05f;
      frame.rgba[index + 2] = square ? 0.05f : 0.05f;
      frame.rgba[index + 3] = 1.0f;
    }
  }
  return frame;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 3 && argc != 4) {
    std::wcerr << L"Usage: RifeSmokeTest.exe <models-root> <model-id> [manifest-path]\n";
    return 2;
  }

  const std::filesystem::path modelsRoot(argv[1]);
  const std::wstring wideId(argv[2]);
  const int modelIdLength = WideCharToMultiByte(CP_UTF8, 0, wideId.c_str(),
                                                 static_cast<int>(wideId.size()),
                                                 nullptr, 0, nullptr, nullptr);
  std::string modelId(static_cast<size_t>(std::max(modelIdLength, 0)), '\0');
  if (modelIdLength > 0) {
    WideCharToMultiByte(CP_UTF8, 0, wideId.c_str(),
                        static_cast<int>(wideId.size()), modelId.data(),
                        modelIdLength, nullptr, nullptr);
  }
  const std::filesystem::path manifestPath =
      argc == 4 ? std::filesystem::path(argv[3]) : modelsRoot / L"registry.csv";
  rifeofx::RifeEngine engine(0, modelsRoot, manifestPath);
  const OfxStatus loadStatus = engine.loadModel(modelId);
  if (loadStatus != kOfxStatOK) {
    std::wcerr << L"RIFE model load status=" << loadStatus << L"\n";
    return 1;
  }
  const rifeofx::CachedFrame frameA = makeTestFrame(8);
  const rifeofx::CachedFrame frameB = makeTestFrame(24);
  std::vector<float> output;
  rifeofx::InferenceDiagnostics diagnostics;

  std::wcerr << L"Starting RIFE Vulkan smoke test...\n";
  const OfxStatus status = engine.interpolate(frameA, frameB, 0.5f, output,
                                               &diagnostics);
  std::wcerr << L"RIFE status=" << status << L", output floats=" << output.size()
             << L", model=" << diagnostics.modelId.c_str()
             << L", padded=" << diagnostics.paddedWidth << L"x"
             << diagnostics.paddedHeight << L", inferenceMs="
             << diagnostics.inferenceMilliseconds << L"\n";
  return status == kOfxStatOK ? 0 : 1;
}
