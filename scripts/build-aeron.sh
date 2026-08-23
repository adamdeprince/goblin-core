#!/usr/bin/env bash
set -euo pipefail

aeron_version="1.51.0"
aeron_commit="9773cba37e4b88b2b7eb9460c4e0050267b71d28"
aeron_arch="$(uname -m)"
aeron_prefix="${1:-${HOME}/opt/aeron-${aeron_version}-${aeron_arch}}"
aeron_source_dir="${GOBLIN_AERON_SOURCE_DIR:-${HOME}/opt/src/aeron-${aeron_version}}"
aeron_build_dir="${GOBLIN_AERON_BUILD_DIR:-${HOME}/opt/build/aeron-${aeron_version}-${aeron_arch}}"
aeron_jobs="${GOBLIN_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}"

case "$(uname -s)" in
  Linux|Darwin) ;;
  *)
    echo "goblin-core: scripts/build-aeron.sh supports Linux and macOS" >&2
    exit 2
    ;;
esac

if [[ ! -d "${aeron_source_dir}/.git" ]]; then
  if [[ -e "${aeron_source_dir}" ]]; then
    echo "goblin-core: ${aeron_source_dir} exists but is not an Aeron checkout" >&2
    exit 2
  fi
  mkdir -p "$(dirname "${aeron_source_dir}")"
  git clone --branch "${aeron_version}" --depth 1 \
    https://github.com/aeron-io/aeron.git "${aeron_source_dir}"
fi

aeron_actual_commit="$(git -C "${aeron_source_dir}" rev-parse HEAD)"
if [[ "${aeron_actual_commit}" != "${aeron_commit}" ]]; then
  echo "goblin-core: Aeron checkout is ${aeron_actual_commit}; expected ${aeron_commit}" >&2
  echo "remove or change GOBLIN_AERON_SOURCE_DIR, then rerun the helper" >&2
  exit 2
fi

cmake -S "${aeron_source_dir}" -B "${aeron_build_dir}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${aeron_prefix}" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_AERON_DRIVER=ON \
  -DBUILD_AERON_ARCHIVE_API=OFF \
  -DAERON_TESTS=OFF \
  -DAERON_UNIT_TESTS=OFF \
  -DAERON_SYSTEM_TESTS=OFF \
  -DAERON_BUILD_SAMPLES=OFF \
  -DAERON_BUILD_DOCUMENTATION=OFF \
  -DAERON_INSTALL_TARGETS=ON
cmake --build "${aeron_build_dir}" --parallel "${aeron_jobs}"
cmake --install "${aeron_build_dir}"
cmake -E make_directory "${aeron_prefix}/share/licenses/aeron"
cmake -E copy_if_different "${aeron_source_dir}/LICENSE" \
  "${aeron_prefix}/share/licenses/aeron/LICENSE"

echo "Aeron ${aeron_version} installed in ${aeron_prefix}"
echo "configure Goblin Core with:"
echo "  cmake -S . -B build-aeron -DGOBLIN_CORE_ENABLE_AERON=ON \\"
echo "    -DGOBLIN_CORE_AERON_ROOT=${aeron_prefix}"
echo "run the external C Media Driver with:"
echo "  ${aeron_prefix}/bin/aeronmd_s"
