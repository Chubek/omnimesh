// Protocol fixture only; this provides no container isolation or conformance.
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>
#include <unistd.h>
using Json = nlohmann::json;
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
Json read(const std::string &path) {
  std::ifstream file(path);
  Json value;
  file >> value;
  return value;
}
void write(const std::string &path, const Json &value) {
  const auto temporary = path + "." + std::to_string(getpid());
  std::ofstream file(temporary);
  file << value.dump();
  file.close();
  std::filesystem::rename(temporary, path);
}
int main(int argc, char **argv) {
  try {
    if (argc < 5 || std::string(argv[1]) != "--root") {
      return 2;
    }
    const std::string root = argv[2], command = argv[3];
    const std::string id = command == "kill" ? argv[5] : argv[argc - 1];
    const auto path = root + "/" + id + ".json";
    if (command == "run") {
      if (argc != 8 || std::string(argv[4]) != "--keep" ||
          std::string(argv[5]) != "--bundle") {
        return 2;
      }
      const auto config = read(std::string(argv[6]) + "/config.json");
      const auto args = config["process"]["args"];
      const std::string mode = args[0];
      Json state{
          {"id", id}, {"status", "running"}, {"pid", getpid()}, {"mode", mode}};
      write(path, state);
      std::signal(SIGTERM, stop);
      if (mode == "/fixture/noisy") {
        std::cout << std::string(200000, 'x');
        std::cerr << std::string(200000, 'e');
        std::cout.flush();
        std::cerr.flush();
      } else {
        std::cout << "fixture started\n";
        std::cout.flush();
      }
      if (mode == "/fixture/args") {
        std::cout << args.dump();
      }
      if (mode == "/fixture/image") {
        // Observe the staged tree through the actual bundle root, without
        // pretending that this protocol fixture provides container isolation.
        const std::string rootfs = config["root"]["path"];
        std::ifstream payload(rootfs + "/bin/hello");
        if (!payload || config["root"]["readonly"] != true) {
          return 1;
        }
        std::cout << payload.rdbuf();
      }
      for (int i = 0; i < 30; ++i) {
        if (stopped && mode != "/fixture/ignore-term" &&
            mode != "/fixture/unstoppable") {
          break;
        }
        if (mode == "/fixture/wait" || mode == "/fixture/ignore-term" ||
            mode == "/fixture/unstoppable") {
          --i;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (mode != "/fixture/unknown" && mode != "/fixture/bad-state" &&
          mode != "/fixture/duplicate-state") {
        state["status"] = "stopped";
        state["pid"] = 0;
        write(path, state);
      }
      if (mode == "/fixture/fail" ||
          (mode == "/fixture/retry" && id == "omni-1")) {
        return 7;
      }
      return 0;
    }
    const auto state = read(path);
    const std::string mode = state["mode"];
    if (command == "state") {
      if (mode == "/fixture/state-timeout") {
        std::this_thread::sleep_for(std::chrono::seconds(10));
      }
      if (mode == "/fixture/bad-state") {
        std::cout << "{invalid";
        return 0;
      }
      if (mode == "/fixture/duplicate-state") {
        std::cout
            << "{\"id\":\"" << id
            << "\",\"status\":\"running\",\"status\":\"stopped\",\"pid\":0}";
        return 0;
      }
      std::cout << state.dump();
      return 0;
    }
    if (command == "kill") {
      if (argc != 7 || std::string(argv[4]) != "--all") {
        return 2;
      }
      if (mode == "/fixture/unstoppable") {
        return 1;
      }
      const bool force = std::string(argv[6]) == "KILL";
      if (force) {
        auto killed = state;
        killed["status"] = "stopped";
        killed["pid"] = 0;
        write(path, killed);
      }
      return kill(state["pid"].get<int>(), force ? SIGKILL : SIGTERM) == 0 ? 0
                                                                           : 1;
    }
    if (command == "delete") {
      if (mode == "/fixture/delete-fail") {
        return 1;
      }
      if (state["status"] != "stopped") {
        return 1;
      }
      std::filesystem::remove(path);
      return 0;
    }
    return 2;
  } catch (const std::exception &) {
    return 1;
  }
}
