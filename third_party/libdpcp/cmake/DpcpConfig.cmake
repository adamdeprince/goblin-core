# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

include(DpcpUtilities)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
    set(DPCP_STATIC ON CACHE INTERNAL "" FORCE)
elseif(NOT DEFINED DPCP_STATIC)
    option(DPCP_STATIC "Enables linking with local version of DPCP." OFF)
endif()

if (DPCP_STATIC)
    set(DPCP_LIBRARY_ATTRIBUTES STATIC)
    set(DPCP_LIBRARY_IMPORTS_SCOPE GLOBAL)
else()
    set(DPCP_LIBRARY_ATTRIBUTES SHARED)
    set(DPCP_LIBRARY_IMPORTS_SCOPE) 
endif()

add_library(dpcp_config INTERFACE)

if (MSVC)
    set_target_properties(dpcp_config PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

    set(DPCP_C_CXX_FLAGS
        /W4
        /WX
        /fp:fast
    )
    target_compile_options(dpcp_config INTERFACE $<$<COMPILE_LANGUAGE:C,CXX>:${DPCP_C_CXX_FLAGS}>)

    target_compile_definitions(dpcp_config INTERFACE
        WIN32_LEAN_AND_MEAN=1
        NOMINMAX
        _WINSOCK_DEPRECATED_NO_WARNINGS
        _CRT_SECURE_NO_WARNINGS
        UNICODE
        _UNICODE
    )

    set(DPCP_MSVC_DEBUG_MODE_WARNINGS
        _SCL_SECURE_NO_WARNINGS
        _SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING
        _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS
    )
    target_compile_definitions(dpcp_config INTERFACE $<$<CONFIG:Debug>:${DPCP_MSVC_DEBUG_MODE_WARNINGS}>)

    # Define the Windows version and the minimum required Windows version as Windows 10 20H2 (Oct 2020),
    # to allow using features like: High-Performance Networking, NUMA Improvements, Large Pages, Modern Synchronization.
    target_compile_definitions(dpcp_config INTERFACE
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
        NTDDI_VERSION=0x0A00000A
    )

else()
    target_compile_options(dpcp_config INTERFACE
        -Wall
        -Wextra
        -Werror
        -Wundef
        -Wsequence-point
        -Wformat=2
        -Wformat-security
        -ffunction-sections
        -fdata-sections
        -pipe
        -Winit-self
        -Wmissing-include-dirs
        -D_GNU_SOURCE
    )

    set(DPCP_CXX_ONLY_FLAGS 
        -Wno-overloaded-virtual
        -Woverloaded-virtual
        -Wnon-virtual-dtor
    )
    target_compile_options(dpcp_config INTERFACE $<$<COMPILE_LANGUAGE:CXX>:${DPCP_CXX_ONLY_FLAGS}>)

    if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "aarch64")
        target_compile_options(dpcp_config INTERFACE -fstack-protector-strong -fstack-clash-protection)
        target_link_options(dpcp_config INTERFACE "-z,relro,-z,now,-z,noexecstack")
    endif()

    set(DPCP_CXX_FLAGS
        -Wshadow
        -Wno-overloaded-virtual
    )
    target_compile_definitions(dpcp_config INTERFACE _FORTIFY_SOURCE=2)

    option(DPCP_IPO "Enable interprocedural optimization (IPO/LTO)." FALSE)
    if (DPCP_IPO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT is_ipo_supported OUTPUT output_log)
        if (is_ipo_supported)
            set_property(TARGET dpcp_config PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        else()
            message(WARNING "IPO is not supported: ${output_log}")
        endif()
    endif()

    set(DPCP_PGO_PROFILE_PATH "" CACHE PATH "PGO Profile directory. When specified, enables PGO Profile optimization.")
    set(DPCP_GENERATE_PGO OFF CACHE BOOL "When PGO is enabled, if ON - generates a new profile, if OFF - uses the existing one. " FORCE)

    if (DPCP_PGO_PROFILE_PATH)
        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(dpcp_config INTERFACE
                -fprofile-$<IF:$<BOOL:${DPCP_GENERATE_PGO}>,generate,use>
                -fprofile-correction
                -Wno-error=missing-profile
                --coverage
                -fprofile-dir=${DPCP_PGO_PROFILE_PATH}
            )
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(dpcp_config INTERFACE
                --coverage
                -fprofile-$<IF:$<BOOL:${DPCP_GENERATE_PGO}>,generate,use>=${DPCP_PGO_PROFILE_PATH}
            )
        endif()
        target_link_libraries(dpcp_config INTERFACE $<$<BOOL:${DPCP_GENERATE_PGO}>:gcov>)
    endif()

endif()
