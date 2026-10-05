#include "omnimesh/process.hpp"
#include "test_support.hpp"
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
using namespace omnimesh;
int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--child") {
    if (std::getenv("OMNIMESH_PROCESS_TEST_SECRET")) { return 99; }
    if (fcntl(std::stoi(argv[3]), F_GETFD) != -1) { return 98; }
    std::cout << argv[2] << std::string(200000, 'o');
    std::cerr << std::string(200000, 'e');
    return 17;
  }
  return test::run([&] {
    setenv("OMNIMESH_PROCESS_TEST_SECRET", "must-not-inherit", 1);
    ChildProcess invalid;
    CHECK(invalid.start({"relative"}).code == StatusCode::invalid_argument);
    CHECK(invalid.start({"/absent/omnimesh-runtime"}).code == StatusCode::unavailable);
    CHECK(invalid.start({"/bin/echo", std::string("a\0b", 3)}).code == StatusCode::invalid_argument);
    ChildProcess child;
    const auto executable = std::filesystem::canonical(argv[0]).string();
    const std::string literal = "$(touch /tmp/omnimesh-should-never-exist); `false`";
    const int caller_descriptor = open("/dev/null", O_RDONLY); CHECK(caller_descriptor >= 3);
    CHECK(child.start({executable, "--child", literal, std::to_string(caller_descriptor)}).ok());
    close(caller_descriptor);
    CHECK(child.start({executable}).code == StatusCode::conflict);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!child.result().finished && std::chrono::steady_clock::now() < deadline) {
      CHECK(child.poll().ok()); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(child.result().finished && child.result().exited && child.result().exit_code == 17);
    CHECK(child.result().output.substr(0, literal.size()) == literal);
    CHECK(child.result().output.size() == kMaxCapturedOutput);
    CHECK(child.result().errors.size() == kMaxCapturedOutput);
    CHECK(child.result().dropped_output == literal.size() + 200000 - kMaxCapturedOutput);
    CHECK(child.result().dropped_errors == 200000 - kMaxCapturedOutput);
    ChildProcess sleeper;
    CHECK(sleeper.start({"/bin/sleep", "10"}).ok());
    sleeper.terminate();
    CHECK(sleeper.result().finished && !sleeper.result().exited && sleeper.result().signal == 9);
    CHECK(sleeper.poll().ok());
  });
}
