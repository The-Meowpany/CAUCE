# Contributing to CAUCE

Thank you for your interest in contributing to CAUCE! This document outlines the guidelines for contributing to the project.

## Code of Conduct

Please read and follow our [Code of Conduct](CODE_OF_CONDUCT.md).

## How to Contribute

### Reporting Bugs

Before reporting a bug, please:
1. Check if the issue already exists in [GitHub Issues](https://github.com/cauce-project/cauce/issues)
2. If not, create a new issue using the **Bug Report** template
3. Include:
   - Firmware version / commit hash
   - Hardware (ESP32 board, sensor model)
   - Steps to reproduce
   - Expected vs actual behavior
   - Logs (with `pio device monitor` output if applicable)

### Feature Requests

1. Check existing issues and discussions first
2. Open a new issue using the **Feature Request** template
3. Describe the problem you're solving and the proposed solution
4. Consider if it fits the project scope (see [ROADMAP.md](docs/en/ROADMAP.md))

### Pull Requests

1. Fork the repository and create a branch from `main`
2. Make your changes in a feature branch
3. Ensure all tests pass:
    ```bash
    # Firmware tests
    cd firmware
    pio test -e native

    # Backend tests
    cd backend
    python -m pytest tests -q
    ```
4. Run linting:
   ```bash
   ruff check backend simulator
   ```
4. Run the full test suite:
   ```bash
   .\scripts\verify-all.ps1
   ```
5. Submit a PR with:
   - Clear description of changes
   - Related issue number (if applicable)
   - Test results (attach output)
   - Screenshots for UI changes

## Code Style

### C++ (Firmware)
- C++17, no exceptions on hot path
- Fixed buffers, no heap on hot path
- `-std=gnu++17 -Wall -Wextra -Wextra`
- No exceptions on hot path; use error codes or `std::optional`
- RAII for resources; no raw `new`/`delete`

### Python (Backend)
- Python 3.12+, type hints required
- `ruff check .` and `ruff format` before commit
- Type hints on all public functions
- Google-style docstrings
- `ruff check .` and `ruff format --check` in CI

### Commit Messages

Follow [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <subject>

<body>

<footer>
```

Types: `feat`, `fix`, `docs`, `refactor`, `test`, `chore`, `perf`, `security`

Examples:
```
feat(firmware): add BME280 humidity compensation
fix(backend): fix rate-limiter memory leak on restart
docs: update DEPLOYMENT.md with OTA section
```

## Testing

All PRs must:
1. Pass existing tests
2. Include tests for new functionality
3. Pass CI pipeline (see `.github/workflows/ci.yml`)

### Running Tests

```bash
# Firmware tests (112 tests)
cd firmware
pio test -e native

# Backend tests (68 tests)
cd backend
python -m pytest tests -q

# Full regression
.\scripts\verify-all.ps1
```

## Code Review

Reviewers check:
- [ ] Tests pass (`pio test -e native`, `pytest tests -q`)
- [ ] Build succeeds (`pio run -e esp32dev`, `python -m pytest`)
- [ ] Linting passes (`ruff check .`, `ruff format --check`)
- [ ] Security considerations addressed
- [ ] Documentation updated
- [ ] Breaking changes noted (CHANGELOG.md when present)
- [ ] No regression in test suite

## Reporting Security Issues

**Do not** open public issues for security vulnerabilities.

Report privately to: **security@cauce-project.org**

Include:
- Description of the vulnerability
- Steps to reproduce
- Potential impact
- Suggested fix (if any)

We aim to respond within 48 hours and patch critical issues within 7 days.

---

Thank you for contributing to CAUCE! 🌱