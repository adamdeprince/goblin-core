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
# The header-only ring client lives in the parent repo's include/ tree.
target_include_directories(_goblin_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# Drop the extension into the package directory so it is importable in place.
set_target_properties(_goblin_core PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/goblin_core)

# For an installed build (scikit-build-core / pip), install into the package.
install(TARGETS _goblin_core LIBRARY DESTINATION goblin_core)
