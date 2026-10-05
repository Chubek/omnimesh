#include "omnimesh/storage.hpp"
#include "test_support.hpp"
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace omnimesh;
namespace {
// Link-time syscall hooks keep deterministic fault injection out of production.
std::function<int(const char *, const char *)> rename_hook;
std::function<int(int, int)> flock_hook;
std::function<int(int)> fsync_hook;
}
extern "C" int __real_rename(const char *, const char *);
extern "C" int __real_flock(int, int);
extern "C" int __real_fsync(int);
extern "C" int __wrap_rename(const char *from, const char *to) {
  return rename_hook ? rename_hook(from, to) : __real_rename(from, to);
}
extern "C" int __wrap_flock(int fd, int operation) {
  return flock_hook ? flock_hook(fd, operation) : __real_flock(fd, operation);
}
extern "C" int __wrap_fsync(int fd) {
  return fsync_hook ? fsync_hook(fd) : __real_fsync(fd);
}

namespace {
struct Sandbox {
  std::string directory;
  Sandbox() {
    char path[] = "/tmp/omnimesh-journal-faults-XXXXXX";
    const auto created = mkdtemp(path);
    CHECK(created != nullptr);
    directory = created;
  }
  ~Sandbox() {
    rename_hook = {};
    flock_hook = {};
    fsync_hook = {};
    std::filesystem::remove_all(directory);
  }
  std::string path() const { return directory + "/journal"; }
  std::string bytes() const {
    std::ifstream stream(path(), std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  void no_temporaries() const {
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
      CHECK(entry.path().filename() == "journal");
    }
  }
};
void wait_success(pid_t child) {
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}
void check_competitor(const std::string &path) {
  const auto child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    Journal contender;
    _exit(contender.open(path).code == StatusCode::conflict ? 0 : 1);
  }
  wait_success(child);
}
std::vector<std::string> replay(Journal &journal) {
  std::vector<std::string> result;
  CHECK(journal.replay([&](std::uint8_t, const std::string &payload) {
    result.push_back(payload);
  }).ok());
  return result;
}
void ownership_across_rename() {
  Sandbox sandbox;
  Journal writer;
  CHECK(writer.open(sandbox.path()).ok());
  CHECK(writer.append(1, "original").ok());
  int renames = 0;
  rename_hook = [&](const char *from, const char *to) {
    ++renames;
    check_competitor(to);
    CHECK(__real_rename(from, to) == 0);
    check_competitor(to);
    return 0;
  };
  CHECK(writer.compact({{2, "replacement"}}).ok());
  CHECK(renames == 1);
  rename_hook = {};
  check_competitor(sandbox.path());
  CHECK(writer.append(3, "after").ok());
  CHECK(writer.close().ok());
  Journal next;
  CHECK(next.open(sandbox.path()).ok());
  CHECK(replay(next) == (std::vector<std::string>{"replacement", "after"}));
  sandbox.no_temporaries();
}
void stale_open_cannot_acquire_retired_inode() {
  Sandbox sandbox;
  Journal writer;
  CHECK(writer.open(sandbox.path()).ok());
  CHECK(writer.append(1, "old").ok());
  bool delayed = false;
  flock_hook = [&](int fd, int operation) {
    if (!delayed) {
      delayed = true;
      // The contender already opened the old inode. Retire it before allowing
      // the contender's flock to run, reproducing a descheduled open exactly.
      CHECK(writer.compact({{2, "new"}}).ok());
    }
    return __real_flock(fd, operation);
  };
  Journal contender;
  CHECK(contender.open(sandbox.path()).code == StatusCode::conflict);
  CHECK(delayed);
  flock_hook = {};
  CHECK(contender.append(3, "lost write").code == StatusCode::unavailable);
  CHECK(writer.close().ok());
  CHECK(contender.open(sandbox.path()).ok());
  CHECK(replay(contender) == (std::vector<std::string>{"new"}));
}
void prepublication_failures_preserve_original() {
  Sandbox sandbox;
  Journal writer;
  CHECK(writer.open(sandbox.path()).ok());
  CHECK(writer.append(1, "original").ok());
  const auto before = sandbox.bytes();
  CHECK(writer.compact({{1, ""}}).code == StatusCode::invalid_argument);
  CHECK(writer.compact({{1, std::string(kMaxRecordBytes + 1, 'x')}}).code ==
        StatusCode::invalid_argument);
  CHECK(writer.compact(std::vector<std::pair<std::uint8_t, std::string>>(
                           kMaxRecordsPerReplay + 1, {1, "x"})).code ==
        StatusCode::invalid_argument);
  rename_hook = [](const char *, const char *) {
    errno = EIO;
    return -1;
  };
  CHECK(!writer.compact({{2, "replacement"}}).ok());
  rename_hook = {};
  CHECK(sandbox.bytes() == before);
  check_competitor(sandbox.path());
  sandbox.no_temporaries();

  // Damage a fully written replacement immediately after its data sync. A
  // strict reread must reject both a short payload and a missing whole frame.
  struct stat original{};
  CHECK(stat(sandbox.path().c_str(), &original) == 0);
  for (const bool whole_frame : {false, true}) {
    bool damaged = false;
    fsync_hook = [&](int fd) {
      const int result = __real_fsync(fd);
      struct stat info{};
      CHECK(fstat(fd, &info) == 0);
      if (!damaged && S_ISREG(info.st_mode) &&
          info.st_ino != original.st_ino && info.st_size > 20) {
        damaged = true;
        CHECK(ftruncate(fd, whole_frame ? 20 : info.st_size - 1) == 0);
      }
      return result;
    };
    CHECK(!writer.compact({{2, "replacement"}}).ok());
    fsync_hook = {};
    CHECK(damaged);
    CHECK(sandbox.bytes() == before);
    sandbox.no_temporaries();
  }
  // A replacement fsync failure also leaves the live journal usable.
  bool failed = false;
  fsync_hook = [&](int fd) {
    struct stat info{};
    CHECK(fstat(fd, &info) == 0);
    if (S_ISREG(info.st_mode) && info.st_ino != original.st_ino) {
      failed = true;
      errno = ENOSPC;
      return -1;
    }
    return __real_fsync(fd);
  };
  CHECK(!writer.compact({{2, "replacement"}}).ok());
  fsync_hook = {};
  CHECK(failed);
  CHECK(sandbox.bytes() == before);
  sandbox.no_temporaries();
  CHECK(writer.append(3, "still usable").ok());
}
void failed_directory_sync_requires_recovery() {
  Sandbox sandbox;
  Journal writer;
  CHECK(writer.open(sandbox.path()).ok());
  CHECK(writer.append(1, "old").ok());
  bool renamed = false;
  rename_hook = [&](const char *from, const char *to) {
    const auto result = __real_rename(from, to);
    renamed = result == 0;
    return result;
  };
  fsync_hook = [&](int fd) {
    struct stat info{};
    CHECK(fstat(fd, &info) == 0);
    if (renamed && S_ISDIR(info.st_mode)) {
      errno = EIO;
      return -1;
    }
    return __real_fsync(fd);
  };
  CHECK(!writer.compact({{2, "new"}}).ok());
  CHECK(renamed);
  fsync_hook = {};
  rename_hook = {};
  check_competitor(sandbox.path());
  CHECK(writer.append(3, "unsafe").code == StatusCode::unavailable);
  CHECK(writer.compact({{3, "unsafe"}}).code == StatusCode::unavailable);
  CHECK(writer.replay([](std::uint8_t, const std::string &) {}).code ==
        StatusCode::unavailable);
  CHECK(writer.close().ok());
  // Recovery must re-establish directory durability even for an existing file.
  bool synced_directory = false;
  fsync_hook = [&](int fd) {
    struct stat info{};
    CHECK(fstat(fd, &info) == 0);
    synced_directory = synced_directory || S_ISDIR(info.st_mode);
    return __real_fsync(fd);
  };
  CHECK(writer.open(sandbox.path()).ok());
  fsync_hook = {};
  CHECK(synced_directory);
  CHECK(replay(writer) == (std::vector<std::string>{"new"}));
  sandbox.no_temporaries();
}
void original_sync_failure_requires_recovery() {
  Sandbox sandbox;
  Journal writer;
  CHECK(writer.open(sandbox.path()).ok());
  CHECK(writer.append(1, "old").ok());
  const auto before = sandbox.bytes();
  fsync_hook = [](int) {
    errno = EIO;
    return -1;
  };
  CHECK(!writer.compact({{2, "new"}}).ok());
  fsync_hook = {};
  CHECK(sandbox.bytes() == before);
  CHECK(writer.append(3, "unsafe").code == StatusCode::unavailable);
  CHECK(writer.compact({}).code == StatusCode::unavailable);
  check_competitor(sandbox.path());
  sandbox.no_temporaries();
  CHECK(writer.close().ok());
  CHECK(writer.open(sandbox.path()).ok());
  CHECK(replay(writer) == (std::vector<std::string>{"old"}));
}
void corruption_cannot_be_compacted_away() {
  Sandbox sandbox;
  Journal writer;
  CHECK(writer.open(sandbox.path()).ok());
  CHECK(writer.append(1, "original").ok());
  {
    std::fstream stream(sandbox.path(), std::ios::in | std::ios::out | std::ios::binary);
    stream.seekp(-1, std::ios::end);
    stream.put('X');
  }
  const auto damaged = sandbox.bytes();
  CHECK(!writer.compact({{2, "replacement"}}).ok());
  CHECK(sandbox.bytes() == damaged);
  CHECK(writer.compact({}).code == StatusCode::unavailable);
  CHECK(writer.append(3, "unsafe").code == StatusCode::unavailable);
  sandbox.no_temporaries();
}
void empty_snapshot_and_orphan_preservation() {
  Sandbox sandbox;
  Journal writer;
  CHECK(writer.open(sandbox.path()).ok());
  CHECK(writer.append(1, "old").ok());
  // The old fixed temporary name may even be held by another Journal. The new
  // implementation must neither unlink it nor overwrite its contents.
  Journal orphan;
  const auto orphan_path = sandbox.path() + ".compact";
  CHECK(orphan.open(orphan_path).ok());
  CHECK(orphan.append(1, "preserved").ok());
  CHECK(writer.compact({}).ok());
  CHECK(writer.stats().sequence == 0);
  CHECK(writer.stats().bytes == 20);
  CHECK(replay(writer).empty());
  CHECK(writer.append(2, "first after empty snapshot").ok());
  CHECK(writer.stats().sequence == 1);
  CHECK(writer.compact({{3, "next snapshot"}}).ok());
  CHECK(replay(orphan) == (std::vector<std::string>{"preserved"}));
  CHECK(orphan.close().ok());
  CHECK(std::filesystem::remove(orphan_path));
  sandbox.no_temporaries();
}
void process_death_at_publication_boundaries() {
  for (int stage = 0; stage < 3; ++stage) {
    Sandbox sandbox;
    {
      Journal writer;
      CHECK(writer.open(sandbox.path()).ok());
      CHECK(writer.append(1, "old").ok());
    }
    const auto child = fork();
    CHECK(child >= 0);
    if (child == 0) {
      Journal writer;
      if (!writer.open(sandbox.path()).ok()) {
        _exit(1);
      }
      rename_hook = [stage](const char *from, const char *to) {
        if (stage == 0) {
          _exit(0);
        }
        if (__real_rename(from, to) != 0) {
          _exit(1);
        }
        if (stage == 1) {
          _exit(0);
        }
        return 0;
      };
      if (!writer.compact({{2, "new"}}).ok()) {
        _exit(1);
      }
      _exit(0); // No destructors: emulate death with the installed lock held.
    }
    wait_success(child);
    Journal recovered;
    CHECK(recovered.open(sandbox.path()).ok());
    CHECK(replay(recovered) ==
          (std::vector<std::string>{stage == 0 ? "old" : "new"}));
    CHECK(recovered.append(3, "after recovery").ok());
  }
}
} // namespace

int main() {
  return test::run([] {
    ownership_across_rename();
    stale_open_cannot_acquire_retired_inode();
    prepublication_failures_preserve_original();
    failed_directory_sync_requires_recovery();
    original_sync_failure_requires_recovery();
    corruption_cannot_be_compacted_away();
    empty_snapshot_and_orphan_preservation();
    process_death_at_publication_boundaries();
  });
}
