#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${root_dir}/third_party/libfabric"
arch="$(uname -m)"
prefix="${1:-${HOME}/opt/libfabric-2.4.0amzn5.0-${arch}}"
build_dir="${GOBLIN_LIBFABRIC_BUILD_DIR:-${HOME}/opt/build/libfabric-2.4.0amzn5.0-${arch}}"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "goblin-core: libfabric/EFA is supported only on Linux" >&2
  exit 2
fi
if [[ ! -x "${source_dir}/configure" ]]; then
  echo "goblin-core: vendored libfabric configure script is missing" >&2
  exit 2
fi

mkdir -p "${build_dir}" "${prefix}"
cd "${build_dir}"

"${source_dir}/configure" \
  --prefix="${prefix}" \
  --disable-shared \
  --enable-static \
  --enable-only \
  --enable-tcp=yes \
  --enable-rxm=yes \
  --enable-verbs=yes \
  --enable-shm=yes \
  --enable-efa=auto \
  --without-dlopen \
  --disable-debug \
  --disable-profile

make -j"${GOBLIN_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}"
make install

echo "libfabric installed in ${prefix}"
echo "configure Goblin Core with:"
echo "  cmake -S . -B build -DGOBLIN_CORE_ENABLE_LIBFABRIC=ON \\"
echo "    -DGOBLIN_CORE_LIBFABRIC_ROOT=${prefix}"
