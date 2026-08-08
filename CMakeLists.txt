# CMakeLists.txt for hsdaoh-rp2350 RP2350 - MISRC GUI (forked from harrypm)
cmake_minimum_required(VERSION 3.15)
project(misrc_gui_rp2350 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Allow include(CheckCPUArch) from harrypm cmake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/misrc_tools/cmake")

# Mirror CI Linux dependency resolution by using system pkg-config.
# This avoids Linuxbrew-isolated pkg-config instances that hide system .pc files.
if(UNIX AND NOT APPLE AND EXISTS "/usr/bin/pkg-config")
    set(PKG_CONFIG_EXECUTABLE "/usr/bin/pkg-config" CACHE FILEPATH "pkg-config executable used for dependency discovery" FORCE)
endif()

# ========================================
# HSDAOH dependency (self-contained machcnz source defaults)
# ========================================
set(HSDAOH_INC "" CACHE PATH "Path to hsdaoh include directory (contains hsdaoh.h or hsdaoh/hsdaoh.h)")
set(HSDAOH_LIB "" CACHE FILEPATH "Path to hsdaoh library file (e.g. libhsdaoh.so or libhsdaoh.so.0)")
set(HSDAOH_LOCAL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hsdaoh" CACHE PATH "Path to vendored machcnz hsdaoh source")
option(HSDAOH_USE_LOCAL_SOURCE "Prefer vendored hsdaoh source from HSDAOH_LOCAL_SOURCE_DIR before network fetch" ON)
option(HSDAOH_FETCH_FROM_GIT "Fetch/build hsdaoh automatically when manual paths are not provided" ON)
set(HSDAOH_GIT_REPOSITORY "https://github.com/machcnz/hsdaoh.git" CACHE STRING "Fallback hsdaoh source repository when vendored source is unavailable")
set(HSDAOH_GIT_TAG "MISRC_GUI_Support" CACHE STRING "Fallback hsdaoh branch/tag when vendored source is unavailable")

set(_hsdaoh_manual_override OFF)
if(HSDAOH_INC OR HSDAOH_LIB)
    if(NOT HSDAOH_INC OR NOT HSDAOH_LIB)
        message(FATAL_ERROR "When overriding hsdaoh paths, set both -DHSDAOH_INC and -DHSDAOH_LIB.")
    endif()
    if(NOT EXISTS "${HSDAOH_INC}")
        message(FATAL_ERROR "HSDAOH_INC path does not exist: ${HSDAOH_INC}")
    endif()
    if(NOT EXISTS "${HSDAOH_INC}/hsdaoh.h" AND NOT EXISTS "${HSDAOH_INC}/hsdaoh/hsdaoh.h")
        message(FATAL_ERROR "HSDAOH_INC must contain hsdaoh.h (or hsdaoh/hsdaoh.h): ${HSDAOH_INC}")
    endif()
    if(NOT EXISTS "${HSDAOH_LIB}")
        message(FATAL_ERROR "HSDAOH_LIB path does not exist: ${HSDAOH_LIB}")
    endif()
    set(_hsdaoh_manual_override ON)
endif()

if(_hsdaoh_manual_override)
    add_library(hsdaoh::hsdaoh UNKNOWN IMPORTED)
    set_target_properties(hsdaoh::hsdaoh PROPERTIES
        IMPORTED_LOCATION "${HSDAOH_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${HSDAOH_INC}"
    )
    get_filename_component(HSDAOH_LIBDIR "${HSDAOH_LIB}" DIRECTORY)
    message(STATUS "hsdaoh: using manual override (HSDAOH_INC/HSDAOH_LIB).")
else()

    if(DEFINED BUILD_SHARED_LIBS)
        set(_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
        set(_had_build_shared_libs TRUE)
    else()
        set(_had_build_shared_libs FALSE)
    endif()
    if(DEFINED INSTALL_UDEV_RULES)
        set(_saved_install_udev_rules "${INSTALL_UDEV_RULES}")
        set(_had_install_udev_rules TRUE)
    else()
        set(_had_install_udev_rules FALSE)
    endif()

    # Keep hsdaoh as static + no udev-rule install inside project builds.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(INSTALL_UDEV_RULES OFF CACHE BOOL "" FORCE)

    set(_hsdaoh_source_label "")
    if(HSDAOH_USE_LOCAL_SOURCE AND EXISTS "${HSDAOH_LOCAL_SOURCE_DIR}/CMakeLists.txt")
        set(_hsdaoh_source_label "vendored")
        set(_hsdaoh_binary_dir "${CMAKE_BINARY_DIR}/_deps/hsdaoh_local-build")
        add_subdirectory("${HSDAOH_LOCAL_SOURCE_DIR}" "${_hsdaoh_binary_dir}")
        set(HSDAOH_INC "${HSDAOH_LOCAL_SOURCE_DIR}/include")
        set(HSDAOH_LIBDIR "${_hsdaoh_binary_dir}/src")
    else()
        if(NOT HSDAOH_FETCH_FROM_GIT)
            message(FATAL_ERROR "HSDAOH_INC/HSDAOH_LIB are unset, vendored hsdaoh source is unavailable, and HSDAOH_FETCH_FROM_GIT=OFF.")
        endif()
        include(FetchContent)
        FetchContent_Declare(
            hsdaoh_external
            GIT_REPOSITORY "${HSDAOH_GIT_REPOSITORY}"
            GIT_TAG "${HSDAOH_GIT_TAG}"
            GIT_PROGRESS TRUE
        )
        FetchContent_MakeAvailable(hsdaoh_external)
        set(_hsdaoh_source_label "fetched")
        if(DEFINED hsdaoh_external_SOURCE_DIR)
            set(HSDAOH_INC "${hsdaoh_external_SOURCE_DIR}/include")
        endif()
        if(DEFINED hsdaoh_external_BINARY_DIR)
            set(HSDAOH_LIBDIR "${hsdaoh_external_BINARY_DIR}/src")
        else()
            set(HSDAOH_LIBDIR "")
        endif()
    endif()

    if(_had_build_shared_libs)
        set(BUILD_SHARED_LIBS "${_saved_build_shared_libs}" CACHE BOOL "" FORCE)
    else()
        unset(BUILD_SHARED_LIBS CACHE)
    endif()
    if(_had_install_udev_rules)
        set(INSTALL_UDEV_RULES "${_saved_install_udev_rules}" CACHE BOOL "" FORCE)
    else()
        unset(INSTALL_UDEV_RULES CACHE)
    endif()

    if(TARGET hsdaoh_static)
        add_library(hsdaoh::hsdaoh ALIAS hsdaoh_static)
    elseif(TARGET hsdaoh)
        add_library(hsdaoh::hsdaoh ALIAS hsdaoh)
    else()
        message(FATAL_ERROR "Fetched hsdaoh did not define expected targets (hsdaoh_static or hsdaoh).")
    endif()

    if(_hsdaoh_source_label STREQUAL "vendored")
        message(STATUS "hsdaoh: using vendored source at ${HSDAOH_LOCAL_SOURCE_DIR}")
    else()
        message(STATUS "hsdaoh: fetched from ${HSDAOH_GIT_REPOSITORY} @ ${HSDAOH_GIT_TAG}")
    endif()
endif()

# Enable upstream mode for RP2350
add_definitions(-DHSDAOH_UPSTREAM=1)

# Build options
option(USE_SYSTEM_RAYLIB "Use system raylib instead of downloading" OFF)

# ========================================
# Find Dependencies
# ========================================
find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
pkg_check_modules(FLAC REQUIRED flac)
pkg_check_modules(SOXR REQUIRED soxr)
pkg_check_modules(FFTW3F REQUIRED fftw3f)

# Test if FLAC library actually has threading support... we need v1.5+
# (Version doesn't guarantee the feature - depends on build flags)
include(CheckLibraryExists)
set(CMAKE_REQUIRED_LIBRARIES ${FLAC_LIBRARIES})
check_library_exists(FLAC FLAC__stream_encoder_set_num_threads "" HAVE_FLAC_THREADING)

if(HAVE_FLAC_THREADING)
    message(STATUS "FLAC threading support: YES")
else()
    message(STATUS "FLAC threading support: NO (library built without pthread)")
endif()

# ========================================
# Raylib
# ========================================
if(USE_SYSTEM_RAYLIB)
    find_package(raylib 5.0 REQUIRED)
else()
    include(FetchContent)
    set(FETCHCONTENT_QUIET FALSE)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 5.0
        GIT_PROGRESS TRUE
    )
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(raylib)
endif()

# ========================================
# Common Sources
# ========================================
set(COMMON_SOURCES
    misrc_tools/common/ringbuffer.c
    misrc_tools/common/flac_writer.c
    misrc_tools/common/buffer_manager.c
    misrc_tools/common/extract.c
    misrc_tools/common/rb_event.c
    misrc_tools/common/file_utils.c
    misrc_tools/common/device_enum.c
    misrc_tools/common/ringbuffer_writer.c

    # Fix unresolved sc_get_* symbols (Linux implementation)
    misrc_tools/misrc_capture/simple_capture/simple_capture_v4l2.c
)

# Extract ASM (x86_64 optimizations) must be before add_executable
if(EXISTS "${CMAKE_SOURCE_DIR}/misrc_tools/common/extract.asm")
    enable_language(ASM_NASM)

    # CRITICAL: NASM needs its own compile command, not C compiler flags
    set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
    set(CMAKE_ASM_NASM_COMPILE_OBJECT "<CMAKE_ASM_NASM_COMPILER> -f elf64 -o <OBJECT> <SOURCE>")

    list(APPEND COMMON_SOURCES misrc_tools/common/extract.asm)
endif()

# ========================================
# GUI Sources
# ========================================
file(GLOB GUI_CORE_SOURCES
    misrc_tools/misrc_gui/core/*.c
)

set(GUI_INPUT_SOURCES
    misrc_tools/misrc_gui/input/gui_capture.c
    misrc_tools/misrc_gui/input/gui_simulated.c
    misrc_tools/misrc_gui/input/gui_playback.c
)

file(GLOB GUI_OUTPUT_SOURCES
    misrc_tools/misrc_gui/output/*.c
)

file(GLOB GUI_PROCESSING_SOURCES
    misrc_tools/misrc_gui/processing/*.c
)

file(GLOB GUI_SIGNAL_SOURCES
    misrc_tools/misrc_gui/signal/*.c
)

file(GLOB GUI_UI_SOURCES
    misrc_tools/misrc_gui/ui/*.c
)

file(GLOB GUI_VIZ_SOURCES
    misrc_tools/misrc_gui/visualization/*.c
)

set(GUI_SOURCES
    ${GUI_CORE_SOURCES}
    ${GUI_INPUT_SOURCES}
    ${GUI_OUTPUT_SOURCES}
    ${GUI_PROCESSING_SOURCES}
    ${GUI_SIGNAL_SOURCES}
    ${GUI_UI_SOURCES}
    ${GUI_VIZ_SOURCES}
)

# =======
