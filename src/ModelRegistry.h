#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace rifeofx {

struct RifeModelDescriptor {
  std::string id;
  std::string displayName;
  std::filesystem::path modelPath;
  int alignment = 32;
  bool supportsArbitraryTimestep = false;
  bool supportsEnsemble = false;
  std::size_t estimatedGpuMemoryMB = 0;
  std::string backend;
  std::string redistributionStatus;

  bool filesPresent() const;
};

class ModelRegistry {
 public:
  ModelRegistry() = default;
  explicit ModelRegistry(std::filesystem::path modelsRoot);

  bool loadManifest(const std::filesystem::path& manifestPath,
                   std::string* error = nullptr);

  const RifeModelDescriptor* find(const std::string& id) const;
  const std::vector<RifeModelDescriptor>& models() const { return models_; }
  const std::filesystem::path& modelsRoot() const { return modelsRoot_; }

 private:
  std::filesystem::path modelsRoot_;
  std::vector<RifeModelDescriptor> models_;
};

}  // namespace rifeofx
