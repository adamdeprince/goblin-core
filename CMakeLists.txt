cmake_minimum_required(VERSION 3.25)

project(goblin-core
  VERSION 0.4.2
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

add_library(goblin_core
  src/command.cpp
  src/luau_script.cpp
  src/rdb.cpp
  src/resp_parser.cpp
  src/resp_writer.cpp
  src/script.cpp
  src/server.cpp
  src/simd.cpp
  src/store.cpp
  src/tcl_script.cpp
  src/upython_script.cpp
  src/wren_script.cpp
  src/quickjs_script.cpp)

target_compile_features(goblin_core PUBLIC cxx_std_23)
set_target_properties(goblin_core PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

target_include_directories(goblin_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# src/script.cpp embeds the vendored Lua runtime. PRIVATE: the Lua headers are an
# implementation detail (script.hpp forward-declares lua_State), so consumers of
# goblin_core neither see nor link Lua directly.
target_link_libraries(goblin_core PRIVATE goblin_lua goblin_luau_vm goblin_wren goblin_jim goblin_upy goblin_qjs)

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
if(GPERF_EXECUTABLE)
  add_custom_target(goblin_core_regen_command_hash
    COMMAND ${GPERF_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/src/command_hash.gperf
            --output-file=${CMAKE_CURRENT_SOURCE_DIR}/src/command_hash.hpp
    COMMENT "Regenerating command-dispatch perfect hash from command_hash.gperf"
    VERBATIM)
endif()

# SBE (Simple Binary Encoding) wire (sbe/goblin_sbe.xml): the "GOBL"-magic binary
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
  target_sources(goblin_core PRIVATE src/sbe_dispatch.cpp)
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
set_target_properties(goblin_core_server PROPERTIES
  OUTPUT_NAME goblin-core
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

  # SBE dispatch test (framed request -> Store -> generic reply).
  if(TARGET goblin_sbe_headers)
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
    else()
      message(WARNING
        "GOBLIN_CORE_REDIS_DIFFERENTIAL_TESTS is ON, but redis-server was not found")
    endif()
  endif()
endif()

include(CMakePackageConfigHelpers)

install(
  TARGETS goblin_core goblin_core_server goblin_lua goblin_luau_vm goblin_wren goblin_jim goblin_upy goblin_qjs
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
