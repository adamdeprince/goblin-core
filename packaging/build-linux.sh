#!/bin/sh
# Run only inside a disposable Debian/Ubuntu build container.
set -eu
suite=$1
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y build-essential ninja-build python3 python3-pip python3-venv \
    liblz4-dev libsodium-dev libssl-dev dpkg-dev ca-certificates curl gnupg
if [ "$suite" = jammy ]; then
    apt-get install -y gcc-12 g++-12
    export CC=gcc-12 CXX=g++-12
fi
python3 -m venv /tmp/build-tools
/tmp/build-tools/bin/pip install 'cmake>=3.25,<5'
export PATH=/tmp/build-tools/bin:$PATH
mkdir -p /work/"$suite"
tar -xzf /release/goblin-core-0.10.6.tar.gz -C /work/"$suite"
cd /work/"$suite"/goblin-core-0.10.6
# Reproducible archives use fixed timestamps; never reuse object files from a
# different source snapshot even if their paths and timestamps are identical.
rm -rf build-package
if [ -f /release/package_deb.py ]; then cp /release/package_deb.py scripts/package_deb.py; fi
cmake -S . -B build-package -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DGOBLIN_CORE_BUILD_TESTS=OFF -DGOBLIN_CORE_BUILD_BENCHMARKS=OFF \
    -DGOBLIN_CORE_BUILD_HTML_DOCS=OFF -DGOBLIN_CORE_ENABLE_KAFKA=OFF \
    -DGOBLIN_CORE_ENABLE_RDMA=OFF
cmake --build build-package --target goblin_core_server goblin_core_auth -j4
python3 scripts/package_deb.py --build build-package --output /release/"$suite" --suite "$suite"
apt-get install -y /release/"$suite"/*_0.10.6-2+*.deb
python3 packaging/service-smoke.py /usr/libexec/goblin-core-service
cmake -S packaging/client-smoke -B /tmp/client-smoke
cmake --build /tmp/client-smoke
/tmp/client-smoke/client-smoke
touch /release/"$suite"/installed.pass
