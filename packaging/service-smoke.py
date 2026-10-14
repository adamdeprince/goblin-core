#!/usr/bin/env python3
"""Exercise an installed service wrapper in an isolated network namespace."""
import os
import socket
import stat
import subprocess
import sys
import time
from pathlib import Path

wrapper=Path(sys.argv[1])
# Linux container checks the unchanged default port, paths and configuration.
subprocess.run(['install','-d','-o','goblin-core','-g','goblin-core','-m','0750','/run/goblin-core'],check=True)
process=subprocess.Popen(['runuser','-u','goblin-core','--',str(wrapper)], stdout=open('/tmp/goblin-service.log','w'), stderr=subprocess.STDOUT)
try:
    path=Path('/run/goblin-core/goblin-core.sock')
    for _ in range(200):
        if path.is_socket(): break
        if process.poll() is not None: raise RuntimeError(Path('/tmp/goblin-service.log').read_text())
        time.sleep(0.05)
    assert path.is_socket(), 'default UDS missing'
    assert stat.S_IMODE(path.stat().st_mode)==0o660
    import pwd
    assert path.stat().st_uid==pwd.getpwnam('goblin-core').pw_uid
    for family,address in [(socket.AF_INET,('127.0.0.1',6379)),
                           (socket.AF_INET6,('::1',6379)),
                           (socket.AF_UNIX,str(path))]:
        with socket.socket(family,socket.SOCK_STREAM) as conn:
            conn.settimeout(5)
            conn.connect(address)
            with conn.makefile('rb') as reply:
                conn.sendall(b'*1\r\n$4\r\nPING\r\n')
                assert reply.readline()==b'+PONG\r\n'
                conn.sendall(b'*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n')
                assert reply.readline()==b'+OK\r\n'
                conn.sendall(b'*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n')
                assert reply.readline()==b'$5\r\n' and reply.readline()==b'value\r\n'
    print('PASS: default TCP 6379 (IPv4/IPv6), UDS 0660, PING, SET/GET')
finally:
    process.terminate()
    process.wait(timeout=10)
