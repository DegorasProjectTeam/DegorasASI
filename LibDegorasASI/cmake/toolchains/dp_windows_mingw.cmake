#
# Windows toolchain for MSYS2 MinGW prefixes.
# Requires environment variable:
#   MINGW_ROOT  -> the MSYS2 prefix. The DegorasSLR environment exports it (${MSYS2_ROOT}/${MSYS2_ENV});
#                  elsewhere set it yourself to a ucrt64, mingw64 or clang64 prefix.
#
# This is the project's REFERENCE (validated) Windows toolchain, not a requirement: LibDegorasASI itself imposes no
# platform or toolchain lock, because the ZWO ASI Camera SDK is shipped for Windows, Linux, macOS and Android.
#

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED ENV{MINGW_ROOT} OR "$ENV{MINGW_ROOT}" STREQUAL "")
    message(FATAL_ERROR "[CMAKE] dp_windows_mingw.cmake: MINGW_ROOT is not set. It names the MSYS2 prefix "
                        "holding gcc; the DegorasSLR environment exports it. Enter that environment, or set it.")
endif()

set(_PFX "$ENV{MINGW_ROOT}")

# Compilers.
set(CMAKE_C_COMPILER   "${_PFX}/bin/gcc.exe" CACHE FILEPATH "MSYS2 GCC (C)"   FORCE)
set(CMAKE_CXX_COMPILER "${_PFX}/bin/g++.exe" CACHE FILEPATH "MSYS2 G++ (C++)" FORCE)

# Resource compiler (optional, but common for Windows builds).
if(EXISTS "${_PFX}/bin/windres.exe")
    set(CMAKE_RC_COMPILER "${_PFX}/bin/windres.exe" CACHE FILEPATH "MSYS2 windres" FORCE)
endif()

# Prefer Ninja from the same prefix if available.
if(EXISTS "${_PFX}/bin/ninja.exe")
    set(CMAKE_MAKE_PROGRAM "${_PFX}/bin/ninja.exe" CACHE FILEPATH "Ninja from MSYS2 prefix" FORCE)
endif()

# Prefer config packages, and ensure the prefix is searched first.
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON CACHE BOOL "" FORCE)
list(PREPEND CMAKE_PREFIX_PATH "${_PFX}")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)
