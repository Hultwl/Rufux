#!/usr/bin/env bash
set -euo pipefail

MSDL_CLI="${1:?usage: test_core.sh <path-to-ctest_msdl_cli>}"
MACOS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MOCK_PY="${MACOS_DIR}/../tests/mock_msdl.py"

if [ ! -f "${MOCK_PY}" ]; then
  echo "ERROR: mock_msdl.py not found at ${MOCK_PY}" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d /tmp/rufux-test-core-XXXXXX)"
SRV_PID1=""
SRV_PID2=""
cleanup() {
  if [ -n "${SRV_PID1}" ]; then kill "${SRV_PID1}" 2>/dev/null || true; wait "${SRV_PID1}" 2>/dev/null || true; fi
  if [ -n "${SRV_PID2}" ]; then kill "${SRV_PID2}" 2>/dev/null || true; wait "${SRV_PID2}" 2>/dev/null || true; fi
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

wait_for_port() {
  local pf="$1"
  for _ in $(seq 1 50); do
    if [ -s "${pf}" ]; then
      cat "${pf}"
      return 0
    fi
    sleep 0.1
  done
  echo "ERROR: timed out waiting for mock_msdl.py port file ${pf}" >&2
  return 1
}

# 1. Test full session whitelist, language listing, link retrieval, and ISO fetch against mock_msdl.py
PORTFILE1="${TMP_DIR}/port1"
LOGFILE1="${TMP_DIR}/log1"
DLDIR="${TMP_DIR}/dl"
mkdir -p "${DLDIR}"

python3 "${MOCK_PY}" "${PORTFILE1}" "${LOGFILE1}" ok &
SRV_PID1=$!
PORT1="$(wait_for_port "${PORTFILE1}")"

EXPECTED_SHA="$(python3 -c 'import hashlib; iso=(hashlib.sha256(b"rufux-iso").digest()*100000)[:3*1024*1024+123]; print(hashlib.sha256(iso).hexdigest())')"
ACTUAL_SHA="$(RUFUX_MSDL_MOCK="http://127.0.0.1:${PORT1}" "${MSDL_CLI}" ok "${DLDIR}")"

if [ "${ACTUAL_SHA}" != "${EXPECTED_SHA}" ]; then
  echo "ERROR: downloaded ISO SHA-256 mismatch: expected ${EXPECTED_SHA}, got ${ACTUAL_SHA}" >&2
  exit 1
fi

# 2. Test IP-ban error code 715-123130 handling
PORTFILE2="${TMP_DIR}/port2"
LOGFILE2="${TMP_DIR}/log2"
python3 "${MOCK_PY}" "${PORTFILE2}" "${LOGFILE2}" banned &
SRV_PID2=$!
PORT2="$(wait_for_port "${PORTFILE2}")"

BAN_RES="$(RUFUX_MSDL_MOCK="http://127.0.0.1:${PORT2}" "${MSDL_CLI}" banned)"
if [ "${BAN_RES}" != "banned-ok" ]; then
  echo "ERROR: unexpected banned mode output: ${BAN_RES}" >&2
  exit 1
fi

echo "test_core.sh: ALL MSDL TESTS PASSED"
