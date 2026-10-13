# SPDX-License-Identifier: CC0-1.0
cmake_minimum_required(VERSION 3.22)
project(cqhost CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

# dynarmic (A32 frontend only)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(Boost_INCLUDE_DIR ${TP}/boost CACHE PATH "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
add_subdirectory(${TP}/dynarmic dynarmic EXCLUDE_FROM_ALL)

add_library(cqhost SHARED
    src/abi.h
    src/common.h
    src/cpu.cpp
    src/datapatch.cpp
    src/datapatch.h
    src/devtools.cpp
    src/devtools.h
    src/gamepad.cpp
    src/gamepad.h
    src/host_main.cpp
    src/jni_bridge.cpp
    src/loader.cpp
    src/mem.cpp
    src/paths.cpp
    src/thunks_android.cpp
    src/thunks_fmod.cpp
    src/thunks_gl.cpp
    src/thunks_libc.cpp
    src/thunks_pthread.cpp
)
target_compile_options(cqhost PRIVATE -Wall -Wno-unused-function -fexceptions)
target_link_libraries(cqhost PRIVATE dynarmic log android EGL GLESv1_CM GLESv2)
target_link_options(cqhost PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
