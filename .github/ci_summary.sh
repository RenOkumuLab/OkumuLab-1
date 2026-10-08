#!/usr/bin/env bash
# OkumuLab 1 CI: a short report of the logs on the run's summary page (readable without signing in to GitHub)
#   ci_summary.sh <title> <log files...>
# Each log: its last line. A log with failures or errors: those lines and the log's end, folded.
title="$1"; shift
out="${GITHUB_STEP_SUMMARY:-/dev/stdout}"
pattern='FAIL|[Ee]rror|cannot|not found|Segmentation|Abort|Assertion|exception'
{
    echo "## $title"
    echo
    echo "| log | last line |"
    echo "|---|---|"
    for f in "$@"; do
        if [ ! -f "$f" ]; then echo "| $f | (not run) |"; continue; fi
        last=$(grep -v '^[[:space:]]*$' "$f" | tail -1 | cut -c1-160 | sed 's/|/\\|/g')
        echo "| $f | ${last:-(empty)} |"
    done
    for f in "$@"; do
        [ -f "$f" ] || continue
        if grep -qE "$pattern" "$f"; then
            echo
            echo "<details><summary>$f</summary>"
            echo
            echo '```'
            grep -nE "$pattern" "$f" | head -60 | cut -c1-400
            echo '--- end of the log ---'
            tail -40 "$f" | cut -c1-400
            echo '```'
            echo '</details>'
        fi
    done
} >> "$out"
exit 0
