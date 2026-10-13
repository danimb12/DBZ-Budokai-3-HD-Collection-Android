# dbz3 - ReXGlue Recompiled Project
# Dragon Ball Z Budokai 3 (PAL / Xbox 360 HD Collection)

cmake_minimum_required(VERSION 3.25)
project(dbz3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find ReXGlue SDK (pre-built distribution). The Android superbuild
# (android/CMakeLists.txt) adds the SDK in-tree instead, so skip it then.
if(NOT TARGET rex::runtime)
    find_package(rexglue REQUIRED CONFIG)
endif()

# User source files
set(DBZ3_SOURCES
    src/main.cpp
    src/hooks.cpp
    src/roster_trace.cpp
    src/roster_ext.cpp
    src/select_ext.cpp
    src/mods.cpp
    src/native_mods.cpp
    src/region.cpp
    src/launcher/settings.cpp
    src/launcher/launcher_state.cpp
    src/launcher/ui_kit.cpp
    src/launcher/mod_pipeline.cpp
    src/launcher/i18n.cpp
    src/launcher/update_check.cpp
    src/launcher/novas_mods.cpp
    src/ingame/menu.cpp
    src/ingame/quick_settings.cpp
    src/ingame/game_style.cpp
    src/ingame/native_ui.cpp
    src/ingame/menu_pilot.cpp
    src/ingame/online_panel.cpp
    src/ingame/tagteam_panel.cpp
    src/net/netplay.cpp
    src/net/room.cpp
    src/net/netplay_hooks.cpp
    src/tagteam.cpp
    src/tagteam_slots.cpp
    src/campaigns.cpp
)

# Codegen output directory. Default is the US/NA build (generated/); a second
# variant built from the EU/PAL executable uses -DDBZ3_GENERATED_DIR=generated_eu
# and produces a core that boots the EU xex (see dbz3_manifest_eu.toml).
set(DBZ3_GENERATED_DIR "generated" CACHE STRING
    "Codegen output dir to build against (generated = US/NA, generated_eu = EU/PAL)")

# Dual-region core: ONE executable containing BOTH codegens. At runtime the
# core detects the default.xex (MD5) and activates the matching image config.
# This is the user-facing release build (single exe for US and EU).
set(DBZ3_DUAL_REGION OFF CACHE BOOL
    "Build one core with BOTH the US and EU codegens (auto-detects the xex)")

# Include generated code if codegen has been run
if(DBZ3_DUAL_REGION OR DBZ3_GENERATED_DIR STREQUAL "generated")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/sources.cmake")
        include(${CMAKE_CURRENT_SOURCE_DIR}/generated/sources.cmake)
        list(APPEND DBZ3_SOURCES ${GENERATED_SOURCES})
    endif()
endif()
if(DBZ3_DUAL_REGION OR DBZ3_GENERATED_DIR STREQUAL "generated_eu")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated_eu/sources.cmake")
        include(${CMAKE_CURRENT_SOURCE_DIR}/generated_eu/sources.cmake)
        list(APPEND DBZ3_SOURCES ${GENERATED_SOURCES})
    endif()
endif()

# hooks.cpp only applies to builds that include the US/NA codegen (single US
# and dual-region). The single-EU build excludes it: the EU guest addresses
# differ and the US hooks would not apply.
if(DBZ3_GENERATED_DIR STREQUAL "generated_eu" AND NOT DBZ3_DUAL_REGION)
    list(REMOVE_ITEM DBZ3_SOURCES src/hooks.cpp)
    list(REMOVE_ITEM DBZ3_SOURCES src/roster_trace.cpp)
    list(REMOVE_ITEM DBZ3_SOURCES src/roster_ext.cpp src/select_ext.cpp)
    list(REMOVE_ITEM DBZ3_SOURCES src/net/netplay_hooks.cpp src/tagteam.cpp src/tagteam_slots.cpp src/campaigns.cpp)
    # The in-game panels still call into those US-only features: link no-op
    # stand-ins (Tag Team, new characters and campaigns are US-only anyway).
    list(APPEND DBZ3_SOURCES src/eu_only_stubs.cpp)
endif()

if(ANDROID)
    # Android: SDLActivity loads libmain.so and calls SDL_main.
    add_library(dbz3 SHARED ${DBZ3_SOURCES})
    set_target_properties(dbz3 PROPERTIES OUTPUT_NAME main)
elseif(WIN32)
    add_executable(dbz3 WIN32 ${DBZ3_SOURCES} src/version.rc)
    # UCRT marks getenv/fopen as "deprecated" even though they are the portable
    # choice here (the game also builds on Linux where they are not). Silence
    # the -Wdeprecated-declarations noise project-wide on MSVC/clang-cl.
    target_compile_definitions(dbz3 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    add_executable(dbz3 ${DBZ3_SOURCES})
endif()

target_include_directories(dbz3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
if(DBZ3_DUAL_REGION OR DBZ3_GENERATED_DIR STREQUAL "generated")
    target_include_directories(dbz3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/generated)
endif()
if(DBZ3_DUAL_REGION OR DBZ3_GENERATED_DIR STREQUAL "generated_eu")
    target_include_directories(dbz3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/generated_eu)
endif()

# Variant compile definitions (selects the init headers in main.cpp).
if(DBZ3_DUAL_REGION)
    target_compile_definitions(dbz3 PRIVATE DBZ3_DUAL_REGION=1)
elseif(DBZ3_GENERATED_DIR STREQUAL "generated_eu")
    target_compile_definitions(dbz3 PRIVATE DBZ3_EU_VARIANT=1)
endif()

target_link_libraries(dbz3 PRIVATE
    rex::runtime
    rex::gpu-xenos
)

if(WIN32)
    # Launcher update check (GitHub releases over WinHTTP) and VERSIONINFO
    # reading (winver) for the running build.
    target_link_libraries(dbz3 PRIVATE winhttp version ws2_32 ole32 oleaut32)   # ws2_32: online (src/net); ole32: UPnP del router
endif()

if(UNIX AND NOT APPLE)
    # Whole-archive linking for kernel hooks
    target_link_options(dbz3 PRIVATE
        -Wl,--whole-archive
        $<TARGET_FILE:rex::kernel>
        -Wl,--no-whole-archive
    )
endif()

# SDK host-app configuration: adds SDL entry point + ReXApp source,
# REXGLUE_BUILD_CONFIG define, runtime DLL staging (rexruntime, TracyClient),
# and copies the xenos GPU plugin next to the executable.
rexglue_configure_target(dbz3 GPU_PLUGINS xenos)

# SDL game controller mappings (community GameControllerDB, zlib). The runtime's
# `hid_mappings_file` cvar defaults to "gamecontrollerdb.txt" and loads it from
# the working directory, so shipping it next to the exe gives the SDL input
# backend real mappings for generic pads. See AGENTS.md seccion 7.
set(DBZ3_CONTROLLERDB "${CMAKE_CURRENT_SOURCE_DIR}/gamecontrollerdb.txt")
if(EXISTS "${DBZ3_CONTROLLERDB}")
    add_custom_command(TARGET dbz3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DBZ3_CONTROLLERDB}" "$<TARGET_FILE_DIR:dbz3>/gamecontrollerdb.txt"
        COMMENT "Staging gamecontrollerdb.txt next to dbz3"
        VERBATIM)
endif()

# Recursos de las pantallas propias (TAG TEAM, ONLINE, fichas del Tag Team): tools/ingame_ui.py
# (el repo publico no lleva res/: sin la carpeta, las pantallas escriben el titulo con texto)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/ingame_ui")
add_custom_command(TARGET dbz3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/res/ingame_ui" "$<TARGET_FILE_DIR:dbz3>/ingame_ui"
    COMMENT "Staging ingame_ui next to dbz3"
    VERBATIM)
endif()

# Codegen target - run 'cmake --build . --target dbz3_codegen'
add_custom_target(dbz3_codegen
    COMMAND $<TARGET_FILE:rex::rexglue> codegen ${CMAKE_CURRENT_SOURCE_DIR}/dbz3_manifest.toml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating recompiled code for dbz3"
    VERBATIM
)
