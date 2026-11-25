cmake_minimum_required(VERSION 3.31)
project(realtek-mic-effects VERSION 0.1.0 LANGUAGES C CXX)

# Clang 23 is the only supported compiler. Use `cmake --preset release`.
foreach(lang C CXX)
    if(NOT CMAKE_${lang}_COMPILER_ID STREQUAL "Clang"
       OR NOT CMAKE_${lang}_COMPILER_VERSION MATCHES "^23\\.")
        message(FATAL_ERROR
            "Clang 23 is required for ${lang} "
            "(found ${CMAKE_${lang}_COMPILER_ID} ${CMAKE_${lang}_COMPILER_VERSION}). "
            "Configure with `cmake --preset release`.")
    endif()
endforeach()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Hide all symbols by default. SPA_EXPORT and LV2_SYMBOL_EXPORT mark the
# plugin entry points, and a linker version script per plugin also hides
# libstdc++ template instances.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# ThinLTO needs LLD 23 and LLVM 23 archive tools that read Clang 23 bitcode.
# The release preset selects ld.lld, llvm-ar-23, and llvm-ranlib-23.
# CMake detects the linker version only on the first configure, so also check
# CMAKE_LINKER_TYPE, which selects the linker for every build.
if(NOT CMAKE_LINKER_TYPE STREQUAL "LLD")
    message(FATAL_ERROR
        "CMAKE_LINKER_TYPE must be LLD (found '${CMAKE_LINKER_TYPE}'). "
        "Configure with `cmake --fresh --preset release`.")
endif()
foreach(lang C CXX)
    if(NOT CMAKE_${lang}_COMPILER_LINKER_ID STREQUAL "LLD"
       OR NOT CMAKE_${lang}_COMPILER_LINKER_VERSION MATCHES "^23\\.")
        message(FATAL_ERROR
            "LLD 23 is required for ${lang} (found ${CMAKE_${lang}_COMPILER_LINKER_ID} "
            "${CMAKE_${lang}_COMPILER_LINKER_VERSION} at ${CMAKE_${lang}_COMPILER_LINKER}). "
            "Configure with `cmake --fresh --preset release`.")
    endif()
endforeach()
foreach(tool IN ITEMS ar ranlib)
    string(TOUPPER "${tool}" var)
    cmake_path(GET CMAKE_${var} FILENAME tool_name)
    execute_process(COMMAND "${CMAKE_${var}}" --version
                    OUTPUT_VARIABLE tool_version ERROR_QUIET RESULT_VARIABLE tool_result)
    if(NOT tool_name MATCHES "^llvm-${tool}(-23)?$"
       OR NOT tool_result EQUAL 0
       OR NOT tool_version MATCHES "LLVM version 23\\.")
        message(FATAL_ERROR
            "llvm-${tool} 23 is required for ThinLTO archives (found ${CMAKE_${var}}). "
            "Configure with `cmake --preset release`.")
    endif()
endforeach()

option(RMIC_NATIVE_ARCH "Optimize for host CPU (-march=native)" ON)

# CMake's Release configuration already adds -O3 -DNDEBUG.
add_compile_options(
    -Wall -Wextra
    "$<$<CONFIG:Release>:-ffp-contract=fast;-fno-math-errno;-fno-trapping-math>"
)
if(RMIC_NATIVE_ARCH)
    add_compile_options("$<$<CONFIG:Release>:-march=native>")
endif()

# Hardening for all configurations: CET shadow stack and IBT, zeroed stack
# locals, stack protector, full RELRO, and a non-executable stack.
add_compile_options(
    -fcf-protection=full
    -ftrivial-auto-var-init=zero
    -fstack-protector-strong
)
add_link_options(
    -Wl,-z,relro,-z,now
    -Wl,-z,noexecstack
)

# Release: ThinLTO, CFI, and _FORTIFY_SOURCE=3 (needs optimization).
# CFI needs LTO and hidden visibility. cfi-icall stays off: the plugins run
# inside non-CFI hosts (PipeWire, LV2 hosts) and call host function pointers,
# such as the spa_log methods, which have no CFI type metadata.
set(RMIC_RELEASE_LTO_CFI -flto=thin -fsanitize=cfi -fno-sanitize=cfi-icall)
add_compile_options(
    "$<$<CONFIG:Release>:${RMIC_RELEASE_LTO_CFI}>"
    "$<$<CONFIG:Release>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=3>"
)
add_link_options("$<$<CONFIG:Release>:${RMIC_RELEASE_LTO_CFI}>")

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
find_library(FFTW3F_THREADS_LIB fftw3f_threads REQUIRED)

# Core DSP static library (compiled with -fPIC for shared plugins)
add_library(rmicdsp STATIC
    src/dsp/hpf.cpp
    src/dsp/delay_est.cpp
    src/dsp/aec.cpp
    src/dsp/omlsa_ns.cpp
    src/dsp/pipeline.cpp
)
target_include_directories(rmicdsp
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp
)
target_link_libraries(rmicdsp PRIVATE ${FFTW3F_THREADS_LIB} PkgConfig::FFTW3F ${CMAKE_DL_LIBS} m)
set_target_properties(rmicdsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(rmicdsp PRIVATE -Wpedantic)

# PipeWire SPA AEC plugin: aec/libspa-aec-realtek.so
pkg_check_modules(SPA REQUIRED IMPORTED_TARGET libspa-0.2)
# The libspa-0.2 module version is always 0.2. The SPA headers ship with
# PipeWire, so gate on the PipeWire version instead.
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3>=1.6)

add_library(spa-aec-realtek SHARED
    src/spa/aec-realtek.c
)
set_target_properties(spa-aec-realtek PROPERTIES
    OUTPUT_NAME "spa-aec-realtek"
    PREFIX "lib"
)
target_link_libraries(spa-aec-realtek PRIVATE rmicdsp PkgConfig::SPA)
set(SPA_EXPORT_MAP ${CMAKE_CURRENT_SOURCE_DIR}/src/spa/aec-realtek.map)
target_link_options(spa-aec-realtek PRIVATE -Wl,--version-script=${SPA_EXPORT_MAP})
set_property(TARGET spa-aec-realtek APPEND PROPERTY LINK_DEPENDS ${SPA_EXPORT_MAP})
# LLD gives the linker-generated __start_/__stop_spa_log_topic symbols
# protected visibility and keeps them exported despite the version script.
target_link_options(spa-aec-realtek PRIVATE -Wl,-z,start-stop-visibility=hidden)
# Strict C23 (no GNU extensions) hides the POSIX locale API that the SPA
# string helpers use.
target_compile_definitions(spa-aec-realtek PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(spa-aec-realtek PRIVATE -Wno-unused-parameter)

# LV2 Noise Suppression plugin: rmic_ns.lv2/rmic_ns.so
pkg_check_modules(LV2 REQUIRED IMPORTED_TARGET lv2)

add_library(rmic_ns SHARED
    src/lv2/rmic_ns_lv2.c
)
set_target_properties(rmic_ns PROPERTIES
    OUTPUT_NAME "rmic_ns"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/rmic_ns.lv2"
)
target_link_libraries(rmic_ns PRIVATE rmicdsp PkgConfig::LV2)
set(LV2_EXPORT_MAP ${CMAKE_CURRENT_SOURCE_DIR}/src/lv2/rmic_ns.map)
target_link_options(rmic_ns PRIVATE -Wl,--version-script=${LV2_EXPORT_MAP})
set_property(TARGET rmic_ns APPEND PROPERTY LINK_DEPENDS ${LV2_EXPORT_MAP})
target_compile_options(rmic_ns PRIVATE -Wno-unused-parameter)

configure_file(lv2/rmic_ns.lv2/manifest.ttl ${CMAKE_BINARY_DIR}/rmic_ns.lv2/manifest.ttl COPYONLY)
configure_file(lv2/rmic_ns.lv2/rmic_ns.ttl ${CMAKE_BINARY_DIR}/rmic_ns.lv2/rmic_ns.ttl COPYONLY)
