#!/usr/bin/env python3
"""TLB / huge-page arena benchmark: --arena-hugetlb ON vs OFF.

Loads one large hash so its member/value arena spans many pages, then hammers it with
random HGET -- random arena pages, so 4 KB pages miss the dTLB (arena >> ~6 MB reach)
while 2 MB pages keep it resident. Runs BOTH servers concurrently and interleaves the
timed trials, so common-mode machine noise cancels in the A/B delta.

Reports huge-page-backed bytes and true resident (VmRSS + HugetlbPages, since MAP_HUGETLB
pages are not in VmRSS) to show memory neutrality, plus random-read throughput per config.
Fixed-width commands (f:%012d) -> exact-byte drain, bounded sub-batches (no deadlock)."""
import argparse, os, random, socket, statistics, subprocess, time

CMD = b"*3\r\n$4\r\nHGET\r\n$1\r\nh\r\n$14\r\n%s\r\n"
SUB = 2000

def connect(sock):
    s = socket.socket(socket.AF_UNIX); s.connect(sock); s.settimeout(60); return s

def drain(s, nbytes):
    got = 0
    while got < nbytes:
        d = s.recv(min(1 << 20, nbytes - got))
        if not d: raise SystemExit("connection closed")
        got += len(d)

def load(s, n, vb):
    B = 20000; buf = bytearray(); cnt = 0
    for i in range(n):
        f = b"f:%012d" % i; v = (b"v:%0*d" % (vb - 2, i))[:vb]
        buf += b"*4\r\n$4\r\nHSET\r\n$1\r\nh\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n" % (len(f), f, len(v), v)
        cnt += 1
        if cnt == B:
            s.sendall(buf); drain(s, 4 * B); buf = bytearray(); cnt = 0
    if cnt:
        s.sendall(buf); drain(s, 4 * cnt)

def mem_stats(pid):
    rss = huge = 0
    with open(f"/proc/{pid}/status") as f:
        for ln in f:
            if ln.startswith("VmRSS:"): rss = int(ln.split()[1])
            elif ln.startswith("HugetlbPages:"): huge = int(ln.split()[1])
    return rss / 1024, huge / 1024

def build_pool(n_fields, pool_ops):
    r = random.Random(12345)
    parts = [CMD % (b"f:%012d" % r.randrange(n_fields)) for _ in range(pool_ops)]
    return b"".join(parts), len(parts[0]), pool_ops

def one_trial(s, pool, cmd_len, pool_ops, vb, seconds):
    reply_sz = len(b"$%d\r\n" % vb) + vb + 2
    sub_bytes = SUB * cmd_len; sub_reply = SUB * reply_sz; n_sub = pool_ops // SUB
    ops = 0; i = 0; t0 = time.time()
    while time.time() - t0 < seconds:
        off = (i % n_sub) * sub_bytes
        s.sendall(pool[off:off + sub_bytes]); drain(s, sub_reply)
        ops += SUB; i += 1
    return ops / (time.time() - t0)

def start_and_load(binp, extra, n, vb):
    sock = f"/tmp/tlb-{'huge' if extra else 'base'}.sock"
    try: os.unlink(sock)
    except FileNotFoundError: pass
    p = subprocess.Popen([binp, "--unixsocket", sock, "--hash-chunk-bytes", "2097152"] + extra,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(200):
        if os.path.exists(sock):
            try:
                s = connect(sock); s.sendall(b"*1\r\n$4\r\nPING\r\n")
                if b"PONG" in s.recv(64): break
            except OSError: pass
        time.sleep(0.05)
    s = connect(sock); load(s, n, vb)
    return p, s, sock

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--fields", type=int, default=8_000_000)
    ap.add_argument("--value-bytes", type=int, default=14)
    ap.add_argument("--pool-ops", type=int, default=400_000)
    ap.add_argument("--seconds", type=float, default=2.0)
    ap.add_argument("--trials", type=int, default=8)
    a = ap.parse_args()
    print(f"fields={a.fields} value_bytes={a.value_bytes} "
          f"~arena={a.fields*(a.value_bytes+14)/2**20:.0f} MiB  trials={a.trials} (interleaved)")
    base_p, base_s, base_sock = start_and_load(a.binary, [], a.fields, a.value_bytes)
    huge_p, huge_s, huge_sock = start_and_load(a.binary, ["--arena-hugetlb"], a.fields, a.value_bytes)
    base_rss, base_huge = mem_stats(base_p.pid)
    huge_rss, huge_huge = mem_stats(huge_p.pid)
    pool, cmd_len, po = build_pool(a.fields, a.pool_ops)
    base_r, huge_r, deltas = [], [], []
    for _ in range(a.trials):
        b = one_trial(base_s, pool, cmd_len, po, a.value_bytes, a.seconds)
        h = one_trial(huge_s, pool, cmd_len, po, a.value_bytes, a.seconds)
        base_r.append(b); huge_r.append(h); deltas.append((h / b - 1) * 100)
    for s, p in ((base_s, base_p), (huge_s, huge_p)):
        s.close(); p.terminate(); p.wait()
    for sk in (base_sock, huge_sock):
        try: os.unlink(sk)
        except FileNotFoundError: pass
    bm, hm = statistics.median(base_r), statistics.median(huge_r)
    print(f"  4 KB baseline : huge-backed={base_huge:6.0f} MiB  resident(VmRSS+Hugetlb)={base_rss+base_huge:6.0f} MiB  median={bm/1e6:.2f} M/s")
    print(f"  2 MB hugetlb  : huge-backed={huge_huge:6.0f} MiB  resident(VmRSS+Hugetlb)={huge_rss+huge_huge:6.0f} MiB  median={hm/1e6:.2f} M/s")
    print(f"  => per-trial hugetlb delta: median {statistics.median(deltas):+.1f}%  "
          f"(min {min(deltas):+.1f}%, max {max(deltas):+.1f}%)   "
          f"resident delta {(huge_rss+huge_huge)-(base_rss+base_huge):+.0f} MiB")

if __name__ == "__main__":
    main()
