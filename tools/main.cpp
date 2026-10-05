#include "omnimesh/manifest.hpp"
#include "omnimesh/artifacts.hpp"
#include "omnimesh/control_plane.hpp"
#include "omnimesh/images.hpp"
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
         << "                [--journal-dir DIR]\n"
         << "  omnimesh recover --journal-dir DIR\n"
         << "  omnimesh artifact put FILE --spool-dir DIR --tenant TENANT\n"
         << "  omnimesh artifact get DIGEST --spool-dir DIR --tenant TENANT --out FILE\n"
         << "  omnimesh artifact list --spool-dir DIR\n"
         << "  omnimesh artifact gc --spool-dir DIR [--keep DIGEST ...]\n"
         << "  omnimesh image inspect --layout DIR [--platform OS/ARCH] [--digest DIGEST]\n"
         << "  omnimesh image unpack --layout DIR --rootfs OUT [--platform OS/ARCH]\n"
         << "                [--digest DIGEST]\n"
         << "\nManifests use strict JSON. Commands emit JSON results.\n"
         << "Planning uses a temporary in-memory control plane; dryRun is always true.\n"
         << "A journal directory additionally records planning facts for later recovery.\n";
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

int run_image(int argc, char** argv) {
  if (argc < 4) {
    usage(std::cerr);
    return 2;
  }
  const std::string subcommand = argv[2];
  std::string layout;
  std::string rootfs;
  std::string platform_text;
  std::string digest;
  for (int i = 3; i < argc;) {
    const std::string option = argv[i];
    if ((option == "--layout" || option == "--rootfs" ||
         option == "--platform" || option == "--digest") &&
        i + 1 < argc) {
      if (option == "--layout" && layout.empty()) {
        layout = argv[i + 1];
      } else if (option == "--rootfs" && rootfs.empty()) {
        rootfs = argv[i + 1];
      } else if (option == "--platform" && platform_text.empty()) {
        platform_text = argv[i + 1];
      } else if (option == "--digest" && digest.empty()) {
        digest = argv[i + 1];
      } else {
        usage(std::cerr);
        return 2;
      }
      i += 2;
    } else {
      usage(std::cerr);
      return 2;
    }
  }
  ImagePlatform select;
  if (!platform_text.empty()) {
    const auto slash = platform_text.find('/');
    if (slash == std::string::npos || slash == 0 ||
        slash + 1 >= platform_text.size() ||
        platform_text.find('/', slash + 1) != std::string::npos) {
      usage(std::cerr);
      return 2;
    }
    select.os = platform_text.substr(0, slash);
    select.architecture = platform_text.substr(slash + 1);
  }
  ImageLoader loader;
  if (subcommand == "inspect") {
    if (layout.empty() || !rootfs.empty()) {
      usage(std::cerr);
      return 2;
    }
    ImageSummary summary;
    const auto status = loader.inspect(layout, select, digest, summary);
    if (!status.ok()) {
      return fail(status);
    }
    Json output{{"apiVersion", kApiVersion},
                {"kind", "ImageManifest"},
                {"manifest", summary.manifest_digest},
                {"config", summary.config_digest},
                {"configSize", summary.config_size},
                {"platform",
                 {{"os", summary.platform.os},
                  {"architecture", summary.platform.architecture}}},
                {"layers", Json::array()}};
    for (const auto &layer : summary.layers) {
      output["layers"].push_back({{"digest", layer.digest},
                                  {"mediaType", layer.media_type},
                                  {"size", layer.size},
                                  {"diffId", layer.diff_id}});
    }
    std::cout << output.dump(2) << '\n';
    return 0;
  }
  if (subcommand == "unpack") {
    if (layout.empty() || rootfs.empty()) {
      usage(std::cerr);
      return 2;
    }
    UnpackOptions options;
    options.layout_directory = layout;
    options.rootfs_directory = rootfs;
    options.platform = select;
    options.expected_digest = digest;
    UnpackReport report;
    const auto status = loader.unpack(options, report);
    if (!status.ok()) {
      return fail(status);
    }
    std::cout << Json({{"apiVersion", kApiVersion},
                       {"kind", "ImageUnpack"},
                       {"manifest", report.manifest_digest},
                       {"layers", report.layers},
                       {"files", report.files},
                       {"bytes", report.bytes},
                       {"rootfs", rootfs}})
                     .dump(2)
              << '\n';
    return 0;
  }
  usage(std::cerr);
  return 2;
}

int run_artifact(int argc, char** argv) {
  if (argc < 4) {
    usage(std::cerr);
    return 2;
  }
  const std::string subcommand = argv[2];
  SpoolOptions options;
  auto spool_option = [&](int index) -> bool {
    if (index + 1 < argc && std::string(argv[index]) == "--spool-dir") {
      options.directory = argv[index + 1];
      return true;
    }
    return false;
  };
  if (subcommand == "put") {
    std::string tenant;
    if (argc != 8 || !spool_option(4) || std::string(argv[6]) != "--tenant") {
      usage(std::cerr);
      return 2;
    }
    tenant = argv[7];
    ArtifactSpool spool;
    auto status = spool.open(options);
    if (!status.ok()) {
      return fail(status);
    }
    ArtifactInfo info;
    status = spool.put(argv[3], tenant, info);
    if (!status.ok()) {
      return fail(status);
    }
    status = spool.close();
    if (!status.ok()) {
      return fail(status);
    }
    std::cout << Json({{"apiVersion", kApiVersion},
                       {"kind", "Artifact"},
                       {"digest", info.digest},
                       {"size", info.size},
                       {"tenant", info.tenant},
                       {"spoolDirectory", options.directory}})
                     .dump(2)
              << '\n';
    return 0;
  }
  if (subcommand == "get") {
    std::string tenant, out;
    if (argc != 10 || !spool_option(4) || std::string(argv[6]) != "--tenant" ||
        std::string(argv[8]) != "--out") {
      usage(std::cerr);
      return 2;
    }
    tenant = argv[7];
    out = argv[9];
    ArtifactSpool spool;
    auto status = spool.open(options);
    if (!status.ok()) {
      return fail(status);
    }
    ArtifactInfo info;
    status = spool.inspect(argv[3], info);
    if (status.ok()) {
      status = spool.fetch(argv[3], tenant, out);
    }
    if (!status.ok()) {
      return fail(status);
    }
    status = spool.close();
    if (!status.ok()) {
      return fail(status);
    }
    std::cout << Json({{"apiVersion", kApiVersion},
                       {"kind", "ArtifactFetch"},
                       {"digest", info.digest},
                       {"size", info.size},
                       {"tenant", info.tenant},
                       {"output", out}})
                     .dump(2)
              << '\n';
    return 0;
  }
  if (subcommand == "list") {
    if (argc != 5 || !spool_option(3)) {
      usage(std::cerr);
      return 2;
    }
    ArtifactSpool spool;
    auto status = spool.open(options);
    if (!status.ok()) {
      return fail(status);
    }
    const auto blobs = spool.list();
    const auto stats = spool.stats();
    const auto notes = spool.diagnostics();
    status = spool.close();
    if (!status.ok()) {
      return fail(status);
    }
    Json output{{"apiVersion", kApiVersion},
                {"kind", "ArtifactList"},
                {"spoolDirectory", options.directory},
                {"capacityBytes", stats.capacity_bytes},
                {"maxArtifacts", stats.max_artifacts},
                {"blobsUsed", stats.blobs},
                {"bytesUsed", stats.bytes},
                {"artifacts", Json::array()},
                {"diagnostics", Json::array()}};
    for (const auto &blob : blobs) {
      output["artifacts"].push_back({{"digest", blob.digest},
                                    {"size", blob.size},
                                    {"tenant", blob.tenant}});
    }
    for (const auto &note : notes) {
      output["diagnostics"].push_back(
          {{"path", note.path}, {"message", note.message}});
    }
    std::cout << output.dump(2) << '\n';
    return 0;
  }
  if (subcommand == "gc") {
    if (argc < 5 || !spool_option(3)) {
      usage(std::cerr);
      return 2;
    }
    std::set<std::string> keep;
    for (int i = 5; i < argc;) {
      if (i + 1 < argc && std::string(argv[i]) == "--keep") {
        keep.insert(argv[i + 1]);
        i += 2;
      } else {
        usage(std::cerr);
        return 2;
      }
    }
    ArtifactSpool spool;
    auto status = spool.open(options);
    if (!status.ok()) {
      return fail(status);
    }
    CollectReport report;
    status = spool.collect(keep, report);
    if (!status.ok()) {
      return fail(status);
    }
    status = spool.close();
    if (!status.ok()) {
      return fail(status);
    }
    std::cout << Json({{"apiVersion", kApiVersion},
                       {"kind", "ArtifactCollection"},
                       {"spoolDirectory", options.directory},
                       {"removed", report.removed},
                       {"reclaimedBytes", report.reclaimed_bytes},
                       {"kept", keep.size()}})
                     .dump(2)
              << '\n';
    return 0;
  }
  usage(std::cerr);
  return 2;
}

int recover_journal(const std::string& directory) {
  DurableControlPlane durable;
  auto status = durable.open(directory);
  if (!status.ok()) {
    return fail(status);
  }
  Allocator allocator;
  WorkloadController controller(allocator);
  const auto report = durable.recover(allocator, controller);
  const auto records = durable.stats().records;
  status = durable.close();
  if (!status.ok()) {
    return fail(status);
  }
  if (!report.status) {
    // The partially reconstructed state dies with the local allocator and
    // controller here. Callers must not schedule against it.
    return fail({StatusCode::unavailable,
                 "control-plane recovery failed; recovered state was discarded"},
                report.diagnostics);
  }
  Json output{{"apiVersion", kApiVersion},
              {"kind", "RecoveryReport"},
              {"status", "ok"},
              {"message",
               "recovered control-plane facts; active attempts are Unknown "
               "until fresh runtime observations arrive"},
              {"journalRecords", records},
              {"recordsApplied", report.records_applied},
              {"reservationsRecovered", report.reservations_recovered},
              {"observationsRecovered", report.observations_recovered},
              {"recordsSkipped", report.records_skipped},
              {"diagnostics", Json::array()}};
  for (const auto& diagnostic : report.diagnostics) {
    output["diagnostics"].push_back(
        {{"path", diagnostic.path}, {"message", diagnostic.message}});
  }
  std::cout << output.dump(2) << '\n';
  return 0;
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
  if (command != "validate" && command != "validate-node" &&
      command != "plan" && command != "recover" && command != "artifact" &&
      command != "image") {
    usage(std::cerr);
    return 2;
  }
  if (command == "artifact") {
    return run_artifact(argc, argv);
  }
  if (command == "image") {
    return run_image(argc, argv);
  }
  if (command == "recover") {
    if (argc != 4 || std::string_view(argv[2]) != "--journal-dir") {
      usage(std::cerr);
      return 2;
    }
    return recover_journal(argv[3]);
  }
  if (argc < 3 || (command != "plan" && argc != 3)) {
    usage(std::cerr);
    return 2;
  }
  std::vector<std::string> node_files;
  std::string journal_directory;
  bool journal_requested = false;
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
    } else if (option == "--journal-dir" && i + 1 < argc && !journal_requested) {
      journal_directory = argv[++i];
      journal_requested = true;
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
  DurableControlPlane durable;
  const bool journal = journal_requested && command == "plan";
  if (journal) {
    status = durable.open(journal_directory);
    if (!status.ok()) {
      return fail(status);
    }
    // Planning facts are recorded before the mutations they describe. The
    // reservations below are the dry run's own accounting, recorded for later
    // recovery; recovery restores them as Unknown, never as running work.
    status = durable.record_quota(workload.tenant, quota);
    if (!status.ok()) {
      return fail(status);
    }
  }
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
    if (journal) {
      status = durable.record_node(node.value);
      if (!status.ok()) {
        return fail(status);
      }
    }
    status = allocator.upsert_node(std::move(node.value));
    if (!status.ok()) {
      return fail(status);
    }
  }
  WorkloadController controller(allocator);
  if (journal) {
    status = durable.record_workload(workload);
    if (!status.ok()) {
      return fail(status);
    }
  }
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
  if (journal) {
    for (const auto& task : record.tasks) {
      if (task.state != TaskState::allocated || task.attempts.empty()) {
        continue;
      }
      const auto& attempt = task.attempts.back();
      status = durable.record_reservation(task.id, attempt.node_id,
                                          attempt.number, attempt.generation);
      if (!status.ok()) {
        return fail(status);
      }
    }
    status = durable.close();
    if (!status.ok()) {
      return fail(status);
    }
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
  output["journaled"] = journal;
  output["journalRecords"] = journal ? durable.stats().records : 0;
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
