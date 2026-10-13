# DBZ3 Android superbuild: ReXGlue SDK (in-tree, with patches/rexglue-sdk
# overlay applied) + the dbz3 game as libmain.so for SDLActivity.
#
#   cmake -G Ninja -S android -B build-android \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
#     -DREXGLUE_SDK_DIR=/path/to/rexglue-sdk-0.10.0-patched \
#     -DDBZ3_GENERATED_DIR=generated_eu \
#     -DCMAKE_C_FLAGS="-march=armv8.2-a -fno-stack-protector -U_FORTIFY_SOURCE" \
#     -DCMAKE_CXX_FLAGS="-march=armv8.2-a -fno-stack-protector -U_FORTIFY_SOURCE"
cmake_minimum_required(VERSION 3.25)
project(dbz3_android C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(REXGLUE_SDK_DIR "" CACHE PATH "ReXGlue SDK v0.10.0 source tree with the overlay applied")
if(NOT REXGLUE_SDK_DIR)
    message(FATAL_ERROR "Set -DREXGLUE_SDK_DIR (see android/README.md)")
endif()

set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
set(REXGLUE_USE_VULKAN ON CACHE BOOL "" FORCE)
add_subdirectory(${REXGLUE_SDK_DIR} rexglue-sdk)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. dbz3)

# SDL3 (and its JNI_OnLoad) lives in librexruntime.so: GameActivity loads
# c++_shared, rexruntime and main, in that order.
target_link_libraries(dbz3 PRIVATE android log)

# rexglue_apply_target_settings() pins -march=armv8-a on the game target; the
# recompiled guest code is the hot path, so re-target ARMv8.2 (LSE atomics,
# FP16, dot product: every arm64 phone since ~2018) and drop the NDK's
# stack protector / fortify checks from it.
target_compile_options(dbz3 PRIVATE -march=armv8.2-a -fno-stack-protector -U_FORTIFY_SOURCE)

# Header-only/bundled third-party includes the installed SDK ships in its
# include dir but the in-tree build keeps per-target.
target_include_directories(dbz3 PRIVATE
    ${REXGLUE_SDK_DIR}/thirdparty/xxHash
    ${REXGLUE_SDK_DIR}/thirdparty/imgui)
