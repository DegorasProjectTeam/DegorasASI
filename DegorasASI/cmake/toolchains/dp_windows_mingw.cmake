#
# Windows toolchain for MSYS2 prefixes.
# Requires environment variable:
#   DEVSYSTEM_TOOLCHAIN_ROOT -> the MSYS2 prefix. The DegorasSLR environment exports it; elsewhere set it
#                               yourself to a ucrt64, mingw64 or clang64 prefix.
# Honoured when present:
#   DEVSYSTEM_TOOLCHAIN                -> "gcc" or "clang"; picks the compiler driver. See below.
#   VCPKG_ROOT + VCPKG_DEFAULT_TRIPLET -> the package prefix is searched AHEAD of the toolchain one. See below.
#
# This is the project's REFERENCE (validated) Windows toolchain, not a requirement: DegorasASI itself imposes no
# platform or toolchain lock, because the ZWO ASI Camera SDK is shipped for Windows, Linux, macOS and Android.
#

# CMAKE_SYSTEM_NAME is deliberately NOT set. MinGW-on-Windows is a NATIVE build, and declaring the system name turns
# CMAKE_CROSSCOMPILING on regardless, which changes how other packages behave -- Qt, for one, then looks only for the
# .bat tool wrappers a real cross-build would ship and never for the .exe that is actually installed. Leaving it unset
# also lets CMake detect CMAKE_SYSTEM_PROCESSOR by itself, which it gets right (x86_64).

if(NOT DEFINED ENV{DEVSYSTEM_TOOLCHAIN_ROOT} OR "$ENV{DEVSYSTEM_TOOLCHAIN_ROOT}" STREQUAL "")
    message(FATAL_ERROR "[CMAKE] dp_windows_mingw.cmake: DEVSYSTEM_TOOLCHAIN_ROOT is not set. It names the "
                        "MSYS2 prefix holding the compiler; the DegorasSLR environment exports it. Enter that "
                        "environment, or set it. It replaced MINGW_ROOT, which named the same directory, so an "
                        "environment generated before the rename wants step 2 of DrivEnv-Win re-run.")
endif()

set(_PFX "$ENV{DEVSYSTEM_TOOLCHAIN_ROOT}")

# COMPILER DRIVER. gcc.exe and g++.exe are right on ucrt64 and mingw64, and they also WORK on clang64, where
# MSYS2 ships them as byte-identical copies of clang -- which is why this named them unconditionally. Those
# copies are exactly what makes the family undiscoverable, though: nothing about the prefix says which compiler
# you are getting, and a configure log reading "GNU" for a clang build is a lie that costs an afternoon. So when
# the environment states the family, believe it and invoke the real driver. Unstated, the old behaviour stands.
set(_CC  "gcc.exe")
set(_CXX "g++.exe")
if("$ENV{DEVSYSTEM_TOOLCHAIN}" STREQUAL "clang" AND EXISTS "${_PFX}/bin/clang.exe")
    set(_CC  "clang.exe")
    set(_CXX "clang++.exe")
endif()

set(CMAKE_C_COMPILER   "${_PFX}/bin/${_CC}"  CACHE FILEPATH "MSYS2 C compiler"   FORCE)
set(CMAKE_CXX_COMPILER "${_PFX}/bin/${_CXX}" CACHE FILEPATH "MSYS2 C++ compiler" FORCE)

# Resource compiler (optional, but common for Windows builds).
if(EXISTS "${_PFX}/bin/windres.exe")
    set(CMAKE_RC_COMPILER "${_PFX}/bin/windres.exe" CACHE FILEPATH "MSYS2 windres" FORCE)
endif()

# Prefer Ninja from the same prefix if available.
if(EXISTS "${_PFX}/bin/ninja.exe")
    set(CMAKE_MAKE_PROGRAM "${_PFX}/bin/ninja.exe" CACHE FILEPATH "Ninja from MSYS2 prefix" FORCE)
endif()

set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON CACHE BOOL "" FORCE)

# ----------------------------------------------------------------------------------------------------------------------
# SEARCH ORDER: PACKAGES BEFORE THE COMPILER PREFIX
#
# This file used to do `list(PREPEND CMAKE_PREFIX_PATH "${_PFX}")`, which put the toolchain prefix ahead of it all.
# That was wrong, and it broke find_package(OpenCV) outright. The toolchain prefix supplies the COMPILER; the packages
# this project consumes come from vcpkg, and when a package exists in both prefixes the two are NOT interchangeable.
#
# The failure it caused, measured rather than guessed: MSYS2 ships a Qt6 (Qt Creator pulls it in) that provides every
# component OpenCV asks for EXCEPT Core5Compat. vcpkg's OpenCV runs find_dependency(Qt6 COMPONENTS ... Core5Compat)
# from inside OpenCVModules.cmake because its highgui is built against Qt. With the MinGW prefix searched first, that
# resolved to MSYS2's own Qt6, failed on the missing component, and -- as the failure lands inside OpenCV's own
# cmake_policy(PUSH) block -- surfaced as the thoroughly misleading:
#
#     CMake Error in .../OpenCVModules.cmake: cmake_policy PUSH without matching POP
#
# Note that CMAKE_PREFIX_PATH is searched BEFORE the prefixes CMake derives from PATH entries ending in /bin, which is
# how the vcpkg prefix was being found at all. Merely appending the toolchain prefix instead of prepending it therefore
# fixes nothing: the vcpkg prefix has to be named here, explicitly, and first.
if(DEFINED ENV{VCPKG_ROOT} AND DEFINED ENV{VCPKG_DEFAULT_TRIPLET}
   AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "" AND NOT "$ENV{VCPKG_DEFAULT_TRIPLET}" STREQUAL "")
    set(_VCPKG_PFX "$ENV{VCPKG_ROOT}/installed/$ENV{VCPKG_DEFAULT_TRIPLET}")
    if(IS_DIRECTORY "${_VCPKG_PFX}")
        list(APPEND CMAKE_PREFIX_PATH "${_VCPKG_PFX}")
    endif()
    unset(_VCPKG_PFX)
endif()

# The compiler prefix last, as a fallback for anything vcpkg does not carry.
list(APPEND CMAKE_PREFIX_PATH "${_PFX}")

# ...and, when CMake is new enough to support it, not even as a fallback for PACKAGES.
#
# Ordering alone is fragile: it only has to be got wrong once, anywhere -- a stray list(PREPEND) in a preset, a
# find_package(HINTS), a dependency's own config -- and MSYS2's copy of a package silently wins again. Measured:
# with the prefix order deliberately broken, neither an environment CMAKE_PREFIX_PATH nor even an environment
# Qt6_DIR pointing at vcpkg rescues it; CMAKE_IGNORE_PREFIX_PATH is the only lever that still does.
#
# So state the policy instead of relying on an order: MSYS2 supplies the COMPILER and its runtime, vcpkg supplies
# the PACKAGES, and the two are never mixed. The compiler, its runtime libraries and the linker's default search
# paths are unaffected -- this touches find_package/find_library prefix search only.
#
# THE TRADE-OFF, stated plainly: a package that exists ONLY in the MSYS2 prefix becomes unfindable, and the error
# will be a plain "Could NOT find <X>" with no hint that it was deliberately hidden. If that happens and the
# package genuinely has no vcpkg port, delete this block -- do not "fix" it by re-prepending the prefix above.
# Verified with the current dependency set: the library, all examples and the whole test suite configure and build
# clean, and SDL2 is still found (from vcpkg).
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.23")
    list(APPEND CMAKE_IGNORE_PREFIX_PATH "${_PFX}")
    list(REMOVE_DUPLICATES CMAKE_IGNORE_PREFIX_PATH)
endif()

# The toolchain file is re-read for every try_compile, and CMAKE_PREFIX_PATH is inherited into those, so without this
# the list grows a duplicate of each entry per pass. Cosmetic, but it makes the configure output unreadable.
list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)
