// Child writer process for the store's cross-process tests. It is started by
// lcm_store_tests only; the parent terminates exactly the process it created.
//
//   lcm_store_child hold-lock           <db> <workspace> <wal|rollback> <ready-file>
//   lcm_store_child crash-before-commit <db> <workspace> <wal|rollback> <ready-file>
//   lcm_store_child publish             <db> <workspace> <wal|rollback> <ready-file> <busy-ms>
//   lcm_store_child hold-spool          <db> <workspace> <wal|rollback> <ready-file>
//   lcm_store_child crash-spooled-publish <db> <workspace> <wal|rollback> <ready-file>
//
// hold-spool and crash-spooled-publish write "<state>\n<spool path>" to the ready file.
//
// Exit codes: 0 committed, 2 writer lock unavailable, 3 publication failed, 4 usage/other error.
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <string>
#include <thread>

#include "lcm/source.hpp"
#include "lcm/store/store.hpp"
#include "store_fixture.hpp"

using namespace lcm;
using namespace lcm::store;

namespace {

void write_ready(const std::string& path, const std::string& text) {
  std::ofstream out(path_from_utf8(path), std::ios::binary);
  out << text;
}

[[noreturn]] void sleep_forever() {
  for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 6) return 4;
  const std::string mode = argv[1];
  const std::string db = argv[2];
  const std::string workspace = argv[3];
  const std::string journal = argv[4];
  const std::string ready = argv[5];
  try {
    StoreOptions options;
    options.journal = journal == "rollback" ? JournalMode::rollback : JournalMode::wal;
    if (mode == "publish" && argc >= 7) options.busy_timeout = std::chrono::milliseconds(std::stoi(argv[6]));
    Store store = Store::open(path_from_utf8(db), path_from_utf8(workspace), options);
    if (mode == "hold-spool") {  // a live spool, without the writer lock
      ContributionSpool spool(path_from_utf8(db).parent_path());
      if (!spool.add(test::shared_tu("S", "src/s.cpp", "cmd-s")).empty()) return 4;
      write_ready(ready, "holding\n" + path_to_utf8_generic(spool.path()));
      sleep_forever();
    }
    auto lock = store.acquire_writer(std::chrono::seconds(5));
    if (!lock) {
      write_ready(ready, "lock-unavailable");
      return 2;
    }
    if (mode == "hold-lock") {
      write_ready(ready, "holding");
      sleep_forever();
    }
    store.ensure_profile(*lock, "default");
    Batch batch;
    batch.profile = "default";
    test::TuOptions v2;
    v2.body_seed = "body-2";
    batch.install.push_back(test::shared_tu("B", "src/b.cpp", "cmd-b-2", v2));
    if (mode == "crash-before-commit") {
      store.publish(*lock, batch, [&](PublishPhase phase) {
        if (phase == PublishPhase::before_commit) {
          write_ready(ready, "inside-transaction");
          sleep_forever();
        }
      });
      return 4;  // unreachable unless the observer did not run
    }
    if (mode == "crash-spooled-publish") {
      ContributionSpool spool(path_from_utf8(db).parent_path());
      if (!spool.add(batch.install.front()).empty()) return 4;
      Batch spooled;
      spooled.profile = "default";
      spooled.spool = &spool;
      spooled.spooled = {0};
      store.publish(*lock, spooled, [&](PublishPhase phase) {
        if (phase == PublishPhase::old_removed) {
          write_ready(ready, "inside-transaction\n" + path_to_utf8_generic(spool.path()));
          sleep_forever();
        }
      });
      return 4;  // unreachable unless the observer did not run
    }
    if (mode == "publish") {
      const PublishResult r = store.publish(*lock, batch);
      write_ready(ready, std::string(to_string(r.outcome)) + "\n" + r.message);
      return r.outcome == PublishOutcome::committed ? 0 : 3;
    }
  } catch (const std::exception& e) {
    write_ready(ready, std::string("error\n") + e.what());
    return 4;
  }
  return 4;
}
