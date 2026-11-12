#!/usr/bin/env bash
# Robustness #2 — reaping children with SIGCHLD + waitpid().
# 3 rounds of 10 short-lived clients. Without reaping every finished child
# stays a <defunct> zombie holding a PID until the server exits.
source "$(dirname "$0")/lib.sh"

run() { # run NAME [extra server args]
    local name=$1; shift
    start_server "$name" -q "$@"
    for round in 1 2 3; do
        "$CLIENT" --bot -p "$PORT" -c 10 -d 1 -n "$name" >> "$LOGDIR/$name.bot.log" 2>&1
    done
    sleep 0.5
    echo "--- $name: children of server pid $SRV_PID ---"
    ps -o pid,stat,cmd --ppid "$SRV_PID" | head -6
    echo "zombie processes: $(zombies)"
    eval "Z_${name#reap_}=$(zombies)"
    stop_server
}

banner "demo 2: child reaping  (30 clients connect and leave)"
run reap_without --no-reap
run reap_with
echo
printf '%-8s %s\n' mode zombies without "$Z_without" with "$Z_with"
[ "$Z_without" -ge 30 ] && [ "$Z_with" -eq 0 ]
verdict $? "without reaping $Z_without zombies piled up; with SIGCHLD+waitpid none remain"
