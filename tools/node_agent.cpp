#include "omnimesh/execution.hpp"
#include "omnimesh/manifest.hpp"
#include <charconv>
#include <csignal>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {
using namespace omnimesh;
using Json = nlohmann::json;
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
int code(const Status &status) {
  if (status.ok()) {
    return 0;
  }
  if (status.code == StatusCode::invalid_argument) {
    return 2;
  }
  if (status.code == StatusCode::internal) {
    return 1;
  }
  return 3;
}
int fail(Status status) {
  std::cout << Json({{"apiVersion", kApiVersion},
                     {"kind", "DiagnosticResult"},
                     {"status", status_code_name(status.code)},
                     {"message", status.message}})
                   .dump(2)
            << '\n';
  return code(status);
}
Status read(const std::string &path, std::string &document) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {StatusCode::internal, "cannot open manifest: " + path};
  }
  document.resize(kMaxManifestBytes + 1);
  file.read(document.data(), static_cast<std::streamsize>(document.size()));
  document.resize(static_cast<std::size_t>(file.gcount()));
  if (file.bad()) {
    return {StatusCode::internal, "cannot read manifest"};
  }
  if (document.size() > kMaxManifestBytes) {
    return {StatusCode::invalid_argument, "manifest exceeds 1 MiB"};
  }
  return Status::Ok();
}
template <typename Integer>
bool integer(const std::string &text, Integer &target) {
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), target);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
int run(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::cout << version() << '\n';
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "omnimesh-node-agent run WORKLOAD.json --node NODE.json\n"
                 "  --runtime /absolute/path/to/crun-or-runc\n"
                 "  (--rootfs /trusted/rootfs | --image-layout /local/oci-layout)\n"
                 "  --state-dir /fresh/private/session [--timeout-ms 60000] "
                 "[--grace-ms 1000]\n"
                 "  [--journal-dir /durable/journal] [--max-image-bytes 1073741824]\n"
                 "  [--spool-dir /local/spool] [--max-input-bytes 268435456]\n"
                 "Foreground local OCI execution. Image layouts are verified "
                 "against the workload manifest digest and staged in the session. "
                 "--rootfs selects an administrator-provisioned tree. "
                 "Declared inputs require --spool-dir and appear read-only at "
                 "/tmp/omnimesh-inputs/<name>. "
                 "A journal directory enables "
                 "durable recording of control-plane facts before use; "
                 "recover it with `omnimesh recover`.\n";
    return 0;
  }
  if (argc < 3 || std::string_view(argv[1]) != "run") {
    return fail({StatusCode::invalid_argument,
                 "execution requires explicit configuration; use --help"});
  }
  std::map<std::string, std::string> arguments;
  for (int i = 3; i < argc; i += 2) {
    const std::string key = argv[i];
    if (i + 1 >= argc ||
        (key != "--node" && key != "--runtime" && key != "--rootfs" &&
         key != "--state-dir" && key != "--timeout-ms" &&
          key != "--grace-ms" && key != "--journal-dir" &&
          key != "--image-layout" && key != "--max-image-bytes" &&
          key != "--spool-dir" && key != "--max-input-bytes") ||
        !arguments.emplace(key, argv[i + 1]).second) {
      return fail({StatusCode::invalid_argument,
                   "unknown, missing or duplicate execution option"});
    }
  }
  for (const auto *key : {"--node", "--runtime", "--state-dir"}) {
    if (!arguments.count(key)) {
      return fail({StatusCode::invalid_argument,
                   std::string("required option: ") + key});
    }
  }
  if (arguments.count("--rootfs") + arguments.count("--image-layout") != 1 ||
      (arguments.count("--max-image-bytes") &&
       !arguments.count("--image-layout"))) {
    return fail({StatusCode::invalid_argument,
                 "select exactly one of --rootfs or --image-layout; "
                 "--max-image-bytes requires --image-layout"});
  }
  LocalExecutionOptions options;
  if (arguments.count("--max-input-bytes") && !arguments.count("--spool-dir")) {
    return fail({StatusCode::invalid_argument, "--max-input-bytes requires --spool-dir"});
  }
  if (arguments.count("--spool-dir")) {
    options.spool_directory = arguments.at("--spool-dir");
    if (options.spool_directory.empty()) {
      return fail({StatusCode::invalid_argument, "spool directory must be nonempty"});
    }
  }
  if (arguments.count("--max-input-bytes") &&
      !integer(arguments.at("--max-input-bytes"), options.max_input_bytes)) {
    return fail({StatusCode::invalid_argument, "input byte bound must be unsigned bytes"});
  }
  options.runtime_executable = arguments.at("--runtime");
  if (arguments.count("--rootfs")) {
    options.rootfs = arguments.at("--rootfs");
  } else {
    options.image_layout = arguments.at("--image-layout");
  }
  if (arguments.count("--max-image-bytes") &&
      !integer(arguments.at("--max-image-bytes"), options.max_image_bytes)) {
    return fail({StatusCode::invalid_argument,
                 "image byte bound must be unsigned bytes"});
  }
  options.state_directory = arguments.at("--state-dir");
  if (arguments.count("--journal-dir")) {
    options.journal_directory = arguments.at("--journal-dir");
  }
  if ((arguments.count("--timeout-ms") &&
       !integer(arguments.at("--timeout-ms"), options.timeout_millis)) ||
      (arguments.count("--grace-ms") &&
       !integer(arguments.at("--grace-ms"), options.grace_millis))) {
    return fail({StatusCode::invalid_argument,
                 "timeouts must be unsigned milliseconds"});
  }
  std::string document;
  auto status = read(argv[2], document);
  if (!status.ok()) {
    return fail(status);
  }
  const auto workload = parse_workload(document);
  if (!workload.status.ok()) {
    return fail(workload.status);
  }
  status = read(arguments.at("--node"), document);
  if (!status.ok()) {
    return fail(status);
  }
  const auto node = parse_node(document);
  if (!node.status.ok()) {
    return fail(node.status);
  }
  struct sigaction action{};
  action.sa_handler = interrupt;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, nullptr) != 0 ||
      sigaction(SIGTERM, &action, nullptr) != 0) {
    return fail({StatusCode::internal, "cannot install shutdown handlers"});
  }
  const auto result = execute_local(workload.value, node.value, options,
                                    [] { return interrupted != 0; });
  Json output{{"apiVersion", kApiVersion},
              {"kind", "LocalExecutionResult"},
              {"dryRun", false},
              {"status", status_code_name(result.status.code)},
              {"message", result.status.message},
              {"workload",
               {{"name", workload.value.name},
                {"tenant", workload.value.tenant},
                {"generation", workload.value.generation},
                {"image", workload.value.image}}},
              {"sessionDirectory", result.session_directory},
              {"reservationsRetained", result.reservations_retained},
              {"journaled", result.journaled},
               {"journalRecords", result.journal_records},
               {"rootfs",
                {{"path", result.rootfs_directory},
                 {"source", options.image_layout.empty() ? "provisioned"
                                                         : "image-layout"},
                 {"verified", result.image_verified},
                 {"manifestDigest", result.image.manifest_digest},
                 {"configDigest", result.image.config_digest},
                 {"layers", result.image.layers},
                 {"files", result.image.files},
                 {"bytes", result.image.bytes}}},
              {"tasks", Json::array()},
              {"inputs", {{"path", result.input_directory},
                           {"verified", result.inputs_verified},
                           {"files", Json::array()}}},
              {"workers", Json::array()}};
  for (std::size_t index = 0; index < result.inputs.size(); ++index) {
    const auto &input = result.inputs[index];
    output["inputs"]["files"].push_back(
        {{"name", workload.value.inputs[index].name}, {"digest", input.digest},
         {"bytes", input.size},
         {"destination", "/tmp/omnimesh-inputs/" + workload.value.inputs[index].name}});
  }
  for (const auto &task : result.workload.tasks) {
    Json item{{"taskId", task.id},
              {"state", task_state_name(task.state)},
              {"attempts", Json::array()}};
    for (const auto &attempt : task.attempts) {
      item["attempts"].push_back({{"attemptId", attempt.id},
                                  {"allocationId", attempt.allocation_id},
                                  {"nodeId", attempt.node_id},
                                  {"state", attempt_state_name(attempt.state)},
                                  {"exitCode", attempt.exit_code},
                                  {"retryable", attempt.retryable}});
    }
    if (task.state == TaskState::queued) {
      item["placementMessage"] = task.placement.status.message;
    }
    output["tasks"].push_back(std::move(item));
  }
  for (const auto &worker : result.workers) {
    output["workers"].push_back(
        {{"attemptId", worker.attempt_id},
         {"containerId", worker.container_id},
         {"state", attempt_state_name(worker.state)},
         {"exitCode", worker.process.exit_code},
         {"exited", worker.process.exited},
         {"signal", worker.process.signal},
         {"stdout", worker.process.output},
         {"stderr", worker.process.errors},
         {"droppedStdoutBytes", worker.process.dropped_output},
         {"droppedStderrBytes", worker.process.dropped_errors},
         {"cleanupStatus", status_code_name(worker.cleanup.code)},
         {"cleanupMessage", worker.cleanup.message}});
  }
  std::cout << output.dump(2, ' ', false, Json::error_handler_t::replace)
            << '\n';
  return code(result.status);
}
} // namespace
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &) {
    return fail({omnimesh::StatusCode::internal,
                 "local execution interrupted by an internal error; inspect "
                 "runtime state before restarting"});
  }
}
