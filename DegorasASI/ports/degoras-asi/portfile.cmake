# Overlay port for DegorasASI.
#
# The public repository is not published yet, so REF/SHA512 below are placeholders. Until then, build against a local
# checkout using the overlay instructions in docs/PACKAGING.md (SOURCE_PATH can be pointed at the working tree). Once
# the repo is public, set REF to the release tag and SHA512 to the archive hash (vcpkg prints the expected value on
# the first configure) and this port works unchanged.
#
# The library vendors the proprietary ZWO ASI Camera SDK (thirdparty/ASI), so this is an OVERLAY / private-registry
# port, never an official curated one.
#
# The "supports" expression in vcpkg.json is windows & x64 because that is the vendored SDK payload and the validated
# toolchain, NOT a limitation of the source: the ASI SDK is shipped by the vendor for Windows, Linux, macOS and Android,
# and the library imposes no platform lock. Widen it once another platform's SDK is vendored and its build is validated.
#
# Linkage: because the public API passes C++ standard-library types across the boundary, the library and its consumers
# must be built with the same toolchain and standard library. Use the MinGW triplets in cmake/triplets/ to build with the
# project's reference toolchain, or an MSVC triplet if the whole stack is MSVC.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO DegorasProjectTeam/DegorasASI
    REF "v${VERSION}"
    SHA512 0   # TODO(publish): replace with the real archive SHA512.
    HEAD_REF main
)

# The CMake project root is the INNER directory of the repository.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/DegorasASI"
    OPTIONS
        -DDEGORASASI_INSTALL=ON
        -DDEGORASASI_BUILD_TESTING=OFF
        -DDEGORASASI_BUILD_EXAMPLES=OFF
        -DDEGORASASI_INSTALL_ASI_RUNTIME=ON
)

vcpkg_cmake_install()

# Relocate the CMake package files from lib/cmake/DegorasASI to share/degorasasi (vcpkg convention).
vcpkg_cmake_config_fixup(PACKAGE_NAME degorasasi CONFIG_PATH lib/cmake/DegorasASI)

# vcpkg forbids a debug/include or debug/share tree, and owns copyright placement.
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share")

# Ship the usage note and the license as the port copyright.
configure_file(
    "${SOURCE_PATH}/DegorasASI/cmake/usage"
    "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage"
    COPYONLY)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
