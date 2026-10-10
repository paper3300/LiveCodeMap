// Internal: serializes Clang's driver phase across concurrent analyses.
#pragma once

#include <mutex>

namespace lcm::analyzer::detail {

// Building a compiler invocation through Clang's driver (option parsing and the
// MSVC toolchain detection it performs) is not safe on several threads at once:
// with two concurrent analyses the process heap was corrupted in about one run
// out of twelve, and in none once this phase alone was serialized. Every
// front-end run therefore holds this process-wide lock from before
// ToolInvocation::run() until its action's PrepareToExecuteAction(), which Clang
// calls once the CompilerInvocation exists. Parsing, the prefix captures and fact
// building still run in parallel.
class DriverPhase {
 public:
  DriverPhase() {
    mutex().lock();
    current() = this;
  }
  // Also releases when the driver failed before the action was reached.
  ~DriverPhase() { release(); }
  DriverPhase(const DriverPhase&) = delete;
  DriverPhase& operator=(const DriverPhase&) = delete;

  // Ends the driver phase of the invocation running on this thread, if any.
  static void end() {
    if (DriverPhase* phase = current()) phase->release();
  }

 private:
  void release() {
    if (!held_) return;
    held_ = false;
    current() = nullptr;
    mutex().unlock();
  }
  static std::mutex& mutex() {
    static std::mutex m;
    return m;
  }
  static DriverPhase*& current() {
    thread_local DriverPhase* phase = nullptr;
    return phase;
  }

  bool held_ = true;
};

}  // namespace lcm::analyzer::detail
