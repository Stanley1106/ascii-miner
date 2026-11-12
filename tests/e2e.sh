#!/usr/bin/env bash
# End-to-end tests: real server + bot clients. Each demo script exits 0 only
# if the with/without behaviour difference was observed.
cd "$(dirname "$0")/.."
export BUILD=${BUILD:-build}
fail=0
run() {
    printf '%-28s ' "$1"
    if out=$("${@:2}" 2>&1); then echo ok; else echo FAIL; echo "$out" | tail -15; fail=1; fi
}
run "concurrency (12 clients)" scripts/stress.sh 12 4
run "demo 1 lock"              scripts/demo_lock.sh
run "demo 2 reap"              scripts/demo_reap.sh
run "demo 3 partial recv"      scripts/demo_partial.sh
run "demo 4 idle timeout"      scripts/demo_timeout.sh
run "demo 5 robust mutex"      scripts/demo_robust.sh
exit $fail
