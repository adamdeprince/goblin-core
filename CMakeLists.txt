cmake_minimum_required(VERSION 3.25)

project(goblin-core
  VERSION 0.8.0
  DESCRIPTION "A compact SIMD-oriented Redis-like server from Goblin Reactor"
  LANGUAGES CXX C)

include(GNUInstallDirs)

set(GOBLIN_CORE_ARCH "" CACHE STRING
    "ISA tuning for the build host: native, avx2, avx512, lsx, or lasx")
set_property(CACHE GOBLIN_CORE_ARCH PROPERTY STRINGS
    "" native avx2 avx512 lsx lasx)

option(GOBLIN_CORE_BUILD_TESTS "Build Goblin Core tests" ON)
option(GOBLIN_CORE_BUILD_BENCHMARKS "Build Goblin Core benchmark executables" ON)
option(GOBLIN_CORE_BUILD_HTML_DOCS "Build HTML documentation from Markdown" ON)
option(GOBLIN_CORE_ENABLE_RDMA
  "Enable InfiniBand/RDMA polled-ring transport (Linux libibverbs + librdmacm; not vendored)"
  ON)
option(GOBLIN_CORE_ENABLE_EXASOCK
  "Enable Cisco ExaSock / Nexus SmartNIC sockets support (requires installed ExaSock SDK; not vendored)"
  OFF)
option(GOBLIN_CORE_ENABLE_KAFKA
  "Enable RESP2 write ingestion from Kafka using vendored librdkafka"
  ON)
option(GOBLIN_CORE_ENABLE_TLS
  "Enable OpenSSL TLS on ordinary TCP listeners"
  ON)
option(GOBLIN_CORE_STATIC_GNU_RUNTIME
  "Statically link the GNU libstdc++ and libgcc runtimes into C++ executables"
  ON)
option(GOBLIN_CORE_REDIS_DIFFERENTIAL_TESTS
  "Enable Redis-backed differential tests for Goblin Core's Redis-compatible subset"
  OFF)

if(GOBLIN_CORE_STATIC_GNU_RUNTIME AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_link_options(
    "$<$<LINK_LANGUAGE:CXX>:-static-libstdc++>"
    "$<$<LINK_LANGUAGE:CXX>:-static-libgcc>")
endif()

# The generator walks the project's complete Markdown set when invoked without
# positional sources. Track the same set here so edits to blogs and nested docs
# rebuild their HTML instead of leaving stale site-wide navigation behind.
file(GLOB_RECURSE GOBLIN_CORE_MARKDOWN_DOC_PATHS CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.md)
list(FILTER GOBLIN_CORE_MARKDOWN_DOC_PATHS EXCLUDE REGEX
  "/(\\.git|html|third_party|memory|benchmark-results|node_modules|build[^/]*)/")
list(FILTER GOBLIN_CORE_MARKDOWN_DOC_PATHS EXCLUDE REGEX
  "/(RELEASE|RING-LATENCY-HANDOFF)\\.md$")

set(GOBLIN_CORE_HTML_DOCS
  ${CMAKE_CURRENT_SOURCE_DIR}/html/BENCHMARKS.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/HSET-BENCHMARK.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/LIST-BENCHMARK.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/LIST-WORK-QUEUE-BENCHMARK.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/SET-BENCHMARK.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/ARRAY-BENCHMARK.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/LATENCY-SHOOTOUT.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/MICROBENCHMARKS.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/LISTS.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/PERFORMANCE_BRIEF.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/PUBSUB-BENCHMARK.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/RING-BENCHMARKS.html
  ${CMAKE_CURRENT_SOURCE_DIR}/html/RELEASE-HISTORY.html
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
    DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_html_docs.py
      ${GOBLIN_CORE_MARKDOWN_DOC_PATHS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building HTML documentation"
    VERBATIM)

  add_custom_target(goblin_core_html_docs ALL
    DEPENDS ${GOBLIN_CORE_HTML_DOCS})
endif()

# Vendored Lua 5.1.5 runtime plus the standard scripting helper libraries
# (cjson, cmsgpack, struct, bit), each taken from its own public upstream -- see
# third_party/README.md. Built as a separate static library so its C sources are
# compiled as plain C and are NOT subject to goblin_core's strict warning flags.
# The public API is reached from src/script.cpp via extern "C"; errors stay on the
# Lua longjmp path, so the trampolines in script.cpp hold no C++ destructors across
# a Lua API call (their scratch buffers live on the ScriptEngine, not the stack).
file(GLOB GOBLIN_LUA_CORE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/*.c)
add_library(goblin_lua STATIC
  ${GOBLIN_LUA_CORE_SOURCES}
  third_party/lua-cjson/lua_cjson.c
  third_party/lua-cjson/strbuf.c
  third_party/lua-cjson/fpconv.c
  third_party/lua-cmsgpack/lua_cmsgpack.c
  third_party/lua-struct/struct.c
  third_party/lua-bitop/bit.c)
# PRIVATE: goblin_core reaches the PUC-Lua headers through a per-source include on
# src/script.cpp (see below), NOT through this library's interface -- otherwise its
# "lua.h" would collide on goblin_core's include path with Luau's own "lua.h".
target_include_directories(goblin_lua PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-cjson)
set_target_properties(goblin_lua PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE ON)
# Keep the vendored C quiet under the project's -Werror-adjacent flags without
# touching upstream source; these are third-party files we do not lint.
if(NOT MSVC)
  target_compile_options(goblin_lua PRIVATE -w)
endif()

# Vendored Luau runtime (Roblox's typed, sandboxed Lua dialect) -- the minimal
# Common + Ast + Bytecode + Compiler + VM subset needed to compile and run source
# (no CodeGen JIT, no Analysis type-checker). Built as its own C++17 static library
# with warnings off. Its public API has C++ linkage, so its symbols do not collide
# with the C-linkage PUC-Lua symbols in goblin_lua even though both derive from Lua.
file(GLOB GOBLIN_LUAU_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau/Common/src/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau/Ast/src/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau/Bytecode/src/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau/Compiler/src/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau/VM/src/*.cpp)
add_library(goblin_luau_vm STATIC ${GOBLIN_LUAU_SOURCES})
# All PRIVATE: goblin_core reaches Luau's public C API (lua.h / lualib.h /
# luacode.h) through a per-source include on src/luau_script.cpp (see below), so
# Luau's "lua.h" never lands on the same include path as PUC-Lua's.
target_include_directories(goblin_luau_vm PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau/VM/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau/Compiler/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau/Common/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau/Ast/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau/Bytecode/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau/Compiler/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau/VM/src)
set_target_properties(goblin_luau_vm PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
  target_compile_options(goblin_luau_vm PRIVATE -w)
endif()
# PUC-Lua (goblin_lua) and Luau (goblin_luau_vm) both derive from Lua, so a few
# internal global *data* symbols share a name (C++ mangles functions apart, but
# not file-scope variables). Rename Luau's copies -- they are internal, not part
# of the public API goblin_core calls -- so the two static libraries link cleanly
# into one binary.
target_compile_definitions(goblin_luau_vm PRIVATE
  lua_ident=luau_lua_ident
  luaO_nilobject_=luau_luaO_nilobject_
  luaT_typenames=luau_luaT_typenames)

# Vendored Wren runtime (https://wren.io) -- a small class-based scripting
# language, powering the WREN.* command family. Pure C, built as its own static
# library with warnings off. Its symbols are wren-prefixed C symbols, so they do
# not collide with either Lua runtime.
file(GLOB GOBLIN_WREN_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wren/vm/*.c
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wren/optional/*.c)
add_library(goblin_wren STATIC ${GOBLIN_WREN_SOURCES})
target_include_directories(goblin_wren PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wren/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wren/vm
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wren/optional)
target_compile_definitions(goblin_wren PRIVATE WREN_OPT_META=1 WREN_OPT_RANDOM=1)
set_target_properties(goblin_wren PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
  target_compile_options(goblin_wren PRIVATE -w)
endif()

# Vendored Jim Tcl runtime (https://jim.tcl.tk) -- a small, embeddable Tcl,
# powering the TCL.* command family. Only the interpreter core plus the stdlib and
# tclcompat Tcl libraries are vendored (no I/O, eventloop, or interp extensions);
# jimautoconf.h is a hand-written portable config so no autosetup step is needed.
# Pure C, built as its own static library; symbols are Jim_-prefixed C symbols.
file(GLOB GOBLIN_JIM_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jimtcl/*.c)
add_library(goblin_jim STATIC ${GOBLIN_JIM_SOURCES})
target_include_directories(goblin_jim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jimtcl)
set_target_properties(goblin_jim PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
  target_compile_options(goblin_jim PRIVATE -w)
endif()

# Vendored MicroPython, generated by its embed port (ports/embed) into a
# self-contained C tree under third_party/micropython/. The config
# (mpconfigport.h) is the minimal ROM level plus byte-strings, floats, big ints,
# and readable errors; the genhdr/ headers are generated from it. Built as its own
# C static library; its symbols are mp_-prefixed and collide with nothing.
file(GLOB GOBLIN_UPY_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/micropython/micropython_embed/py/*.c
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/micropython/micropython_embed/shared/runtime/*.c
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/micropython/micropython_embed/port/*.c)
add_library(goblin_upy STATIC ${GOBLIN_UPY_SOURCES})
target_include_directories(goblin_upy PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/micropython
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/micropython/micropython_embed
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/micropython/micropython_embed/port)
set_target_properties(goblin_upy PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
  target_compile_options(goblin_upy PRIVATE -w)
endif()

# Vendored QuickJS (the quickjs-ng fork, https://github.com/quickjs-ng/quickjs),
# the JavaScript engine powering the QUICKJS.* command family. Only the four core
# sources are built -- the quickjs-libc.c file-/os-binding module is deliberately
# left out for the sandbox. Pure C11, built as its own static library with
# warnings off; its symbols are JS_-prefixed C symbols and collide with nothing.
add_library(goblin_qjs STATIC
  third_party/quickjs/quickjs.c
  third_party/quickjs/libregexp.c
  third_party/quickjs/libunicode.c
  third_party/quickjs/dtoa.c)
target_include_directories(goblin_qjs PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs)
target_compile_definitions(goblin_qjs PRIVATE _GNU_SOURCE)
set_target_properties(goblin_qjs PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
  target_compile_options(goblin_qjs PRIVATE -w)
endif()
# QuickJS uses libm, and pthreads for its atomics surface; propagate both so the
# final binary links them (a no-op on macOS, required on Linux).
find_package(Threads REQUIRED)
target_link_libraries(goblin_qjs PUBLIC m Threads::Threads)

find_path(LZ4_INCLUDE_DIR NAMES lz4.h REQUIRED)
find_library(LZ4_LIBRARY NAMES lz4 REQUIRED)
find_path(SODIUM_INCLUDE_DIR NAMES sodium.h REQUIRED)
find_library(SODIUM_LIBRARY NAMES sodium REQUIRED)

set(GOBLIN_HAS_TLS_BUILD OFF)
if(GOBLIN_CORE_ENABLE_TLS)
  find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
  set(GOBLIN_HAS_TLS_BUILD ON)
endif()

set(GOBLIN_HAS_KAFKA_BUILD OFF)
if(GOBLIN_CORE_ENABLE_KAFKA)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/librdkafka/CMakeLists.txt)
    message(FATAL_ERROR
      "goblin-core: GOBLIN_CORE_ENABLE_KAFKA=ON but vendored librdkafka is missing")
  endif()

  # Keep the embedded client reproducible and dependency-light. librdkafka's
  # bundled Snappy and LZ4 codecs remain enabled; TLS/SASL and extra codecs can
  # be added deliberately without making them accidental host dependencies.
  set(RDKAFKA_BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(RDKAFKA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(RDKAFKA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(WITH_SSL OFF CACHE BOOL "" FORCE)
  set(WITH_SASL OFF CACHE BOOL "" FORCE)
  set(WITH_CURL OFF CACHE BOOL "" FORCE)
  set(WITH_ZLIB OFF CACHE BOOL "" FORCE)
  set(WITH_ZSTD OFF CACHE BOOL "" FORCE)
  set(WITH_PLUGINS OFF CACHE BOOL "" FORCE)
  set(ENABLE_LZ4_EXT OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/librdkafka EXCLUDE_FROM_ALL SYSTEM)
  set(GOBLIN_HAS_KAFKA_BUILD ON)
endif()

set(GOBLIN_RDMA_SOURCES)
set(GOBLIN_HAS_RDMA_BUILD OFF)
if(GOBLIN_CORE_ENABLE_RDMA)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_path(IBVERBS_INCLUDE_DIR NAMES infiniband/verbs.h)
    find_path(RDMACM_INCLUDE_DIR NAMES rdma/rdma_cma.h)
    find_library(IBVERBS_LIBRARY NAMES ibverbs)
    find_library(RDMACM_LIBRARY NAMES rdmacm)
    if(IBVERBS_INCLUDE_DIR AND RDMACM_INCLUDE_DIR AND
       IBVERBS_LIBRARY AND RDMACM_LIBRARY)
      set(GOBLIN_HAS_RDMA_BUILD ON)
      list(APPEND GOBLIN_RDMA_SOURCES src/rdma_ring.cpp)
    else()
      message(STATUS
        "goblin-core: GOBLIN_CORE_ENABLE_RDMA=ON but libibverbs/librdmacm not found; RDMA disabled")
    endif()
  else()
    message(STATUS
      "goblin-core: GOBLIN_CORE_ENABLE_RDMA=ON is Linux-only; RDMA disabled on this host")
  endif()
endif()

set(GOBLIN_HAS_EXASOCK_BUILD OFF)
if(GOBLIN_CORE_ENABLE_EXASOCK)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
      "goblin-core: GOBLIN_CORE_ENABLE_EXASOCK=ON requires Linux with the ExaSock SDK installed")
  endif()
  find_path(EXASOCK_INCLUDE_DIR
    NAMES exasock/extensions.h exasock/socket.h
    PATHS /usr/local/include /usr/include)
  find_library(EXASOCK_EXT_LIBRARY
    NAMES exasock_ext
    PATHS /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu)
  if(NOT EXASOCK_INCLUDE_DIR OR NOT EXASOCK_EXT_LIBRARY)
    message(FATAL_ERROR
      "goblin-core: GOBLIN_CORE_ENABLE_EXASOCK=ON but the ExaSock SDK was not found.\n"
      "  Install Cisco/Exablaze ExaSock (headers exasock/*.h and libexasock_ext).\n"
      "  Do not vendor ExaSock into this tree; point CMAKE_PREFIX_PATH if needed.\n"
      "  Or configure without -DGOBLIN_CORE_ENABLE_EXASOCK=ON.")
  endif()
  set(GOBLIN_HAS_EXASOCK_BUILD ON)
endif()

add_library(goblin_core
  src/auth.cpp
  src/command.cpp
  src/pubsub.cpp
  src/luau_script.cpp
  src/numa_topology.cpp
  src/rdb.cpp
  src/resp_parser.cpp
  src/resp_writer.cpp
  src/script.cpp
  src/server.cpp
  src/simd.cpp
  src/string_encoding.cpp
  src/store.cpp
  src/transaction.cpp
  src/tcl_script.cpp
  src/upython_script.cpp
  src/wren_script.cpp
  src/quickjs_script.cpp
  src/replication.cpp
  src/replication_follower.cpp
  ${GOBLIN_RDMA_SOURCES})

if(GOBLIN_HAS_KAFKA_BUILD)
  target_sources(goblin_core PRIVATE src/kafka_ingest.cpp)
  target_compile_definitions(goblin_core PUBLIC GOBLIN_HAS_KAFKA=1)
  target_link_libraries(goblin_core PRIVATE rdkafka)
  message(STATUS
    "goblin-core: Kafka RESP2 write ingestion enabled (vendored librdkafka 2.15.0)")
endif()

target_compile_features(goblin_core PUBLIC cxx_std_23)
target_compile_definitions(goblin_core PRIVATE
  GOBLIN_CORE_VERSION="${PROJECT_VERSION}")
set_target_properties(goblin_core PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

target_include_directories(goblin_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    # XXH3 is used from the public linear-hash header (RT field/key indexes).
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash>
    # fast_float is used from public parse_int.hpp (RESP + encoding integers).
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/fast_float/include>
  PRIVATE
    ${LZ4_INCLUDE_DIR}
    ${SODIUM_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unordered_dense/include)

# src/script.cpp embeds the vendored Lua runtime. PRIVATE: the Lua headers are an
# implementation detail (script.hpp forward-declares lua_State), so consumers of
# goblin_core neither see nor link Lua directly.
target_link_libraries(goblin_core PRIVATE goblin_lua goblin_luau_vm goblin_wren goblin_jim goblin_upy goblin_qjs ${LZ4_LIBRARY} ${SODIUM_LIBRARY})

if(GOBLIN_HAS_TLS_BUILD)
  target_compile_definitions(goblin_core PUBLIC GOBLIN_HAS_TLS=1)
  target_link_libraries(goblin_core PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  message(STATUS "goblin-core: OpenSSL TLS listeners enabled (GOBLIN_HAS_TLS)")
endif()

if(GOBLIN_HAS_RDMA_BUILD)
  target_compile_definitions(goblin_core PUBLIC GOBLIN_HAS_RDMA=1)
  target_include_directories(goblin_core PRIVATE
    ${IBVERBS_INCLUDE_DIR}
    ${RDMACM_INCLUDE_DIR})
  target_link_libraries(goblin_core PRIVATE
    ${IBVERBS_LIBRARY}
    ${RDMACM_LIBRARY})
  message(STATUS "goblin-core: InfiniBand/RDMA polled-ring transport enabled (GOBLIN_HAS_RDMA)")
endif()

if(GOBLIN_HAS_EXASOCK_BUILD)
  target_compile_definitions(goblin_core PUBLIC GOBLIN_HAS_EXASOCK=1)
  target_include_directories(goblin_core PUBLIC ${EXASOCK_INCLUDE_DIR})
  target_link_libraries(goblin_core PUBLIC ${EXASOCK_EXT_LIBRARY})
  message(STATUS
    "goblin-core: ExaSock support enabled (GOBLIN_HAS_EXASOCK); run under the exasock wrapper for acceleration")
endif()

# The two Lua runtimes each ship a "lua.h" (both derive from Lua). Keep every
# vendored runtime's headers off the shared target include path and hand each
# engine source only its own runtime's headers, so none can pick up another's.
set_source_files_properties(src/script.cpp PROPERTIES
  INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua)
set_source_files_properties(src/luau_script.cpp PROPERTIES
  INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau/VM/include;${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau/Compiler/include")
set_source_files_properties(src/wren_script.cpp PROPERTIES
  INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wren/include)
set_source_files_properties(src/tcl_script.cpp PROPERTIES
  INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jimtcl)
set_source_files_properties(src/upython_script.cpp PROPERTIES
  INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/micropython;${CMAKE_CURRENT_SOURCE_DIR}/third_party/micropython/micropython_embed;${CMAKE_CURRENT_SOURCE_DIR}/third_party/micropython/micropython_embed/port")
set_source_files_properties(src/quickjs_script.cpp PROPERTIES
  INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs)

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

# COMMAND and COMMAND INFO enumerate the same keywords that feed gperf. Generate
# a C++ initializer at configure time so command discovery cannot drift from
# dispatch when a maintainer adds another keyword.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/command_hash.gperf)
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/src/command_hash.gperf
  GOBLIN_COMMAND_CATALOG_LINES REGEX "^[A-Z0-9_.]+,")
set(GOBLIN_COMMAND_CATALOG_CONTENT "")
foreach(GOBLIN_COMMAND_CATALOG_LINE IN LISTS GOBLIN_COMMAND_CATALOG_LINES)
  string(REGEX REPLACE "^([^,]+),.*$" "\\1"
    GOBLIN_COMMAND_CATALOG_NAME "${GOBLIN_COMMAND_CATALOG_LINE}")
  string(APPEND GOBLIN_COMMAND_CATALOG_CONTENT
    "std::string_view{\"${GOBLIN_COMMAND_CATALOG_NAME}\"},\n")
endforeach()
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/command_catalog.inc
  "${GOBLIN_COMMAND_CATALOG_CONTENT}")
target_include_directories(goblin_core PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/generated)

if(GPERF_EXECUTABLE)
  add_custom_target(goblin_core_regen_command_hash
    COMMAND ${GPERF_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/src/command_hash.gperf
            --output-file=${CMAKE_CURRENT_SOURCE_DIR}/src/command_hash.hpp
    COMMENT "Regenerating command-dispatch perfect hash from command_hash.gperf"
    VERBATIM)
endif()

# SBE (Simple Binary Encoding) wire (sbe/goblin_sbe.xml): the "GOBLINS!"-magic binary
# alternative to RESP. SBE C++ is header-only -- there is NO runtime library to
# find_package; ordinary builds just compile the checked-in generated codecs under
# sbe/generated/ (a maintainer step regenerates them with SbeTool, the same pattern
# as gperf -> command_hash.hpp). Enabled whenever those headers are present.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sbe/generated/goblin_sbe/ZAdd.h)
  add_library(goblin_sbe_headers INTERFACE)
  # SYSTEM so the machine-generated headers do not trip goblin_core's strict warnings;
  # BUILD_INTERFACE only -- the generated headers are server-internal, not shipped API.
  target_include_directories(goblin_sbe_headers SYSTEM INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sbe/generated>)
  target_link_libraries(goblin_core PRIVATE goblin_sbe_headers)
  target_compile_definitions(goblin_core PRIVATE GOBLIN_HAS_SBE=1)
  target_sources(goblin_core PRIVATE
    src/pubsub_listener.cpp
    src/sbe_dispatch.cpp)
  set_source_files_properties(src/sbe_dispatch.cpp PROPERTIES COMPILE_OPTIONS "-Wno-pedantic")
  # goblin_core is a static lib in the install export, so even this header-only
  # INTERFACE dep must be a member of the same export set (it installs nothing).
  install(TARGETS goblin_sbe_headers EXPORT goblinCoreTargets)
  message(STATUS "goblin-core: SBE wire enabled (header-only, sbe/generated)")

  find_program(JAVA_EXECUTABLE java)
  find_file(SBE_JAR sbe-all.jar PATHS $ENV{HOME}/tools /opt/sbe NO_DEFAULT_PATH)
  if(JAVA_EXECUTABLE AND SBE_JAR)
    add_custom_target(goblin_core_regen_sbe
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_SOURCE_DIR}/sbe/generated
      COMMAND ${JAVA_EXECUTABLE} -Dsbe.target.language=Cpp
              -Dsbe.output.dir=${CMAKE_CURRENT_SOURCE_DIR}/sbe/generated
              -jar ${SBE_JAR} ${CMAKE_CURRENT_SOURCE_DIR}/sbe/goblin_sbe.xml
      COMMENT "Regenerating sbe/generated/ from goblin_sbe.xml (SbeTool)"
      VERBATIM)
  endif()
else()
  message(STATUS "goblin-core: sbe/generated not present; building without the SBE wire")
endif()

if(MSVC)
  target_compile_options(goblin_core PRIVATE /W4)
else()
  target_compile_options(goblin_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(goblin_core_server src/main.cpp)
target_link_libraries(goblin_core_server PRIVATE goblin_core)
if(TARGET goblin_sbe_headers)
  target_compile_definitions(goblin_core_server PRIVATE GOBLIN_HAS_SBE=1)
endif()
set_target_properties(goblin_core_server PROPERTIES
  OUTPUT_NAME goblin-core
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

add_executable(goblin_core_auth src/auth_cli.cpp)
target_link_libraries(goblin_core_auth PRIVATE goblin_core)
set_target_properties(goblin_core_auth PROPERTIES
  OUTPUT_NAME goblin-core-auth
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

# Proof-of-concept ring-buffer client. Only needs the header-only ring client, but
# links goblin_core for its include path.
add_executable(goblin_core_ring_cli src/ring_cli.cpp)
target_link_libraries(goblin_core_ring_cli PRIVATE goblin_core)
set_target_properties(goblin_core_ring_cli PROPERTIES
  OUTPUT_NAME redis-cli-ring
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

if(GOBLIN_HAS_RDMA_BUILD)
  add_executable(goblin_core_rdma_cli src/rdma_cli.cpp)
  target_link_libraries(goblin_core_rdma_cli PRIVATE goblin_core)
  set_target_properties(goblin_core_rdma_cli PROPERTIES
    OUTPUT_NAME redis-cli-rdma
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

  if(GOBLIN_CORE_BUILD_TESTS AND TARGET goblin_sbe_headers)
    add_executable(goblin_core_rdma_roundtrip_test
      tests/rdma_roundtrip_test.cpp)
    target_link_libraries(goblin_core_rdma_roundtrip_test PRIVATE
      goblin_core goblin_sbe_headers)
    set_target_properties(goblin_core_rdma_roundtrip_test PROPERTIES
      CXX_STANDARD 23
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF)
  endif()

  if(GOBLIN_CORE_BUILD_BENCHMARKS AND TARGET goblin_sbe_headers)
    add_executable(goblin_core_rdma_latency_benchmark
      benchmarks/rdma_latency_benchmark.cpp)
    target_link_libraries(goblin_core_rdma_latency_benchmark PRIVATE
      goblin_core goblin_sbe_headers)
    set_target_properties(goblin_core_rdma_latency_benchmark PROPERTIES
      CXX_STANDARD 23
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF)
    target_compile_options(goblin_core_rdma_latency_benchmark PRIVATE
      -Wall -Wextra -Wpedantic)

    add_executable(goblin_core_pubsub_rdma_relay_benchmark
      benchmarks/pubsub_rdma_relay_benchmark.cpp)
    target_link_libraries(goblin_core_pubsub_rdma_relay_benchmark PRIVATE
      goblin_core goblin_sbe_headers)
    set_target_properties(goblin_core_pubsub_rdma_relay_benchmark PROPERTIES
      CXX_STANDARD 23
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF)
    target_compile_options(goblin_core_pubsub_rdma_relay_benchmark PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
endif()

if(GOBLIN_HAS_EXASOCK_BUILD)
  add_executable(goblin_core_exasock_cli src/exasock_cli.cpp)
  target_link_libraries(goblin_core_exasock_cli PRIVATE goblin_core)
  set_target_properties(goblin_core_exasock_cli PROPERTIES
    OUTPUT_NAME redis-cli-exasock
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

  if(GOBLIN_CORE_BUILD_TESTS AND TARGET goblin_sbe_headers)
    add_executable(goblin_core_exasock_roundtrip_test
      tests/exasock_roundtrip_test.cpp)
    target_link_libraries(goblin_core_exasock_roundtrip_test PRIVATE
      goblin_core goblin_sbe_headers)
    set_target_properties(goblin_core_exasock_roundtrip_test PROPERTIES
      CXX_STANDARD 23
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF)
  endif()
endif()

if(GOBLIN_CORE_BUILD_BENCHMARKS)
  add_executable(goblin_core_microbench benchmarks/microbench.cpp)
  target_link_libraries(goblin_core_microbench PRIVATE goblin_core)
  set_target_properties(goblin_core_microbench PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

  # Local shared-memory ring latency (same ops as RDMA latency probe).
  if(TARGET goblin_sbe_headers)
    add_executable(goblin_core_ring_latency_benchmark
      benchmarks/ring_latency_benchmark.cpp)
    target_link_libraries(goblin_core_ring_latency_benchmark PRIVATE
      goblin_core goblin_sbe_headers)
    set_target_properties(goblin_core_ring_latency_benchmark PROPERTIES
      CXX_STANDARD 23
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF)
    target_compile_options(goblin_core_ring_latency_benchmark PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()

  if(UNIX)
    add_executable(goblin_core_hset_benchmark benchmarks/hset_benchmark.cpp)
    set_target_properties(goblin_core_hset_benchmark PROPERTIES
      CXX_STANDARD 23
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF)
    target_compile_options(goblin_core_hset_benchmark PRIVATE
      -Wall -Wextra -Wpedantic)

    add_executable(goblin_core_list_work_queue_benchmark
      benchmarks/list_work_queue_benchmark.cpp)
    target_link_libraries(goblin_core_list_work_queue_benchmark PRIVATE
      goblin_core)
    set_target_properties(goblin_core_list_work_queue_benchmark PROPERTIES
      CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(goblin_core_list_work_queue_benchmark PRIVATE
      -Wall -Wextra -Wpedantic)

    if(TARGET goblin_sbe_headers)
      add_executable(goblin_core_collection_benchmark_worker
        benchmarks/collection_benchmark_worker.cpp)
      target_link_libraries(goblin_core_collection_benchmark_worker PRIVATE
        goblin_sbe_headers)
      target_include_directories(goblin_core_collection_benchmark_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
      set_target_properties(goblin_core_collection_benchmark_worker PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
      target_compile_options(goblin_core_collection_benchmark_worker PRIVATE
        -Wall -Wextra -Wpedantic)

      add_executable(goblin_core_pubsub_benchmark
        benchmarks/pubsub_benchmark.cpp)
      target_link_libraries(goblin_core_pubsub_benchmark PRIVATE
        goblin_sbe_headers)
      target_include_directories(goblin_core_pubsub_benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
      set_target_properties(goblin_core_pubsub_benchmark PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
      target_compile_options(goblin_core_pubsub_benchmark PRIVATE
        -Wall -Wextra -Wpedantic)

      add_executable(goblin_core_pubsub_rebroadcast_benchmark
        benchmarks/pubsub_rebroadcast_benchmark.cpp)
      target_link_libraries(goblin_core_pubsub_rebroadcast_benchmark PRIVATE
        goblin_sbe_headers)
      target_include_directories(goblin_core_pubsub_rebroadcast_benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
      set_target_properties(goblin_core_pubsub_rebroadcast_benchmark PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
      target_compile_options(goblin_core_pubsub_rebroadcast_benchmark PRIVATE
        -Wall -Wextra -Wpedantic)

      add_executable(goblin_core_list_sbe_benchmark
        benchmarks/list_sbe_benchmark.cpp)
      target_link_libraries(goblin_core_list_sbe_benchmark PRIVATE
        goblin_sbe_headers)
      target_include_directories(goblin_core_list_sbe_benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
      set_target_properties(goblin_core_list_sbe_benchmark PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
      target_compile_options(goblin_core_list_sbe_benchmark PRIVATE
        -Wall -Wextra -Wpedantic)
    endif()
  endif()
endif()

if(GOBLIN_CORE_BUILD_TESTS)
  include(CTest)

  add_executable(goblin_core_numa_topology_test
    tests/numa_topology_test.cpp)
  target_link_libraries(goblin_core_numa_topology_test PRIVATE goblin_core)
  set_target_properties(goblin_core_numa_topology_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME goblin_core_numa_topology COMMAND goblin_core_numa_topology_test)

  add_executable(goblin_core_tests tests/goblin_core_tests.cpp)
  target_link_libraries(goblin_core_tests PRIVATE goblin_core)
  set_target_properties(goblin_core_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

  add_test(NAME goblin_core_tests COMMAND goblin_core_tests)

  add_executable(goblin_core_replication_test tests/replication_test.cpp)
  target_link_libraries(goblin_core_replication_test PRIVATE goblin_core)
  set_target_properties(goblin_core_replication_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME goblin_core_replication COMMAND goblin_core_replication_test)

  add_executable(goblin_core_auth_test tests/auth_test.cpp)
  target_link_libraries(goblin_core_auth_test PRIVATE goblin_core)
  set_target_properties(goblin_core_auth_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME goblin_core_auth COMMAND goblin_core_auth_test)

  add_executable(goblin_core_auth_socket_test tests/auth_socket_test.cpp)
  target_link_libraries(goblin_core_auth_socket_test PRIVATE goblin_core)
  set_target_properties(goblin_core_auth_socket_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME goblin_core_auth_socket
    COMMAND goblin_core_auth_socket_test $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_auth_socket PROPERTIES TIMEOUT 60)

  add_executable(goblin_core_transaction_socket_test
    tests/transaction_socket_test.cpp)
  target_link_libraries(goblin_core_transaction_socket_test PRIVATE goblin_core)
  set_target_properties(goblin_core_transaction_socket_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME goblin_core_transaction_socket
    COMMAND goblin_core_transaction_socket_test
            $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_transaction_socket PROPERTIES TIMEOUT 60)

  add_executable(goblin_core_list_blocking_socket_test
    tests/list_blocking_socket_test.cpp)
  target_link_libraries(goblin_core_list_blocking_socket_test PRIVATE goblin_core)
  set_target_properties(goblin_core_list_blocking_socket_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME goblin_core_list_blocking_socket
    COMMAND goblin_core_list_blocking_socket_test
            $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_list_blocking_socket PROPERTIES TIMEOUT 60)

  add_executable(goblin_core_array_test tests/array_test.cpp)
  target_link_libraries(goblin_core_array_test PRIVATE goblin_core)
  set_target_properties(goblin_core_array_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME goblin_core_array COMMAND goblin_core_array_test)

  add_executable(goblin_core_pubsub_test tests/pubsub_test.cpp)
  target_link_libraries(goblin_core_pubsub_test PRIVATE goblin_core)
  target_include_directories(goblin_core_pubsub_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unordered_dense/include)
  if(TARGET goblin_sbe_headers)
    target_link_libraries(goblin_core_pubsub_test PRIVATE goblin_sbe_headers)
    target_compile_definitions(goblin_core_pubsub_test PRIVATE GOBLIN_HAS_SBE=1)
  endif()
  set_target_properties(goblin_core_pubsub_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME goblin_core_pubsub COMMAND goblin_core_pubsub_test)

  if(GOBLIN_HAS_KAFKA_BUILD)
    add_executable(goblin_core_kafka_ingest_test tests/kafka_ingest_test.cpp)
    target_link_libraries(goblin_core_kafka_ingest_test PRIVATE goblin_core)
    set_target_properties(goblin_core_kafka_ingest_test PROPERTIES
      CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME goblin_core_kafka_ingest COMMAND goblin_core_kafka_ingest_test)

    # Manual end-to-end test: it needs a broker and an empty pre-created
    # one-partition topic.
    # Usage: goblin_core_kafka_server_test <goblin-core> <brokers> <topic>
    add_executable(goblin_core_kafka_server_test tests/kafka_server_test.cpp)
    target_link_libraries(goblin_core_kafka_server_test PRIVATE goblin_core rdkafka)
    set_target_properties(goblin_core_kafka_server_test PROPERTIES
      CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  endif()

  add_executable(goblin_core_resp3_socket_test tests/resp3_socket_test.cpp)
  target_link_libraries(goblin_core_resp3_socket_test PRIVATE goblin_core)
  set_target_properties(goblin_core_resp3_socket_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME goblin_core_resp3_socket
    COMMAND goblin_core_resp3_socket_test $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_resp3_socket PROPERTIES TIMEOUT 60)

  add_executable(goblin_core_socket_listeners_test
    tests/socket_listeners_test.cpp)
  target_link_libraries(goblin_core_socket_listeners_test PRIVATE goblin_core)
  set_target_properties(goblin_core_socket_listeners_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME goblin_core_socket_listeners
    COMMAND goblin_core_socket_listeners_test
            $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_socket_listeners PROPERTIES TIMEOUT 60)

  if(GOBLIN_HAS_TLS_BUILD)
    add_executable(goblin_core_tls_socket_test tests/tls_socket_test.cpp)
    target_link_libraries(goblin_core_tls_socket_test PRIVATE
      goblin_core OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(goblin_core_tls_socket_test PROPERTIES
      CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME goblin_core_tls_socket
      COMMAND goblin_core_tls_socket_test
              $<TARGET_FILE:goblin_core_server>)
    set_tests_properties(goblin_core_tls_socket PROPERTIES TIMEOUT 60)

    add_executable(goblin_core_replication_tls_socket_test
      tests/replication_tls_socket_test.cpp)
    target_link_libraries(goblin_core_replication_tls_socket_test PRIVATE
      goblin_core OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(goblin_core_replication_tls_socket_test PROPERTIES
      CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME goblin_core_replication_tls_socket
      COMMAND goblin_core_replication_tls_socket_test
              $<TARGET_FILE:goblin_core_server>)
    set_tests_properties(goblin_core_replication_tls_socket PROPERTIES TIMEOUT 60)
  endif()

  add_executable(goblin_core_pubsub_socket_test tests/pubsub_socket_test.cpp)
  target_link_libraries(goblin_core_pubsub_socket_test PRIVATE goblin_core)
  set_target_properties(goblin_core_pubsub_socket_test PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  add_test(NAME goblin_core_pubsub_socket
    COMMAND goblin_core_pubsub_socket_test $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_pubsub_socket PROPERTIES TIMEOUT 60)

  # SBE dispatch test (framed request -> Store -> generic reply).
  if(TARGET goblin_sbe_headers)
    add_executable(goblin_core_replication_socket_test
      tests/replication_socket_test.cpp)
    target_link_libraries(goblin_core_replication_socket_test PRIVATE
      goblin_core goblin_sbe_headers)
    set_target_properties(goblin_core_replication_socket_test PROPERTIES
      CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME goblin_core_replication_socket
      COMMAND goblin_core_replication_socket_test
              $<TARGET_FILE:goblin_core_server>)
    set_tests_properties(goblin_core_replication_socket PROPERTIES TIMEOUT 60)

    add_executable(goblin_core_sbe_test tests/sbe_dispatch_test.cpp)
    target_link_libraries(goblin_core_sbe_test PRIVATE goblin_core goblin_sbe_headers)
    set_target_properties(goblin_core_sbe_test PROPERTIES
      CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME goblin_core_sbe COMMAND goblin_core_sbe_test)

    # End-to-end SBE over a UNIX socket, across two processes (magic detection on the
    # socket accept path, not just the ring).
    add_executable(goblin_core_sbe_socket_test tests/sbe_socket_test.cpp)
    target_link_libraries(goblin_core_sbe_socket_test PRIVATE goblin_core goblin_sbe_headers)
    set_target_properties(goblin_core_sbe_socket_test PROPERTIES
      CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME goblin_core_sbe_socket
      COMMAND goblin_core_sbe_socket_test $<TARGET_FILE:goblin_core_server>)
    set_tests_properties(goblin_core_sbe_socket PROPERTIES TIMEOUT 60)

    # The full C++ client (SbeRingClient) across every command family, over a ring.
    add_executable(goblin_core_sbe_client_test tests/sbe_client_test.cpp)
    target_link_libraries(goblin_core_sbe_client_test PRIVATE goblin_core goblin_sbe_headers)
    set_target_properties(goblin_core_sbe_client_test PROPERTIES
      CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME goblin_core_sbe_client
      COMMAND goblin_core_sbe_client_test $<TARGET_FILE:goblin_core_server>)
    set_tests_properties(goblin_core_sbe_client PROPERTIES TIMEOUT 60)

    add_executable(goblin_core_pubsub_listener_ring_test
      tests/pubsub_listener_ring_test.cpp)
    target_link_libraries(goblin_core_pubsub_listener_ring_test PRIVATE
      goblin_core goblin_sbe_headers)
    set_target_properties(goblin_core_pubsub_listener_ring_test PROPERTIES
      CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME goblin_core_pubsub_listener_ring
      COMMAND goblin_core_pubsub_listener_ring_test
              $<TARGET_FILE:goblin_core_server>)
    set_tests_properties(goblin_core_pubsub_listener_ring PROPERTIES TIMEOUT 60)

    add_executable(goblin_core_pubsub_listener_socket_test
      tests/pubsub_listener_socket_test.cpp)
    target_link_libraries(goblin_core_pubsub_listener_socket_test PRIVATE
      goblin_core goblin_sbe_headers)
    set_target_properties(goblin_core_pubsub_listener_socket_test PROPERTIES
      CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME goblin_core_pubsub_listener_socket
      COMMAND goblin_core_pubsub_listener_socket_test
              $<TARGET_FILE:goblin_core_server>)
    set_tests_properties(goblin_core_pubsub_listener_socket PROPERTIES TIMEOUT 60)

    # A client that SIGKILLs mid-operation leaves the ring dirty; a fresh client must
    # reconnect and recover it with no server restart (the ring reconnect handshake).
    add_executable(goblin_core_sbe_reconnect_test tests/sbe_reconnect_test.cpp)
    target_link_libraries(goblin_core_sbe_reconnect_test PRIVATE goblin_core goblin_sbe_headers)
    set_target_properties(goblin_core_sbe_reconnect_test PROPERTIES
      CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME goblin_core_sbe_reconnect
      COMMAND goblin_core_sbe_reconnect_test $<TARGET_FILE:goblin_core_server>)
    set_tests_properties(goblin_core_sbe_reconnect PROPERTIES TIMEOUT 60)
  endif()

  # End-to-end ring buffer: fork+exec the server with a ring, drive it through the
  # header-only RingClient across two processes.
  add_executable(goblin_core_ring_test tests/ring_roundtrip_test.cpp)
  target_link_libraries(goblin_core_ring_test PRIVATE goblin_core)
  set_target_properties(goblin_core_ring_test PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  add_test(
    NAME goblin_core_ring_roundtrip
    COMMAND goblin_core_ring_test $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_ring_roundtrip PROPERTIES TIMEOUT 60)

  # A RESP RingClient that SIGKILLs mid-operation leaves the ring dirty; a fresh client
  # must reconnect and recover it with no server restart (RESP side of the epoch handshake).
  add_executable(goblin_core_ring_reconnect_test tests/ring_reconnect_test.cpp)
  target_link_libraries(goblin_core_ring_reconnect_test PRIVATE goblin_core)
  set_target_properties(goblin_core_ring_reconnect_test PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  add_test(
    NAME goblin_core_ring_reconnect
    COMMAND goblin_core_ring_reconnect_test $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_ring_reconnect PROPERTIES TIMEOUT 60)

  # Unit test for the ring producer's backpressure guarantees: block while full,
  # unblock the instant a record is freed, and throw when a record can never fit.
  # Self-contained -- drives both ends of a ring in-process, no server needed.
  add_executable(goblin_core_ring_backpressure_test tests/ring_backpressure_test.cpp)
  target_link_libraries(goblin_core_ring_backpressure_test PRIVATE goblin_core)
  set_target_properties(goblin_core_ring_backpressure_test PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  add_test(
    NAME goblin_core_ring_backpressure
    COMMAND goblin_core_ring_backpressure_test)
  set_tests_properties(goblin_core_ring_backpressure PROPERTIES TIMEOUT 60)

  # Mirror-mapped rings: a record straddling the ring end round-trips (double-map),
  # and the forced single-map + WRAP-filler fallback round-trips the same traffic.
  add_executable(goblin_core_ring_mirror_test tests/ring_mirror_test.cpp)
  target_link_libraries(goblin_core_ring_mirror_test PRIVATE goblin_core)
  set_target_properties(goblin_core_ring_mirror_test PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  add_test(
    NAME goblin_core_ring_mirror
    COMMAND goblin_core_ring_mirror_test)
  set_tests_properties(goblin_core_ring_mirror PROPERTIES TIMEOUT 60)

  # Hugetlb-backed rings (Linux): size rounds up to the huge page, --ring PATH becomes
  # a symlink into hugetlbfs, and records round-trip. Skips cleanly without huge pages.
  add_executable(goblin_core_ring_hugetlb_test tests/ring_hugetlb_test.cpp)
  target_link_libraries(goblin_core_ring_hugetlb_test PRIVATE goblin_core)
  set_target_properties(goblin_core_ring_hugetlb_test PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  add_test(
    NAME goblin_core_ring_hugetlb
    COMMAND goblin_core_ring_hugetlb_test)
  set_tests_properties(goblin_core_ring_hugetlb PROPERTIES TIMEOUT 60)

  # Huge-page-backed arena blocks: a block that freezes at a huge-multiple chunk is
  # re-homed onto a huge page. Skips cleanly without reserved huge pages.
  add_executable(goblin_core_arena_hugetlb_test tests/arena_hugetlb_test.cpp)
  target_link_libraries(goblin_core_arena_hugetlb_test PRIVATE goblin_core)
  set_target_properties(goblin_core_arena_hugetlb_test PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  add_test(
    NAME goblin_core_arena_hugetlb
    COMMAND goblin_core_arena_hugetlb_test)
  set_tests_properties(goblin_core_arena_hugetlb PROPERTIES TIMEOUT 60)

  add_test(
    NAME goblin_core_output_backpressure
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/output_backpressure.py
      --goblin-bin $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_output_backpressure
    PROPERTIES TIMEOUT 60)

  # End-to-end scripting over a real socket (EVAL / EVALSHA / SCRIPT), exercising
  # the server's dispatch path in addition to the direct-engine unit tests.
  add_test(
    NAME goblin_core_scripting
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripting.py
      --goblin-bin $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_scripting
    PROPERTIES TIMEOUT 60)

  # End-to-end Luau scripting (LUAU.EVAL / LUAU.EVALSHA / LUAU.SCRIPT), including
  # that it is a distinct interpreter from EVAL sharing the same key space.
  add_test(
    NAME goblin_core_luau_scripting
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/luau_scripting.py
      --goblin-bin $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_luau_scripting
    PROPERTIES TIMEOUT 60)

  # End-to-end Wren scripting (WREN.EVAL / WREN.EVALSHA / WREN.SCRIPT), including
  # that it is a distinct interpreter from the Lua engines sharing the key space.
  add_test(
    NAME goblin_core_wren_scripting
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/wren_scripting.py
      --goblin-bin $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_wren_scripting
    PROPERTIES TIMEOUT 60)

  # End-to-end Tcl scripting (TCL.EVAL / TCL.EVALSHA / TCL.SCRIPT), including that
  # it is a distinct interpreter from the Lua and Wren engines.
  add_test(
    NAME goblin_core_tcl_scripting
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/tcl_scripting.py
      --goblin-bin $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_tcl_scripting
    PROPERTIES TIMEOUT 60)

  # End-to-end MicroPython scripting (UPYTHON.EVAL / UPYTHON.EVALSHA /
  # UPYTHON.SCRIPT), including that it is a distinct interpreter from the others.
  add_test(
    NAME goblin_core_upython_scripting
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/upython_scripting.py
      --goblin-bin $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_upython_scripting
    PROPERTIES TIMEOUT 60)

  # End-to-end JavaScript scripting (QUICKJS.EVAL / QUICKJS.EVALSHA /
  # QUICKJS.SCRIPT), including that it is a distinct interpreter from the others.
  add_test(
    NAME goblin_core_quickjs_scripting
    COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/quickjs_scripting.py
      --goblin-bin $<TARGET_FILE:goblin_core_server>)
  set_tests_properties(goblin_core_quickjs_scripting
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

      add_test(
        NAME goblin_core_pma_redis_differential_sequential
        COMMAND
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/redis_differential.py
          --goblin-bin $<TARGET_FILE:goblin_core_server>
          --redis-server ${REDIS_SERVER_EXECUTABLE}
          --goblin-list-prefix GOBLIN.PMA.)
      set_tests_properties(goblin_core_pma_redis_differential_sequential
        PROPERTIES TIMEOUT 30)

      add_test(
        NAME goblin_core_pma_redis_differential_pipelined
        COMMAND
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/redis_differential.py
          --goblin-bin $<TARGET_FILE:goblin_core_server>
          --redis-server ${REDIS_SERVER_EXECUTABLE}
          --goblin-list-prefix GOBLIN.PMA.
          --pipeline-depth 32)
      set_tests_properties(goblin_core_pma_redis_differential_pipelined
        PROPERTIES TIMEOUT 30)
    else()
      message(WARNING
        "GOBLIN_CORE_REDIS_DIFFERENTIAL_TESTS is ON, but redis-server was not found")
    endif()
  endif()
endif()

include(CMakePackageConfigHelpers)

set(GOBLIN_CORE_INSTALL_TARGETS
  goblin_core goblin_core_server goblin_core_auth goblin_lua goblin_luau_vm goblin_wren
  goblin_jim goblin_upy goblin_qjs)
if(GOBLIN_HAS_KAFKA_BUILD)
  list(APPEND GOBLIN_CORE_INSTALL_TARGETS rdkafka)
endif()

install(
  TARGETS ${GOBLIN_CORE_INSTALL_TARGETS}
  EXPORT goblinCoreTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(GOBLIN_HAS_RDMA_BUILD)
  install(
    TARGETS goblin_core_rdma_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(GOBLIN_HAS_EXASOCK_BUILD)
  install(
    TARGETS goblin_core_exasock_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(
  DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
  FILES
    README.md
    BENCHMARKS.md
    MICROBENCHMARKS.md
    PERFORMANCE_BRIEF.md
    PUBSUB-BENCHMARK.md
    RELEASE.md
    LICENSE
    NOTICE
  DESTINATION ${CMAKE_INSTALL_DOCDIR})

if(GOBLIN_HAS_KAFKA_BUILD)
  install(
    FILES
      third_party/librdkafka/LICENSE
      third_party/librdkafka/LICENSES.txt
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/third_party/librdkafka)
endif()

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
