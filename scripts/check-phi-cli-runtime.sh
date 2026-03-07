#!/usr/bin/env bash
set -euo pipefail

SOCKET_PATH="${1:-/var/lib/phi/cli.sock}"

if [[ ! -S "${SOCKET_PATH}" ]]; then
  echo "transport socket missing: ${SOCKET_PATH}" >&2
  exit 1
fi

PERMS="$(stat -c '%a %U %G' "${SOCKET_PATH}")"
EXPECTED_PERMS="660"

set -- ${PERMS}
MODE="$1"; OWNER="$2"; GROUP="$3"

if [[ "${MODE}" != "${EXPECTED_PERMS}" ]]; then
  echo "unexpected socket mode: ${PERMS} (expected ${EXPECTED_PERMS} owner/group phi)" >&2
  exit 2
fi

if [[ "${OWNER}" != "phi" || "${GROUP}" != "phi" ]]; then
  echo "unexpected socket owner/group: ${PERMS}" >&2
  exit 3
fi

cat <<EOF_MSG
phi-cli runtime check OK
socket=${SOCKET_PATH}
mode=${MODE}
owner=${OWNER}:${GROUP}
EOF_MSG

exit 0
