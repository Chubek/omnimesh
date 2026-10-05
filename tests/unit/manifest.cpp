#include "omnimesh/manifest.hpp"
#include "test_support.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>

using namespace omnimesh;
using Json = nlohmann::json;

namespace {

Json document() {
  return {{"apiVersion", kApiVersion}, {"kind", "Workload"},
          {"metadata", {{"name", "hello"}}},
          {"spec", {{"image", "sha256:" + std::string(64, 'a')},
                    {"command", {"/bin/echo", "hello"}}}}};
}

bool has_path(const ManifestResult<Workload>& result, const std::string& path) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const Diagnostic& entry) { return entry.path == path; });
}

void defaults_and_types() {
  const auto result = parse_workload(document().dump());
  CHECK(result.status.ok());
  CHECK(result.value.tenant == "local");
  CHECK(result.value.generation == 1);
  CHECK(result.value.replicas == 1);
  CHECK(result.value.resources == (Resources{1000, 134217728}));
  CHECK(result.value.retry.max_attempts == 1);

  for (const Json& invalid : {Json(0), Json(-1), Json(1.0), Json(257), Json(true), Json("2")}) {
    auto value = document();
    value["spec"]["replicas"] = invalid;
    const auto rejected = parse_workload(value.dump());
    CHECK(!rejected.status.ok());
    CHECK(has_path(rejected, "$.spec.replicas"));
  }
  auto value = document();
  value["spec"]["command"] = {"/bin/echo", "", "\u03c0"};
  CHECK(parse_workload(value.dump()).status.ok());
  value["spec"]["command"][0] = "";
  CHECK(has_path(parse_workload(value.dump()), "$.spec.command[0]"));
  value["spec"]["command"] = {"/bin/echo", std::string("a\0b", 3)};
  CHECK(has_path(parse_workload(value.dump()), "$.spec.command[1]"));
  value["spec"]["command"] = Json::array();
  CHECK(!parse_workload(value.dump()).status.ok());
  value["spec"]["command"] = {"/bin/echo", 7};
  CHECK(has_path(parse_workload(value.dump()), "$.spec.command[1]"));

  value = document();
  value["spec"]["resources"] = {{"cpuMillis", std::numeric_limits<std::uint64_t>::max()},
                                 {"memoryBytes", std::numeric_limits<std::uint64_t>::max()}};
  CHECK(parse_workload(value.dump()).value.resources.cpu_millis ==
        std::numeric_limits<std::uint64_t>::max());
  value["spec"]["resources"]["cpuMillis"] = 0;
  CHECK(has_path(parse_workload(value.dump()), "$.spec.resources.cpuMillis"));
  value["spec"]["resources"].erase("memoryBytes");
  CHECK(has_path(parse_workload(value.dump()), "$.spec.resources.memoryBytes"));
}

void admission_and_unknown_fields() {
  auto value = document();
  value["metadata"]["labels"] = Json::object();
  value["spec"]["mounts"] = Json::array();
  value["unexpected"] = true;
  const auto result = parse_workload(value.dump());
  CHECK(has_path(result, "$.metadata.labels"));
  CHECK(has_path(result, "$.spec.mounts"));
  CHECK(has_path(result, "$.unexpected"));
  value = document();
  value["apiVersion"] = "omnimesh.io/v2";
  CHECK(has_path(parse_workload(value.dump()), "$.apiVersion"));
  value = document();
  value["spec"]["privileged"] = true;
  CHECK(has_path(parse_workload(value.dump()), "$.spec.privileged"));
  value["spec"]["privileged"] = "false";
  CHECK(!parse_workload(value.dump()).status.ok());
  value["spec"]["privileged"] = false;
  value["spec"]["runtime"] = "kvm";
  CHECK(has_path(parse_workload(value.dump()), "$.spec.runtime"));
  value = document();
  for (const auto& image : std::vector<std::string>{"alpine:latest", "sha256:REPLACE_WITH_VERIFIED_DIGEST", "sha256:" + std::string(64, 'A')}) {
    value["spec"]["image"] = image;
    CHECK(has_path(parse_workload(value.dump()), "$.spec.image"));
  }
  value["spec"]["image"] = "registry.example/hello@sha256:" + std::string(64, 'b');
  CHECK(parse_workload(value.dump()).status.ok());
  value["spec"]["platform"] = {{"architecture", ""}};
  CHECK(has_path(parse_workload(value.dump()), "$.spec.platform.architecture"));
  value["spec"].erase("platform");
  value["spec"]["capabilities"] = {"zstd", "checkpoint"};
  CHECK(parse_workload(value.dump()).value.capabilities.front() == "checkpoint");
  value["spec"]["capabilities"] = {"checkpoint", "checkpoint"};
  CHECK(!parse_workload(value.dump()).status.ok());
  value = document();
  value["spec"]["retry"] = {{"maxAttempts", 17}};
  CHECK(has_path(parse_workload(value.dump()), "$.spec.retry.maxAttempts"));
}

void bounded_strict_json() {
  CHECK(!parse_workload("{\"kind\":\"Workload\",\"kind\":\"Workload\"}").status.ok());
  CHECK(!parse_workload("{\"spec\":{\"a\":1,\"a\":2}}").status.ok());
  CHECK(!parse_workload("{} {}").status.ok());
  CHECK(!parse_workload("// comment\n{}").status.ok());
  CHECK(!parse_workload("{\"x\":1,}").status.ok());
  CHECK(!parse_workload("apiVersion: omnimesh.io/v1alpha1").status.ok());
  CHECK(!parse_workload(std::string(40, '[') + "0" + std::string(40, ']')).status.ok());
  CHECK(!parse_workload(std::string(kMaxManifestBytes + 1, ' ')).status.ok());
  auto value = document().dump();
  value.replace(value.find("hello", value.find("spec")), 5, std::string(1, char(0xff)));
  CHECK(!parse_workload(value).status.ok());
  std::string tokens = "[";
  for (int i = 0; i < 17000; ++i) {
    tokens += i == 0 ? "0" : ",0";
  }
  tokens += "]";
  const auto result = parse_workload(tokens);
  CHECK(!result.status.ok());
  CHECK(result.diagnostics.front().message.find("token limit") != std::string::npos);
  CHECK(!parse_workload("{\"spec\":{\"replicas\":18446744073709551616}}").status.ok());
}

void nodes() {
  Json value{{"apiVersion", kApiVersion}, {"kind", "Node"},
             {"metadata", {{"name", "node-a"}}},
             {"spec", {{"resources", {{"cpuMillis", 2000}, {"memoryBytes", 4096}}},
                       {"platform", {{"os", "linux"}, {"architecture", "amd64"}}},
                       {"runtimes", {"oci"}}, {"tenants", {"local"}}, {"verified", true}}}};
  CHECK(parse_node(value.dump()).status.ok());
  value["spec"]["verified"] = false;
  CHECK(parse_node(value.dump()).status.ok());
  value["spec"]["tenants"] = Json::array();
  CHECK(!parse_node(value.dump()).status.ok());
  value["spec"]["tenants"] = {"local", "local"};
  CHECK(!parse_node(value.dump()).status.ok());
  value["spec"]["tenants"] = {"local"};
  value["spec"]["labels"] = {{"site", 5}};
  CHECK(!parse_node(value.dump()).status.ok());
  value["spec"].erase("labels");
  value["metadata"]["tenant"] = "local";
  CHECK(!parse_node(value.dump()).status.ok());
}

void artifact_inputs() {
  auto value = document();
  const Json input{{"name", "dataset"}, {"digest", "sha256:" + std::string(64, 'b')}};
  value["spec"]["inputs"] = Json::array({input});
  const auto parsed = parse_workload(value.dump());
  CHECK(parsed.status.ok());
  CHECK(parsed.value.inputs.size() == 1);
  CHECK(parse_workload(encode_workload(parsed.value)).value == parsed.value);
  auto changed = parsed.value;
  changed.inputs[0].digest = "sha256:" + std::string(64, 'c');
  CHECK(!(changed == parsed.value));
  value["spec"]["inputs"].push_back(input);
  CHECK(has_path(parse_workload(value.dump()), "$.spec.inputs[1].name"));
  for (const std::string name : {"../escape", "/absolute", "nested/file", "", "Upper", "."}) {
    value["spec"]["inputs"] = Json::array({input});
    value["spec"]["inputs"][0]["name"] = name;
    CHECK(has_path(parse_workload(value.dump()), "$.spec.inputs[0].name"));
  }
  value["spec"]["inputs"] = Json::array({input});
  value["spec"]["inputs"][0]["digest"] = "repo@sha256:" + std::string(64, 'b');
  CHECK(has_path(parse_workload(value.dump()), "$.spec.inputs[0].digest"));
  value["spec"]["inputs"] = Json::array({input});
  value["spec"]["inputs"][0]["destination"] = "/etc";
  CHECK(has_path(parse_workload(value.dump()), "$.spec.inputs[0].destination"));
  value["spec"]["inputs"][0].erase("destination");
  value["spec"]["inputs"][0].erase("digest");
  CHECK(has_path(parse_workload(value.dump()), "$.spec.inputs[0].digest"));
  for (const Json& invalid : {Json::object(), Json(nullptr), Json("input"), Json::array({7})}) {
    value["spec"]["inputs"] = invalid;
    CHECK(!parse_workload(value.dump()).status.ok());
  }
  value["spec"]["inputs"] = Json::array();
  for (std::size_t index = 0; index < kMaxWorkloadInputs; ++index) {
    auto entry = input;
    entry["name"] = "input-" + std::to_string(index);
    value["spec"]["inputs"].push_back(entry);
  }
  CHECK(parse_workload(value.dump()).status.ok());
  value["spec"]["inputs"].push_back(input);
  CHECK(has_path(parse_workload(value.dump()), "$.spec.inputs"));
}

} // namespace

int main() {
  return test::run([] {
    defaults_and_types();
    admission_and_unknown_fields();
    bounded_strict_json();
    nodes();
    artifact_inputs();
  });
}
