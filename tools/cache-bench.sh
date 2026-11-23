#!/usr/bin/env bash
# Times a scan, analysis and compilation (no export) and prints what the build cache reused, for comparing a
# cold cache, a warm one, and the first run after an app update. Runs in Git Bash on Windows too.
#
#   tools/cache-bench.sh LABEL FO4MX DB -- <install options>
#     LABEL  a name for this run (printed)
#     FO4MX  the fo4mx command line tool to time (e.g. the new build's fo4mx.exe)
#     DB     results database; the build cache is the file next to it (<name>.cache.sqlite)
#   Example (Vortex):
#     tools/cache-bench.sh warm "/c/Multiplexer/fo4mx.exe" /c/bench/bench.sqlite -- --vortex --game "/e/SteamLibrary/steamapps/common/Fallout 4"
#
# A cold run: delete <name>.cache.sqlite first. Compare the "seconds" and the cache lines between runs.
set -euo pipefail
label=$1 fo4mx=$2 db=$3
shift 3
[ "${1:-}" = "--" ] && shift
log=$(mktemp)
start=$(date +%s%N)
"$fo4mx" compile "$@" --db "$db" --dry-run >"$log" 2>&1 || true
end=$(date +%s%N)
ms=$(( (end - start) / 1000000 ))
echo "== $label: $(( ms / 1000 )).$(printf %03d $(( ms % 1000 ))) seconds"
grep -E "^\[[^]]*\] \[[a-z]+\] (Build cache: |.*cache: |Indexed [0-9]+ records|Compiled [0-9]+ records|Discovered [0-9]+ plugins|Wrote the scan results|Scan results unchanged)" "$log" \
    | sed -E 's/^\[[^]]*\] \[[a-z]+\] /  /'
echo "  (full log: $log)"
