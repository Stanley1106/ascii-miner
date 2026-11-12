#!/usr/bin/env bash
# Capture MGP traffic on loopback with tcpdump and analyse it.
# usage: scripts/capture.sh [normal|fragment]   (needs root: uses sudo)
#   normal   — 2 bots play for 3 s (all message types)
#   fragment — 1 bot sends every byte as its own segment (partial recv)
source "$(dirname "$0")/lib.sh"
MODE=${1:-normal}
PCAP=$LOGDIR/miner_$MODE.pcap
SUDO=sudo; [ "$EUID" -eq 0 ] && SUDO=

# -w - : write to stdout; our (unprivileged) shell creates the file, so it
# is owned by the user and tcpdump never needs to chown it.
$SUDO tcpdump -i lo -U -s 0 -w - "tcp port $PORT" > "$PCAP" 2> "$LOGDIR/tcpdump.log" &
DUMP=$!
sleep 1
start_server "capture_$MODE" -q -W 40 -H 20
if [ "$MODE" = fragment ]; then
    "$CLIENT" --bot -p "$PORT" -c 1 -d 1 -i 300 --fragment-us 2000 > /dev/null 2>&1
else
    "$CLIENT" --bot -p "$PORT" -c 2 -d 3 -i 100 > /dev/null 2>&1
fi
stop_server
sleep 0.5
$SUDO pkill -INT -f "tcpdump -i lo -U -s 0 -w - tcp port $PORT" 2>/dev/null
wait "$DUMP" 2>/dev/null

banner "capture saved: $PCAP (port $PORT)"
echo "--- TCP connection setup / first data (tcpdump -nn) ---"
tcpdump -nn -r "$PCAP" 2>/dev/null | head -8
echo "--- first MGP frame in hex: magic 4d47 'MG', version 01, type, seq, length ---"
tcpdump -nn -X -r "$PCAP" 'tcp[tcpflags] & tcp-push != 0' 2>/dev/null | head -6
banner "decoded MGP frames"
python3 "$ROOT/tools/mgp_stats.py" "$PCAP" "$PORT" --frames 20
