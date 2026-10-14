#!/bin/sh
# Isolated Linux wheel validation. Does not upload to PyPI.
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y python3-dev python3-venv python3-pip patchelf
apt-get install -y /release/bookworm/*_0.10.6-2+*.deb
python3 -m venv /tmp/python-build
/tmp/python-build/bin/pip install build 'scikit-build-core>=0.10,<1' 'nanobind>=2.12' 'cmake>=3.25' 'ninja>=1.5' auditwheel 'patchelf>=0.14.5'
export PATH=/tmp/python-build/bin:$PATH
mkdir -p /tmp/python-source /release/python-linux
tar -xzf /release/goblin_core-0.10.6.tar.gz -C /tmp/python-source
python -m build --wheel --no-isolation /tmp/python-source/goblin_core-0.10.6 --outdir /tmp/wheels
auditwheel repair --plat manylinux_2_36_x86_64 /tmp/wheels/*.whl -w /release/python-linux
pip install --no-deps /release/python-linux/*.whl
python -c 'import goblin_core; assert goblin_core.__version__ == "0.10.6"; print("PASS: Linux wheel version", goblin_core.__version__)'
GOBLIN_TEST_INSTALLED_WHEEL=1 python /release/work/bookworm/goblin-core-0.10.6/python/tests/test_ring_roundtrip.py /usr/bin/goblin-core
GOBLIN_TEST_INSTALLED_WHEEL=1 python /release/work/bookworm/goblin-core-0.10.6/python/tests/test_reconnect.py /usr/bin/goblin-core
touch /release/python-linux/installed.pass
