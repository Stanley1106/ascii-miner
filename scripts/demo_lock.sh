#!/usr/bin/env bash
# Robustness #1 — shared-state lock.
# 40 bots dig a tiny 8x4 map. Digging takes time (--dig-delay-us), so
# without the lock several processes pass the "not dug yet" check for the
# same cell and all receive its ore: more value is mined than exists.
source "$(dirname "$0")/lib.sh"
MAP=(-W 8 -H 4 -s 42 --dig-delay-us 3000)

run() { # run NAME [extra server args]
    local name=$1; shift
    start_server "$name" "${MAP[@]}" "$@"
    "$CLIENT" --bot -p "$PORT" -c 40 -i 0 -d 3 -n "$name" > "$LOGDIR/$name.bot.log" 2>&1
    stop_server
}

banner "demo 1: dig lock  (40 bots, 8x4 map, 3 ms dig work)"
run lock_without --no-lock
run lock_with

printf '\n%-8s %-8s %-18s %-10s %s\n' mode cells "mined/initial" dup_claims dup_value
for m in without with; do
    s=$(last_stat "lock_$m")
    dup=$(field "$s" dup)
    printf '%-8s %-8s %-18s %-10s %s\n' "$m" "$(field "$s" cells)" "$(field "$s" mined)" \
        "${dup%%(*}" "$(sed 's/.*(+\(.*\))/\1/' <<<"$dup")"
done

W=$(last_stat lock_without); L=$(last_stat lock_with)
dup_w=$(field "$W" dup); dup_w=${dup_w%%(*}
dup_l=$(field "$L" dup); dup_l=${dup_l%%(*}
mined_l=$(field "$L" mined)
[ "$dup_w" -gt 0 ] && [ "$dup_l" -eq 0 ] && [ "${mined_l%/*}" -le "${mined_l#*/}" ]
verdict $? "without lock ore was duplicated $dup_w times; with lock every cell paid out exactly once"
