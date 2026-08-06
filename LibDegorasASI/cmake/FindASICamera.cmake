# FindASICamera.cmake
#
# Locates the (vendored or system) ZWO ASI Camera SDK and defines an imported interface target:
#
#     ZWO::ASICamera
#
# The ZWO ASI Camera SDK is a proprietary, closed-source component and is NOT redistributed by this package. This
# module only *locates* it. Search order:
#   1. ASI_SDK_ROOT (CMake variable or environment variable)
#   2. the usual system locations (/usr/local, /usr on UNIX)
#
# Unlike the Thorlabs Kinesis SDK, the ASI SDK is shipped by the vendor for Windows, Linux, macOS and Android, so this
# module is deliberately platform-neutral: it resolves whichever architecture/platform library subdirectory matches the
# target, and does not restrict the toolchain. The vendored tree keeps the vendor's own original layout
# (include/ + lib/<arch>/) so the SDK can be updated by dropping in a new release unmodified.
#
# Result variables: ASICamera_FOUND, ASICamera_INCLUDE_DIR, ASICamera_LIBRARY, ASICamera_RUNTIME_DIR.

if(TARGET ZWO::ASICamera)
    set(ASICamera_FOUND TRUE)
    return()
endif()

set(_asi_root "${ASI_SDK_ROOT}")
if(NOT _asi_root AND DEFINED ENV{ASI_SDK_ROOT})
    set(_asi_root "$ENV{ASI_SDK_ROOT}")
endif()

set(_asi_hints "${_asi_root}" /usr/local /usr)

# Headers: the SDK ships ASICamera2.h under include/ (vendor layout) or directly (system install).
find_path(ASICamera_INCLUDE_DIR
    NAMES ASICamera2.h
    HINTS ${_asi_hints}
    PATH_SUFFIXES include includes include/ASICamera2 "")

# Library subdirectory for the target platform/architecture, following the vendor's own tree layout:
#   Windows : lib/x64, lib/x86
#   Linux   : lib/x64, lib/x86, lib/armv6, lib/armv7, lib/armv8, lib/mac
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_asi_arch_suffixes lib/x64 lib/armv8 lib/mac lib)
else()
    set(_asi_arch_suffixes lib/x86 lib/armv7 lib/armv6 lib)
endif()

# On Windows the SDK ships an MSVC import library (.lib). GNU ld links it directly for the extern "C" ASI API, but a
# GNU toolchain does not search .lib by default, so add the suffix explicitly.
set(_asi_saved_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
if(WIN32)
    list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES ".lib")
endif()

find_library(ASICamera_LIBRARY
    NAMES ASICamera2
    HINTS ${_asi_hints}
    PATH_SUFFIXES ${_asi_arch_suffixes} "")

set(CMAKE_FIND_LIBRARY_SUFFIXES "${_asi_saved_suffixes}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ASICamera
    REQUIRED_VARS
        ASICamera_INCLUDE_DIR
        ASICamera_LIBRARY)

if(ASICamera_FOUND)
    # The vendored Windows tree keeps ASICamera2.dll next to the import library; expose that directory so the build can
    # stage the runtime next to the binaries. On UNIX the shared object *is* the found library.
    get_filename_component(ASICamera_RUNTIME_DIR "${ASICamera_LIBRARY}" DIRECTORY)

    add_library(ZWO::ASICamera INTERFACE IMPORTED)
    set_target_properties(ZWO::ASICamera PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ASICamera_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${ASICamera_LIBRARY}")

    # The vendor header decorates every entry point with __declspec(dllexport) when _WINDOWS is defined, which is wrong
    # for a *consumer*. Leaving _WINDOWS undefined yields plain extern "C" declarations that link against the import
    # library, so nothing extra is required here -- recorded so a future reader does not "helpfully" define it.
endif()

mark_as_advanced(ASICamera_INCLUDE_DIR ASICamera_LIBRARY)
