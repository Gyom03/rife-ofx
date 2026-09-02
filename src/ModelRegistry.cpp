#include "ModelRegistry.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace rifeofx {

namespace {

std::string trim(std::string value) {
  const auto notSpace = [](unsigned char character) {
    return std::isspace(character) == 0;
  };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
  return value;
}

std::vector<std::string> splitFields(const std::string& line) {
  std::vector<std::string> fields;
  std::stringstream stream(line);
  std::string field;
  while (std::getline(stream, field, ';')) {
    fields.push_back(trim(field));
  }
  return fields;
}

bool parseBool(const std::string& value, bool* result) {
  if (!result) {
    return false;
  }
  if (value == "true" || value == "1") {
    *result = true;
    return true;
  }
  if (value == "false" || value == "0") {
    *result = false;
    return true;
  }
  return false;
}

bool parseSize(const std::string& value, std::size_t* result) {
  try {
    *result = static_cast<std::size_t>(std::stoull(value));
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

ModelRegistry::ModelRegistry(std::filesystem::path modelsRoot)
    : modelsRoot_(std::move(modelsRoot)) {}

bool RifeModelDescriptor::filesPresent() const {
  return !modelPath.empty() &&
         std::filesystem::is_regular_file(modelPath / "flownet.param") &&
         std::filesystem::is_regular_file(modelPath / "flownet.bin");
}

bool ModelRegistry::loadManifest(const std::filesystem::path& manifestPath,
                                 std::string* error) {
  std::ifstream input(manifestPath);
  if (!input) {
    if (error) {
      *error = "cannot open model manifest: " + manifestPath.string();
    }
    return false;
  }

  std::vector<RifeModelDescriptor> parsed;
  std::string line;
  std::size_t lineNumber = 0;
  while (std::getline(input, line)) {
    ++lineNumber;
    line = trim(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }

    const std::vector<std::string> fields = splitFields(line);
    if (fields.size() != 9) {
      if (error) {
        *error = "invalid model manifest line " + std::to_string(lineNumber) +
                 ": expected 9 semicolon-separated fields";
      }
      return false;
    }

    RifeModelDescriptor descriptor;
    descriptor.id = fields[0];
    descriptor.displayName = fields[1];
    descriptor.modelPath = modelsRoot_ / fields[2];
    try {
      descriptor.alignment = std::stoi(fields[3]);
    } catch (...) {
      if (error) {
        *error = "invalid alignment on model manifest line " +
                 std::to_string(lineNumber);
      }
      return false;
    }
    if (descriptor.alignment <= 0 ||
        !parseBool(fields[4], &descriptor.supportsArbitraryTimestep) ||
        !parseBool(fields[5], &descriptor.supportsEnsemble) ||
        !parseSize(fields[6], &descriptor.estimatedGpuMemoryMB)) {
      if (error) {
        *error = "invalid metadata on model manifest line " +
                 std::to_string(lineNumber);
      }
      return false;
    }
    descriptor.backend = fields[7];
    descriptor.redistributionStatus = fields[8];
    parsed.push_back(std::move(descriptor));
  }

  models_ = std::move(parsed);
  return true;
}

const RifeModelDescriptor* ModelRegistry::find(const std::string& id) const {
  const auto iterator = std::find_if(
      models_.begin(), models_.end(),
      [&id](const RifeModelDescriptor& descriptor) { return descriptor.id == id; });
  return iterator == models_.end() ? nullptr : &*iterator;
}

}  // namespace rifeofx
