#!/usr/bin/env bash
# Robustness #3 — handling TCP partial recv (stream reassembly).
# Bots send every byte as its own TCP segment (--fragment-us). The robust
# server buffers bytes until a whole frame is present; the naive server
# assumes one recv() == one frame and misparses the first byte as a header.
source "$(dirname "$0")/lib.sh"

run() { # run NAME [extra server args]
    local name=$1; shift
    start_server "$name" -q "$@"
    "$CLIENT" --bot -p "$PORT" -c 3 -d 3 -i 200 --fragment-us 300 -n "$name" \
        > "$LOGDIR/$name.bot.log" 2>&1
    stop_server
    echo "--- $name ---"
    grep -E "connected|done" "$LOGDIR/$name.bot.log"
    grep -m2 -E "bad magic|handshake failed|protocol error" "$LOGDIR/$name.server.log" || true
}

banner "demo 3: partial recv  (3 bots, every byte sent separately)"
run partial_without --naive-recv
run partial_with
echo
printf '%-8s %-10s %-6s %s\n' mode connected recv lost
for m in without with; do
    f=$LOGDIR/partial_$m.bot.log
    printf '%-8s %-10s %-6s %s\n' "$m" "$(sed -n 's/.*connected \([^ ]*\).*/\1/p' "$f")" \
        "$(bot_field "$f" recv)" "$(bot_field "$f" lost)"
done
grep -q "connected 0/3" "$LOGDIR/partial_without.bot.log" &&
    grep -q "connected 3/3" "$LOGDIR/partial_with.bot.log" &&
    [ "$(bot_field "$LOGDIR/partial_with.bot.log" lost)" -eq 0 ] &&
    [ "$(bot_field "$LOGDIR/partial_with.bot.log" recv)" -gt 0 ]
verdict $? "naive recv rejected every fragmented client; framed recv reassembled all frames"
