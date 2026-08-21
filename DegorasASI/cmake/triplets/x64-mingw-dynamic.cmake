# vcpkg community-style triplet: 64-bit MinGW, dynamic linkage.
#
# This library's public API passes C++ standard-library types across the boundary, so it must be built with the same
# toolchain and standard library as its consumers. This triplet chainloads the project's MinGW toolchain file, which is
# the project's reference (validated) Windows toolchain. Requires the MINGW_ROOT environment variable (e.g.
# the MSYS2 prefix, exported by the DegorasSLR environment); it is passed through to the build.
#
# Use with: --overlay-triplets=<this dir> --triplet x64-mingw-dynamic   (see docs/PACKAGING.md)

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME "")   # native Windows (not a cross build)
set(VCPKG_ENV_PASSTHROUGH MINGW_ROOT)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/dp_windows_mingw.cmake")

# The vendored ZWO SDK ships its DLL with an MSVC-style import lib rather than a MinGW one; allow it in the package.
set(VCPKG_POLICY_DLLS_WITHOUT_LIBS enabled)
