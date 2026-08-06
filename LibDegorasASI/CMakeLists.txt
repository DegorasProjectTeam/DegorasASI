
# ----------------------------------------------------------------------------------------------------------------------
# INITIALIZATION

# Cmake minimum.
cmake_minimum_required(VERSION 3.21)

# Project name.
project(LibDegorasASI VERSION 0.0.1 LANGUAGES C CXX)

# Set cmake policy.
if(COMMAND cmake_policy)
    cmake_policy(SET CMP0054 NEW)
endif()

# Standard install directories (bin/lib/include/share/...) and our own cmake/ modules (FindASICamera).
include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# ----------------------------------------------------------------------------------------------------------------------
# OPTIONS

# Linkage: BUILD_SHARED_LIBS is the source of truth (vcpkg/triplets set it). LIBDEGORASASI_BUILD_SHARED stays for
# standalone ergonomics and simply seeds its default from BUILD_SHARED_LIBS when that is defined.
if(DEFINED BUILD_SHARED_LIBS)
    set(_la_default_shared ${BUILD_SHARED_LIBS})
else()
    set(_la_default_shared ON)
endif()

option(LIBDEGORASASI_BUILD_SHARED   "Build LibDegorasASI as a shared library." ${_la_default_shared})
option(LIBDEGORASASI_BUILD_TESTING  "Build the assert-based test executables."  ON)
option(LIBDEGORASASI_BUILD_EXAMPLES "Build the example executables."            ON)
option(LIBDEGORASASI_BUILD_DOCS     "Install the documentation (README)."       OFF)
option(LIBDEGORASASI_INSTALL_EXAMPLES "Install the example sources."            OFF)
option(LIBDEGORASASI_INSTALL_ASI_RUNTIME
       "Install the vendored ZWO ASI runtime library next to the library." ON)

# Install rules default ON only when this is the top-level project (off when embedded via add_subdirectory).
option(LIBDEGORASASI_INSTALL "Generate install and export rules." ${PROJECT_IS_TOP_LEVEL})

# ----------------------------------------------------------------------------------------------------------------------
# BASIC PROJECT CONFIGURATION

# Vendored ZWO ASI Camera SDK, kept in the vendor's own original layout (include/ + lib/<arch>/) so a new SDK release
# can be dropped in unmodified. Used as the default location for the ASICamera package below; a consumer or a packager
# can point ASI_SDK_ROOT at a system install instead.
set(THIRDPARTY_ASI ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/ASI)
if(NOT DEFINED ASI_SDK_ROOT AND NOT DEFINED ENV{ASI_SDK_ROOT})
    set(ASI_SDK_ROOT "${THIRDPARTY_ASI}" CACHE PATH "Root of the ZWO ASI Camera SDK.")
endif()

# Output folders (build tree).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Language standard.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The public API passes std::string / std::vector / std::function across the library boundary, so a shared build and its
# consumers must use one C++ runtime instance, i.e. a compatible toolchain on both sides (see docs/PACKAGING.md). Unlike
# the Thorlabs Kinesis SDK, the ZWO ASI SDK is shipped by the vendor for Windows, Linux, macOS and Android, so this
# project imposes NO platform or toolchain lock: MinGW/UCRT64 on Windows is the reference (validated) toolchain, and any
# other platform builds as soon as the matching vendor SDK is present. Thread support is required for the status poller.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ----------------------------------------------------------------------------------------------------------------------
# THIRD-PARTY DEPENDENCY

# Locate the ZWO ASI Camera SDK (vendored by default) -> imported target ZWO::ASICamera. Kept PRIVATE: only the Layer-2
# adapter translation unit sees it, and it never leaks into the public compile interface.
find_package(ASICamera REQUIRED)

# ----------------------------------------------------------------------------------------------------------------------
# STAGE RUNTIME LIBRARIES NEXT TO THE BUILD BINARIES (developer convenience; independent of install)

# The vendored ASI runtime sits next to its import library in the vendor tree.
file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
file(GLOB ASI_RUNTIME_FILES "${ASICamera_RUNTIME_DIR}/*${CMAKE_SHARED_LIBRARY_SUFFIX}")
foreach(runtime_file ${ASI_RUNTIME_FILES})
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${runtime_file}"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/")
endforeach()

# MinGW C++ runtime (resolved from the active compiler's bin directory) so binaries launch without MSYS2 on PATH.
if(MINGW)
    get_filename_component(MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(runtime_dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
        if(EXISTS "${MINGW_BIN_DIR}/${runtime_dll}")
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MINGW_BIN_DIR}/${runtime_dll}"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${runtime_dll}")
        endif()
    endforeach()
endif()

# ----------------------------------------------------------------------------------------------------------------------
# SETUP LIBRARY

# Gather headers and sources (organised in layer subfolders: Common / Helpers / ASI / ASICamera / Devices).
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/includes/*.h)
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/sources/*.cpp)

# Module aggregator headers (extensionless) that re-export a whole layer, e.g. <LibDegorasASI/Modules/Devices>.
file(GLOB MODULES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/includes/LibDegorasASI/Modules/*)
set_source_files_properties(${MODULES} PROPERTIES HEADER_FILE_ONLY ON)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/includes PREFIX "Headers" FILES ${HEADERS})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/sources  PREFIX "Sources" FILES ${SOURCES})
source_group("Modules" FILES ${MODULES})

# Shared or static, controlled by the option (seeded from BUILD_SHARED_LIBS).
if(LIBDEGORASASI_BUILD_SHARED)
    add_library(LibDegorasASI SHARED ${HEADERS} ${SOURCES} ${MODULES})
else()
    add_library(LibDegorasASI STATIC ${HEADERS} ${SOURCES} ${MODULES})
    target_compile_definitions(LibDegorasASI PUBLIC LIBDEGORASASI_STATIC)
endif()

# Namespaced alias so in-tree consumers (examples/tests) and add_subdirectory users link the same name as installed
# consumers (find_package(DegorasASI) -> Degoras::ASI).
add_library(Degoras::ASI ALIAS LibDegorasASI)

set_target_properties(LibDegorasASI PROPERTIES
    PREFIX ""                       # output LibDegorasASI.{dll,lib,a}
    EXPORT_NAME ASI                 # exported as Degoras::ASI
    POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(LibDegorasASI PRIVATE LIBDEGORASASI_LIBRARY)

# Warnings / optimisation as PRIVATE build flags (never global CMAKE_CXX_FLAGS; multi-config safe; GCC/Clang only).
target_compile_options(LibDegorasASI PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wextra>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wpedantic>
    $<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-O0>
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-O3>)

# Public include root (build tree vs installed tree). Consumers write #include "LibDegorasASI/...".
target_include_directories(LibDegorasASI
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/includes>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# The vendored SDK is PRIVATE: its headers/link requirements do not propagate to consumers of a shared build. Threads is
# PUBLIC because the status poller's types appear in the public headers.
target_link_libraries(LibDegorasASI PRIVATE ZWO::ASICamera PUBLIC Threads::Threads)

# ----------------------------------------------------------------------------------------------------------------------
# SUBDIRECTORIES

if(LIBDEGORASASI_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(LIBDEGORASASI_BUILD_TESTING)
    include(CTest)
    add_subdirectory(testing)
endif()

# ----------------------------------------------------------------------------------------------------------------------
# INSTALL / EXPORT
#
# One prefix-relative layout (GNUInstallDirs) that serves a standalone `cmake --install` AND a vcpkg port: vcpkg
# re-roots this tree into installed/<triplet>/(debug/) and vcpkg_cmake_config_fixup() relocates the CMake package
# files from lib/cmake/ to share/. See docs/PACKAGING.md.

if(LIBDEGORASASI_INSTALL)

    include(CMakePackageConfigHelpers)

    set(LA_PKG_NAME  DegorasASI)
    set(LA_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/${LA_PKG_NAME})

    # Library + export set. Runtime component = what an end user runs; Development = what a downstream builder needs.
    install(TARGETS LibDegorasASI
        EXPORT ${LA_PKG_NAME}Targets
        RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
        LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Runtime NAMELINK_COMPONENT Development
        ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # Public headers, including the extensionless Modules aggregators.
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/includes/LibDegorasASI
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT Development)

    # Namespaced targets file.
    install(EXPORT ${LA_PKG_NAME}Targets
        FILE ${LA_PKG_NAME}Targets.cmake
        NAMESPACE Degoras::
        DESTINATION ${LA_CMAKE_DIR}
        COMPONENT Development)

    # A STATIC build leaks the PRIVATE ZWO dependency into the link interface ($<LINK_ONLY:ZWO::ASICamera>), so a static
    # consumer must re-find it; emit find_dependency only then. A shared build seals it into the DLL.
    if(LIBDEGORASASI_BUILD_SHARED)
        set(LA_ASI_FIND_DEPENDENCY "")
    else()
        set(LA_ASI_FIND_DEPENDENCY "find_dependency(ASICamera)")
    endif()

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/${LA_PKG_NAME}Config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/${LA_PKG_NAME}Config.cmake
        INSTALL_DESTINATION ${LA_CMAKE_DIR})

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/${LA_PKG_NAME}ConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/${LA_PKG_NAME}Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/${LA_PKG_NAME}ConfigVersion.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindASICamera.cmake
        DESTINATION ${LA_CMAKE_DIR}
        COMPONENT Development)

    # License -> vcpkg-style copyright (also useful standalone) and a usage snippet.
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/${LA_PKG_NAME}
        RENAME copyright
        COMPONENT Runtime)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/cmake/usage
        DESTINATION ${CMAKE_INSTALL_DATADIR}/${LA_PKG_NAME}
        COMPONENT Runtime)

    # Vendored ZWO ASI runtime library (opt-out via LIBDEGORASASI_INSTALL_ASI_RUNTIME=OFF).
    if(LIBDEGORASASI_INSTALL_ASI_RUNTIME AND ASI_RUNTIME_FILES)
        install(FILES ${ASI_RUNTIME_FILES}
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT Runtime)
    endif()

    # Optional documentation.
    if(LIBDEGORASASI_BUILD_DOCS)
        install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../README.md
            DESTINATION ${CMAKE_INSTALL_DOCDIR}
            COMPONENT Documentation)
    endif()

    # Optional example sources (as reference material under the doc dir).
    if(LIBDEGORASASI_INSTALL_EXAMPLES)
        install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/examples/
            DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples
            COMPONENT Examples)
    endif()

endif()

# **********************************************************************************************************************
