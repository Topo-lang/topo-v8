#!/usr/bin/env bash
# assert-live-cases.sh — shared gtest live-case assert template.
# Canonical pattern instantiated (copied) into:
#   topo-lsp/.github/scripts/assert-live-cases.sh
#   topo-lang-{cpp,rust,java,python,typescript}/.github/scripts/assert-live-cases.sh
# Keep the six copies byte-identical (header carries a sync note).
#
# usage: assert-live-cases.sh <strict|lenient> <binary-name> <Suite.Case>...
#   strict  — additionally FAIL when the suite reports any SKIPPED case
#             (lang-repo semantics: skip != pass, zero skips tolerated)
#   lenient — tolerate skips (suites carrying permanent/deadline skips,
#             e.g. JdtBridge.RestartCycleIsolatesState), but every NAMED
#             case must appear in the [       OK ] (passed) set
# env:
#   TOPO_ASSERT_LOG=<file> — parse a pre-captured log instead of running
#                            the binary (local replay / negative probe)
#
# set +e semantics: errors handled explicitly; grep no-match inside a
# command substitution must not abort the step under GHA's `bash -e`.
set -u
mode="$1"; bin="$2"; shift 2
if [ "${mode}" != "strict" ] && [ "${mode}" != "lenient" ]; then
  echo "::error::assert-live-cases: mode must be strict|lenient, got '${mode}'"; exit 2
fi
if [ -n "${TOPO_ASSERT_LOG:-}" ]; then
  out=$(cat "${TOPO_ASSERT_LOG}"); rc=0
else
  exe=$(find build -type f \( -name "${bin}" -o -name "${bin}.exe" \) 2>/dev/null | head -1)
  if [ -z "${exe}" ]; then
    echo "::error::${bin}: executable not found under build/"; exit 1
  fi
  out=$("${exe}" --gtest_print_time=1 2>&1); rc=$?
fi
passed=$(printf '%s\n' "${out}" | grep -oE '\[  PASSED  \] [0-9]+ test' | grep -oE '[0-9]+' | tail -1)
skipped=$(printf '%s\n' "${out}" | grep -oE '\[  SKIPPED \] [0-9]+ test' | grep -oE '[0-9]+' | tail -1)
passed=${passed:-0}; skipped=${skipped:-0}
echo "${bin}: ${passed} PASSED, ${skipped} skipped (exit ${rc})"
fail=0
if [ "${rc}" -ne 0 ]; then
  printf '%s\n' "${out}" | tail -25
  echo "::error::${bin}: exited ${rc}"; fail=1
fi
if [ "${passed}" -lt 1 ]; then
  echo "::error::${bin}: ran 0 non-skipped cases (skip != pass)"; fail=1
fi
if [ "${mode}" = "strict" ] && [ "${skipped}" -gt 0 ]; then
  echo "::error::${bin}: ${skipped} case(s) SKIPPED (skip != pass)"; fail=1
fi
for name in "$@"; do
  # gtest pass line: '[       OK ] Suite.Case (N ms)' — the ' (' suffix
  # anchors the name end (Suite.Case must not prefix-match Suite.CaseX);
  # --gtest_print_time=1 above guarantees the suffix exists.
  if printf '%s\n' "${out}" | grep -qF "[       OK ] ${name} ("; then
    echo "live case PASSED: ${name}"
  elif printf '%s\n' "${out}" | grep -qF "[  SKIPPED ] ${name}"; then
    echo "::error::${bin}: required live case was SKIPPED: ${name}"; fail=1
  else
    echo "::error::${bin}: required live case ABSENT from passed set: ${name} (renamed? filtered out? not compiled in?)"; fail=1
  fi
done
exit "${fail}"
