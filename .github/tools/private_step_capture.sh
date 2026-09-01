#!/usr/bin/env bash
# Sourced through BASH_ENV after the immutable private checkout has completed.

if [[ "${GDPP_PRIVATE_CAPTURE_ACTIVE:-}" == 1 ]]; then
  return 0
fi

export GDPP_PRIVATE_CAPTURE_ACTIVE=1
umask 077
GDPP_PRIVATE_CAPTURE_FINISHED=0
GDPP_PRIVATE_CAPTURE_TOOL="$(
  cd "$(dirname "${BASH_SOURCE[0]}")" && pwd
)/private_step_capture.py"
GDPP_PRIVATE_CAPTURE_TEMP="${RUNNER_TEMP:?}"
if [[ "${RUNNER_OS:-}" == Windows ]]; then
  GDPP_PRIVATE_CAPTURE_TEMP="$(cygpath -u "$GDPP_PRIVATE_CAPTURE_TEMP")"
fi
GDPP_PRIVATE_CAPTURE_LOG="$(
  mktemp "$GDPP_PRIVATE_CAPTURE_TEMP/gdpp-private-step.XXXXXX.log"
)"
chmod 600 "$GDPP_PRIVATE_CAPTURE_LOG"

exec 171>&1 172>&2

gdpp_private_capture_exit() {
  local status="${1:-1}"
  if [[ "$GDPP_PRIVATE_CAPTURE_FINISHED" == 1 ]]; then
    exit "$status"
  fi
  GDPP_PRIVATE_CAPTURE_FINISHED=1
  trap - EXIT
  exec 1>&171 2>&172
  python3 "$GDPP_PRIVATE_CAPTURE_TOOL" \
    --log "$GDPP_PRIVATE_CAPTURE_LOG" \
    --status "$status" \
    --job "${GITHUB_JOB:-unknown-job}" \
    --step "${GITHUB_ACTION:-run}" 2>/dev/null || \
    printf 'private-stage status=%s category=summary exit=%s\n' \
      "$(if [[ "$status" == 0 ]]; then printf success; else printf failed; fi)" \
      "$status"
  if [[ "$status" == 0 ]]; then
    rm -f "$GDPP_PRIVATE_CAPTURE_LOG"
  fi
  exit "$status"
}

trap 'gdpp_private_capture_exit "$?"' EXIT
exec >"$GDPP_PRIVATE_CAPTURE_LOG" 2>&1
