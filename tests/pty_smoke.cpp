// End-to-end smoke test through a real pseudo terminal.
//
// Usage: avionix-pty-smoke <program> <expected-text> <keys-to-send>
//
// Starts <program> attached to a pseudo terminal (ConPTY on Windows, openpty
// on POSIX), waits for <expected-text> to appear in its output, resizes the
// terminal, sends <keys-to-send>, and requires the program to exit with
// status 0 within a few seconds. This exercises terminal_session setup,
// input decoding, rendering, resize handling, and teardown together.
//
// This harness is a plain C++ program: it drives the platform PTY APIs
// directly and does not use Avionix.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <csignal>
#if defined(__APPLE__)
// Declared in <util.h>, which cross-compilation SDKs do not always ship.
extern "C" int forkpty(int* master, char* name, struct termios* term,
                       struct winsize* size);
#else
#include <pty.h>
#endif
#endif

namespace {

using namespace std::chrono_literals;

class output_log {
 public:
  void append(const char* data, std::size_t n) {
    std::scoped_lock lock{mutex_};
    text_.append(data, n);
  }
  bool contains(std::string_view needle) const {
    std::scoped_lock lock{mutex_};
    return text_.find(needle) != std::string::npos;
  }
  // The last `n` bytes with control characters made visible.
  std::string tail(std::size_t n) const {
    std::scoped_lock lock{mutex_};
    const std::string_view recent =
        std::string_view{text_}.substr(text_.size() > n ? text_.size() - n : 0);
    std::string out;
    for (const char c : recent) {
      if (c == '\x1b') {
        out += "\\e";
      } else if (static_cast<unsigned char>(c) < 0x20 && c != '\n') {
        out += '^';
        out += static_cast<char>(c + 0x40);
      } else {
        out += c;
      }
    }
    return out;
  }
  std::size_t size() const {
    std::scoped_lock lock{mutex_};
    return text_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::string text_;
};

template <typename Predicate>
bool wait_for(Predicate done, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return done();
}

int fail(std::string_view what) {
  std::println(stderr, "pty-smoke: FAIL: {}", what);
  return 1;
}

#if defined(_WIN32)

int run(const char* program, std::string_view expected, std::string_view keys) {
  HANDLE input_read = nullptr, input_write = nullptr, output_read = nullptr,
         output_write = nullptr;
  if (!CreatePipe(&input_read, &input_write, nullptr, 0) ||
      !CreatePipe(&output_read, &output_write, nullptr, 0)) {
    return fail("CreatePipe");
  }
  HPCON console = nullptr;
  if (FAILED(
          CreatePseudoConsole(COORD{80, 24}, input_read, output_write, 0, &console))) {
    return fail("CreatePseudoConsole (requires Windows 10 1809 or later)");
  }
  CloseHandle(input_read);
  CloseHandle(output_write);

  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::string attr_storage(attr_size, '\0');
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
  InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console,
                            sizeof(console), nullptr, nullptr);

  STARTUPINFOEXA startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = attrs;
  // Without this the child inherits this process's (possibly redirected)
  // std handles instead of attaching them to the pseudo console.
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  PROCESS_INFORMATION process{};
  std::string command = std::string{"\""} + program + "\"";
  if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE,
                      EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                      &startup.StartupInfo, &process)) {
    return fail("CreateProcess");
  }

  output_log log;
  std::atomic<bool> reading{true};
  std::thread reader{[&] {
    char buffer[4096];
    DWORD n = 0;
    while (reading && ReadFile(output_read, buffer, sizeof(buffer), &n, nullptr) &&
           n > 0) {
      log.append(buffer, n);
    }
  }};

  const auto send = [&](std::string_view bytes) {
    DWORD written = 0;
    WriteFile(input_write, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
              nullptr);
  };

  int status = 0;
  if (!wait_for([&] { return log.contains(expected); }, 5s)) {
    status = fail("expected text never appeared");
  } else {
    ResizePseudoConsole(console, COORD{100, 30});
    std::this_thread::sleep_for(300ms);
    send(keys);
    if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
      TerminateProcess(process.hProcess, 99);
      status = fail("program did not exit after input");
    } else {
      DWORD code = 1;
      GetExitCodeProcess(process.hProcess, &code);
      if (code != 0) status = fail("program exited with non-zero status");
    }
  }

  ClosePseudoConsole(console);  // unblocks the reader
  reading = false;
  CloseHandle(input_write);
  reader.join();
  CloseHandle(output_read);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  DeleteProcThreadAttributeList(attrs);
  if (status == 0) std::println("pty-smoke: ok ({} bytes of output)", log.size());
  return status;
}

#else

int run(const char* program, std::string_view expected, std::string_view keys) {
  int master = -1;
  winsize ws{24, 80, 0, 0};
  const pid_t child = forkpty(&master, nullptr, nullptr, &ws);
  if (child < 0) return fail("forkpty");
  if (child == 0) {
    setenv("TERM", "xterm-256color", 1);
    execl(program, program, static_cast<char*>(nullptr));
    _exit(127);
  }

  output_log log;
  std::atomic<bool> reading{true};
  std::thread reader{[&] {
    char buffer[4096];
    while (reading) {
      pollfd pfd{master, POLLIN, 0};
      if (poll(&pfd, 1, 50) <= 0) continue;
      const ssize_t n = read(master, buffer, sizeof(buffer));
      if (n <= 0) break;
      log.append(buffer, static_cast<std::size_t>(n));
    }
  }};

  int status = 0;
  if (!wait_for([&] { return log.contains(expected); }, 5s)) {
    status = fail("expected text never appeared");
  } else {
    winsize bigger{30, 100, 0, 0};
    ioctl(master, TIOCSWINSZ, &bigger);
    kill(child, SIGWINCH);
    std::this_thread::sleep_for(300ms);
    (void)write(master, keys.data(), keys.size());
    int wstatus = 0;
    const bool exited =
        wait_for([&] { return waitpid(child, &wstatus, WNOHANG) == child; }, 5s);
    if (!exited) {
      kill(child, SIGKILL);
      waitpid(child, &wstatus, 0);
      status = fail("program did not exit after input");
    } else if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
      std::println(
          stderr,
          "pty-smoke: exited={} status={} signaled={} signal={} output tail: {}",
          WIFEXITED(wstatus), WEXITSTATUS(wstatus), WIFSIGNALED(wstatus),
          WIFSIGNALED(wstatus) ? WTERMSIG(wstatus) : 0, log.tail(400));
      status = fail("program exited with non-zero status");
    } else if (!log.contains("\x1b[?1049l")) {
      status = fail("terminal was not restored (no alternate-screen exit)");
    }
  }
  reading = false;
  reader.join();
  close(master);
  if (status == 0) std::println("pty-smoke: ok ({} bytes of output)", log.size());
  return status;
}

#endif

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::println(stderr, "usage: avionix-pty-smoke <program> <expected-text> <keys>");
    return 2;
  }
  return run(argv[1], argv[2], argv[3]);
}
