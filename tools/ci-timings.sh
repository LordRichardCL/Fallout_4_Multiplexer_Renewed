#!/usr/bin/env bash
# Per-step durations of GitHub Actions runs, for comparing build times before and after a CI change.
# Usage: tools/ci-timings.sh RUN_ID [RUN_ID...]   (needs gh, authenticated for this repository)
set -euo pipefail
for run in "$@"; do
    echo "== run $run"
    gh run view "$run" --json jobs --jq '
      def secs(a; b): ((b | fromdateiso8601) - (a | fromdateiso8601));
      .jobs[] | "\(.name): \(secs(.startedAt; .completedAt))s",
        (.steps[] | select(.conclusion != "skipped") | "  \(secs(.startedAt; .completedAt))s\t\(.name)")'
    # sccache prints its statistics in the post-job step: hits and misses show how much was reused.
    gh run view "$run" --log 2>/dev/null | grep -E "Cache hits rate|Compile requests executed|Cache hits  |Cache misses  " \
        | sed -E 's/^([^\t]*)\t[^\t]*\t[^ ]+ /\1: /' || true
done
