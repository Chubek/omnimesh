#include "omnimesh/manifest.hpp"
#include "omnimesh/orchestrator.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>

namespace {

using namespace omnimesh;
using Json = nlohmann::json;

void usage(std::ostream& stream) {
  stream << "OmniMesh " << version() << "\n"
         << "  omnimesh --version\n"
         << "  omnimesh validate WORKLOAD.json\n"
         << "  omnimesh validate-node NODE.json\n"
         << "  omnimesh plan WORKLOAD.json --node NODE.json [--node NODE.json ...]\n"
         << "                [--quota CPU_MILLIS MEMORY_BYTES MAX_ALLOCATIONS]\n"
         << "\nManifests use strict JSON. Commands emit JSON results.\n"
         << "Planning uses a temporary in-memory control plane; dryRun is always true.\n";
}

Status read_document(const std::string& path, std::string& document) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {StatusCode::internal, "cannot open manifest: " + path};
  }
  document.resize(kMaxManifestBytes + 1);
  file.read(document.data(), static_cast<std::streamsize>(document.size()));
  const auto size = file.gcount();
  if (file.bad() || (file.fail() && !file.eof())) {
    return {StatusCode::internal, "cannot read manifest: " + path};
  }
  document.resize(static_cast<std::size_t>(size));
  if (document.size() > kMaxManifestBytes) {
    return {StatusCode::invalid_argument, "manifest exceeds the 1 MiB limit: " + path};
  }
  return Status::Ok();
}

int exit_code(const Status& status) {
  switch (status.code) {
  case StatusCode::ok: return 0;
  case StatusCode::invalid_argument: return 2;
  case StatusCode::unavailable:
  case StatusCode::not_implemented:
  case StatusCode::conflict:
  case StatusCode::resource_exhausted:
  case StatusCode::permission_denied: return 3;
  default: return 1;
  }
}

int fail(const Status& status, const std::vector<Diagnostic>& diagnostics = {}) {
  Json output{{"apiVersion", kApiVersion}, {"kind", "DiagnosticResult"},
              {"status", status_code_name(status.code)}, {"message", status.message},
              {"diagnostics", Json::array()}};
  for (const auto& diagnostic : diagnostics) {
    output["diagnostics"].push_back({{"path", diagnostic.path}, {"message", diagnostic.message}});
  }
  std::cout << output.dump(2) << '\n';
  return exit_code(status);
}

Json resources_json(const Resources& resources) {
  return {{"cpuMillis", resources.cpu_millis}, {"memoryBytes", resources.memory_bytes}};
}

template <typename Integer>
bool unsigned_integer(const char* argument, Integer& value) {
  const std::string_view input(argument);
  const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
  return result.ec == std::errc{} && result.ptr == input.data() + input.size();
}

int run(int argc, char** argv) {
  if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
    usage(std::cout);
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::cout << version() << '\n';
    return 0;
  }
  const std::string command = argv[1];
  if (argc < 3 || (command != "validate" && command != "validate-node" && command != "plan") ||
      (command != "plan" && argc != 3)) {
    usage(std::cerr);
    return 2;
  }
  std::vector<std::string> node_files;
  TenantQuota quota{{std::numeric_limits<std::uint64_t>::max(),
                     std::numeric_limits<std::uint64_t>::max()}, kMaxReplicas};
  bool explicit_quota = false;
  for (int i = 3; i < argc; ++i) {
    const std::string_view option = argv[i];
    if (option == "--node" && i + 1 < argc) {
      node_files.emplace_back(argv[++i]);
      if (node_files.size() > kMaxNodes) {
        return fail({StatusCode::invalid_argument, "at most 256 node manifests are allowed"});
      }
    } else if (option == "--quota" && i + 3 < argc && !explicit_quota) {
      if (!unsigned_integer(argv[i + 1], quota.limit.cpu_millis) ||
          !unsigned_integer(argv[i + 2], quota.limit.memory_bytes) ||
          !unsigned_integer(argv[i + 3], quota.max_allocations)) {
        return fail({StatusCode::invalid_argument, "quota values must be unsigned integers"});
      }
      explicit_quota = true;
      i += 3;
    } else {
      usage(std::cerr);
      return 2;
    }
  }
  if (command == "plan" && node_files.empty()) {
    return fail({StatusCode::invalid_argument, "plan requires at least one --node manifest"});
  }
  std::string document;
  auto status = read_document(argv[2], document);
  if (!status.ok()) {
    return fail(status);
  }
  if (command == "validate-node") {
    const auto result = parse_node(document);
    if (!result.status.ok()) {
      return fail(result.status, result.diagnostics);
    }
    std::cout << Json({{"apiVersion", kApiVersion}, {"kind", "ValidationResult"},
                       {"valid", true}, {"name", result.value.id}}).dump(2) << '\n';
    return 0;
  }
  const auto result = parse_workload(document);
  if (!result.status.ok()) {
    return fail(result.status, result.diagnostics);
  }
  const auto& workload = result.value;
  if (command == "validate") {
    std::cout << Json({{"apiVersion", kApiVersion}, {"kind", "ValidationResult"},
                       {"valid", true}, {"name", workload.name}, {"tenant", workload.tenant},
                       {"generation", workload.generation}, {"replicas", workload.replicas},
                       {"resources", resources_json(workload.resources)}}).dump(2) << '\n';
    return 0;
  }
  Allocator allocator;
  status = allocator.set_quota(workload.tenant, quota);
  if (!status.ok()) {
    return fail(status);
  }
  std::set<std::string> node_ids;
  for (const auto& path : node_files) {
    status = read_document(path, document);
    if (!status.ok()) {
      return fail(status);
    }
    auto node = parse_node(document);
    if (!node.status.ok()) {
      return fail(node.status, node.diagnostics);
    }
    if (!node_ids.insert(node.value.id).second) {
      return fail({StatusCode::invalid_argument, "duplicate node identity: " + node.value.id});
    }
    status = allocator.upsert_node(std::move(node.value));
    if (!status.ok()) {
      return fail(status);
    }
  }
  WorkloadController controller(allocator);
  status = controller.submit(workload, workload.tenant);
  if (!status.ok()) {
    return fail(status);
  }
  status = controller.reconcile();
  if (!status.ok()) {
    return fail(status);
  }
  WorkloadRecord record;
  status = controller.inspect(workload.tenant + "/" + workload.name, workload.tenant, record);
  if (!status.ok()) {
    return fail(status);
  }
  Json output{{"apiVersion", kApiVersion}, {"kind", "PlacementPlan"}, {"dryRun", true},
              {"workload", {{"name", workload.name}, {"tenant", workload.tenant},
                            {"generation", workload.generation}}},
              {"image", workload.image}, {"complete", true},
              {"quota", {{"source", explicit_quota ? "explicit" : "planning-default"},
                         {"resources", resources_json(quota.limit)},
                         {"maxAllocations", quota.max_allocations}}},
              {"tasks", Json::array()}, {"nodes", Json::array()}};
  bool complete = true;
  for (const auto& task : record.tasks) {
    Json planned{{"taskId", task.id}, {"replica", task.replica},
                 {"state", task_state_name(task.state)},
                 {"resources", resources_json(workload.resources)}};
    if (task.state == TaskState::allocated) {
      const auto& attempt = task.attempts.back();
      planned["attemptId"] = attempt.id;
      planned["allocationId"] = attempt.allocation_id;
      planned["nodeId"] = attempt.node_id;
    } else {
      complete = false;
      planned["status"] = status_code_name(task.placement.status.code);
      planned["message"] = task.placement.status.message;
      planned["nodes"] = Json::array();
      planned["omittedNodes"] = task.placement.omitted_nodes;
      for (const auto& node : task.placement.nodes) {
        planned["nodes"].push_back({{"nodeId", node.node_id}, {"reasons", node.reasons},
                                    {"omittedReasons", node.omitted_reasons}});
      }
    }
    output["tasks"].push_back(std::move(planned));
  }
  for (const auto& entry : allocator.snapshot().nodes) {
    output["nodes"].push_back({{"nodeId", entry.first}, {"reserved", resources_json(entry.second.used)}});
  }
  output["complete"] = complete;
  std::cout << output.dump(2) << '\n';
  return complete ? 0 : 3;
}

} // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::bad_alloc&) {
    std::cerr << "resource_exhausted: insufficient memory for the bounded planning operation\n";
    return 1;
  } catch (const std::exception&) {
    std::cerr << "internal: planning operation failed\n";
    return 1;
  }
}
