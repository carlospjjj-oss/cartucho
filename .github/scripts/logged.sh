#!/bin/bash
# Runs a command, and if it fails, repeats the end of its output as GitHub
# error annotations: they can be read through the checks API even where the
# full job log cannot be downloaded.
#   .github/scripts/logged.sh <label> <command...>
label=$1; shift
log=$(mktemp)
set -o pipefail
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
if [ "$status" -ne 0 ]; then
    # Prefer the lines that look like errors, then the plain tail.
    {
        grep -nE "error:|Error:|FAILED|FAIL |e: file|undefined reference|fatal|Unresolved|Segmentation|What went wrong" "$log" | tail -60
        echo "----- tail -----"
        tail -60 "$log"
    } > "$log.short"
    i=0
    while [ $i -lt 8 ]; do
        chunk=$(sed -n "$((i*16+1)),$((i*16+16))p" "$log.short")
        [ -z "$chunk" ] && break
        chunk=${chunk//'%'/'%25'}
        chunk=${chunk//$'\r'/}
        chunk=${chunk//$'\n'/'%0A'}
        echo "::error title=$label ($((i+1)))::$chunk"
        i=$((i+1))
    done
fi
exit "$status"
