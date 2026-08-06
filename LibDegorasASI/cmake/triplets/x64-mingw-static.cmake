# vcpkg community-style triplet: 64-bit MinGW, static library linkage (dynamic CRT).
#
# See x64-mingw-dynamic.cmake for the rationale. NOTE: with static linkage the ZWO dependency is NOT sealed into a DLL,
# so a consumer's final link must resolve the ASI import library (set ASI_SDK_ROOT, or provide an asi-sdk port). The
# dynamic triplet is the recommended path for this library.

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME "")
set(VCPKG_ENV_PASSTHROUGH MINGW_ROOT)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/dp_windows_mingw.cmake")

set(VCPKG_POLICY_DLLS_WITHOUT_LIBS enabled)
