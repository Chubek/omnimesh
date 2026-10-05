#include "omnimesh/execution.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sys/utsname.h>
#include <unistd.h>

int main() {
  const auto runtime = std::getenv("OMNIMESH_TEST_OCI_RUNTIME");
  const auto rootfs = std::getenv("OMNIMESH_TEST_OCI_ROOTFS");
  if (!runtime || !rootfs) {
    std::cout << "SKIP: set OMNIMESH_TEST_OCI_RUNTIME and OMNIMESH_TEST_OCI_ROOTFS; requires Linux user namespaces and delegated cgroups\n";
    return 77;
  }
  char path[] = "/tmp/omnimesh-real-oci-XXXXXX";
  const auto directory = mkdtemp(path);
  if (!directory) { return 1; }
  utsname info{}; uname(&info);
  omnimesh::Workload workload;
  workload.name = "real-oci";
  workload.image = "sha256:" + std::string(64, 'a'); // Administrator-supplied rootfs; no image verification.
  workload.command = {"/bin/true"};
  workload.resources = {1000, 64 * 1024 * 1024};
  omnimesh::Node node;
  node.id = "real-oci"; node.capacity = workload.resources;
  node.platform.architecture = std::string(info.machine) == "aarch64" ? "arm64" : "amd64";
  node.verified = true; node.runtimes = {"oci"}; node.tenants = {"local"};
  const auto result = omnimesh::execute_local(workload, node,
    {runtime, rootfs, std::string(directory) + "/session", 10000, 1000});
  if (!result.status.ok()) {
    std::cerr << result.status.message << "; preserved state: " << directory << '\n';
    for (const auto& worker : result.workers) { std::cerr << worker.process.errors << '\n'; }
    return 1;
  }
  std::filesystem::remove_all(directory);
  return 0;
}
