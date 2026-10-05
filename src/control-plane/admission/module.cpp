#include "omnimesh/manifest.hpp"
#include "omnimesh/allocator.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>

namespace omnimesh {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaxDiagnostics = 64;

void error(std::vector<Diagnostic>& diagnostics, const std::string& path,
           const std::string& message) {
  if (diagnostics.size() < kMaxDiagnostics) {
    diagnostics.push_back({path, message});
  }
}

bool identifier(const std::string& value) {
  const auto alnum = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  return !value.empty() && value.size() <= 63 && alnum(value.front()) &&
         alnum(value.back()) &&
         std::all_of(value.begin(), value.end(), [&](char c) {
           return alnum(c) || c == '-';
         });
}

bool text(const std::string& value, std::size_t maximum, bool empty = false) {
  return (empty || !value.empty()) && value.size() <= maximum &&
         value.find('\0') == std::string::npos;
}

bool immutable_image(const std::string& image) {
  if (!text(image, 1024)) {
    return false;
  }
  const auto at = image.rfind('@');
  const auto digest = at == std::string::npos ? image : image.substr(at + 1);
  if (at != std::string::npos &&
      (at == 0 || !std::all_of(image.begin(), image.begin() + at, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '/' || c == ':' || c == '_' || c == '-';
      }))) {
    return false;
  }
  return digest.size() == 71 && digest.compare(0, 7, "sha256:") == 0 &&
         std::all_of(digest.begin() + 7, digest.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

void check_identifier(std::vector<Diagnostic>& diagnostics,
                      const std::string& path, const std::string& value) {
  if (!identifier(value)) {
    error(diagnostics, path,
          "expected 1-63 lowercase letters, digits or hyphens, starting and "
          "ending with a letter or digit");
  }
}

void check_resources(std::vector<Diagnostic>& diagnostics,
                     const Resources& resources) {
  if (resources.cpu_millis == 0) {
    error(diagnostics, "$.spec.resources.cpuMillis", "must be greater than zero");
  }
  if (resources.memory_bytes == 0) {
    error(diagnostics, "$.spec.resources.memoryBytes", "must be greater than zero");
  }
}

void check_platform(std::vector<Diagnostic>& diagnostics,
                    const Platform& platform, bool require_architecture) {
  if (platform.os != "linux") {
    error(diagnostics, "$.spec.platform.os", "only linux is supported by this contract");
  }
  if ((require_architecture || !platform.architecture.empty()) &&
      platform.architecture != "amd64" && platform.architecture != "arm64") {
    error(diagnostics, "$.spec.platform.architecture", "expected amd64 or arm64");
  }
}

void check_strings(std::vector<Diagnostic>& diagnostics, const std::string& path,
                   const std::vector<std::string>& values, bool tenants = false) {
  if (values.size() > 64) {
    error(diagnostics, path, "at most 64 entries are allowed");
  }
  std::set<std::string> seen;
  for (std::size_t i = 0; i < std::min(values.size(), std::size_t{64}); ++i) {
    const auto entry = path + "[" + std::to_string(i) + "]";
    if (tenants) {
      check_identifier(diagnostics, entry, values[i]);
    } else if (!text(values[i], 128)) {
      error(diagnostics, entry, "expected a nonempty string of at most 128 bytes without NUL");
    }
    if (!seen.insert(values[i]).second) {
      error(diagnostics, entry, "duplicate entry");
    }
  }
}

void check_labels(std::vector<Diagnostic>& diagnostics, const std::string& path,
                  const std::map<std::string, std::string>& values) {
  if (values.size() > 64) {
    error(diagnostics, path, "at most 64 entries are allowed");
  }
  std::size_t count = 0;
  for (const auto& entry : values) {
    if (++count > 64) {
      break;
    }
    if (!text(entry.first, 128) || !text(entry.second, 256, true)) {
      error(diagnostics, path,
            "keys must be 1-128 bytes and values at most 256 bytes, without NUL");
    }
  }
}

void check_requirements(std::vector<Diagnostic>& diagnostics,
                        const PlacementRequirements& requirements) {
  check_identifier(diagnostics, "$.metadata.tenant", requirements.tenant);
  check_resources(diagnostics, requirements.resources);
  check_platform(diagnostics, requirements.platform, false);
  if (requirements.runtime != "oci") {
    error(diagnostics, "$.spec.runtime", "only oci placement requirements are supported");
  }
  check_labels(diagnostics, "$.spec.nodeSelector", requirements.node_selector);
  check_strings(diagnostics, "$.spec.capabilities", requirements.capabilities);
}

struct DocumentError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

bool decode_document(std::string_view document, Json& result,
                     std::vector<Diagnostic>& diagnostics) {
  if (document.size() > kMaxManifestBytes) {
    error(diagnostics, "$", "manifest exceeds the 1 MiB limit");
    return false;
  }
  std::vector<std::set<std::string>> objects;
  std::size_t events = 0;
  const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
    if (depth > 32 || ++events > 16384) {
      throw DocumentError("manifest exceeds the nesting or token limit");
    }
    if (event == Json::parse_event_t::object_start) {
      objects.emplace_back();
    } else if (event == Json::parse_event_t::object_end) {
      objects.pop_back();
    } else if (event == Json::parse_event_t::key) {
      if (!objects.back().insert(value.get<std::string>()).second) {
        throw DocumentError("duplicate object key");
      }
    }
    return true;
  };
  try {
    result = Json::parse(document.begin(), document.end(), callback);
    return true;
  } catch (const Json::parse_error& exception) {
    // Do not echo manifest fragments (which might contain sensitive input).
    error(diagnostics, "$", "invalid JSON at byte " + std::to_string(exception.byte));
  } catch (const DocumentError& exception) {
    error(diagnostics, "$", exception.what());
  } catch (const Json::exception&) {
    error(diagnostics, "$", "invalid JSON value");
  }
  return false;
}

class Decoder {
public:
  std::vector<Diagnostic> diagnostics;

  bool object(const Json& value, const std::string& path,
              std::initializer_list<const char*> allowed,
              std::initializer_list<const char*> required = {}) {
    if (!value.is_object()) {
      error(diagnostics, path, "expected an object");
      return false;
    }
    for (const auto& item : value.items()) {
      if (std::none_of(allowed.begin(), allowed.end(), [&](const char* key) {
            return item.key() == key;
          })) {
        error(diagnostics, path + "." + item.key(), "unknown field");
      }
    }
    for (const auto* key : required) {
      if (!value.contains(key)) {
        error(diagnostics, path + "." + key, "required field is missing");
      }
    }
    return true;
  }

  void string(const Json& value, const std::string& path, std::string& target) {
    if (!value.is_string()) {
      error(diagnostics, path, "expected a string");
    } else {
      target = value.get<std::string>();
    }
  }

  void literal(const Json& value, const std::string& path, const char* expected) {
    if (!value.is_string() || value.get<std::string>() != expected) {
      error(diagnostics, path, std::string("expected ") + expected);
    }
  }

  void boolean(const Json& value, const std::string& path, bool& target) {
    if (!value.is_boolean()) {
      error(diagnostics, path, "expected a boolean");
    } else {
      target = value.get<bool>();
    }
  }

  template <typename Integer>
  void integer(const Json& value, const std::string& path, Integer& target) {
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) {
      error(diagnostics, path, "expected a nonnegative integer");
      return;
    }
    const auto number = value.get<std::uint64_t>();
    if (number > std::numeric_limits<Integer>::max()) {
      error(diagnostics, path, "integer is out of range");
    } else {
      target = static_cast<Integer>(number);
    }
  }

  void strings(const Json& value, const std::string& path,
               std::vector<std::string>& target, std::size_t limit = 64) {
    if (!value.is_array() || value.size() > limit) {
      error(diagnostics, path, "expected an array with at most " +
                                  std::to_string(limit) + " entries");
      return;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
      std::string entry;
      string(value[i], path + "[" + std::to_string(i) + "]", entry);
      target.push_back(std::move(entry));
    }
  }

  void labels(const Json& value, const std::string& path,
              std::map<std::string, std::string>& target) {
    if (!value.is_object() || value.size() > 64) {
      error(diagnostics, path, "expected an object with at most 64 entries");
      return;
    }
    for (const auto& item : value.items()) {
      std::string entry;
      string(item.value(), path + "." + item.key(), entry);
      target.emplace(item.key(), std::move(entry));
    }
  }

  void resources(const Json& value, Resources& target) {
    const std::string path = "$.spec.resources";
    if (object(value, path, {"cpuMillis", "memoryBytes"},
               {"cpuMillis", "memoryBytes"})) {
      if (value.contains("cpuMillis")) {
        integer(value["cpuMillis"], path + ".cpuMillis", target.cpu_millis);
      }
      if (value.contains("memoryBytes")) {
        integer(value["memoryBytes"], path + ".memoryBytes", target.memory_bytes);
      }
    }
  }

  void platform(const Json& value, Platform& target, bool node = false) {
    const std::string path = "$.spec.platform";
    const bool valid = node ? object(value, path, {"os", "architecture"},
                                    {"os", "architecture"})
                            : object(value, path, {"os", "architecture"});
    if (valid) {
      if (value.contains("os")) {
        string(value["os"], path + ".os", target.os);
      }
      if (value.contains("architecture")) {
        string(value["architecture"], path + ".architecture", target.architecture);
        if (target.architecture.empty()) {
          error(diagnostics, path + ".architecture", "must not be empty when specified");
        }
      }
    }
  }

  bool envelope(const Json& root, const char* kind) {
    if (!object(root, "$", {"apiVersion", "kind", "metadata", "spec"},
                {"apiVersion", "kind", "metadata", "spec"})) {
      return false;
    }
    if (root.contains("apiVersion")) {
      literal(root["apiVersion"], "$.apiVersion", kApiVersion);
    }
    if (root.contains("kind")) {
      literal(root["kind"], "$.kind", kind);
    }
    return true;
  }
};

template <typename T>
ManifestResult<T> finish(T value, std::vector<Diagnostic> diagnostics) {
  Status status = diagnostics.empty()
                      ? Status::Ok()
                      : Status{StatusCode::invalid_argument,
                               diagnostics.front().path + ": " +
                                   diagnostics.front().message};
  return {std::move(status), std::move(value), std::move(diagnostics)};
}

} // namespace

Status validate_requirements(const PlacementRequirements& requirements) {
  std::vector<Diagnostic> diagnostics;
  check_requirements(diagnostics, requirements);
  return diagnostics.empty()
             ? Status::Ok()
             : Status{StatusCode::invalid_argument,
                      diagnostics.front().path + ": " + diagnostics.front().message};
}

std::vector<Diagnostic> validate_workload(const Workload& workload) {
  std::vector<Diagnostic> diagnostics;
  check_identifier(diagnostics, "$.metadata.name", workload.name);
  if (workload.generation == 0) {
    error(diagnostics, "$.metadata.generation", "must be greater than zero");
  }
  if (!immutable_image(workload.image)) {
    error(diagnostics, "$.spec.image",
          "expected sha256:<64 lowercase hex digits> or an image reference pinned to that digest");
  }
  if (workload.command.empty() || workload.command.size() > 256) {
    error(diagnostics, "$.spec.command", "expected 1-256 command arguments");
  }
  for (std::size_t i = 0; i < std::min(workload.command.size(), std::size_t{256}); ++i) {
    if (!text(workload.command[i], 4096, i != 0)) {
      error(diagnostics, "$.spec.command[" + std::to_string(i) + "]",
            "arguments must be at most 4096 bytes without NUL; executable must be nonempty");
    }
  }
  if (workload.replicas == 0 || workload.replicas > kMaxReplicas) {
    error(diagnostics, "$.spec.replicas", "expected an integer from 1 to 256");
  }
  check_requirements(diagnostics, requirements_for(workload));
  if (workload.retry.max_attempts == 0 || workload.retry.max_attempts > 16) {
    error(diagnostics, "$.spec.retry.maxAttempts", "expected an integer from 1 to 16");
  }
  if (workload.retry.backoff_millis == 0 || workload.retry.backoff_millis > 60000) {
    error(diagnostics, "$.spec.retry.backoffMillis", "expected an integer from 1 to 60000");
  }
  return diagnostics;
}

std::vector<Diagnostic> validate_node(const Node& node) {
  std::vector<Diagnostic> diagnostics;
  check_identifier(diagnostics, "$.metadata.name", node.id);
  check_resources(diagnostics, node.capacity);
  check_platform(diagnostics, node.platform, true);
  check_strings(diagnostics, "$.spec.runtimes", node.runtimes);
  for (const auto& runtime : node.runtimes) {
    if (runtime != "oci") {
      error(diagnostics, "$.spec.runtimes", "only oci inventory entries are supported");
      break;
    }
  }
  if (node.tenants.empty()) {
    error(diagnostics, "$.spec.tenants", "at least one authorized tenant is required");
  }
  check_strings(diagnostics, "$.spec.tenants", node.tenants, true);
  check_strings(diagnostics, "$.spec.capabilities", node.capabilities);
  check_labels(diagnostics, "$.spec.labels", node.labels);
  return diagnostics;
}

std::string encode_workload(const Workload& workload) {
  Json document{
      {"apiVersion", kApiVersion},
      {"kind", "Workload"},
      {"metadata", {{"name", workload.name},
                    {"tenant", workload.tenant},
                    {"generation", workload.generation}}},
      {"spec", {{"image", workload.image},
                {"command", workload.command},
                {"replicas", workload.replicas},
                {"privileged", false},
                {"capabilities", workload.capabilities},
                {"resources", {{"cpuMillis", workload.resources.cpu_millis},
                               {"memoryBytes", workload.resources.memory_bytes}}},
                {"platform", {{"os", workload.platform.os},
                              {"architecture", workload.platform.architecture}}},
                {"runtime", workload.runtime},
                {"nodeSelector", workload.node_selector},
                {"retry", {{"maxAttempts", workload.retry.max_attempts},
                           {"backoffMillis", workload.retry.backoff_millis}}}}}};
  if (workload.platform.architecture.empty()) { document["spec"]["platform"].erase("architecture"); }
  return document.dump();
}

std::string encode_node(const Node& node) {
  const Json document{
      {"apiVersion", kApiVersion},
      {"kind", "Node"},
      {"metadata", {{"name", node.id}}},
      {"spec", {{"resources", {{"cpuMillis", node.capacity.cpu_millis},
                                {"memoryBytes", node.capacity.memory_bytes}}},
                {"platform", {{"os", node.platform.os},
                              {"architecture", node.platform.architecture}}},
                {"runtimes", node.runtimes},
                {"tenants", node.tenants},
                {"labels", node.labels},
                {"capabilities", node.capabilities},
                {"ready", node.ready},
                {"verified", node.verified}}}};
  return document.dump();
}

ManifestResult<Workload> parse_workload(std::string_view document) {
  Decoder decoder;
  Json root;
  Workload workload;
  if (!decode_document(document, root, decoder.diagnostics) ||
      !decoder.envelope(root, "Workload")) {
    return finish(std::move(workload), std::move(decoder.diagnostics));
  }
  if (root.contains("metadata") &&
      decoder.object(root["metadata"], "$.metadata", {"name", "tenant", "generation"}, {"name"})) {
    const auto& metadata = root["metadata"];
    if (metadata.contains("name")) {
      decoder.string(metadata["name"], "$.metadata.name", workload.name);
    }
    if (metadata.contains("tenant")) {
      decoder.string(metadata["tenant"], "$.metadata.tenant", workload.tenant);
    }
    if (metadata.contains("generation")) {
      decoder.integer(metadata["generation"], "$.metadata.generation", workload.generation);
    }
  }
  if (root.contains("spec") &&
      decoder.object(root["spec"], "$.spec",
                     {"image", "command", "replicas", "privileged", "capabilities",
                      "resources", "platform", "runtime", "nodeSelector", "retry"},
                     {"image", "command"})) {
    const auto& spec = root["spec"];
    if (spec.contains("image")) {
      decoder.string(spec["image"], "$.spec.image", workload.image);
    }
    if (spec.contains("command")) {
      decoder.strings(spec["command"], "$.spec.command", workload.command, 256);
    }
    if (spec.contains("replicas")) {
      decoder.integer(spec["replicas"], "$.spec.replicas", workload.replicas);
    }
    if (spec.contains("privileged")) {
      bool privileged = false;
      decoder.boolean(spec["privileged"], "$.spec.privileged", privileged);
      if (privileged) {
        error(decoder.diagnostics, "$.spec.privileged", "privileged workloads are unsupported");
      }
    }
    if (spec.contains("resources")) {
      decoder.resources(spec["resources"], workload.resources);
    }
    if (spec.contains("platform")) {
      decoder.platform(spec["platform"], workload.platform);
    }
    if (spec.contains("runtime")) {
      decoder.string(spec["runtime"], "$.spec.runtime", workload.runtime);
    }
    if (spec.contains("nodeSelector")) {
      decoder.labels(spec["nodeSelector"], "$.spec.nodeSelector", workload.node_selector);
    }
    if (spec.contains("capabilities")) {
      decoder.strings(spec["capabilities"], "$.spec.capabilities", workload.capabilities);
    }
    if (spec.contains("retry") &&
        decoder.object(spec["retry"], "$.spec.retry", {"maxAttempts", "backoffMillis"})) {
      if (spec["retry"].contains("maxAttempts")) {
        decoder.integer(spec["retry"]["maxAttempts"], "$.spec.retry.maxAttempts", workload.retry.max_attempts);
      }
      if (spec["retry"].contains("backoffMillis")) {
        decoder.integer(spec["retry"]["backoffMillis"], "$.spec.retry.backoffMillis", workload.retry.backoff_millis);
      }
    }
  }
  if (decoder.diagnostics.empty()) {
    decoder.diagnostics = validate_workload(workload);
  }
  std::sort(workload.capabilities.begin(), workload.capabilities.end());
  return finish(std::move(workload), std::move(decoder.diagnostics));
}

ManifestResult<Node> parse_node(std::string_view document) {
  Decoder decoder;
  Json root;
  Node node;
  if (!decode_document(document, root, decoder.diagnostics) ||
      !decoder.envelope(root, "Node")) {
    return finish(std::move(node), std::move(decoder.diagnostics));
  }
  if (root.contains("metadata") &&
      decoder.object(root["metadata"], "$.metadata", {"name"}, {"name"}) &&
      root["metadata"].contains("name")) {
    decoder.string(root["metadata"]["name"], "$.metadata.name", node.id);
  }
  if (root.contains("spec") &&
      decoder.object(root["spec"], "$.spec",
                     {"resources", "platform", "runtimes", "tenants", "labels",
                      "capabilities", "ready", "verified"},
                     {"resources", "platform", "runtimes", "tenants", "verified"})) {
    const auto& spec = root["spec"];
    if (spec.contains("resources")) {
      decoder.resources(spec["resources"], node.capacity);
    }
    if (spec.contains("platform")) {
      decoder.platform(spec["platform"], node.platform, true);
    }
    if (spec.contains("runtimes")) {
      decoder.strings(spec["runtimes"], "$.spec.runtimes", node.runtimes);
    }
    if (spec.contains("tenants")) {
      decoder.strings(spec["tenants"], "$.spec.tenants", node.tenants);
    }
    if (spec.contains("labels")) {
      decoder.labels(spec["labels"], "$.spec.labels", node.labels);
    }
    if (spec.contains("capabilities")) {
      decoder.strings(spec["capabilities"], "$.spec.capabilities", node.capabilities);
    }
    if (spec.contains("ready")) {
      decoder.boolean(spec["ready"], "$.spec.ready", node.ready);
    }
    if (spec.contains("verified")) {
      decoder.boolean(spec["verified"], "$.spec.verified", node.verified);
    }
  }
  if (decoder.diagnostics.empty()) {
    decoder.diagnostics = validate_node(node);
  }
  std::sort(node.runtimes.begin(), node.runtimes.end());
  std::sort(node.tenants.begin(), node.tenants.end());
  std::sort(node.capabilities.begin(), node.capabilities.end());
  return finish(std::move(node), std::move(decoder.diagnostics));
}

} // namespace omnimesh
