#include "omnimesh/execution.hpp"
#include "omnimesh/manifest.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace omnimesh {
namespace {
using Json = nlohmann::json;
bool container_id(const std::string &id) {
  return !id.empty() && id.size() <= 128 &&
         std::all_of(id.begin(), id.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
         });
}
bool absolute_text(const std::string &path) {
  return !path.empty() && path[0] == '/' && path.size() <= 4096 &&
         path.find('\0') == std::string::npos;
}
} // namespace
OciRuntime::OciRuntime(std::string executable, std::string state_root)
    : executable_(std::move(executable)), state_root_(std::move(state_root)) {}
std::vector<std::string> OciRuntime::prefix() const {
  return {executable_, "--root", state_root_};
}
Status OciRuntime::command(const std::vector<std::string> &arguments,
                           ProcessResult &result) const {
  if (!absolute_text(executable_) || !absolute_text(state_root_)) {
    return {StatusCode::invalid_argument,
            "runtime and state root must be absolute paths"};
  }
  auto argv = prefix();
  argv.insert(argv.end(), arguments.begin(), arguments.end());
  ChildProcess child;
  auto status = child.start(argv);
  if (!status.ok()) {
    return status;
  }
  const auto deadline = MonotonicClock::now() + std::chrono::seconds(2);
  while (!child.result().finished) {
    status = child.poll();
    if (!status.ok()) {
      return status;
    }
    if (!child.result().finished && MonotonicClock::now() >= deadline) {
      child.terminate();
      result = child.result();
      return {StatusCode::unavailable, "runtime control command timed out"};
    }
    if (!child.result().finished) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  result = child.result();
  if (!result.exited || result.exit_code != 0) {
    return {StatusCode::unavailable,
            "runtime control command failed; termination is unconfirmed"};
  }
  return Status::Ok();
}
Status OciRuntime::launch(const std::string &id, const std::string &bundle,
                          ChildProcess &child) const {
  if (!container_id(id) || !absolute_text(bundle) ||
      !absolute_text(state_root_)) {
    return {StatusCode::invalid_argument,
            "invalid container identity or bundle/state path"};
  }
  auto argv = prefix();
  argv.insert(argv.end(), {"run", "--keep", "--bundle", bundle, id});
  return child.start(argv);
}
Status OciRuntime::state(const std::string &id, RuntimeState &state) const {
  if (!container_id(id)) {
    return {StatusCode::invalid_argument, "invalid container identity"};
  }
  ProcessResult result;
  auto status = command({"state", id}, result);
  if (!status.ok()) {
    return status;
  }
  if (result.dropped_output != 0) {
    return {StatusCode::unavailable, "runtime state exceeds output limit"};
  }
  try {
    std::size_t events = 0;
    std::vector<std::set<std::string>> keys;
    const auto callback = [&](int depth, Json::parse_event_t event,
                              Json &value) {
      if (depth > 16 || ++events > 1024) {
        throw std::runtime_error("state limit");
      }
      if (event == Json::parse_event_t::object_start) {
        keys.emplace_back();
      } else if (event == Json::parse_event_t::object_end) {
        keys.pop_back();
      } else if (event == Json::parse_event_t::key &&
                 !keys.back().insert(value.get<std::string>()).second) {
        throw std::runtime_error("duplicate key");
      }
      return true;
    };
    const auto document = Json::parse(result.output, callback);
    if (!document.is_object() || document.value("id", std::string{}) != id ||
        !document.contains("status") || !document["status"].is_string()) {
      return {StatusCode::unavailable,
              "runtime returned an invalid state identity/status"};
    }
    RuntimeState parsed;
    parsed.status = document["status"].get<std::string>();
    if (parsed.status != "creating" && parsed.status != "created" &&
        parsed.status != "running" && parsed.status != "stopped") {
      return {StatusCode::unavailable,
              "runtime returned an unsupported lifecycle state"};
    }
    if (document.contains("pid")) {
      if (!document["pid"].is_number_unsigned()) {
        return {StatusCode::unavailable, "runtime returned an invalid pid"};
      }
      parsed.pid = document["pid"].get<std::uint64_t>();
    }
    if (parsed.status == "running" && parsed.pid == 0) {
      return {StatusCode::unavailable,
              "running runtime state has no process identity"};
    }
    state = std::move(parsed);
    return Status::Ok();
  } catch (const std::exception &) {
    return {StatusCode::unavailable,
            "runtime returned malformed or excessive state JSON"};
  }
}
Status OciRuntime::signal(const std::string &id, bool force) const {
  if (!container_id(id)) {
    return {StatusCode::invalid_argument, "invalid container identity"};
  }
  ProcessResult result;
  return command({"kill", "--all", id, force ? "KILL" : "TERM"}, result);
}
Status OciRuntime::remove(const std::string &id) const {
  RuntimeState observed;
  auto status = state(id, observed);
  if (!status.ok()) {
    return status;
  }
  if (observed.status != "stopped") {
    return {StatusCode::conflict,
            "cannot delete a container before confirmed stop"};
  }
  ProcessResult result;
  return command({"delete", id}, result);
}
Status prepare_bundle(const Workload &workload, const Allocation &allocation,
                      const std::string &rootfs, const std::string &bundle) {
  const auto diagnostics = validate_workload(workload);
  if (!diagnostics.empty()) {
    return {StatusCode::invalid_argument, diagnostics.front().message};
  }
  const auto &request = allocation.request;
  if (!allocation.active ||
      request.workload_id != workload.tenant + "/" + workload.name ||
      request.generation != workload.generation ||
      request.requirements.tenant != workload.tenant ||
      !(request.requirements.resources == workload.resources)) {
    return {StatusCode::permission_denied,
            "bundle does not match the active allocation"};
  }
  std::error_code error;
  const auto resolved = std::filesystem::canonical(rootfs, error);
  if (!absolute_text(rootfs) || !absolute_text(bundle) || error ||
      resolved == "/" || !std::filesystem::is_directory(resolved)) {
    return {StatusCode::invalid_argument,
            "rootfs and bundle must be absolute; rootfs must be a provisioned "
            "directory other than /"};
  }
  if (!workload.capabilities.empty()) {
    return {StatusCode::unavailable,
            "local execution has no device or optional capability adapters"};
  }
  if (workload.resources.memory_bytes >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()) ||
      workload.resources.cpu_millis >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
              100) {
    return {StatusCode::invalid_argument,
            "resource limits exceed OCI signed integer range"};
  }
  // mkdir, rather than recursive create, gives this invocation exclusive
  // ownership.
  if (mkdir(bundle.c_str(), 0700) != 0) {
    return {StatusCode::conflict,
            "bundle directory must be fresh and have an existing parent"};
  }
  Json config{
      {"ociVersion", "1.0.2"},
      {"root", {{"path", resolved.string()}, {"readonly", true}}},
      {"process",
       {{"terminal", false},
        {"user", {{"uid", 0}, {"gid", 0}}},
        {"args", workload.command},
        {"cwd", "/"},
        {"env", Json::array({"PATH=/usr/bin:/bin", "LANG=C"})},
        {"noNewPrivileges", true},
        {"capabilities",
         {{"bounding", Json::array()},
          {"effective", Json::array()},
          {"inheritable", Json::array()},
          {"permitted", Json::array()},
          {"ambient", Json::array()}}},
        {"rlimits",
         Json::array(
             {{{"type", "RLIMIT_NOFILE"}, {"hard", 1024}, {"soft", 1024}}})}}},
      {"mounts",
       Json::array(
           {{{"destination", "/proc"},
             {"type", "proc"},
             {"source", "proc"},
             {"options", {"nosuid", "noexec", "nodev"}}},
            {{"destination", "/dev"},
             {"type", "tmpfs"},
             {"source", "tmpfs"},
             {"options", {"nosuid", "strictatime", "mode=755", "size=65536k"}}},
            {{"destination", "/tmp"},
             {"type", "tmpfs"},
             {"source", "tmpfs"},
             {"options", {"nosuid", "nodev", "mode=1777", "size=65536k"}}}})},
      {"linux",
       {{"namespaces", Json::array({{{"type", "pid"}},
                                    {{"type", "network"}},
                                    {{"type", "ipc"}},
                                    {{"type", "uts"}},
                                    {{"type", "mount"}},
                                    {{"type", "user"}},
                                    {{"type", "cgroup"}}})},
        {"uidMappings",
         Json::array(
             {{{"containerID", 0}, {"hostID", getuid()}, {"size", 1}}})},
        {"gidMappings",
         Json::array(
             {{{"containerID", 0}, {"hostID", getgid()}, {"size", 1}}})},
        {"resources",
         {{"memory", {{"limit", workload.resources.memory_bytes}}},
          {"cpu",
           {{"period", 100000},
            {"quota", workload.resources.cpu_millis * 100}}},
          {"pids", {{"limit", 256}}}}},
        {"maskedPaths",
         {"/proc/kcore", "/proc/keys", "/proc/timer_list", "/proc/scsi"}},
        {"readonlyPaths",
         {"/proc/sys", "/proc/sysrq-trigger", "/proc/irq", "/proc/bus"}}}},
      {"annotations",
       {{"io.omnimesh.image", workload.image},
        {"io.omnimesh.attempt", request.attempt_id},
        {"io.omnimesh.allocation", allocation.id},
        {"io.omnimesh.generation", std::to_string(workload.generation)}}}};
  std::ofstream file(bundle + "/config.json", std::ios::binary);
  file << config.dump(2) << '\n';
  file.close();
  if (!file) {
    return {StatusCode::internal, "cannot publish OCI configuration"};
  }
  return Status::Ok();
}
} // namespace omnimesh
