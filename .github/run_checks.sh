#!/usr/bin/env bash
# OkumuLab 1 CI: labium_check, one section after another (it goes on after a failure; the step fails at the end)
#   run_checks.sh "<labium_check command>" <log file> <sections...>
# Each section: a line with its result and time; a failed one also as an annotation on the run's page.
cmd="$1"; log="$2"; shift 2
: > "$log"
fail=0
for s in "$@"; do
    t0=$(date +%s)
    if $cmd "$s" > "lc_$s.out" 2>&1; then r=ok; else r=FAILED; fail=1; fi
    cat "lc_$s.out" >> "$log"
    printf '%-10s %-6s %5d s   %s\n' "$s" "$r" $(( $(date +%s) - t0 )) "$(grep -E '^(OK|FAILED)' "lc_$s.out" | tail -1)"
    if [ "$r" = FAILED ]; then
        msg=$(grep -E 'FAIL|rror|Assert' "lc_$s.out" | head -4 | tr '\n' ' ' | cut -c1-700 | sed -e 's/%/%25/g')
        echo "::error title=labium_check $s::${msg:-no output (crashed?)}"
    fi
done
exit $fail
