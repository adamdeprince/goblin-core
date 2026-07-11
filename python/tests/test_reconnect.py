"""A goblin_core client that dies abruptly (SIGKILL) must not wedge its ring: a fresh
client has to reconnect to the SAME ring and keep working, with no server restart.

    PYTHONPATH=python python python/tests/test_reconnect.py [path-to-goblin-core]

The dying client is a separate process so the kill is a real, uncleaned crash -- it
hammers the ring in a tight loop (so it is almost certainly mid-round-trip, leaving the
server's reply stranded in the CQ) and is then SIGKILLed while still holding the mapping.
The reconnecting client relies on the ring's epoch handshake: it bumps the epoch, the
server drains the corpse's leftovers and re-arms, then acks.
"""
import os
import signal
import subprocess
import sys
import tempfile
import textwrap
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PYDIR = os.path.join(REPO, "python")
sys.path.insert(0, PYDIR)

from goblin_core import Redis  # noqa: E402


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "goblin-core")
    if not os.path.exists(binary):
        print(f"goblin-core binary not found: {binary}", file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="goblin-reconnect-")
    ring_path = os.path.join(tmp, "ring")
    sock_path = os.path.join(tmp, "sock")
    server = subprocess.Popen(
        [binary, "--unixsocket", sock_path, "--ring", ring_path, "256kb"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        # A first client establishes some state, then disconnects cleanly.
        r0 = Redis(ring_path, connect_timeout=5.0)
        assert r0.set("survivor", "1") is True
        assert r0.get("survivor") == b"1"
        del r0  # clean disconnect -- reconnection must still work after this

        # A child client crashes messily: it hammers the ring, then is SIGKILLed mid-flight
        # while still attached (no cleanup, its reply likely stranded in the CQ).
        child_src = textwrap.dedent(
            f"""
            import sys
            from goblin_core import Redis
            r = Redis({ring_path!r}, connect_timeout=5.0)
            r.set("hammer", "start")
            sys.stdout.write("ready\\n"); sys.stdout.flush()
            i = 0
            while True:            # round-trips; a SIGKILL here leaves the ring dirty
                r.set("hammer", str(i)); i += 1
            """
        )
        env = dict(os.environ)
        env["PYTHONPATH"] = PYDIR + os.pathsep + env.get("PYTHONPATH", "")
        child = subprocess.Popen(
            [sys.executable, "-c", child_src], env=env,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        )
        assert child.stdout.readline().strip() == "ready"  # it is attached and hammering
        time.sleep(0.1)                                     # let it get well into the mess
        child.send_signal(signal.SIGKILL)                  # abrupt, uncleaned crash
        child.wait(timeout=5)

        # A brand-new client recovers the SAME ring -- the server was never restarted.
        r = Redis(ring_path, connect_timeout=5.0)
        # If the drain had failed we would read the corpse's stranded reply here (a status
        # reply) instead of our bulk string -> wrong value or an error. Getting b"1" proves
        # both that we read OUR reply and that the store survived the crash.
        assert r.get("survivor") == b"1", "reconnected client read a stale/foreign reply"
        assert r.ping() is True
        assert r.set("after", "ok") is True
        assert r.get("after") == b"ok"
        assert r.incr("n") == 1 and r.incr("n") == 2
        assert r.zadd("z", {"a": 1, "b": 2}) == 2 and r.zcard("z") == 2
        print("goblin_core reconnect OK: a SIGKILLed client left the ring dirty; "
              "a fresh client recovered it with no server restart")
        return 0
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()


if __name__ == "__main__":
    raise SystemExit(main())
