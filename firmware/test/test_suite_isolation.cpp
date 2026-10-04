#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <unity.h>

#include "test_suite_isolation.h"

// Suite isolation for a single Unity binary.
//
// WHY THIS EXISTS
//
// Every firmware suite runs in one process. Anything a suite leaves behind -
// a static, a file, a global counter - is visible to whatever runs after it, and a
// test that then passes for the wrong reason is worse than one that fails. The
// failure mode is silent and order-dependent, which is the worst combination a test
// suite has.
//
// This file does not make that impossible. It makes it DETECTABLE, which is the part
// that is actually achievable without splitting twenty-eight suites into twenty-eight
// binaries: a suite that touches a shared resource has to say so, and the check at
// the end of the run fails if it did not.
//
// WHAT IS ACTUALLY ENFORCED
//
//   1. Each suite that uses a file declares a private data directory. Two suites
//      declaring the same directory is caught at the end of the run, instead of
//      being a race somebody has to notice.
//   2. A suite that mutates process-wide state registers it, so a reader can see
//      what it touched.
//   3. The run ends with a report of every declared directory and mutation, so the
//      isolation story is inspectable from the test output rather than from a
//      convention nobody checks.
//
// ADOPTION, STATED PLAINLY
//
// Rule 2 is in use: the three file-scope mutable statics in the suite declare themselves.
// Rule 1 has no callers, because no suite shares a filesystem today - each one that writes
// files builds its own in-memory or temporary store. Saying so here is the point; a
// mechanism that claimed coverage it does not have would be the same defect as the one it
// exists to catch.

namespace {

struct Isolation {
  static Isolation& instance() {
    static Isolation state;
    return state;
  }

  void claim(const char* suite, const char* directory) {
    std::string path = directory ? directory : "(null)";
    auto existing = owners_.find(path);
    if (existing != owners_.end()) {
      // The same suite reaching for its own directory again is not a clash. Two
      // DIFFERENT suites sharing one is an order-dependent race, and treating the
      // first case as a clash would train readers to ignore the warning.
      if (existing->second != suite) {
        clashes_.push_back(std::string(existing->second) + " and " + suite +
                           " both use " + path);
      }
      return;
    }
    owners_[path] = suite;
    directories_.push_back(path);
  }

  void noteMutation(const char* suite, const char* what) {
    mutations_.push_back(std::string(suite) + ": " + what);
  }

  size_t directoryCount() const { return directories_.size(); }
  size_t mutationCount() const { return mutations_.size(); }
  size_t clashCount() const { return clashes_.size(); }

  const std::vector<std::string>& clashes() const { return clashes_; }
  const std::vector<std::string>& directories() const { return directories_; }
  const std::vector<std::string>& mutations() const { return mutations_; }

  private:
  std::map<std::string, std::string> owners_;
  std::vector<std::string> directories_;
  std::vector<std::string> mutations_;
  std::vector<std::string> clashes_;
};

}  // namespace

// Declared by a suite that uses files. Two suites claiming one directory is a
// clash, and the run fails on it.
void TEST_SUITE_CLAIMS_DIRECTORY(const char* suite, const char* directory) {
  Isolation::instance().claim(suite, directory);
}

// Declared by a suite that mutates state another suite could observe.
void TEST_SUITE_MUTATES(const char* suite, const char* what) {
  Isolation::instance().noteMutation(suite, what);
}

// The end-of-run report. Printed rather than only asserted, because the value of
// this file is that a reader can see the isolation story without reading twenty-eight
// suites.
//
// Takes the registry as an argument so the tests below can report on a local
// instance. Reporting the shared one from a test would leave whatever the test
// declared in the report the real reader then sees, which is the order-dependence
// this file exists to catch.
void printIsolationReport(const Isolation& state) {
  printf("\n=== suite isolation ===\n");
  printf("directories claimed: %u\n",
         static_cast<unsigned>(state.directoryCount()));
  printf("process mutations declared: %u\n",
         static_cast<unsigned>(state.mutationCount()));
  printf("directory clashes: %u\n", static_cast<unsigned>(state.clashCount()));
  for (const std::string& directory : state.directories()) {
    printf("  dir   %s\n", directory.c_str());
  }
  for (const std::string& mutation : state.mutations()) {
    printf("  state %s\n", mutation.c_str());
  }
  for (const std::string& clash : state.clashes()) {
    printf("  CLASH %s\n", clash.c_str());
  }
  std::fflush(stdout);
}

void printIsolationReport() {
  printIsolationReport(Isolation::instance());
}

unsigned isolationClashCount() {
  return static_cast<unsigned>(Isolation::instance().clashCount());
}

// --- the mechanism, tested against itself ---------------------------------
//
// Each test below builds its own Isolation rather than touching the singleton. A
// self-test that used the shared registry would be the exact defect it is written
// to catch: its leftovers would surface in the end-of-run report and in any later
// assertion on the clash count, so the suite would depend on its own order.

void test_two_suites_cannot_claim_one_directory() {
  // The whole point: a shared directory between suites is an order-dependent race,
  // and it is caught by declaration rather than by whoever loses the race.
  Isolation state;
  state.claim("suite-alpha", "/data/alpha");
  state.claim("suite-beta", "/data/alpha");
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<unsigned>(state.clashCount()));
  TEST_ASSERT_EQUAL_STRING("suite-alpha and suite-beta both use /data/alpha",
                           state.clashes().front().c_str());
}

void test_distinct_directories_do_not_clash() {
  Isolation state;
  state.claim("suite-gamma", "/data/gamma");
  state.claim("suite-delta", "/data/delta");
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<unsigned>(state.clashCount()));
  TEST_ASSERT_EQUAL_UINT32(2, static_cast<unsigned>(state.directoryCount()));
}

void test_a_suite_can_claim_a_directory_twice_without_being_a_clash() {
  // One suite reaching for its own directory from two of its own tests is fine.
  // It must also be recorded once: the report is a list of directories, so a repeat
  // would read as two suites owning one.
  Isolation state;
  state.claim("suite-epsilon", "/data/epsilon");
  state.claim("suite-epsilon", "/data/epsilon");
  TEST_ASSERT_EQUAL_UINT32(0, static_cast<unsigned>(state.clashCount()));
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<unsigned>(state.directoryCount()));
}

void test_a_missing_directory_is_named_rather_than_crashing() {
  // A suite that passes nothing is a bug in the suite, and it has to arrive as a
  // reportable "(null)" directory rather than as a segfault halfway through the run.
  Isolation state;
  state.claim("suite-zeta", nullptr);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<unsigned>(state.directoryCount()));
  TEST_ASSERT_EQUAL_STRING("(null)", state.directories().front().c_str());
}

void test_declared_mutations_are_recorded() {
  Isolation state;
  state.noteMutation("suite-eta", "a static counter");
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<unsigned>(state.mutationCount()));
  TEST_ASSERT_EQUAL_STRING("suite-eta: a static counter",
                           state.mutations().front().c_str());
}

void test_the_report_lists_what_was_declared() {
  Isolation state;
  state.claim("suite-theta", "/data/theta");
  state.claim("suite-theta", "/data/theta");
  state.claim("suite-iota", "/data/theta");
  printIsolationReport(state);
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<unsigned>(state.directoryCount()));
  TEST_ASSERT_EQUAL_UINT32(1, static_cast<unsigned>(state.clashCount()));
}

void registerSuiteIsolationTests() {
  RUN_TEST(test_two_suites_cannot_claim_one_directory);
  RUN_TEST(test_distinct_directories_do_not_clash);
  RUN_TEST(test_a_suite_can_claim_a_directory_twice_without_being_a_clash);
  RUN_TEST(test_a_missing_directory_is_named_rather_than_crashing);
  RUN_TEST(test_declared_mutations_are_recorded);
  RUN_TEST(test_the_report_lists_what_was_declared);
}