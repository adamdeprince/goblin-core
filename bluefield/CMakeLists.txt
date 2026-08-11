cmake_minimum_required(VERSION 3.16)

project(goblin-core-bluefield
  VERSION 0.10.3
  DESCRIPTION "Low-latency RESP Pub/Sub edge for NVIDIA BlueField"
  LANGUAGES CXX)

include(GNUInstallDirs)

option(GOBLIN_BLUEFIELD_LTO "Enable link-time optimization in Release builds" ON)
option(GOBLIN_BLUEFIELD_TUNE_CORTEX_A72
  "Tune generated AArch64 code for the BlueField-2 Cortex-A72 cores" ON)
option(GOBLIN_BLUEFIELD_STATIC_GNU_RUNTIME
  "Statically link libstdc++ and libgcc into the edge executable" ON)
option(GOBLIN_BLUEFIELD_BUILD_BENCHMARKS
  "Build the native client-observed local Pub/Sub latency probe" OFF)
option(GOBLIN_BLUEFIELD_ENABLE_XLIO
  "Enable native NVIDIA XLIO Ultra for the client-facing listener" OFF)

find_path(SODIUM_INCLUDE_DIR NAMES sodium.h REQUIRED)
find_library(SODIUM_LIBRARY NAMES sodium libsodium.so.23 REQUIRED)

set(GOBLIN_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

add_executable(goblin_core_bluefield
  ${GOBLIN_SOURCE_ROOT}/src/bluefield_main.cpp
  ${GOBLIN_SOURCE_ROOT}/src/bluefield_relay.cpp
  ${GOBLIN_SOURCE_ROOT}/src/bluefield_sbe.cpp
  ${GOBLIN_SOURCE_ROOT}/src/auth.cpp
  ${GOBLIN_SOURCE_ROOT}/src/pubsub.cpp
  ${GOBLIN_SOURCE_ROOT}/src/resp_parser.cpp)

if(GOBLIN_BLUEFIELD_ENABLE_XLIO)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
      "goblin-core-bluefield: GOBLIN_BLUEFIELD_ENABLE_XLIO requires Linux")
  endif()
  if(NOT EXISTS
      ${GOBLIN_SOURCE_ROOT}/third_party/xlio/src/core/xlio_extra.h)
    message(FATAL_ERROR
      "goblin-core-bluefield: vendored XLIO Ultra headers are missing")
  endif()
  target_sources(goblin_core_bluefield PRIVATE
    ${GOBLIN_SOURCE_ROOT}/src/xlio_transport.cpp)
  target_compile_definitions(goblin_core_bluefield PRIVATE GOBLIN_HAS_XLIO=1)
  target_include_directories(goblin_core_bluefield SYSTEM PRIVATE
    ${GOBLIN_SOURCE_ROOT}/third_party/xlio/src/core)
endif()

target_include_directories(goblin_core_bluefield SYSTEM BEFORE PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/compat)
target_include_directories(goblin_core_bluefield PRIVATE
  ${GOBLIN_SOURCE_ROOT}/include
  ${GOBLIN_SOURCE_ROOT}/src
  ${SODIUM_INCLUDE_DIR})
target_include_directories(goblin_core_bluefield SYSTEM PRIVATE
  ${GOBLIN_SOURCE_ROOT}/sbe/generated
  ${GOBLIN_SOURCE_ROOT}/third_party/xxhash
  ${GOBLIN_SOURCE_ROOT}/third_party/fast_float/include
  ${GOBLIN_SOURCE_ROOT}/third_party/unordered_dense/include)
target_link_libraries(goblin_core_bluefield PRIVATE ${SODIUM_LIBRARY})
target_compile_definitions(goblin_core_bluefield PRIVATE
  GOBLIN_BLUEFIELD_STANDALONE=1
  GOBLIN_CORE_VERSION="${PROJECT_VERSION}")
set_target_properties(goblin_core_bluefield PROPERTIES
  OUTPUT_NAME goblin-core-bluefield
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(goblin_core_bluefield PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O3>)
  if(GOBLIN_BLUEFIELD_TUNE_CORTEX_A72 AND
     CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    target_compile_options(goblin_core_bluefield PRIVATE
      -mcpu=cortex-a72 -mtune=cortex-a72)
  endif()
endif()

if(GOBLIN_BLUEFIELD_STATIC_GNU_RUNTIME AND
   CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_link_options(goblin_core_bluefield PRIVATE
    -static-libstdc++ -static-libgcc)
endif()

if(GOBLIN_BLUEFIELD_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT GOBLIN_BLUEFIELD_IPO_SUPPORTED
                      OUTPUT GOBLIN_BLUEFIELD_IPO_ERROR)
  if(GOBLIN_BLUEFIELD_IPO_SUPPORTED)
    set_property(TARGET goblin_core_bluefield PROPERTY
      INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  else()
    message(WARNING "BlueField LTO unavailable: ${GOBLIN_BLUEFIELD_IPO_ERROR}")
  endif()
endif()

if(GOBLIN_BLUEFIELD_BUILD_BENCHMARKS)
  add_executable(goblin_bluefield_local_latency
    ${GOBLIN_SOURCE_ROOT}/benchmarks/bluefield_local_latency.cpp)
  set_target_properties(goblin_bluefield_local_latency PROPERTIES
    OUTPUT_NAME goblin-bluefield-local-latency
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(goblin_bluefield_local_latency PRIVATE
      -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3>)
    if(GOBLIN_BLUEFIELD_TUNE_CORTEX_A72 AND
       CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
      target_compile_options(goblin_bluefield_local_latency PRIVATE
        -mcpu=cortex-a72 -mtune=cortex-a72)
    endif()
  endif()
endif()

install(TARGETS goblin_core_bluefield
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(
  FILES
    ${GOBLIN_SOURCE_ROOT}/LICENSE
    ${GOBLIN_SOURCE_ROOT}/NOTICE
  DESTINATION ${CMAKE_INSTALL_DOCDIR})

install(
  FILES ${GOBLIN_SOURCE_ROOT}/third_party/unordered_dense/LICENSE
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/third_party/unordered_dense)
install(
  FILES ${GOBLIN_SOURCE_ROOT}/third_party/xxhash/LICENSE
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/third_party/xxhash)
install(
  FILES ${GOBLIN_SOURCE_ROOT}/third_party/fast_float/LICENSE-APACHE
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/third_party/fast_float)

if(GOBLIN_BLUEFIELD_ENABLE_XLIO)
  install(
    FILES
      ${GOBLIN_SOURCE_ROOT}/third_party/xlio/LICENSE
      ${GOBLIN_SOURCE_ROOT}/third_party/xlio/debian/copyright
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/third_party/xlio)
  install(
    FILES
      ${GOBLIN_SOURCE_ROOT}/third_party/libdpcp/LICENSE
      ${GOBLIN_SOURCE_ROOT}/third_party/libdpcp/debian/copyright
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/third_party/libdpcp)
endif()
