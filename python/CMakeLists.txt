# nanobind (>= 2.12) C++23 build of the goblin_core Python extension.
#
#   python3 -m venv .venv && .venv/bin/pip install "nanobind>=2.12"
#   cmake -S python -B python/build -DPython_EXECUTABLE=$(.venv/bin/python -c 'import sys;print(sys.executable)')
#   cmake --build python/build
#
# The built _goblin_core extension is placed next to goblin_core/__init__.py, so
# `PYTHONPATH=python python -c "import goblin_core"` works after a build.
cmake_minimum_required(VERSION 3.25)
project(goblin_core_python LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find the active interpreter and its development module, then nanobind's CMake
# package (installed in the same environment: pip install "nanobind>=2.12").
find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module)

execute_process(
  COMMAND "${Python_EXECUTABLE}" -c "import nanobind; print(nanobind.cmake_dir())"
  OUTPUT_VARIABLE NB_CMAKE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE NB_RESULT)
if(NOT NB_RESULT EQUAL 0)
  message(FATAL_ERROR "nanobind not found in ${Python_EXECUTABLE}; run: pip install \"nanobind>=2.12\"")
endif()
list(APPEND CMAKE_PREFIX_PATH "${NB_CMAKE_DIR}")
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(_goblin_core src/goblin_core_ext.cpp)
target_compile_features(_goblin_core PRIVATE cxx_std_23)
# The header-only SBE ring client lives in the parent repo's include/ tree; the
# generated SBE codecs it includes (goblin_sbe/*.h) live under sbe/generated/.
target_include_directories(_goblin_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../sbe/generated)

# Optional RDMA / ExaSock (same flags as the parent tree; not vendored).
option(GOBLIN_CORE_ENABLE_RDMA
  "Enable InfiniBand/RDMA support in the Python extension"
  OFF)
option(GOBLIN_CORE_ENABLE_EXASOCK
  "Enable Cisco ExaSock support in the Python extension"
  OFF)

if(GOBLIN_CORE_ENABLE_RDMA)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "GOBLIN_CORE_ENABLE_RDMA requires Linux + libibverbs/librdmacm")
  endif()
  find_path(IBVERBS_INCLUDE_DIR NAMES infiniband/verbs.h)
  find_path(RDMACM_INCLUDE_DIR NAMES rdma/rdma_cma.h)
  find_library(IBVERBS_LIBRARY NAMES ibverbs)
  find_library(RDMACM_LIBRARY NAMES rdmacm)
  if(NOT IBVERBS_INCLUDE_DIR OR NOT RDMACM_INCLUDE_DIR OR
     NOT IBVERBS_LIBRARY OR NOT RDMACM_LIBRARY)
    message(FATAL_ERROR
      "GOBLIN_CORE_ENABLE_RDMA=ON but libibverbs/librdmacm development files not found")
  endif()
  target_compile_definitions(_goblin_core PRIVATE GOBLIN_HAS_RDMA=1)
  target_include_directories(_goblin_core PRIVATE
    ${IBVERBS_INCLUDE_DIR} ${RDMACM_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
  # ClientTransport lives in rdma_ring.cpp (not header-only).
  target_sources(_goblin_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rdma_ring.cpp)
  target_link_libraries(_goblin_core PRIVATE
    ${IBVERBS_LIBRARY} ${RDMACM_LIBRARY})
  message(STATUS "goblin_core Python: RDMA support enabled")
endif()

if(GOBLIN_CORE_ENABLE_EXASOCK)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "GOBLIN_CORE_ENABLE_EXASOCK requires Linux + ExaSock SDK")
  endif()
  find_path(EXASOCK_INCLUDE_DIR NAMES exasock/extensions.h PATHS /usr/local/include /usr/include)
  find_library(EXASOCK_EXT_LIBRARY NAMES exasock_ext PATHS /usr/local/lib /usr/lib)
  if(NOT EXASOCK_INCLUDE_DIR OR NOT EXASOCK_EXT_LIBRARY)
    message(FATAL_ERROR
      "GOBLIN_CORE_ENABLE_EXASOCK=ON but ExaSock SDK not found (exasock/*.h, libexasock_ext)")
  endif()
  target_compile_definitions(_goblin_core PRIVATE GOBLIN_HAS_EXASOCK=1)
  target_include_directories(_goblin_core PRIVATE ${EXASOCK_INCLUDE_DIR})
  target_link_libraries(_goblin_core PRIVATE ${EXASOCK_EXT_LIBRARY})
  message(STATUS "goblin_core Python: ExaSock support enabled")
endif()

# Drop the extension into the package directory so it is importable in place.
set_target_properties(_goblin_core PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/goblin_core)

# For an installed build (scikit-build-core / pip), install into the package.
install(TARGETS _goblin_core LIBRARY DESTINATION goblin_core)
