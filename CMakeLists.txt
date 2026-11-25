cmake_minimum_required(VERSION 3.20)
project(ProjectDragon VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DRAGON_BUILD_TOOLS "Build PC asset validation CLI tools" ON)
option(DRAGON_BUILD_TESTS "Build automated unit & integration tests" ON)
option(DRAGON_PS4_VULKAN  "Link ps4-kit Mesa/RADV Vulkan backend on PS4 (requires PS4_MESA_BUILD)" OFF)

# =============================================================================
# 1. Portable codol_zone (FastFile v334 loader, MIT-derived from sh1xo-radiant)
# =============================================================================
add_library(codol_zone_portable STATIC
    third_party/codol_zone_portable/codol_zone/src/assets.cpp
    third_party/codol_zone_portable/codol_zone/src/crypto_portable.cpp
    third_party/codol_zone_portable/codol_zone/src/fastfile.cpp
    third_party/codol_zone_portable/codol_zone/src/inflate.cpp
    third_party/codol_zone_portable/codol_zone/src/zone.cpp
    third_party/codol_zone_portable/codol_zone/src/zone_load.cpp
)
target_include_directories(codol_zone_portable
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/codol_zone_portable/codol_zone/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/codol_zone_portable/codol_zone/src
)
if (MSVC)
    target_compile_options(codol_zone_portable PRIVATE /bigobj /EHsc)
endif ()

# =============================================================================
# 2. Portable codol_textures (IWD AES-192-CTR + IWI + DXT1/3/5 + Wavelet)
# =============================================================================
add_library(codol_textures_portable STATIC
    third_party/codol_zone_portable/codol_textures/src/crypto_portable.cpp
    third_party/codol_zone_portable/codol_textures/src/inflate.cpp
    third_party/codol_zone_portable/codol_textures/src/iwd.cpp
    third_party/codol_zone_portable/codol_textures/src/iwi.cpp
    third_party/codol_zone_portable/codol_textures/src/material.cpp
    third_party/codol_zone_portable/codol_textures/src/texture_decode.cpp
    third_party/codol_zone_portable/codol_textures/src/texture_upload.cpp
    third_party/codol_zone_portable/codol_textures/src/wavelet.cpp
)
target_include_directories(codol_textures_portable
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/codol_zone_portable/codol_textures/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/codol_zone_portable/codol_zone/src
)
target_link_libraries(codol_textures_portable PUBLIC codol_zone_portable)

# =============================================================================
# 3. Project Dragon Core Engine Library (Shared PS4 OrbisOS & PC Debug)
# =============================================================================
set(DRAGON_ENGINE_SOURCES
    engine/platform/crash_log.cpp
    engine/platform/debug_console.cpp
    engine/assets/asset_manager.cpp
    engine/world/bsp_world.cpp
    engine/world/collision.cpp
    engine/world/entities.cpp
    engine/world/player_movement.cpp
    engine/world/gameplay.cpp
    engine/gfx/software_rasterizer.cpp
)

if (DRAGON_PS4)
    list(APPEND DRAGON_ENGINE_SOURCES
        engine/platform/platform_ps4.cpp
        engine/platform/input_ps4.cpp
        engine/platform/audio_ps4.cpp
        engine/gfx/ps4_renderer.cpp
    )
else ()
    list(APPEND DRAGON_ENGINE_SOURCES
        engine/platform/platform_pc.cpp
        engine/gfx/pc_renderer.cpp
    )
endif ()

add_library(dragon_engine STATIC ${DRAGON_ENGINE_SOURCES})
target_include_directories(dragon_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine)
target_link_libraries(dragon_engine PUBLIC codol_zone_portable codol_textures_portable)

if (NOT DRAGON_PS4)
    find_package(Threads REQUIRED)
    target_link_libraries(dragon_engine PUBLIC Threads::Threads)
    if (WIN32)
        target_link_libraries(dragon_engine PUBLIC bcrypt)
    endif ()
endif ()

# =============================================================================
# 4. Main Executable (Native PS4 ELF + eboot.bin FSELF, or PC Debug binary)
# =============================================================================
add_executable(project_dragon engine/app/main.cpp)
target_link_libraries(project_dragon PRIVATE dragon_engine)

if (DRAGON_PS4)
    # Link official OpenOrbis PS4 system library stubs (zero hard dependency on Piglet .sprx: loaded dynamically!)
    target_link_libraries(project_dragon PRIVATE
        -lSceVideoOut
        -lScePad
        -lSceAudioOut
        -lSceUserService
        -lSceSystemService
        -lSceSysmodule
        -lSceRtc
        -lSceNet
    )
    set_target_properties(project_dragon PROPERTIES OUTPUT_NAME "project_dragon.elf")

    set(PS4_PAID "0x3800000000000035" CACHE STRING "PS4 Program Authority ID")
    set(PS4_AUTH_INFO "000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000")

    if (CMAKE_HOST_WIN32)
        set(OO_TOOLS_DIR "${OO_PS4_TOOLCHAIN}/bin/windows")
        set(CREATE_FSELF "${OO_TOOLS_DIR}/create-fself.exe")
    else ()
        set(OO_TOOLS_DIR "${OO_PS4_TOOLCHAIN}/bin/linux")
        set(CREATE_FSELF "${OO_TOOLS_DIR}/create-fself")
    endif ()

    set(DRAGON_OELF "${CMAKE_CURRENT_BINARY_DIR}/project_dragon.oelf")
    set(DRAGON_EBOOT "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin")

    add_custom_command(
        OUTPUT "${DRAGON_EBOOT}" "${DRAGON_OELF}"
        COMMAND ${CMAKE_COMMAND} -E env OO_PS4_TOOLCHAIN=${OO_PS4_TOOLCHAIN}
            ${CREATE_FSELF}
            -in=$<TARGET_FILE:project_dragon>
            -out=${DRAGON_OELF}
            --eboot=${DRAGON_EBOOT}
            --paid=${PS4_PAID}
            --authinfo=${PS4_AUTH_INFO}
        COMMAND ${CMAKE_COMMAND} -DOELF=${DRAGON_OELF} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check-oelf.cmake
        DEPENDS project_dragon
        COMMENT "Converting native PS4 ELF -> OELF -> eboot.bin (FSELF) & verifying PT_LOAD segments"
        VERBATIM
    )
    add_custom_target(dragon_eboot ALL DEPENDS "${DRAGON_EBOOT}")
else ()
    if (DRAGON_BUILD_TOOLS)
        add_executable(dragon_validate tools/dragon_validate/main.cpp)
        target_link_libraries(dragon_validate PRIVATE dragon_engine)
    endif ()

    if (DRAGON_BUILD_TESTS)
        enable_testing()
        add_executable(dragon_tests tests/test_suite.cpp)
        target_link_libraries(dragon_tests PRIVATE dragon_engine)
        target_include_directories(dragon_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/codol_zone_portable/codol_zone/src)
        add_test(NAME dragon_tests COMMAND dragon_tests)
    endif ()
endif ()
