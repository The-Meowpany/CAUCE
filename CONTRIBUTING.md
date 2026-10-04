# Contributing to CAUCE

This document outlines the guidelines for contributing to the project. Where it
disagreed with the code, the code won and this was corrected: the rules below
are the ones the repository actually follows.

## Code of Conduct

Please read and follow our [Code of Conduct](CODE_OF_CONDUCT.md).

## How to Contribute

### Reporting Bugs

Open an issue at <https://github.com/The-Meowpany/CAUCE/issues> and include:

- Firmware version / commit hash
- Hardware (ESP32 board, sensor model)
- Steps to reproduce
- Expected vs actual behavior
- Logs (with `pio device monitor` output if applicable)

Check for an existing issue first.

### Pull Requests

1. Branch from `main`.
2. Make the change.
3. Run the gate. It runs everything: firmware host tests, the ESP32 build, the
   backend suite and a live node-to-server integration run.
   ```powershell
   .\scripts\verify-all.ps1
   ```
4. Run the linter on its own if you want fast feedback:
   ```bash
   ruff check backend simulator
   ```
5. Open the PR describing what changed and what you verified. Attach the
   command output rather than claiming it passed.

`verify-all.ps1` needs a MinGW-w64 `g++` on `PATH` for the host builds. It looks
for one in the usual places; if it cannot find it, set `CAUCE_MINGW_BIN` to the
directory holding `g++.exe`.

## Code Style

### C++ (Firmware)

- C++17, `-std=gnu++17 -Wall -Wextra`, clean.
- **No exceptions.** There is not one `throw` in the firmware. Failure is a
  `bool` plus an out-parameter, a named status enum, or an `int` count.
- No raw `new`/`delete`; prefer RAII and fixed buffers on the hot path.
- Heap allocation is not banned. It is used where it is deliberate and
  documented, and adding more casually than that is the thing to avoid.
- Comments explain why, including the rejected alternative and the bug that
  motivated the decision. Do not restate the code in English.

### Python (Backend)

- Python 3.12+.
- Return annotations on everything in `cauce_server/` and `tools/`. The test
  modules do not carry them today; that is a known gap, not the standard.
- `ruff check backend simulator` must be clean. That is the whole lint step:
  there is no formatter check in CI and no type checker configured.
- Docstrings are prose explaining the decision and its failure modes, often with
  ALL-CAPS section headings. They are not Google-style.
- Errors: `HTTPException` with a snake_case machine `detail` code and
  `raise ... from exc`; custom exception classes for domain failures;
  `SystemExit` in CLI tools.

### Language

Code identifiers, comments and machine values are English only. Machine tokens
shared with the firmware or the CI (`E2E_OK`, `invalid_cursor`, CSV headers) must
stay byte-identical. See `docs/en/I18N.md`.

## Testing

- Every PR must pass `.\scripts\verify-all.ps1`.
- New behaviour needs a test that fails without the change. The current counts
  are in `STATUS.md`, which is the source of truth; the numbers quoted in older
  prose are not.
- A bug fix gets a regression test.
- Do not fix a flaky test with a sleep.

## Code Review

- [ ] `.\scripts\verify-all.ps1` exits 0
- [ ] `ruff check backend simulator` is clean
- [ ] New behaviour has a test that fails without the change
- [ ] Security considered: input validation, authorization, secrets, error leakage
- [ ] `STATUS.md` updated if what is implemented or verified changed
- [ ] User-facing docs updated in **both** `docs/en/` and `docs/es/`

## Reporting Security Issues

**Do not** open a public issue for a security vulnerability.

Use GitHub's private reporting on this repository
(<https://github.com/The-Meowpany/CAUCE/security/advisories/new>), which keeps
the discussion private until a fix exists. Include a description, steps to
reproduce, impact, and a suggested fix if you have one.

For the record, the project's own view of its security posture is
`docs/en/SECURITY.md`, including what it does not protect against.

---

Thank you for contributing to CAUCE.
