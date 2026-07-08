cmake_minimum_required(VERSION 3.25)

project(goblin-core
  VERSION 0.4.1
  DESCRIPTION "A compact SIMD-oriented Redis-like server from Goblin Reactor"
  LANGUAGES CXX)

include(GNUInstallDirs)

set(GOBLIN_CORE_ARCH "" CACHE STRING
    "ISA tuning for the build host: native, avx2, avx512, lsx, or lasx")
set_property(CACHE GOBLIN_CORE_ARCH PROPERTY STRINGS
    "" native avx2 avx512 lsx lasx)

option(GOBLIN_CORE_BUILD_TESTS "Build Goblin Core tests" ON)
option(GOBLIN_CORE_BUILD_BENCHMARKS "Build Goblin Core benchmark executables" ON)
option(GOBLIN_CORE_BUILD_HTML_DOCS "Build HTML documentation from Markdown" ON)
option(GOBLIN_CORE_REDIS_DIFFERENTIAL_TESTS
  "Enable Redis-backed differential tests for Goblin Core's Redis-compatible subset"
  OFF)

set(GOBLIN_CORE_MARKDOWN_DOCS
  README.md
  BENCHMARKS.md
  MICROBENCHMARKS.md
  PERFORMANCE_BRIEF.md
  RELEASE.md)

set(GOBLIN_CORE_MARKDOWN_DOC_PATHS)
foreach(GOBLIN_CORE_MARKDOWN_DOC IN LISTS GOBLIN_CORE_MARKDOWN_DOCS)
  list(APPEND GOBLIN_CORE_MARKDOWN_DOC_PATHS
    ${CMAKE_CURRENT_SOURCE_DIR}/${GOBLIN_CORE_MARKDOWN_DOC})
endforeach()

set(GOBLIN_CORE_HTML_DOCS
  ${CMAKE_CURRENT_SOURCE_DIR}/html/index.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/BENCHMARKS.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/MICROBENCHMARKS.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/PERFORMANCE_BRIEF.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/RELEASE.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/styles.css)

if(GOBLIN_CORE_BUILD_HTML_DOCS OR GOBLIN_CORE_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()

if(GOBLIN_CORE_BUILD_HTML_DOCS)

  add_custom_command(
    OUTPUT ${GOBLIN_CORE_HTML_DOCS}
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_html_docs.py
      --output ${CMAKE_CURRENT_SOURCE_DIR}/html
      ${GOBLIN_CORE_MARKDOWN_DOC_PATHS}
    DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_html_docs.py
      ${GOBLIN_CORE_MARKDOWN_DOC_PATHS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building HTML documentation"
    VERBATIM)

  add_custom_target(goblin_core_html_docs ALL
    DEPENDS ${GOBLIN_CORE_HTML_DOCS})
endif()

add_library(goblin_core
  src/command.cpp
  src/rdb.cpp
  src/resp_parser.cpp
  src/resp_writer.cpp
  src/server.cpp
  src/simd.cpp
  src/store.cpp)

target_compile_features(goblin_core PUBLIC cxx_std_23)
set_target_properties(goblin_core PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

target_include_directories(goblin_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Enable the CRC32C hardware instruction used by the snapshot checksum on x86
# (SSE4.2). AArch64 uses its CRC extension where the platform enables it (default
# on Apple; -march=native on Linux ARM), and LoongArch uses crcc unconditionally;
# elsewhere the software table runs. PUBLIC so the header's inline checksum takes
# the same path in the library, server, and tests.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-msse4.2" GOBLIN_HAS_SSE42)
  if(GOBLIN_HAS_SSE42)
    target_compile_options(goblin_core PUBLIC -msse4.2)
  endif()
endif()

if(GOBLIN_CORE_ARCH STREQUAL "native")
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(goblin_core PUBLIC -march=native)
  endif()
elseif(GOBLIN_CORE_ARCH STREQUAL "avx2")
  target_compile_options(goblin_core PUBLIC -mavx2)
elseif(GOBLIN_CORE_ARCH STREQUAL "avx512")
  target_compile_options(goblin_core PUBLIC -mavx512f -mavx512bw -mavx512vl)
elseif(GOBLIN_CORE_ARCH STREQUAL "lsx")
  target_compile_options(goblin_core PUBLIC -mlsx)
elseif(GOBLIN_CORE_ARCH STREQUAL "lasx")
  target_compile_options(goblin_core PUBLIC -mlasx)
endif()

# Optional maintainer target: regenerate the checked-in command-dispatch perfect
# hash after editing src/command_hash.gperf. The checked-in src/command_hash.hpp is
# authoritative, so builds without gperf are unaffected.
find_program(GPERF_EXECUTABLE gperf)
if(GPERF_EXECUTABLE)
  add_custom_target(goblin_core_regen_command_hash
    COMMAND ${GPERF_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/src/command_hash.gperf
            --output-file=${CMAKE_CURRENT_SOURCE_DIR}/src/command_hash.hpp
    COMMENT "Regenerating command-dispatch perfect hash from command_hash.gperf"
    VERBATIM)
endif()

if(MSVC)
  target_compile_options(goblin_core PRIVATE /W4)
else()
  target_compile_options(goblin_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(goblin_core_server src/main.cpp)
target_link_libraries(goblin_core_server PRIVATE goblin_core)
set_target_properties(goblin_core_server PROPERTIES
  OUTPUT_NAME goblin-core
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

if(GOBLIN_CORE_BUILD_BENCHMARKS)
  add_executable(goblin_core_microbench benchmarks/microbench.cpp)
  target_link_libraries(goblin_core_microbench PRIVATE goblin_core)
  set_target_properties(goblin_core_microbench PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
endif()

if(GOBLIN_CORE_BUILD_TESTS)
  include(CTest)

  add_executable(goblin_core_tests tests/goblin_core_tests.cpp)
  target_link_libraries(goblin_core_tests PRIVATE goblin_core)
  set_target_properties(goblin_core_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

  add_test(NAME goblin_core_tests COMMAND goblin_core_tests)

  add_test(
    NAME goblin_core_output_backpressure
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/output_backpressure.py
      --goblin-bin $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_output_backpressure
    PROPERTIES TIMEOUT 60)

  if(GOBLIN_CORE_REDIS_DIFFERENTIAL_TESTS)
    find_program(REDIS_SERVER_EXECUTABLE NAMES redis-server)
    if(REDIS_SERVER_EXECUTABLE)
      add_test(
        NAME goblin_core_redis_differential_sequential
        COMMAND
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/redis_differential.py
          --goblin-bin $<TARGET_FILE:goblin_core_server>
          --redis-server ${REDIS_SERVER_EXECUTABLE})
      set_tests_properties(goblin_core_redis_differential_sequential
        PROPERTIES TIMEOUT 30)

      add_test(
        NAME goblin_core_redis_differential_pipelined
        COMMAND
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/redis_differential.py
          --goblin-bin $<TARGET_FILE:goblin_core_server>
          --redis-server ${REDIS_SERVER_EXECUTABLE}
          --pipeline-depth 32)
      set_tests_properties(goblin_core_redis_differential_pipelined
        PROPERTIES TIMEOUT 30)
    else()
      message(WARNING
        "GOBLIN_CORE_REDIS_DIFFERENTIAL_TESTS is ON, but redis-server was not found")
    endif()
  endif()
endif()

include(CMakePackageConfigHelpers)

install(
  TARGETS goblin_core goblin_core_server
  EXPORT goblinCoreTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
  DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
  FILES
    README.md
    BENCHMARKS.md
    MICROBENCHMARKS.md
    PERFORMANCE_BRIEF.md
    RELEASE.md
    LICENSE
    NOTICE
  DESTINATION ${CMAKE_INSTALL_DOCDIR})

if(GOBLIN_CORE_BUILD_HTML_DOCS)
  install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/html/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/html
    OPTIONAL)
endif()

install(
  EXPORT goblinCoreTargets
  NAMESPACE goblin::core::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/goblin-core)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/goblin-coreConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/goblin-coreConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/goblin-core)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/goblin-coreConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/goblin-coreConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/goblin-coreConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/goblin-core)
