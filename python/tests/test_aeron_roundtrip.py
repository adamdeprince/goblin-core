"""End-to-end Aeron UDP/IPC test for the redis-py-shaped wrapper.

    PYTHONPATH=python python python/tests/test_aeron_roundtrip.py \
        ./build/goblin-core /path/to/aeronmd_s

The extension and server must both be built with GOBLIN_CORE_ENABLE_AERON=ON.
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PACKAGE_ROOT = os.environ.get("GOBLIN_CORE_PYTHON_PATH", os.path.join(REPO, "python"))
sys.path.insert(0, PACKAGE_ROOT)

from goblin_core import AeronIpcRedis, AeronUdpRedis, HAS_AERON  # noqa: E402


def reserve_port(socket_type: int) -> int:
    with socket.socket(socket.AF_INET, socket_type) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def stop(process: subprocess.Popen | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def main() -> int:
    if len(sys.argv) != 3:
        print(
            f"usage: {sys.argv[0]} <goblin-core> <aeronmd_s-or-aeronmd>",
            file=sys.stderr,
        )
        return 2
    if not HAS_AERON:
        print("goblin_core extension was built without Aeron", file=sys.stderr)
        return 2

    server_binary, driver_binary = sys.argv[1:]
    server_driver: subprocess.Popen | None = None
    client_driver: subprocess.Popen | None = None
    server: subprocess.Popen | None = None
    with tempfile.TemporaryDirectory(
        prefix="goblin-python-aeron-server-"
    ) as server_directory, tempfile.TemporaryDirectory(
        prefix="goblin-python-aeron-client-"
    ) as client_directory:
        try:
            server_driver = subprocess.Popen(
                [
                    driver_binary,
                    f"-Daeron.dir={server_directory}",
                    "-Daeron.dir.delete.on.start=true",
                    "-Daeron.dir.delete.on.shutdown=true",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            client_driver = subprocess.Popen(
                [
                    driver_binary,
                    f"-Daeron.dir={client_directory}",
                    "-Daeron.dir.delete.on.start=true",
                    "-Daeron.dir.delete.on.shutdown=true",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            deadline = time.monotonic() + 10.0
            while not (
                os.path.exists(os.path.join(server_directory, "cnc.dat"))
                and os.path.exists(os.path.join(client_directory, "cnc.dat"))
            ):
                if (
                    server_driver.poll() is not None
                    or client_driver.poll() is not None
                    or time.monotonic() >= deadline
                ):
                    raise RuntimeError("Aeron Media Drivers did not become ready")
                time.sleep(0.01)

            request_port = reserve_port(socket.SOCK_DGRAM)
            response_port = reserve_port(socket.SOCK_DGRAM)
            ordinary_port = reserve_port(socket.SOCK_STREAM)
            server = subprocess.Popen(
                [
                    server_binary,
                    "--enable-sbe",
                    "--aeron-dir",
                    server_directory,
                    "--aeron-ipc",
                    "3101",
                    "3102",
                    "--aeron-udp",
                    f"127.0.0.1:{request_port}",
                    "3201",
                    f"127.0.0.1:{response_port}",
                    "3202",
                    "--port",
                    str(ordinary_port),
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

            ipc = AeronIpcRedis(
                request_stream_id=3101,
                response_stream_id=3102,
                aeron_directory=server_directory,
                connect_timeout=10.0,
            )
            assert ipc.ping() is True
            assert ipc.set("python:aeron:ipc", "inside") is True
            assert ipc.get("python:aeron:ipc") == b"inside"

            udp = AeronUdpRedis(
                f"127.0.0.1:{request_port}",
                f"127.0.0.1:{response_port}",
                request_stream_id=3201,
                response_stream_id=3202,
                aeron_directory=client_directory,
                decode_responses=True,
                connect_timeout=10.0,
            )
            assert udp.ping() is True
            large_value = "u" * (32 * 1024)
            assert udp.set("python:aeron:udp", large_value) is True
            assert udp.get("python:aeron:udp") == large_value
            assert udp.info()["aeron_support"] == 1

            print("goblin_core Aeron IPC/UDP Python roundtrip OK")
            return 0
        finally:
            stop(server)
            stop(client_driver)
            stop(server_driver)


if __name__ == "__main__":
    raise SystemExit(main())
