#pragma once

// Suite isolation declarations for the single Unity test binary.
//
// WHAT THIS IS FOR
//
// Every firmware suite runs in one process. Anything a suite leaves behind - a file-scope
// static, a counter, a file - is visible to whatever runs after it, and a test that then
// passes for the wrong reason is worse than one that fails. The failure mode is silent and
// order-dependent, which is the worst combination a test suite has.
//
// This does not make that impossible. It makes it DECLARED and CHECKED, which is the part
// that is achievable without splitting twenty-eight suites into twenty-eight binaries: a
// suite that touches shared state says so, and two suites claiming the same thing fail the
// run at the end.
//
// Global scope, like every register*Tests() in this directory, because a namespace here
// would be the only thing in the suite that has one.
//
// See test_suite_isolation.cpp for the registry and the report.

// Declares the data directory this suite writes to. Two suites claiming one directory is
// an order-dependent race, and the run fails on it.
//
// Nothing calls this yet, and that is accurate rather than unfinished: no suite currently
// shares a filesystem. The ones that write files give each test its own in-memory or
// temporary backing store, so a claim here would be a false positive rather than a
// finding. The first suite that gets a real shared directory claims it here.
void TEST_SUITE_CLAIMS_DIRECTORY(const char* suite, const char* directory);


// Declares process-wide state this suite mutates, so a reader can see what it touched and
// so two suites touching the same state stand out side by side in the end-of-run report.
//
// Called from `test_main.cpp` rather than from inside each suite. That is a deliberate
// reversal: the earlier arrangement had the suites declare their own state, which is tidier
// and less discoverable - a reader looking at the run order saw nothing. Declaring all of it
// in one place means the shared state of a twenty-eight-suite single binary can be read off
// one screen, which is the only property that makes this worth having.
void TEST_SUITE_MUTATES(const char* suite, const char* what);

// Directory clashes declared so far. Non-zero fails the run; see test_main.cpp.
unsigned isolationClashCount();
