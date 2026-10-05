#include "omnimesh/execution.hpp"
#include "test_support.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <sys/utsname.h>
#include <unistd.h>
using namespace omnimesh;
using Json = nlohmann::json;
namespace {
struct Fixture {
  std::string root;
  Fixture() {
    char path[] = "/tmp/omnimesh-execution-XXXXXX";
    const auto created = mkdtemp(path); CHECK(created != nullptr); root = created;
    std::filesystem::create_directory(root + "/rootfs");
  }
  void recover_fixture() {
    // Simulated administrator recovery: fixture workers are host subprocesses
    // and the tests verify they were reaped. Never touch another session's barrier.
    const auto path = "/tmp/omnimesh-local-agent-" + std::to_string(getuid()) + ".lock";
    std::ifstream file(path);
    std::string session; std::getline(file, session); file.close();
    if (session.compare(0, root.size() + 1, root + "/") == 0) {
      std::ofstream clear(path, std::ios::trunc);
    }
  }
  ~Fixture() { recover_fixture(); std::filesystem::remove_all(root); }
  LocalExecutionOptions options(const std::string& runtime, const std::string& suffix = "session") {
    return {runtime, root + "/rootfs", root + "/" + suffix, 5000, 20};
  }
};
Node node() {
  auto result = test::node();
  utsname info{}; CHECK(uname(&info) == 0);
  result.platform.architecture = std::string(info.machine) == "aarch64" ? "arm64" : "amd64";
  return result;
}
Workload spec(const std::string& mode) {
  auto result = test::workload(); result.command = {"/fixture/" + mode}; return result;
}
void bundle_validation() {
  Fixture fixture;
  auto workload = spec("args");
  Allocator allocator; test::configure(allocator);
  Allocation allocation; CHECK(allocator.reserve(test::request(), allocation).ok());
  const auto bundle = fixture.root + "/bundle";
  CHECK(prepare_bundle(workload, allocation, fixture.root + "/rootfs", bundle).ok());
  std::ifstream input(bundle + "/config.json"); Json config; input >> config;
  CHECK(config["ociVersion"] == "1.0.2");
  CHECK(config["root"]["readonly"] == true);
  CHECK(config["process"]["args"] == workload.command);
  CHECK(config["process"]["noNewPrivileges"] == true);
  CHECK(config["process"]["capabilities"]["permitted"].empty());
  CHECK(config["linux"]["namespaces"].size() == 7);
  CHECK(config["linux"]["resources"]["cpu"]["quota"] == 100000);
  CHECK(config["linux"]["resources"]["memory"]["limit"] == 1024);
  CHECK(config["linux"]["uidMappings"][0]["hostID"] == getuid());
  CHECK(prepare_bundle(workload, allocation, fixture.root + "/rootfs", bundle).code == StatusCode::conflict);
  CHECK(prepare_bundle(workload, allocation, "/", fixture.root + "/unsafe").code == StatusCode::invalid_argument);
  auto wrong = allocation; wrong.request.generation = 2;
  CHECK(prepare_bundle(workload, wrong, fixture.root + "/rootfs", fixture.root + "/wrong").code == StatusCode::permission_denied);
  workload.resources.memory_bytes = std::numeric_limits<std::uint64_t>::max();
  allocation.request.requirements.resources = workload.resources;
  CHECK(prepare_bundle(workload, allocation, fixture.root + "/rootfs", fixture.root + "/overflow").code == StatusCode::invalid_argument);
}
void execution(const std::string& runtime) {
  Fixture fixture;
  auto workload = spec("args"); workload.replicas = 2;
  workload.command.push_back("space ' quote $(false) `literal`");
  auto options = fixture.options(runtime);
  const auto success = execute_local(workload, node(), options);
  CHECK(success.status.ok()); CHECK(!success.reservations_retained);
  CHECK(success.workers.size() == 2);
  CHECK(success.workers[0].process.output.find(workload.command[1]) != std::string::npos);
  CHECK(success.workers[0].cleanup.ok());
  CHECK(success.workload.tasks[0].state == TaskState::succeeded);
  CHECK(success.workload.tasks[1].state == TaskState::succeeded);
  CHECK(std::filesystem::is_empty(options.state_directory + "/runtime"));
  CHECK(execute_local(workload, node(), options).status.code == StatusCode::conflict);
  const auto noisy = execute_local(spec("noisy"), node(), fixture.options(runtime, "noisy"));
  CHECK(noisy.status.ok()); CHECK(noisy.workers[0].process.output.size() == kMaxCapturedOutput);
  CHECK(noisy.workers[0].process.dropped_output > 100000);
  CHECK(noisy.workers[0].process.dropped_errors > 100000);
  const auto failed = execute_local(spec("fail"), node(), fixture.options(runtime, "fail"));
  CHECK(!failed.status.ok()); CHECK(!failed.reservations_retained);
  CHECK(failed.workload.tasks[0].state == TaskState::failed);
  CHECK(failed.workers[0].process.exit_code == 7);
  auto retry_spec = spec("retry"); retry_spec.retry = {2, 1};
  const auto retried = execute_local(retry_spec, node(), fixture.options(runtime, "retry"));
  CHECK(retried.status.ok()); CHECK(retried.workers.size() == 2);
  CHECK(retried.workload.tasks[0].attempts.size() == 2);
  CHECK(retried.workload.tasks[0].attempts[0].state == AttemptState::failed);
  CHECK(retried.workload.tasks[0].attempts[1].state == AttemptState::succeeded);
}
void failures(const std::string& runtime) {
  Fixture fixture;
  for (const auto& mode : {"unknown", "bad-state", "duplicate-state"}) {
    const auto result = execute_local(spec(mode), node(), fixture.options(runtime, mode));
    CHECK(!result.status.ok()); CHECK(result.reservations_retained);
    CHECK(result.workload.tasks[0].state == TaskState::unknown);
    CHECK(result.workers[0].state == AttemptState::unknown);
    CHECK(!std::filesystem::is_empty(result.session_directory + "/runtime"));
    CHECK(result.workers[0].process.finished);
    const auto blocked = execute_local(spec("args"), node(), fixture.options(runtime, std::string(mode) + "-blocked"));
    CHECK(blocked.status.code == StatusCode::conflict); CHECK(blocked.workers.empty());
    fixture.recover_fixture();
  }
  const auto cleanup = execute_local(spec("delete-fail"), node(), fixture.options(runtime, "delete"));
  CHECK(!cleanup.status.ok()); CHECK(!cleanup.reservations_retained);
  CHECK(cleanup.workload.tasks[0].state == TaskState::succeeded);
  CHECK(!cleanup.workers[0].cleanup.ok());
  auto unavailable = fixture.options("/absent/runtime", "missing");
  CHECK(execute_local(spec("args"), node(), unavailable).workers.empty());
  CHECK(!std::filesystem::exists(unavailable.state_directory));
  auto unverified = node(); unverified.verified = false;
  const auto rejected = execute_local(spec("args"), unverified, fixture.options(runtime, "unverified"));
  CHECK(!rejected.status.ok()); CHECK(rejected.workers.empty()); CHECK(!rejected.reservations_retained);
  auto unsupported = spec("args"); unsupported.capabilities = {"gpu"};
  CHECK(execute_local(unsupported, node(), fixture.options(runtime, "gpu")).status.code == StatusCode::invalid_argument);
  auto unsafe = fixture.options(runtime, "rootfs/state");
  CHECK(execute_local(spec("args"), node(), unsafe).status.code == StatusCode::invalid_argument);
}
void cancellation(const std::string& runtime) {
  Fixture fixture;
  for (const auto& mode : {"wait", "ignore-term"}) {
    auto options = fixture.options(runtime, mode); options.timeout_millis = 100;
    const auto result = execute_local(spec(mode), node(), options);
    CHECK(!result.status.ok()); CHECK(!result.reservations_retained);
    CHECK(result.workload.tasks[0].state == TaskState::cancelled);
    CHECK(result.workers.size() == 1);
    if (std::string(mode) == "ignore-term") { CHECK(result.workers[0].process.signal == 9); }
  }
  auto workload = spec("wait"); workload.replicas = 2;
  const auto start = MonotonicClock::now();
  const auto signalled = execute_local(workload, node(), fixture.options(runtime, "signal"), [&] {
    return MonotonicClock::now() - start >= std::chrono::milliseconds(50);
  });
  CHECK(!signalled.reservations_retained); CHECK(signalled.workers.size() == 1);
  CHECK(signalled.workload.tasks[1].state == TaskState::cancelled);
  CHECK(signalled.workload.tasks[1].attempts.empty());
  auto options = fixture.options(runtime, "unstoppable"); options.timeout_millis = 100;
  const auto unknown = execute_local(spec("unstoppable"), node(), options);
  CHECK(unknown.reservations_retained); CHECK(unknown.workers[0].state == AttemptState::unknown);
  CHECK(unknown.workers[0].process.finished); fixture.recover_fixture();
  options = fixture.options(runtime, "control-timeout"); options.timeout_millis = 100;
  const auto control_timeout = execute_local(spec("state-timeout"), node(), options);
  CHECK(!control_timeout.status.ok()); CHECK(control_timeout.reservations_retained);
}
void crash_barrier(const std::string& runtime, const std::string& agent) {
  Fixture fixture;
  const auto local_node = node();
  const auto workload = spec("wait");
  const auto workload_path = fixture.root + "/workload.json";
  const auto node_path = fixture.root + "/node.json";
  std::ofstream(workload_path) << Json({{"apiVersion", kApiVersion}, {"kind", "Workload"},
    {"metadata", {{"name", workload.name}}},
    {"spec", {{"image", workload.image}, {"command", workload.command},
      {"resources", {{"cpuMillis", 1000}, {"memoryBytes", 1024}}}}}}).dump();
  std::ofstream(node_path) << Json({{"apiVersion", kApiVersion}, {"kind", "Node"},
    {"metadata", {{"name", local_node.id}}},
    {"spec", {{"resources", {{"cpuMillis", 4000}, {"memoryBytes", 4096}}},
      {"platform", {{"os", "linux"}, {"architecture", local_node.platform.architecture}}},
      {"verified", true}, {"runtimes", {"oci"}}, {"tenants", {"local"}}}}}).dump();
  const auto options = fixture.options(runtime);
  OciRuntime backend(runtime, options.state_directory + "/runtime");
  struct Cleanup {
    OciRuntime& backend;
    ~Cleanup() { backend.signal("omni-1", true); backend.remove("omni-1"); }
  } cleanup{backend};
  ChildProcess child;
  CHECK(child.start({agent, "run", workload_path, "--node", node_path,
    "--runtime", runtime, "--rootfs", options.rootfs, "--state-dir", options.state_directory}).ok());
  const auto deadline = MonotonicClock::now() + std::chrono::seconds(3);
  while (!std::filesystem::exists(options.state_directory + "/runtime/omni-1.json") && MonotonicClock::now() < deadline) {
    CHECK(child.poll().ok()); std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(std::filesystem::exists(options.state_directory + "/runtime/omni-1.json"));
  child.terminate(); CHECK(child.result().finished);
  const auto blocked = execute_local(spec("args"), local_node, fixture.options(runtime, "after-crash"));
  CHECK(blocked.status.code == StatusCode::conflict); CHECK(blocked.workers.empty());
  CHECK(backend.signal("omni-1", true).ok());
  CHECK(backend.remove("omni-1").ok());
  fixture.recover_fixture();
  CHECK(execute_local(spec("args"), local_node, fixture.options(runtime, "recovered")).status.ok());
}

}
int main(int argc, char** argv) {
  if (argc != 3) { return 2; }
  return test::run([&] { bundle_validation(); execution(argv[1]); failures(argv[1]); cancellation(argv[1]); crash_barrier(argv[1], argv[2]); });
}
