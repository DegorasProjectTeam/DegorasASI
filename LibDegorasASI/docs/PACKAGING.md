# Packaging, Install & Deployment

LibDegorasASI installs a single prefix-relative layout (via `GNUInstallDirs`) that serves **both** a standard
standalone install and a **vcpkg** overlay port. This document covers both.

> **Platform:** the library imposes **no platform or toolchain lock**. The ZWO ASI Camera SDK is shipped by the vendor
> for Windows, Linux, macOS and Android, and `cmake/FindASICamera.cmake` resolves whichever platform/architecture
> payload is present. **MinGW/UCRT64 on Windows x64 is the reference (validated) toolchain**; other platforms build as
> soon as the matching vendor SDK is vendored or pointed at with `ASI_SDK_ROOT`.
>
> **ABI:** the public API passes C++ standard-library types (`std::string`, `std::vector`, `std::function`,
> `std::chrono`) across the library boundary, so a **shared** build and its consumers must use one compatible toolchain
> and standard library. This constrains **linkage, not platform**: mixing a MinGW-built DLL with an MSVC consumer will
> not work, but an all-MSVC or all-GCC stack is fine.

---

## 1. Standalone install (no vcpkg)

Configure, build and install with the provided presets. They read three variables the DegorasSLR environment
exports — `MINGW_ROOT`, `DEVSYSTEM_BUILDTREES` and `DEVSYSTEM_DEPLOYS` — and are reported as disabled if any is
missing, so a build never lands somewhere unintended:

```sh
cd LibDegorasASI
cmake --preset mingw-dynamic-rel
cmake --build --preset mingw-dynamic-rel
cmake --install "$DEVSYSTEM_BUILDTREES/LibDegorasASI/mingw-dynamic-rel"
```

The preset already sets the install prefix to `$DEVSYSTEM_DEPLOYS/LibDegorasASI/<preset>/`, so no `--prefix` is
needed; pass one to install elsewhere. The prefix is per preset on purpose — the library defines no debug
postfix, so a Debug and a Release install sharing a prefix would overwrite each other.

Installed layout (relative to the prefix):

| Path | Contents |
|------|----------|
| `bin/` | `LibDegorasASI.dll` and (opt-out) the vendored ZWO `ASICamera2` runtime |
| `lib/` | import/static library |
| `include/LibDegorasASI/` | public headers, including the extensionless `Modules/` aggregators |
| `lib/cmake/DegorasASI/` | `DegorasASIConfig.cmake`, `…ConfigVersion.cmake`, `DegorasASITargets*.cmake`, `FindASICamera.cmake` |
| `share/DegorasASI/` | `copyright`, `usage` |

### Install options

| Option | Default | Effect |
|--------|---------|--------|
| `LIBDEGORASASI_INSTALL` | ON if top-level | Master switch for all install/export rules |
| `LIBDEGORASASI_BUILD_SHARED` | follows `BUILD_SHARED_LIBS`, else ON | Shared vs static |
| `LIBDEGORASASI_INSTALL_ASI_RUNTIME` | ON | Install the vendored ZWO runtime library into `bin/` |
| `LIBDEGORASASI_BUILD_DOCS` | OFF | Install `README.md` into the doc dir |
| `LIBDEGORASASI_INSTALL_EXAMPLES` | OFF | Install the example sources |
| `LIBDEGORASASI_BUILD_TESTING` | ON | Build tests and register them with CTest |
| `LIBDEGORASASI_BUILD_EXAMPLES` | ON | Build the example executables |

Locating the SDK: `ASI_SDK_ROOT` (CMake or environment variable) overrides the vendored
`thirdparty/ASI`, so a packager can build against a system install instead.

Run the tests with CTest from the build tree: `ctest --output-on-failure`. The `UT_*` tests need no hardware; the
`Test_*` tests exercise a real camera and **self-skip with success** when none is attached, so the suite is safe to run
unattended. Any `Monitor` or `Hardware` named test is intentionally not registered.

## 2. Consuming the installed package

```cmake
find_package(DegorasASI CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE Degoras::ASI)
```

Point CMake at the prefix with `-DCMAKE_PREFIX_PATH=/path/to/prefix` and use a compatible toolchain. A **shared**
consumer needs nothing else at configure time — the ZWO dependency is sealed inside the library, so a consumer never
sees `ASICamera2.h` and does not need the SDK to build. A **static** consumer additionally re-resolves the SDK through
`find_dependency(ASICamera)`; set `ASI_SDK_ROOT` if it is not auto-detected. At run time the ZWO runtime library must be
discoverable (installed alongside, on `PATH` / `LD_LIBRARY_PATH`, or via the ZWO driver package).

## 3. vcpkg (overlay / private registry)

Because the ZWO ASI SDK is proprietary and vendored, this is an **overlay / private-registry** port, never an official
curated one. Files are provided:

- `cmake/triplets/x64-mingw-dynamic.cmake` and `x64-mingw-static.cmake` — chainload the MinGW toolchain.
- `ports/libdegoras-asi/{vcpkg.json,portfile.cmake}` — the port.

### Manifest or classic mode

```sh
# classic mode
vcpkg install libdegoras-asi \
    --overlay-ports=LibDegorasASI/ports \
    --overlay-triplets=LibDegorasASI/cmake/triplets \
    --triplet x64-mingw-dynamic
```

```json
// manifest mode (vcpkg.json in your project), then `vcpkg install`
{ "dependencies": ["libdegoras-asi"] }
```
…invoked with the same `--overlay-ports` / `--overlay-triplets` (or configured in `vcpkg-configuration.json`), and
`MINGW_ROOT` exported in the environment.

The port runs `vcpkg_cmake_configure` → `vcpkg_cmake_install` → `vcpkg_cmake_config_fixup(PACKAGE_NAME degorasasi
CONFIG_PATH lib/cmake/DegorasASI)`, which relocates the CMake package files to `share/degorasasi/` and merges the
debug/release trees — producing the standard vcpkg installed layout (`bin`, `lib`, `include`, `share`, `debug/bin`,
`debug/lib`, …). Consumers then use the exact same `find_package(DegorasASI)` / `Degoras::ASI`.

The port's `"supports": "windows & x64"` reflects the **vendored payload and the validated toolchain**, not a limit of
the source. Widen it once another platform's vendor SDK is vendored and that build is validated.

### Publishing note

`portfile.cmake` uses `vcpkg_from_github` with a placeholder `REF`/`SHA512`. When the repository is published, set
`REF` to the release tag and `SHA512` to the archive hash (vcpkg prints the expected value on the first configure);
no other change is needed. Until then, point the port at a local checkout for testing.

### Why not an official vcpkg port?

The curated registry forbids vendored/prebuilt proprietary binaries and requires from-source, redistributable,
CI-built dependencies. The ZWO ASI Camera SDK ships as closed-source prebuilt binaries, so an official port is not
feasible; an overlay or private registry is the supported route. (The SDK's own `license.txt` is permissive, so
redistributing its runtime alongside the package is defensible — but the library itself is GPL-3.0-or-later, and every
source file carries the matching `SPDX-License-Identifier`.)

## 4. Vendored SDK layout

`thirdparty/` holds **only** the vendor SDK, in a folder named after the vendor family, mirroring how the reference
project vendors `thirdparty/Thorlabs/`. Within it, the vendor's own subdirectory shape is preserved so a new SDK release
can be dropped in unmodified:

| Path | Contents |
|------|----------|
| `thirdparty/ASI/include/ASICamera2.h` | the entire C API |
| `thirdparty/ASI/lib/x64/`, `lib/x86/` | import library + runtime, per architecture |
| `thirdparty/ASI/license.txt`, `Version_<x>` | vendor licence and the vendored SDK version marker |

**Building on Linux or macOS.** ZWO publishes a separate *ASI Camera SDK [Linux & macOS]* package, which lays its
payload out the same way — `lib/<arch>/libASICamera2.so` for `x64`, `armv6`, `armv7`, `armv8` and `mac`. Drop that
`lib/<arch>/` directory into the vendored SDK root, or point `ASI_SDK_ROOT` at a system install (Debian and Ubuntu
package it as `libasicamera2`). `FindASICamera.cmake` already selects by architecture, and the `unix-dynamic-*`
presets then configure unchanged; only the vendored payload is Windows-specific, never the source. Note the vendored
tree here carries the Windows payload only, so a Unix build needs that step first — the Find module says as much if
the library for the host architecture is missing.

Everything else the vendor ships — its PDF documentation and its MFC/OpenCV sample application — lives at the
**repository root** under `reference/`, outside the CMake project and gitignored. It is kept for consultation but is not
part of the build, not redistributed, and deliberately not tracked: the sample bundles OpenCV binaries this library does
not depend on. Re-populate `reference/` from the official SDK download when needed.

`FindASICamera.cmake` selects the subdirectory matching the target architecture, adds `.lib` to the searched suffixes
under Windows so a GNU toolchain finds the MSVC-style import library, and exposes the runtime directory so the build can
stage the DLL next to the binaries.

> The vendor header decorates every entry point with `__declspec(dllexport)` when `_WINDOWS` is defined, which is wrong
> for a *consumer*. Do **not** define `_WINDOWS`: leaving it undefined yields plain declarations that link correctly
> against the import library.
