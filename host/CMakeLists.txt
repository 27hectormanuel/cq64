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
if(WIN32)
    # dynarmic and its fmt rely on <cstdlib>/<vector> arriving through other headers, which
    # llvm-mingw's libc++ doesn't do.
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdlib>" "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include vector>")
endif()
add_subdirectory(${TP}/dynarmic dynarmic EXCLUDE_FROM_ALL)

# Game profile: cq (Costume Quest) or mmoj (Middle Manager of Justice).
set(GAME cq CACHE STRING "Game profile to build")
set_property(CACHE GAME PROPERTY STRINGS cq mmoj)
set(GAME_SOURCES src/game_${GAME}.cpp)
# Costume Quest variant: "optimized" (controller support, battle-prompt and icon fixes, data
# patches, debug hooks) or "original" (the game as the 32-bit APK ships it, running on 64-bit).
set(CQ_VARIANT optimized CACHE STRING "Costume Quest build variant")
set_property(CACHE CQ_VARIANT PROPERTY STRINGS optimized original)
if(GAME STREQUAL "cq" AND NOT CQ_VARIANT STREQUAL "original")
    list(APPEND GAME_SOURCES src/gamepad.cpp src/gamepad.h src/simrate.cpp src/simrate.h)
endif()

set(HOST_SOURCES
    src/abi.h
    src/common.h
    src/cpu.cpp
    src/datapatch.cpp
    src/datapatch.h
    src/devtools.cpp
    src/devtools.h
    src/luahints.cpp
    src/luahints.h
    src/affinity.h
    src/game.h
    src/host_main.cpp
    src/jni_bridge.cpp
    src/loader.cpp
    src/mem.cpp
    src/natives.h
    src/paths.cpp
    src/thunks_android.cpp
    src/thunks_fmod.cpp
    src/thunks_gl.cpp
    src/thunks_libc.cpp
    src/thunks_pthread.cpp
    ${GAME_SOURCES}
)

if(WIN32)
    # Windows proof of concept: the same host as a desktop program. A small stand-in for the Java
    # VM (src/win/fakejvm.cpp) plays the part of the Android Java code, SDL2 provides the window,
    # input and audio, and ANGLE provides EGL/OpenGL ES on Direct3D. See docs/windows.md.
    set(WIN_DEPS "" CACHE PATH "Directory with sdl2/, zlib/, angle/ and the import libraries (fetch_win_deps.sh)")
    set(GUEST_RT "" CACHE FILEPATH "libguestrt.so to build into cq64.exe (guest/build.sh)")
    if(NOT EXISTS "${GUEST_RT}")
        message(FATAL_ERROR "Set GUEST_RT to a built libguestrt.so (guest/build.sh)")
    endif()
    enable_language(ASM)
    configure_file(src/win/guest_rt.S.in ${CMAKE_CURRENT_BINARY_DIR}/guest_rt.S @ONLY)
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/guest_rt.S PROPERTIES OBJECT_DEPENDS "${GUEST_RT}")
    add_executable(cq64
        ${HOST_SOURCES}
        ${CMAKE_CURRENT_BINARY_DIR}/guest_rt.S
        src/win/affinity_win.cpp
        src/win/fakejvm.cpp
        src/win/fakejvm.h
        src/win/posix_win.cpp
        src/win/win_main.cpp
    )
    target_include_directories(cq64 BEFORE PRIVATE src/win/include)
    target_include_directories(cq64 PRIVATE src ${WIN_DEPS}/sdl2/ucrt64/include ${WIN_DEPS}/zlib/ucrt64/include)
    target_include_directories(cq64 SYSTEM PRIVATE ${WIN_DEPS}/khronos)
    target_compile_options(cq64 PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wno-unused-function;-fexceptions;SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/win/include/win_compat.h>")
    target_compile_definitions(cq64 PRIVATE _FILE_OFFSET_BITS=64 WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_directories(cq64 PRIVATE ${WIN_DEPS}/sdl2/ucrt64/lib ${WIN_DEPS}/zlib/ucrt64/lib ${WIN_DEPS}/lib)
    target_link_libraries(cq64 PRIVATE dynarmic mingw32 SDL2main ${WIN_DEPS}/sdl2/ucrt64/lib/libSDL2.dll.a EGL GLESv2 ${WIN_DEPS}/zlib/ucrt64/lib/libz.a ws2_32 shlwapi comdlg32 pthread)
    target_link_options(cq64 PRIVATE -static-libstdc++ -static-libgcc -mconsole)
else()
    add_library(cqhost SHARED ${HOST_SOURCES} src/affinity.cpp)
    target_compile_options(cqhost PRIVATE -Wall -Wno-unused-function -fexceptions)
    target_link_libraries(cqhost PRIVATE dynarmic log android EGL GLESv1_CM GLESv2 z)
    target_link_options(cqhost PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
endif()
if(WIN32)
    set(HOST_TARGET cq64)
else()
    set(HOST_TARGET cqhost)
endif()

if(GAME STREQUAL "cq" AND CQ_VARIANT STREQUAL "original")
    target_compile_definitions(${HOST_TARGET} PRIVATE CQ_ORIGINAL=1)
endif()
