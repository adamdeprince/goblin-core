#!/bin/sh
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y python3-venv /release/bookworm/*_0.10.6-2+*.deb
python3 -m venv /tmp/python-test
/tmp/python-test/bin/pip install /release/python-linux/*.whl
export PATH=/tmp/python-test/bin:$PATH
python -c 'import goblin_core; assert goblin_core.__version__ == "0.10.6"'
GOBLIN_TEST_INSTALLED_WHEEL=1 python /release/work/bookworm/goblin-core-0.10.6/python/tests/test_ring_roundtrip.py /usr/bin/goblin-core
GOBLIN_TEST_INSTALLED_WHEEL=1 python /release/work/bookworm/goblin-core-0.10.6/python/tests/test_reconnect.py /usr/bin/goblin-core
touch /release/python-linux/installed.pass
