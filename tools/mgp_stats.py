#!/usr/bin/env python3
"""Decode Miner Game Protocol (MGP) frames from a pcap captured by tcpdump.

usage: tools/mgp_stats.py capture.pcap [port] [--frames N]

Reassembles each TCP direction (so frames split across segments, or several
frames in one segment, are both handled), prints the first N decoded frames
and a per-type summary. Standard library only.
"""
import struct
import sys
from collections import Counter, defaultdict

TYPES = {0x01: "HELLO", 0x02: "WELCOME", 0x10: "MOVE", 0x11: "POS", 0x12: "DIG",
         0x13: "DIG_RESULT", 0x20: "INV_REQ", 0x21: "INV_RESP", 0x30: "SYSINFO_REQ",
         0x31: "SYSINFO_RESP", 0x40: "SNAPSHOT", 0x50: "PING", 0x51: "PONG",
         0x60: "BYE", 0x7F: "ERROR"}
ORES = ["nothing", "copper", "silver", "gold", "diamond"]
DIRS = ["up", "down", "left", "right"]


def packets(path):
    """Yield (ts, src, sport, dst, dport, payload, tcp_flags) for IPv4/TCP."""
    with open(path, "rb") as f:
        magic = f.read(24)
        endian = "<" if magic[:4] in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1") else ">"
        linktype = struct.unpack(endian + "I", magic[20:24])[0]
        l2 = {1: 14, 113: 16, 276: 20, 0: 4}.get(linktype)
        if l2 is None:
            sys.exit(f"unsupported linktype {linktype}")
        while True:
            h = f.read(16)
            if len(h) < 16:
                return
            sec, usec, caplen, _ = struct.unpack(endian + "IIII", h)
            data = f.read(caplen)
            ip = data[l2:]
            if len(ip) < 20 or ip[0] >> 4 != 4 or ip[9] != 6:
                continue
            ihl = (ip[0] & 15) * 4
            tcp = ip[ihl:]
            sport, dport = struct.unpack(">HH", tcp[:4])
            off = (tcp[12] >> 4) * 4
            yield (sec + usec / 1e6, ip[12:16], sport, ip[16:20], dport, tcp[off:], tcp[13])


def describe(t, p):
    try:
        if t == 0x01:
            name = p[1:17].rstrip(b"\x00").decode(errors="replace")
            return f"flags={p[0]} name={name!r}"
        if t == 0x02:
            i, w, h, x, y = struct.unpack(">5H", p[:10])
            return f"id={i} map={w}x{h} pos=({x},{y})"
        if t == 0x10:
            return f"dir={DIRS[p[0]] if p[0] < 4 else p[0]}"
        if t == 0x11:
            return "pos=(%d,%d)" % struct.unpack(">HH", p[:4])
        if t == 0x13:
            x, y, st, ore, val, tot = struct.unpack(">HHBBII", p[:14])
            return f"({x},{y}) {['nothing', 'FOUND', 'already dug'][st]} {ORES[ore]} +{val} total={tot}"
        if t == 0x21:
            c = struct.unpack(">7I", p[:28])
            return f"copper={c[1]} silver={c[2]} gold={c[3]} diamond={c[4]} total={c[5]} digs={c[6]}"
        if t == 0x31:
            return p.decode(errors="replace").split("\n")[0] + " ..."
        if t == 0x40:
            v, w, h, n = struct.unpack(">IHHH", p[:10])
            return f"version={v} map={w}x{h} players={n} bytes={len(p)}"
        if t == 0x7F:
            return f"code={p[0]} {p[1:].decode(errors='replace')!r}"
    except (IndexError, struct.error):
        return "(truncated)"
    return ""


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    nframes = 25
    if "--frames" in sys.argv:
        nframes = int(sys.argv[sys.argv.index("--frames") + 1])
        args.remove(str(nframes))
    if not args:
        sys.exit(__doc__)
    port = int(args[1]) if len(args) > 1 else None
    streams = defaultdict(bytes)
    counts, sizes = Counter(), Counter()
    segs = data_segs = 0
    shown = bad = 0
    t0 = None
    for ts, src, sp, dst, dp, payload, flags in packets(args[0]):
        if port and port not in (sp, dp):
            continue
        t0 = t0 or ts
        segs += 1
        if not payload:
            continue
        data_segs += 1
        to_server = dp == port if port else dp < sp
        key = (sp, dp)
        buf = streams[key] + payload
        while len(buf) >= 12:
            if buf[:2] != b"MG":
                bad += 1
                buf = b""
                break
            t, seq, ln = buf[3], *struct.unpack(">II", buf[4:12])
            if len(buf) < 12 + ln:
                break
            p = buf[12:12 + ln]
            buf = buf[12 + ln:]
            name = TYPES.get(t, f"0x{t:02x}")
            counts[name] += 1
            sizes[name] += 12 + ln
            if shown < nframes:
                arrow = "C->S" if to_server else "S->C"
                print(f"{ts - t0:8.3f}s {arrow} :{sp}->:{dp} {name:<12} seq={seq:<5} len={ln:<5} {describe(t, p)}")
                shown += 1
        streams[key] = buf
    print(f"\nTCP segments: {segs} ({data_segs} with payload), MGP frames: {sum(counts.values())}, "
          f"non-MGP streams: {bad}")
    print(f"{'type':<14}{'frames':>8}{'bytes':>10}")
    for name, n in counts.most_common():
        print(f"{name:<14}{n:>8}{sizes[name]:>10}")


if __name__ == "__main__":
    main()
