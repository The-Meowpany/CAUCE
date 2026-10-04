#!/usr/bin/env bash
# Release gate: refuses to call a build releasable unless it has actually been
# checked. Run from the repository root, on a POSIX shell (WSL, Linux, macOS).
#
# On Windows use scripts\release-gate.ps1, which does the same checks.
#
# The point is not to run the tests. The point is that "I ran the tests" is a
# recollection, and a release decision made from a recollection is how a red build
# ships. Every check here fails loudly rather than printing something reassuring.
#
#   ./scripts/release-gate.sh
#   ./scripts/release-gate.sh --tag v1.0.0
#
# Exit code is what matters. Failures are collected rather than aborting on the
# first, because an operator fixing a release wants the whole list.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

WANTED_TAG=""
if [ "${1:-}" = "--tag" ]; then
  WANTED_TAG="${2:-}"
fi

FAILURES=()
NOTES=()

step() { printf '\n=== %s\n' "$1"; }

fail() {
  printf '  FAIL  %s\n' "$1"
  FAILURES+=("$1")
}

ok() { printf '  ok    %s\n' "$1"; }

check_docs() {
  local dir="$1"; shift
  local missing=""
  local doc
  for doc in "$@"; do
    [ -f "$dir/$doc" ] || missing="$missing $doc"
  done
  if [ -z "$missing" ]; then
    ok "every indexed document exists in $dir"
  else
    fail "missing from $dir:$missing"
  fi
}

step "the tree is clean"
if git diff --quiet && git diff --cached --quiet; then
  ok "no uncommitted changes"
else
  printf '  uncommitted changes:\n'
  git status --short
  fail "uncommitted changes"
fi

step "version and tag"
VERSION="$(git describe --tags --abbrev=0 2>/dev/null || echo none)"
printf '  current tag: %s\n' "$VERSION"
if [ -n "$WANTED_TAG" ]; then
  if [ "$VERSION" = "$WANTED_TAG" ]; then
    ok "HEAD is $WANTED_TAG"
  else
    fail "expected tag $WANTED_TAG, found $VERSION"
  fi
else
  NOTES+=("no --tag given, so this run checks the tree but not the tag")
fi

step "full verification (firmware, ESP32 build, backend, E2E)"
SHELL_PS=""; for candidate in pwsh powershell; do
  if command -v "$candidate" >/dev/null 2>&1; then SHELL_PS="$candidate"; break; fi
done
if [ -n "$SHELL_PS" ]; then
  "$SHELL_PS" -NoProfile -ExecutionPolicy Bypass -File scripts/verify-all.ps1
  if [ $? -eq 0 ]; then ok "verify-all.ps1"; else fail "verify-all.ps1"; fi
else
  fail "no PowerShell found; cannot run verify-all.ps1"
fi

step "SBOM and its pins"
PY=""; for candidate in python3 python; do
  if command -v "$candidate" >/dev/null 2>&1; then PY="$candidate"; break; fi
done
if [ -n "$PY" ]; then
  ( cd backend && "$PY" tools/sbom.py --out sbom/central.cdx.json --strict )
  if [ $? -eq 0 ]; then ok "sbom.py --strict"; else fail "sbom.py --strict"; fi
else
  fail "no python found for the SBOM step"
fi

step "documentation"
# Both lists are the full set of docs/ files. RUNBOOK.md and DOCUMENTATION_INDEX.md were
# missing from one side each, so the check passed while a document existed that no list
# mentioned - which is the drift this check exists to catch, applied to itself.
DOCS_EN="RELEASE_READINESS.md ROADMAP.md BENCH_PLAN.md SECURITY.md OTA.md SYNC.md \
API.md BACKEND.md DATA_MODEL.md ARCHITECTURE.md CALIBRATION.md DEPLOYMENT.md \
HARDWARE.md TESTING.md DASHBOARD.md LEGAL.md PILOT_SPEC.md I18N.md \
DOMAIN_GLOSSARY.md DOCUMENTATION_INDEX.md RUNBOOK.md"
DOCS_ES="$DOCS_EN"
# shellcheck disable=SC2086
check_docs docs/en $DOCS_EN
# shellcheck disable=SC2086
check_docs docs/es $DOCS_ES

# The lists above are hand-maintained, so they drift. Compare them against the tree, in
# both languages, so a new document cannot ship without being listed and a renamed one
# cannot leave a stale entry behind.
for dir in docs/en docs/es; do
  case "$dir" in
    docs/en) listed="$DOCS_EN" ;;
    *)       listed="$DOCS_ES" ;;
  esac
  on_disk="$(cd "$dir" && ls -1 ./*.md 2>/dev/null | sed 's|^\./||' | sort)"
  # shellcheck disable=SC2086
  want="$(printf '%s\n' $listed | sort)"
  if [ "$on_disk" = "$want" ]; then
    ok "the $dir list matches the tree"
  else
    unlisted="$(comm -23 <(printf '%s\n' "$on_disk") <(printf '%s\n' "$want") | tr '\n' ' ')"
    phantom="$(comm -13 <(printf '%s\n' "$on_disk") <(printf '%s\n' "$want") | tr '\n' ' ')"
    # Written as if/then rather than `[ -n "$x" ] && fail ...`: under `set -e` a false
    # test on the last line of a loop body ends the script, so the clean case would exit
    # non-zero after passing every check.
    if [ -n "$unlisted" ]; then fail "not listed in the gate ($dir): $unlisted"; fi
    if [ -n "$phantom" ]; then fail "listed but absent ($dir): $phantom"; fi
  fi
done

step "the firmware signs with both algorithms"
# Checked in pieces rather than by grepping for one string, because the natural
# spelling varies between header and source. Each piece is something that must
# exist for Ed25519 frames to actually be signed and verified at the central.
check_piece() {
  if grep -q "$2" "$1"; then ok "$3"; else fail "$3"; fi
}
check_piece firmware/lib/cauce_app/include/cauce/app/LoRaSyncTransport.h \
  'setFrameAlgorithm' "transport exposes the algorithm"
check_piece firmware/lib/cauce_app/include/cauce/app/LoRaSyncTransport.h \
  'frameSignatureBytes' "transport selects a trailer size"
check_piece firmware/lib/cauce_core/include/cauce/core/LoRaBatchCodec.h \
  'kEd25519' "codec declares the algorithm enum"
check_piece firmware/lib/cauce_core/src/LoRaBatchCodec.cpp \
  'ed25519Sign' "codec signs with ed25519"
check_piece firmware/lib/cauce_core/src/LoRaBatchCodec.cpp \
  'hmacSha256' "codec signs with hmac"
check_piece firmware/lib/cauce_app/src/LoRaSyncTransport.cpp \
  'frameAlgorithmIsAsymmetric' "transport refuses a bad key length"

step "the group order constant is still the verified one"
# It was once typed from memory with a byte missing, which compiled cleanly and
# reduced to a plausible wrong scalar on every signature. This check is why that
# cannot happen again unnoticed.
if grep -q 'edd3f55c' firmware/lib/cauce_core/src/Ed25519Points.cpp; then
  ok "kGroupOrder carries its verified hex"
else
  fail "kGroupOrder lost its verified hex"
fi

step "no unregistered failing test is hiding in the tree"
if grep -rq 'NOT REGISTERED' firmware/test/ 2>/dev/null; then
  printf '  note  files mention NOT REGISTERED:\n'
  grep -rl 'NOT REGISTERED' firmware/test/ | sed 's/^/        /'
  NOTES+=("some tests are present but unregistered; check they are intentional")
else
  ok "no unregistered tests"
fi

printf '\n=== summary\n'
if [ "${#NOTES[@]}" -gt 0 ]; then
  for note in "${NOTES[@]}"; do printf '  note  %s\n' "$note"; done
fi

if [ "${#FAILURES[@]}" -eq 0 ]; then
  printf '\nRELEASE GATE: PASS\n'
  printf 'tag: %s\n' "$VERSION"
  printf '\nThis says nothing is obviously broken. It does not certify the\n'
  printf 'hardware: docs/en/RELEASE_READINESS.md Phase 3 is still open.\n'
  exit 0
fi

printf '\nRELEASE GATE: FAIL\n'
for failure in "${FAILURES[@]}"; do printf '  - %s\n' "$failure"; done
exit 1
