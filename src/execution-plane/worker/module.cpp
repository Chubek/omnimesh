#include "omnimesh/process.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace omnimesh {
namespace {
void close_fd(int& fd) { if (fd >= 0) { close(fd); fd = -1; } }
void drain(int& fd, std::string& captured, std::uint64_t& dropped) {
  char bytes[4096];
  for (int reads = 0; fd >= 0 && reads < 32; ++reads) {
    const auto count = read(fd, bytes, sizeof(bytes));
    if (count > 0) {
      const auto keep = std::min(static_cast<std::size_t>(count), kMaxCapturedOutput - captured.size());
      captured.append(bytes, keep);
      dropped += static_cast<std::size_t>(count) - keep;
    } else if (count == 0) { close_fd(fd); }
    else if (errno != EINTR) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) { close_fd(fd); }
      break;
    }
  }
}
}
ChildProcess::~ChildProcess() {
  terminate(); close_fd(output_fd_); close_fd(error_fd_);
}
Status ChildProcess::start(const std::vector<std::string>& arguments) {
  if (started_) { return {StatusCode::conflict, "child process already started"}; }
  if (arguments.empty() || arguments[0].empty() || arguments[0][0] != '/' || arguments.size() > 512) {
    return {StatusCode::invalid_argument, "executable must be absolute; at most 512 arguments"};
  }
  for (const auto& argument : arguments) {
    if (argument.size() > 8192 || argument.find('\0') != std::string::npos) {
      return {StatusCode::invalid_argument, "invalid child argument"};
    }
  }
  int out[2]{-1, -1}, err[2]{-1, -1};
  if (pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0) {
    close_fd(out[0]); close_fd(out[1]); close_fd(err[0]); close_fd(err[1]);
    return {StatusCode::resource_exhausted, "cannot create child output pipes"};
  }
  for (auto* fd : {&out[0], &out[1], &err[0], &err[1]}) {
    if (*fd < 3) {
      const int moved = fcntl(*fd, F_DUPFD_CLOEXEC, 3);
      if (moved < 0) {
        close_fd(out[0]); close_fd(out[1]); close_fd(err[0]); close_fd(err[1]);
        return {StatusCode::resource_exhausted, "cannot reserve child descriptors"};
      }
      close(*fd); *fd = moved;
    }
  }
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  int code = posix_spawn_file_actions_init(&actions);
  if (code != 0) {
    close_fd(out[0]); close_fd(out[1]); close_fd(err[0]); close_fd(err[1]);
    return {StatusCode::internal, "cannot initialize child file actions"};
  }
  code = posix_spawnattr_init(&attributes);
  if (code != 0) {
    posix_spawn_file_actions_destroy(&actions);
    close_fd(out[0]); close_fd(out[1]); close_fd(err[0]); close_fd(err[1]);
    return {StatusCode::internal, "cannot initialize child attributes"};
  }
  auto check = [&](int value) { if (code == 0) { code = value; } };
  check(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
  check(posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO));
  check(posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO));
  // Close caller-owned descriptors too: they may carry credentials or sockets.
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 34)
  check(posix_spawn_file_actions_addclosefrom_np(&actions, 3));
#else
  for (int fd : {out[0], out[1], err[0], err[1]}) { check(posix_spawn_file_actions_addclose(&actions, fd)); }
#endif
  sigset_t empty, defaults;
  sigemptyset(&empty); sigfillset(&defaults);
  check(posix_spawnattr_setsigmask(&attributes, &empty));
  check(posix_spawnattr_setsigdefault(&attributes, &defaults));
  check(posix_spawnattr_setpgroup(&attributes, 0));
  check(posix_spawnattr_setflags(&attributes,
        POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));
  std::vector<char*> argv;
  for (const auto& argument : arguments) { argv.push_back(const_cast<char*>(argument.c_str())); }
  argv.push_back(nullptr);
  char path[] = "PATH=/usr/bin:/bin", locale[] = "LANG=C";
  char* environment[]{path, locale, nullptr};
  pid_t child = -1;
  if (code == 0) {
    code = posix_spawn(&child, arguments[0].c_str(), &actions, &attributes, argv.data(), environment);
  }
  posix_spawnattr_destroy(&attributes); posix_spawn_file_actions_destroy(&actions);
  close_fd(out[1]); close_fd(err[1]);
  if (code != 0) {
    close_fd(out[0]); close_fd(err[0]);
    return {StatusCode::unavailable, "cannot spawn selected runtime: " + std::string(std::strerror(code))};
  }
  pid_ = child; started_ = true;
  output_fd_ = out[0]; error_fd_ = err[0];
  if (fcntl(output_fd_, F_SETFL, O_NONBLOCK) < 0 || fcntl(error_fd_, F_SETFL, O_NONBLOCK) < 0) {
    terminate();
    return {StatusCode::internal, "cannot configure nonblocking output"};
  }
  return Status::Ok();
}
Status ChildProcess::poll() {
  if (!started_) { return {StatusCode::conflict, "child process has not started"}; }
  drain(output_fd_, result_.output, result_.dropped_output);
  drain(error_fd_, result_.errors, result_.dropped_errors);
  if (pid_ < 0) { return Status::Ok(); }
  int status = 0;
  const auto waited = waitpid(pid_, &status, WNOHANG);
  if (waited < 0 && errno != EINTR) { return {StatusCode::internal, "cannot observe child process"}; }
  if (waited == pid_) {
    pid_ = -1; result_.finished = true; result_.exited = WIFEXITED(status);
    result_.exit_code = result_.exited ? WEXITSTATUS(status) : 0;
    result_.signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    drain(output_fd_, result_.output, result_.dropped_output);
    drain(error_fd_, result_.errors, result_.dropped_errors);
  }
  return Status::Ok();
}
void ChildProcess::terminate() noexcept {
  if (pid_ < 0) { return; }
  kill(-pid_, SIGKILL);
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = waitpid(pid_, &status, WNOHANG);
    if (waited == pid_ || (waited < 0 && errno == ECHILD)) {
      pid_ = -1; result_.finished = true; break;
    }
    if (waited < 0 && errno != EINTR) { break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  result_.exited = false; result_.signal = SIGKILL;
}
} // namespace omnimesh
