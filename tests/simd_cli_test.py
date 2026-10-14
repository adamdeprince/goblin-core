"""Check SIMD option errors and serve RESP under auto and scalar dispatch."""

import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def main():
    binary = sys.argv[1]
    for args, message in [
        (["--simd"], "--simd requires a model"),
        (["--simd", "invalid"], "unknown SIMD model"),
    ]:
        result = subprocess.run([binary, *args], capture_output=True, text=True,
                                timeout=10)
        assert result.returncode == 2, result
        assert message in result.stderr, result.stderr

    automatic = None
    for model in ["auto", "scalar"]:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        # Keep the UDS path short enough for sockaddr_un on macOS.
        with tempfile.TemporaryDirectory(prefix="simd-", dir="/tmp") as tmp:
            path = str(Path(tmp) / "server.sock")
            with tempfile.TemporaryFile(mode="w+") as log:
                server = subprocess.Popen(
                    [binary, "--simd", model, "--uds-listen", path,
                     "--port", str(port)],
                    stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 10
                    while True:
                        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        client.settimeout(2)
                        try:
                            client.connect(path)
                            break
                        except OSError:
                            client.close()
                            if server.poll() is not None or time.monotonic() > deadline:
                                log.seek(0)
                                raise AssertionError(log.read())
                            time.sleep(0.01)
                    with client:
                        client.sendall(b"*1\r\n$4\r\nPING\r\n")
                        assert client.recv(1024) == b"+PONG\r\n"
                    log.seek(0)
                    output = log.read()
                    selection = re.search(r"SIMD caps: selected=(\w+)", output)
                    assert selection, output
                    selected = selection.group(1)
                    assert selected != "auto", output
                    if model == "scalar":
                        assert selected == "scalar", output
                    else:
                        automatic = selected
                finally:
                    server.terminate()
                    try:
                        server.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=5)

    # A foreign architecture is always incompatible, regardless of CPU tier.
    foreign = "neon" if automatic in {"sse2", "avx2", "avx512"} else "avx512"
    result = subprocess.run([binary, "--simd", foreign], capture_output=True,
                            text=True, timeout=10)
    assert result.returncode == 2, result
    assert "incompatible with this CPU/OS" in result.stderr, result.stderr


if __name__ == "__main__":
    main()
