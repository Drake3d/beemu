cmake_minimum_required(VERSION 3.20)
project(beemu C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES arm64)

# Optimized with debug info unless asked otherwise (-DCMAKE_BUILD_TYPE=Debug for -O0).
# An empty build type means no optimization at all, which made boot ~2x slower.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g" CACHE STRING "" FORCE)   # keep assert() enabled

find_package(SDL3 REQUIRED CONFIG)

# ---- Berkeley SoftFloat 3e (x87 arithmetic), vendored in third_party/softfloat ----
include(third_party/softfloat/sources.cmake)
add_library(softfloat STATIC ${SOFTFLOAT_SOURCES})
target_include_directories(softfloat PUBLIC third_party/softfloat/source/include
                                     PRIVATE third_party/softfloat/source/8086)
# PUBLIC: users of softfloat.h must agree with the library on struct layouts (LITTLEENDIAN)
# and on how the rounding mode / exception flags are stored (THREAD_LOCAL).
# THREAD_LOCAL is empty: all x87 work runs on the emulation thread, and on macOS every
# _Thread_local access is a call to _tlv_get_addr (12% of an FPU-heavy guest's time).
# Per-CPU host threads (SMP) will need _Thread_local back, or the state passed explicitly.
target_compile_definitions(softfloat PUBLIC SOFTFLOAT_FAST_INT64 LITTLEENDIAN=1 THREAD_LOCAL=)
# Same options as upstream's build/Linux-x86_64-GCC; the file list assumes INLINE_LEVEL=5.
target_compile_definitions(softfloat PRIVATE SOFTFLOAT_ROUND_ODD INLINE_LEVEL=5 SOFTFLOAT_FAST_DIV32TO16
                                             SOFTFLOAT_FAST_DIV64TO32)
target_compile_options(softfloat PRIVATE -O2 -w)     # upstream code: keep its warnings out of ours

# ---- emulator core: CPU, JIT, system architecture, machine and devices (no SDL) ----
add_library(beemu_core STATIC
    core/cpu.c core/decode.c core/mem.c core/jit.c core/flags.c core/helpers.c core/system.c
    core/io.c core/fpu.c core/fpu_math.c
    core/machine.c
    core/dev/pic.c core/dev/pit.c core/dev/cmos.c core/dev/ps2.c core/dev/pci.c core/dev/fwcfg.c core/dev/rtl8139.c core/dev/g400.c core/dev/ne2k.c core/dev/es1370.c core/dev/dma.c core/dev/sb16.c core/dev/apic.c
    core/dev/vga.c core/dev/disk.c core/dev/ide.c)
target_include_directories(beemu_core PUBLIC core)
target_compile_options(beemu_core PRIVATE -Wall -Wextra)
target_link_libraries(beemu_core PUBLIC softfloat)

enable_language(OBJC)
add_executable(beemu main.c platform/platform_sdl3.c platform/png.c platform/vmstore.c platform/launcher_mac.m)
set_source_files_properties(platform/launcher_mac.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra")
target_include_directories(beemu PRIVATE platform)
target_link_libraries(beemu PRIVATE beemu_core SDL3::SDL3 objc "-framework Foundation" "-framework Cocoa")

# User-mode networking via libslirp (brew install libslirp; QEMU already depends on it).
# Optional: without it the RTL8139 is still there, with no network attached.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(SLIRP IMPORTED_TARGET slirp)
endif()
if(SLIRP_FOUND)
  target_sources(beemu PRIVATE platform/net_slirp.c platform/net_dns.c)
  target_link_libraries(beemu PRIVATE PkgConfig::SLIRP)
  target_compile_definitions(beemu PRIVATE BEEMU_HAVE_SLIRP=1)
  message(STATUS "libslirp ${SLIRP_VERSION}: user-mode networking enabled")
else()
  message(STATUS "libslirp not found: building without a network backend")
endif()
target_compile_options(beemu PRIVATE -Wall -Wextra)

# ---- tests ----
enable_testing()
function(beemu_test name)
    add_executable(${name} tests/${name}.c)
    target_link_libraries(${name} PRIVATE beemu_core)
    target_include_directories(${name} PRIVATE tests)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
endfunction()

beemu_test(test_decode)
add_test(NAME decode COMMAND test_decode)
beemu_test(test_mem)
add_test(NAME mem COMMAND test_mem)
# JIT: translation checks run anywhere; execution checks need an ARM64 host.
beemu_test(test_jit)
add_test(NAME jit COMMAND test_jit)
# Lazy flags: hand vectors everywhere, plus a randomized check against the real CPU on x86-64.
beemu_test(test_flags)
add_test(NAME flags COMMAND test_flags)
# Devices with a fake clock.
beemu_test(test_devices)
add_test(NAME devices COMMAND test_devices)

add_executable(test_vmstore tests/test_vmstore.c platform/vmstore.c)
target_include_directories(test_vmstore PRIVATE platform)
target_compile_options(test_vmstore PRIVATE -Wall -Wextra)
add_test(NAME vmstore COMMAND test_vmstore)

# System tests: self-checking guest programs assembled with nasm (brew install nasm).
find_program(NASM nasm)
if(NASM)
    set(ASM_OUT ${CMAKE_BINARY_DIR}/asm)
    file(MAKE_DIRECTORY ${ASM_OUT})
    set(ASM_BINS "")
    foreach(prog basic32 modes ide x87 paging)
        set(src ${CMAKE_SOURCE_DIR}/tests/asm/${prog}.asm)
        add_custom_command(OUTPUT ${ASM_OUT}/${prog}.bin
            COMMAND ${NASM} -f bin -I ${CMAKE_SOURCE_DIR}/tests/asm/ -o ${ASM_OUT}/${prog}.bin ${src}
            DEPENDS ${src} ${CMAKE_SOURCE_DIR}/tests/asm/check.inc
            COMMENT "nasm ${prog}.asm")
        list(APPEND ASM_BINS ${ASM_OUT}/${prog}.bin)
    endforeach()
    add_custom_target(asm_progs DEPENDS ${ASM_BINS})
    beemu_test(test_sys)
    target_compile_definitions(test_sys PRIVATE ASM_DIR="${ASM_OUT}")
    add_dependencies(test_sys asm_progs)
    add_test(NAME sys COMMAND test_sys)
else()
    message(STATUS "nasm not found: system tests (test_sys) are skipped")
endif()

# Cross-checks against real x86 behavior. On Apple Silicon these x86-64 binaries
# run under Rosetta 2; they are only added when Rosetta works here.
execute_process(COMMAND arch -x86_64 /usr/bin/true RESULT_VARIABLE ROSETTA_RC OUTPUT_QUIET ERROR_QUIET)
if(ROSETTA_RC EQUAL 0)
    add_executable(ref_alu_x86 tests/ref_alu_x86.c)
    set_target_properties(ref_alu_x86 PROPERTIES OSX_ARCHITECTURES x86_64)
    target_compile_options(ref_alu_x86 PRIVATE -Wall -Wextra -mno-red-zone -O1)
    beemu_test(test_alu)
    target_compile_definitions(test_alu PRIVATE REF_ALU="$<TARGET_FILE:ref_alu_x86>")
    add_dependencies(test_alu ref_alu_x86)
    add_test(NAME alu_vs_x86 COMMAND test_alu)
    set_tests_properties(alu_vs_x86 PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(ref_x87_x86 tests/ref_x87_x86.c)
    set_target_properties(ref_x87_x86 PROPERTIES OSX_ARCHITECTURES x86_64)
    target_compile_options(ref_x87_x86 PRIVATE -Wall -Wextra -mno-red-zone -O1)
    beemu_test(test_x87)
    target_compile_definitions(test_x87 PRIVATE REF_X87="$<TARGET_FILE:ref_x87_x86>")
    add_dependencies(test_x87 ref_x87_x86)
    add_test(NAME x87_vs_x86 COMMAND test_x87)
    set_tests_properties(x87_vs_x86 PROPERTIES SKIP_RETURN_CODE 77)

    # The lazy-flags randomized cross-check (x86-64 only) also runs this way.
    add_executable(test_flags_x86 tests/test_flags.c core/flags.c)
    set_target_properties(test_flags_x86 PROPERTIES OSX_ARCHITECTURES x86_64)
    target_include_directories(test_flags_x86 PRIVATE core)
    target_compile_options(test_flags_x86 PRIVATE -Wall -Wextra)
    add_test(NAME flags_vs_x86 COMMAND test_flags_x86)
else()
    message(STATUS "Rosetta 2 not available: x86 cross-checks are skipped")
endif()

# SeaBIOS end to end (skipped when QEMU's firmware images are not installed).
find_file(SEABIOS_BIN bios.bin PATHS ${CMAKE_SOURCE_DIR}/bios /opt/homebrew/share/qemu /usr/local/share/qemu NO_DEFAULT_PATH)
find_file(SEAVGABIOS_BIN vgabios-stdvga.bin PATHS ${CMAKE_SOURCE_DIR}/bios /opt/homebrew/share/qemu /usr/local/share/qemu NO_DEFAULT_PATH)
beemu_test(test_bios)
target_compile_definitions(test_bios PRIVATE BIOS_PATH="${SEABIOS_BIN}" VGABIOS_PATH="${SEAVGABIOS_BIN}")
add_test(NAME bios COMMAND test_bios)
set_tests_properties(bios PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# ---- beemu.app (cmake --build build --target app) ----
# A double-clickable bundle with SDL3, libslirp, SeaBIOS and their licenses inside (tools/make_app.py).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_custom_target(app
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/make_app.py ${CMAKE_BINARY_DIR}
    DEPENDS beemu
    COMMENT "Building beemu.app"
    VERBATIM)
endif()
