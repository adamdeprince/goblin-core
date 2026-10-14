#!/bin/sh
# Run in a fresh disposable Debian/Ubuntu container after publishing metadata.
set -eu
suite=$1
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y ca-certificates curl gnupg python3-venv python3-pip build-essential
mkdir -p /etc/apt/keyrings
curl -fsSL https://apt.goblinreactor.com/goblinreactor-archive-keyring.gpg \
    -o /etc/apt/keyrings/goblinreactor-archive-keyring.gpg
fingerprint=$(gpg --batch --show-keys --with-colons /etc/apt/keyrings/goblinreactor-archive-keyring.gpg |
    awk -F: '$1 == "fpr" {print $10; exit}')
test "$fingerprint" = B7B61E8CB85A9270530364F23273CB62402FC793
printf '%s\n' 'Types: deb' 'URIs: https://apt.goblinreactor.com/' \
    "Suites: $suite" 'Components: main' 'Architectures: amd64' \
    'Signed-By: /etc/apt/keyrings/goblinreactor-archive-keyring.gpg' \
    > /etc/apt/sources.list.d/goblinreactor.sources
apt-get update
apt-get install -y goblin-core goblin-core-cpp
test "$(dpkg-query -W -f='${Version}' goblin-core)" = "0.10.6-2+${suite}1"
python3 /release/work/"$suite"/goblin-core-0.10.6/packaging/service-smoke.py /usr/libexec/goblin-core-service
python3 -m venv /tmp/test-tools
/tmp/test-tools/bin/pip install 'cmake>=3.25,<5'
export PATH=/tmp/test-tools/bin:$PATH
cmake -S /release/work/"$suite"/goblin-core-0.10.6/packaging/client-smoke -B /tmp/client-smoke
cmake --build /tmp/client-smoke
/tmp/client-smoke/client-smoke
# Test removal, configuration preservation, and installation again.
echo '# install/remove validation' >> /etc/goblin-core/goblin-core.conf
apt-get remove -y goblin-core goblin-core-cpp
test -f /etc/goblin-core/goblin-core.conf
apt-get install -y goblin-core goblin-core-cpp
grep -q 'install/remove validation' /etc/goblin-core/goblin-core.conf
touch /release/"$suite"/public.pass
echo "PASS: signed public APT install, default service, SDK, remove/reinstall: $suite"
