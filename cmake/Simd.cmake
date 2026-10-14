# Compile each ISA in isolation. Optional instructions must never leak into
# goblin_core, its public headers, or consumers through PUBLIC compiler flags.
include(CheckCXXCompilerFlag)

function(goblin_simd_backend target source_root name)
  set(prefix goblin_simd_${target})
  add_library(${prefix}_${name} OBJECT ${source_root}/src/simd_backend.cpp)
  target_include_directories(${prefix}_${name} PRIVATE
    ${source_root}/include ${source_root}/third_party/xxhash)
  target_compile_definitions(${prefix}_${name} PRIVATE
    GOBLIN_SIMD_BACKEND=${name} ${ARGN})
  set_target_properties(${prefix}_${name} PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
  target_sources(${target} PRIVATE $<TARGET_OBJECTS:${prefix}_${name}>)
  if(NOT name STREQUAL "scalar")
    string(TOUPPER ${name} upper_name)
    target_compile_definitions(${target} PRIVATE GOBLIN_BUILD_${upper_name})
  endif()
endfunction()

function(goblin_add_simd_backends target source_root)
  set(prefix goblin_simd_${target})
  goblin_simd_backend(${target} ${source_root} scalar)
  # Keep the fallback free of compiler-generated vector loops as well.
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${prefix}_scalar PRIVATE
      -fno-vectorize -fno-slp-vectorize)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${prefix}_scalar PRIVATE -fno-tree-vectorize)
  endif()

  set(goblin_crc_flags "")
  set(goblin_crc_definition "")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    check_cxx_compiler_flag("-msse2" GOBLIN_HAS_SSE2)
    if(GOBLIN_HAS_SSE2)
      goblin_simd_backend(${target} ${source_root} sse2 GOBLIN_SIMD_SSE2)
      target_compile_options(${prefix}_sse2 PRIVATE -msse2)
    endif()
    check_cxx_compiler_flag("-mavx2" GOBLIN_HAS_AVX2)
    if(GOBLIN_HAS_AVX2)
      goblin_simd_backend(${target} ${source_root} avx2 GOBLIN_SIMD_SSE2 GOBLIN_SIMD_AVX2)
      target_compile_options(${prefix}_avx2 PRIVATE -mavx2)
    endif()
    check_cxx_compiler_flag("-mavx2 -mavx512f -mavx512bw -mavx512vl"
      GOBLIN_HAS_AVX512)
    if(GOBLIN_HAS_AVX512)
      goblin_simd_backend(${target} ${source_root} avx512
        GOBLIN_SIMD_SSE2 GOBLIN_SIMD_AVX2 GOBLIN_SIMD_AVX512)
      target_compile_options(${prefix}_avx512 PRIVATE
        -mavx2 -mavx512f -mavx512bw -mavx512vl)
    endif()
    check_cxx_compiler_flag("-msse4.2" GOBLIN_HAS_SSE42)
    if(GOBLIN_HAS_SSE42)
      set(goblin_crc_flags -msse4.2)
      set(goblin_crc_definition GOBLIN_CRC_X86)
    endif()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    goblin_simd_backend(${target} ${source_root} neon GOBLIN_SIMD_NEON)
    check_cxx_compiler_flag("-march=armv8-a+crc" GOBLIN_HAS_ARM_CRC)
    if(GOBLIN_HAS_ARM_CRC)
      set(goblin_crc_flags -march=armv8-a+crc)
      set(goblin_crc_definition GOBLIN_CRC_ARM)
    endif()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^loongarch")
    check_cxx_compiler_flag("-mlsx" GOBLIN_HAS_LSX)
    if(GOBLIN_HAS_LSX)
      goblin_simd_backend(${target} ${source_root} lsx GOBLIN_SIMD_LSX)
      target_compile_options(${prefix}_lsx PRIVATE -mlsx)
    endif()
    check_cxx_compiler_flag("-mlasx" GOBLIN_HAS_LASX)
    if(GOBLIN_HAS_LASX)
      goblin_simd_backend(${target} ${source_root} lasx GOBLIN_SIMD_LSX GOBLIN_SIMD_LASX)
      target_compile_options(${prefix}_lasx PRIVATE -mlasx)
    endif()
    set(goblin_crc_definition GOBLIN_CRC_LOONGARCH)
  endif()

  if(goblin_crc_definition)
    add_library(${prefix}_crc OBJECT ${source_root}/src/simd_crc.cpp)
    set_target_properties(${prefix}_crc PROPERTIES
      CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
      POSITION_INDEPENDENT_CODE ON)
    target_compile_options(${prefix}_crc PRIVATE ${goblin_crc_flags})
    target_compile_definitions(${prefix}_crc PRIVATE ${goblin_crc_definition})
    target_compile_definitions(${target} PRIVATE GOBLIN_BUILD_CRC)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:${prefix}_crc>)
  endif()

  if(GOBLIN_CORE_ARCH)
    message(WARNING "GOBLIN_CORE_ARCH is deprecated and ignored. SIMD backends "
      "are now selected at runtime; use the server's --simd option to override.")
  endif()
endfunction()
