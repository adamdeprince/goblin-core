# SPDX-FileCopyrightText: Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

project(dpcp_gtest DESCRIPTION "DPCP internal tests based on derivative of GoogleTest framework." LANGUAGES C CXX)

option(ENABLE_CUDA "Enable CUDA" OFF)

if(ENABLE_CUDA)
    include(CheckLanguage)
    check_language(CUDA)
    enable_language(CUDA)
    find_package(CUDA REQUIRED)
endif()

add_executable(${PROJECT_NAME})
target_sources(${PROJECT_NAME}
    PRIVATE ${CMAKE_CURRENT_LIST_DIR}/main.cpp
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include/getopt.h
)
target_include_directories(${PROJECT_NAME}
    PUBLIC ${CMAKE_CURRENT_LIST_DIR}
    PRIVATE $<$<BOOL:${ENABLE_CUDA}>:${CUDA_INCLUDE_DIRS}>
)
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME}
    dpcp
    dpcp_common_deps
    Threads::Threads
    $<$<BOOL:${ENABLE_CUDA}>:cuda>
)

if (MSVC)
    set(DPCP_GTEST_C_CXX_FLAGS
        /TP /W4 /Gy
        /fp:fast
        /D_UNICODE /DUNICODE
        /DWIN32_LEAN_AND_MEAN
        /D_CRT_SECURE_NO_WARNINGS
        /D_SILENCE_TR1_NAMESPACE_DEPRECATION_WARNING=1
        /DGTEST_HAS_PTHREAD=0
        /D_WINSOCK_DEPRECATED_NO_WARNINGS
        /WX-
        /Oi
        /FC
    )
    target_include_directories(${PROJECT_NAME} PUBLIC include)
    set_target_properties(${PROJECT_NAME} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
else()
    set(DPCP_GTEST_C_CXX_FLAGS -g -fno-tree-vectorize)
endif()
target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX,C>:${DPCP_GTEST_C_CXX_FLAGS}>
    $<$<COMPILE_LANGUAGE:CUDA>:-m64>
    $<$<BOOL:${ENABLE_CUDA}>:-DGPU_DIRECT>
)

foreach(lib_dir "common" "dcmd" "dpcp")
    add_subdirectory(${lib_dir})
endforeach()
