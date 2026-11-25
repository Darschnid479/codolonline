# Project Dragon — Native PlayStation 4 (OrbisOS x86-64) CMake Toolchain
# Targets AMD Jaguar (-march=btver2), OpenOrbis v0.5.4 + LLVM 18 + ps4-kit linker script fixes.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if (DEFINED ENV{OO_PS4_TOOLCHAIN} AND NOT "$ENV{OO_PS4_TOOLCHAIN}" STREQUAL "")
    set(OO_PS4_TOOLCHAIN "$ENV{OO_PS4_TOOLCHAIN}")
elseif (NOT DEFINED OO_PS4_TOOLCHAIN)
    get_filename_component(_dragon_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    set(OO_PS4_TOOLCHAIN "${_dragon_root}/toolchains/openorbis/OpenOrbis/PS4Toolchain")
endif ()
set(OO_PS4_TOOLCHAIN "${OO_PS4_TOOLCHAIN}" CACHE PATH "OpenOrbis PS4 Toolchain root directory")

if (NOT EXISTS "${OO_PS4_TOOLCHAIN}/lib/crt1.o")
    message(FATAL_ERROR "OpenOrbis PS4 Toolchain not found at '${OO_PS4_TOOLCHAIN}' (missing lib/crt1.o). Run scripts/setup.sh or SETUP.bat first.")
endif ()

find_program(DRAGON_PS4_CC NAMES clang-18 clang REQUIRED)
find_program(DRAGON_PS4_CXX NAMES clang++-18 clang++ REQUIRED)
find_program(DRAGON_PS4_LD NAMES ld.lld-18 ld.lld REQUIRED)
find_program(DRAGON_PS4_AR NAMES llvm-ar-18 llvm-ar ar REQUIRED)
find_program(DRAGON_PS4_RANLIB NAMES llvm-ranlib-18 llvm-ranlib ranlib REQUIRED)

set(CMAKE_C_COMPILER "${DRAGON_PS4_CC}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${DRAGON_PS4_CXX}" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER "${DRAGON_PS4_CC}" CACHE FILEPATH "" FORCE)
set(CMAKE_LINKER "${DRAGON_PS4_LD}" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${DRAGON_PS4_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${DRAGON_PS4_RANLIB}" CACHE FILEPATH "" FORCE)

set(DRAGON_PS4 TRUE CACHE BOOL "Building native PS4 OrbisOS target" FORCE)
set(DRAGON_PS4_LINK_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/ps4-link.x" CACHE FILEPATH "Patched OpenOrbis linker script")

set(_ps4_common_flags
    "--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -march=btver2 -D__PS4__ -D__ORBIS__ -D__OPENORBIS__ -DDRAGON_PS4 -D_BSD_SOURCE -D__BSD_VISIBLE -isysroot ${OO_PS4_TOOLCHAIN} -isystem ${OO_PS4_TOOLCHAIN}/include")

set(CMAKE_C_FLAGS_INIT "${_ps4_common_flags}")
set(CMAKE_CXX_FLAGS_INIT "-nostdinc++ -isystem ${OO_PS4_TOOLCHAIN}/include/c++/v1 ${_ps4_common_flags} -fexceptions -fcxx-exceptions")
set(CMAKE_ASM_FLAGS_INIT "${_ps4_common_flags}")

# Direct ld.lld invocation producing PIE ELF ready for OpenOrbis create-fself
set(_ps4_base_libs
    "-L${OO_PS4_TOOLCHAIN}/lib ${OO_PS4_TOOLCHAIN}/lib/crt1.o -lc -lc++ -lc++abi -lunwind -lclang_rt.builtins-x86_64 -lkernel -lSceLibcInternal")

set(CMAKE_C_LINK_EXECUTABLE
    "${DRAGON_PS4_LD} -m elf_x86_64 -pie --script ${DRAGON_PS4_LINK_SCRIPT} --eh-frame-hdr <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> ${_ps4_base_libs}")

set(CMAKE_CXX_LINK_EXECUTABLE
    "${DRAGON_PS4_LD} -m elf_x86_64 -pie --script ${DRAGON_PS4_LINK_SCRIPT} --eh-frame-hdr <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> ${_ps4_base_libs}")

set(CMAKE_FIND_ROOT_PATH "${OO_PS4_TOOLCHAIN}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_HAVE_LIBC_PTHREAD 1 CACHE INTERNAL "")
set(CMAKE_THREAD_LIBS_INIT "" CACHE INTERNAL "")
set(Threads_FOUND TRUE CACHE INTERNAL "")
