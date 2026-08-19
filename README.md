<a id="readme-top"></a>

<!-- PROJECT SHIELDS -->
[![C++17][cpp-shield]][cpp-url]
[![CMake][cmake-shield]][cmake-url]
[![License: GPL v3][license-shield]][license-url]
[![Platform][platform-shield]][platform-url]

<!-- PROJECT TITLE -->
<div align="center">
  <h1 align="center">LibDegorasASI</h1>

  <p align="center">
    An extensible C++17 library for controlling ZWO ASI astronomy cameras.
    <br />
    <a href="#about-the-project"><strong>Explore the docs »</strong></a>
    <br />
    <br />
    <a href="#usage">View Usage</a>
    &middot;
    <a href="https://github.com/DegorasProjectTeam/LibDegorasASI/issues">Report Bug</a>
    &middot;
    <a href="https://github.com/DegorasProjectTeam/LibDegorasASI/issues">Request Feature</a>
  </p>
</div>

<!-- TABLE OF CONTENTS -->
<details>
  <summary>Table of Contents</summary>
  <ol>
    <li>
      <a href="#about-the-project">About The Project</a>
      <ul>
        <li><a href="#architecture">Architecture</a></li>
        <li><a href="#built-with">Built With</a></li>
      </ul>
    </li>
    <li>
      <a href="#getting-started">Getting Started</a>
      <ul>
        <li><a href="#prerequisites">Prerequisites</a></li>
        <li><a href="#build">Build</a></li>
      </ul>
    </li>
    <li><a href="#usage">Usage</a></li>
    <li><a href="#sdk-behaviour-worth-knowing">SDK Behaviour Worth Knowing</a></li>
    <li><a href="#testing">Testing</a></li>
    <li><a href="#roadmap">Roadmap</a></li>
    <li><a href="#license">License</a></li>
    <li><a href="#contact">Contact</a></li>
    <li><a href="#acknowledgments">Acknowledgments</a></li>
  </ol>
</details>

<!-- ABOUT THE PROJECT -->
## About The Project

LibDegorasASI is a production-quality, extensible C++17 library that wraps the official ZWO ASI Camera SDK (a C API)
in a modern, strongly-typed C++ interface. It is built so that additional ASI camera models join without
architectural changes.

It ships **one driver class for every ASI model**, `AsiCamera`, with capability-driven controls, deterministic
resource ownership, a thread-safe per-camera locking model, an explicit error model, and asynchronous
telemetry-polling and frame-delivery callbacks.

The library is deliberately free of any GUI, Qt or OpenCV dependency: it is the low-level backend that higher-level
visualisation software builds on.

### Architecture

The library is organised in layers. The layer-boundary rule: the vendor SDK header is included **only** inside the
`ASI/` layer, so the driver and everything above it are written against this library's own vocabulary and never
against the vendor's.

| Layer | Folders | Knows about | Examples |
|-------|---------|-------------|----------|
| 1 — generic infrastructure | `Global/`, `Common/`, `Helpers/` | nothing vendor-specific | export macro; `OperationResult`, `DeviceError`, camera vocabulary, `Frame`, image geometry; `StatusPoller<StatusT>`, `FramePump`, `waitForCondition`, frame writers, JSON helpers |
| 2 — ZWO ASI vendor layer | `ASI/` | the ASICamera2 C API | `categoryFromAsi`; the discovery/per-camera/acquisition lock scopes; `enumerateCameras`; the camera-ownership registry; `AsiCameraController` |
| 3 — camera driver | `Devices/` | nothing model-specific | `AsiCamera`, one class for every ASI model |

Within Layer 1: `Global/` holds the export macro, `Common/` the camera vocabulary and result/error model, and
`Helpers/` the generic infrastructure. Headers are consumed individually as `#include "LibDegorasASI/<Folder>/<file>.h"`,
or a whole group via a module aggregator: `#include <LibDegorasASI/Modules/Devices>` (also `Common`, `Helpers`, `ASI`).

> **On one class for every model.** The reference sibling project has a class per device, and this library
> deliberately does not. Its vendor SDK exposes a **different C API per module** (`BDC_*` versus `ISC_*`) and its
> devices differ structurally — one axis versus two — so a per-device class there composes different adapters and has
> something real to model. ZWO gives **every camera the same C API**: an ASI224MC and an ASI2600MM differ only in DATA
> the SDK reports at run time (which controls exist; the descriptor's colour/cooled/trigger flags). A class per model
> would duplicate identical logic behind different names, so supporting another model here costs **no library code at
> all**. Ask the camera what it can do — `hasControl`, `hasCooler`, `isColour`, `supportsFormat` — and act on the answer.
>
> **On the single vendor layer.** The reference splits its vendor code into a shared *family* layer plus one adapter per
> SDK module. ASICamera2 is a single module, so a second folder would buy nothing today. The module-agnostic pieces
> (`asi_error`, `asi_api_lock`, `asi_discovery`, `asi_camera_registry`) are nevertheless kept as *separate files* from
> the adapter (`asi_camera_controller`), so if ZWO's EFW/EAF SDKs are ever added they already form the family layer and
> only need moving.

### Built With

* C++17
* CMake (>= 3.21) and Ninja, driven by CMake Presets
* MSYS2 MinGW (UCRT64) toolchain — GCC — as the reference toolchain (the library carries no platform lock)
* ZWO ASI Camera SDK, vendored under `thirdparty/ASI/`

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- GETTING STARTED -->
## Getting Started

### Prerequisites

* An MSYS2 MinGW prefix providing GCC, Ninja and (optionally) windres. UCRT64 is the reference prefix.
* CMake >= 3.21.
* The `MINGW_ROOT` environment variable pointing at the prefix (e.g. `E:/msys64/ucrt64`). The provided
  `CMakeUserPresets.json` sets this for the local machine — adjust it for yours.
* A ZWO ASI camera and its driver installed, to run anything that touches hardware.

### Build

The CMake project root is the inner `LibDegorasASI/` directory. Configure and build with a preset:

```sh
cd LibDegorasASI
cmake --preset local-mingw-dynamic-deb
cmake --build --preset local-mingw-dynamic-deb
```

Project presets in `CMakePresets.json` cover `mingw-{dynamic,static}-{deb,rel}` plus `unix-dynamic-{deb,rel}` for a
non-Windows host; the `local-*` user presets just add your `MINGW_ROOT`. Build options:
`LIBDEGORASASI_BUILD_SHARED` (default ON), `LIBDEGORASASI_BUILD_TESTING`, `LIBDEGORASASI_BUILD_EXAMPLES`.

Artifacts land in `build/<preset>/bin/` at the repository root. The build stages, next to the binaries, both the
vendored ZWO runtime and the MinGW C++ runtime (`libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`), so
the executables run without the toolchain on `PATH`.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- USAGE -->
## Usage

```cpp
#include <LibDegorasASI/Modules/Devices>   // AsiCamera, Frame, CameraStatus

using namespace dpasi;
using dpasi::types::OperationResult;

int main()
{
    types::CameraDescriptorList cameras;
    if (AsiCamera::getDeviceList(cameras) != OperationResult::OPERATION_OK || cameras.empty())
        return 1;

    AsiCamera camera(cameras.front().id);       // any ASI model; no device I/O in the constructor
    if (camera.doConnect() != OperationResult::OPERATION_OK)
        return 1;

    types::RoiFormat roi;
    roi.width = 640;                            // multiple of 8
    roi.height = 480;                           // multiple of 2
    roi.bin = 1;
    roi.format = types::ImageFormat::RAW8;
    camera.doSetRoi(roi, types::RoiPosition()); // geometry and origin together: the format change recentres the origin

    camera.doSetExposure(std::chrono::milliseconds(20));

    // One still frame.
    types::Frame still;
    camera.doCaptureSingleFrame(still, std::chrono::seconds(5));

    // Or stream. Reuse one Frame: its buffer is reallocated only when the geometry changes.
    camera.doStartVideoCapture();
    types::Frame frame;
    for (int i = 0; i < 100; ++i)
        if (camera.doGetVideoFrame(frame, std::chrono::milliseconds(500)) == OperationResult::OPERATION_OK)
            consume(frame);                     // frame.data is width * height * bytesPerPixel(format)
    camera.doStopVideoCapture();

    camera.doDisconnect();                      // also handled by the destructor
}
```

Controls are **capability-driven**: which controls a camera exposes varies by model and must be discovered, never
assumed. Ask for one the camera does not have and you get `OperationResult::UNSUPPORTED_CAPABILITY`, not a fabricated
value — the ASI224MC, for instance, exposes 14 of the SDK's controls and has **no gamma control at all**. That is what
lets one class serve every model: query first, then act.

```cpp
if (camera.hasCooler())                          // an ASI2600MM answers yes, an ASI224MC no
    camera.doSetCoolerTarget(-10);               // whole degrees; refused with UNSUPPORTED_CAPABILITY otherwise
if (camera.isColour())
    camera.doSetControl(types::ControlType::WB_RED, wb);
if (camera.supportsFormat(types::ImageFormat::RAW16))
    camera.doSetFullFrameRoi(types::ImageFormat::RAW16, 1);
```

### Seeing the image

The library draws nothing and owns no image pipeline — it stays free of GUI toolkits, Qt and OpenCV. What it does
own is **what the vendor never documented about its own pixel data**, because a consumer forced to rediscover that
gets silently wrong images until it does. `Helpers/frame_writer.h` is therefore part of the library:

```cpp
imgio::writeFrame(frame, "shot.bmp");        // BMP for RGB24, PGM for RAW8 / RAW16 / Y8
std::cout << imgio::framePreview(frame, 72); // an ASCII brightness map, returned as a string
```

Nothing there needs more than `<fstream>`. `asi_camera_view` is the example to run first on new hardware: it prints
the brightness map, so *"is the camera seeing anything?"* is answered before any file is opened, then saves a
full-sensor frame in every format the camera supports. `asi_camera_shoot` is the smallest useful capture tool —
`--exposure`, `--gain`, `--format`, `--bin` — validated against the limits *this* camera reports rather than any baked
in, and the seed of a capture button in a real application.

Two properties of the pixel data make that nearly free, neither documented by the vendor and both measured here:

| Format | Finding | Consequence |
|--------|---------|-------------|
| `RGB24` | Channel order is **B, G, R** (confirmed by driving `WB_RED` / `WB_BLUE` to opposite extremes and watching which byte followed) | Identical to what BMP stores, so writing a BMP is a row-by-row copy with no channel swap |
| `RAW16` | Already spans the **full 16-bit range** on a 12-bit sensor — minimum step 1, values to 65534 — so the SDK scales rather than shifts | Consume it directly; no `>> 4` and no left-shift |

For a live view, hand the frame straight to your toolkit. With Qt nothing needs converting, because `Format_BGR888`
is exactly the ASI RGB24 layout:

```cpp
QImage(frame.data.data(), frame.width, frame.height, QImage::Format_BGR888);       // RGB24
QImage(frame.data.data(), frame.width, frame.height, QImage::Format_Grayscale8);   // RAW8 / Y8
QImage(frame.data.data(), frame.width, frame.height, QImage::Format_Grayscale16);  // RAW16
```

> [!WARNING]
> `RAW8` and `RAW16` from a colour camera are **Bayer-mosaiced**, not grey pictures: displayed as-is they look like a
> fine checkerboard. Ask for `RGB24` when a viewable colour image is what you want — the SDK demosaics that one for
> you. The raw formats are for processing.
>
> A Qt application must also be built with the **same toolchain** as the library: the public API passes C++
> standard-library types across the boundary, so a MinGW/UCRT64 build of this library cannot be linked into a Qt kit
> built with a different GCC or C runtime. Build the library with your Qt kit's toolchain — it carries no platform
> lock, so only the preset changes.

### USB link and traffic

The USB link is the usual cause of dropped frames, so it is first-class. A USB3 camera in a USB2 port works but
delivers a fraction of its rated frame rate, and **nothing else in the system tells you** — the vendor's own capture
software puts it in its title bar for that reason:

```cpp
camera.getUsbLinkSpeed();        // UsbLinkSpeed::USB2 or ::USB3 -- the NEGOTIATED link, not the camera's rating
camera.isLinkFullSpeed();        // false for a USB3 camera on a USB2 host

camera.doSetUsbBandwidth(80);           // share of the bus this camera may occupy; LOWER it if frames drop
camera.doSetUsbBandwidth(80, true);     // or let the camera manage it, like the vendor software's "Auto" box
int percent = 0; bool automatic = false;
camera.getUsbBandwidth(percent, automatic);

camera.doSetHighSpeedMode(true);        // a different lever: bit depth traded for frame rate
camera.getDroppedFrames(dropped);       // read before stopping capture
```

Slider bounds come from the same place as every other control —
`getControlCaps(ControlType::BANDWIDTH_OVERLOAD, caps)` yields this camera's min, max, default and auto-capability, so
a user interface binds every control through one uniform path.

```cpp
types::ControlCapsList caps;
camera.getControlCaps(caps);                    // discovered once at connect

types::ControlValue gain;
gain.value = 200;
camera.doSetControl(types::ControlType::GAIN, gain);   // range-checked against the discovered limits

double celsius = 0;
camera.getSensorTemperature(celsius);           // unscaled from the SDK's tenths of a degree
```

### Frames by callback, instead of owning the loop

The loop above is one of **two** ways in. The other is to register a callback and let the library run the acquisition
loop on a worker thread, so the application never blocks a thread of its own on a capture:

```cpp
camera.setNewFrameCb([&](OperationResult res, const types::Frame& frame)
{
    if (res != OperationResult::OPERATION_OK)
        return;                                  // a timeout or a lost camera is delivered, not swallowed
    queue.push(frame);                           // COPY: the frame reference dies when this returns
});

camera.doStartVideoCapture();                    // the worker pumps an existing stream; it never starts one
camera.startFrameAcquisition(std::chrono::milliseconds(1000));
// ... the application does its own work ...
camera.stopFrameAcquisition();                   // also done by doStopVideoCapture / doDisconnect / the destructor
camera.doStopVideoCapture();
```

Both styles drive the same acquisition path and the same guards, so pick whichever suits the application — but do not
mix them on one camera at once: two consumers pulling from one stream would each get an arbitrary half of it. Two rules
apply to the callback, and `ASI224MC_frame_callback` demonstrates both:

* **The frame is a reference into the worker's reusable buffer** and is only valid for the duration of the call. Copy
  whatever must outlive it — delivering by reference is what makes a multi-megabyte frame cost nothing to hand over.
* **Keep it short.** It runs between captures, so time spent in it is time not spent retrieving, and a slow callback
  shows up as dropped frames. Hand heavy work to your own queue; the example does exactly that, and prints the
  backpressure it produces.

Requiring a particular model is optional: `AsiCamera camera(id, "ASI2600MM")` makes `doConnect()` refuse anything
else, while `AsiCamera camera(id)` accepts whatever is there. `asi::nameMatchesModel(descriptor.name, model)` performs
the same check on a discovery result without contacting a camera. See the `examples/` directory for complete programs
(`basic_camera_discovery`, `basic_camera_connection`, `asi_camera_capture`, `asi_camera_frame_callback`); each accepts
an optional camera id on the command line and otherwise uses the first discovered camera.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- SDK BEHAVIOUR -->
## SDK Behaviour Worth Knowing

Several guarantees this library provides exist because the SDK does **not** provide them. All of the following were
measured against a real ASI224MC, and each is why a particular piece of the design is there:

| Vendor behaviour | What the library does |
|------------------|-----------------------|
| Frame retrieval performs **no bounds check** and is documented to crash on a short buffer. | Every buffer is sized from the camera's **live** ROI, read back from the SDK, never from anything a caller supplied. `frameBufferSize()` is the single place that arithmetic happens, in `std::size_t`. |
| `ASISetROIFormat` **succeeds while streaming**, silently changing the frame geometry a caller has already sized a buffer for. | The library tracks the acquisition mode and refuses the change with `INVALID_SEQUENCE`. |
| Changing the ROI format **silently recentres** the origin (a 1304×976 → 640×480 change moved it from (0,0) to (332,248)). | Geometry and origin are set in one call, origin applied *after* the format took, then both read back. |
| `ASIOpenCamera` on an already-open camera returns **success**, so two objects would each believe they owned it. | A process-wide ownership registry makes the second one fail with `CAMERA_IN_USE`. |
| `ASISetControlValue` **silently clamps** an out-of-range value and reports success. | Values are range-checked against the discovered capabilities first, so a refused write is reported rather than quietly altered. |
| `ASIStopVideoCapture` does **not** cancel a retrieval already blocked; it runs to its full timeout. | Frame retrieval holds a *separate* per-camera acquisition lock, so a disconnect waits the retrieval out instead of closing the camera underneath it. The SDK's "wait forever" value is never passed, and timeouts are clamped, so that wait is always bounded. |
| A short control call **is** safe concurrently with a blocked retrieval (measured: 249 frames + 321 control reads in 5 s, zero errors). | The control lock therefore never covers the blocking call, so control reads and telemetry polling run freely during streaming. |
| `ASIGetCameraProperty` fails until `ASIGetNumOfConnectedCameras` has been called — it *populates* the internal list. | Discovery always rebuilds the list first, under a process-global lock. |
| Dark-frame subtraction **persists in the Windows registry** across process lifetimes. | Cleared at connect by default, so a fresh connection means the same thing every time. |
| `AUTO_MAX_EXPOSURE` is documented as microseconds but is really **milliseconds** (the control reports itself as `AutoExpMaxExpMS`, range 1–60000). | Documented on the enumerator, so the 1000× error cannot be inherited. |
| The vendor documents neither the RGB24 channel order nor RAW16 bit justification. | The library never reinterprets pixels: a `Frame` carries its format and the bytes exactly as delivered. |

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- TESTING -->
## Testing

There is no test-framework dependency: the `testing/` executables are plain `assert()`-based checks, named `UT_*` for
hardware-free unit tests and `Test_*` for integration tests (build with `LIBDEGORASASI_BUILD_TESTING=ON`). The
`examples/` demos (`Example_*`) build with `LIBDEGORASASI_BUILD_EXAMPLES=ON`; each is a self-contained subproject
(`<name>/CMakeLists.txt` + `main.cpp`). Everything runs from `build/<preset>/bin/`.

* `UT_ImageGeometry` — the safety-critical buffer arithmetic, ROI alignment rules, and the validated numeric adapters.
* `UT_Json` — JSON round-trip for every serialisable value type, compact and pretty.
* `UT_ErrorMapping` — raw ASI code → category mapping, including that an unmodelled code can never read as success.
* `UT_CameraRegistry` — camera-ownership exclusivity under a 16-thread race, and the three lock scopes' identity.
* `UT_DeviceGuards` — every device operation refuses with `NOT_CONNECTED` before connecting; a failed connect leaves no
  ownership claim behind; destruction is safe without ever connecting.
* `UT_FrameWriter` — the BMP and PGM writers checked byte for byte against synthetic frames, including the row-padding
  path no ASI camera can reach, the 16-bit byte swap, and refusal of mismatched or truncated frames.
* `UT_FramePump` — the frame worker driven by SYNTHETIC producers no camera could reproduce on demand: the backoff that
  stops a failing producer spinning, prompt cancellation during that backoff, containment of a throwing producer or
  callback, observers callable from inside the callback, and per-run counters.
* `UT_HeaderHygiene` — includes every public header; since a test target never gets the vendor SDK on its include path,
  this **fails to compile** if the SDK ever leaks into a public header.
* `Test_AsiCameraCapture [id]` — full lifecycle on real hardware: capability discovery, control range enforcement, ROI
  rules, both acquisition workflows, geometry changes across RAW8/RAW16/RGB24, telemetry, and destructor teardown of a
  streaming camera.
* `Test_FrameCallback [id]` — callback-driven acquisition: delivery and ordering, buffer reuse across a run, the start
  guards, swapping or clearing the callback mid-run, agreement with the polling style, and every teardown path (explicit
  stop, stopping the stream, disconnecting, destructor).
* `Test_Concurrency [id]` — concurrent connects electing one owner; control calls and telemetry progressing during
  streaming; a disconnect waiting out an in-flight retrieval.

> [!NOTE]
> ZWO ships no camera simulator, so unlike the reference sibling project the `Test_*` executables need real hardware.
> They **self-skip with success** when no camera is attached, so `ctest` is safe to run unattended — which is also why
> the hardware-free `UT_*` set deliberately carries as much of the logic as possible (the buffer arithmetic, the error
> mapping and the device guards are all pure and tested without a camera).

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- ROADMAP -->
## Roadmap

- [x] Install/export + CMake package config (`find_package(DegorasASI)` → `Degoras::ASI`) and a vcpkg overlay port. See [`docs/PACKAGING.md`](LibDegorasASI/docs/PACKAGING.md).
- [x] One generic `AsiCamera` covering every ASI model, validated on the ASI224MC.
- [ ] Validate further models against hardware (ASI2600MM next). No library code is needed — only tests.
- [ ] Cooled-camera support (the cooler controls are already in the vocabulary; no cooled camera has been validated).
- [ ] Trigger-mode and ST4 pulse-guiding support, once hardware that has them is available (the ASI224MC reports
      neither a trigger capability nor GPS).
- [x] Asynchronous acquisition: a frame-delivery worker (`FramePump`) delivering frames to a callback, alongside the
      caller-owned polling loop. Both drive the same path; see `ASI224MC_frame_callback`.
- [ ] Migrate the generic, project-agnostic infrastructure (status poller, frame pump, wait helper, JSON utilities) into
      LibDegorasBase so it is shared rather than reimplemented per project.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- LICENSE -->
## License

Distributed under the GNU General Public License v3.0 (or later). See the full text in the
[`LICENSE`](LICENSE) file for details.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- CONTACT -->
## Contact

Degoras Project Team — Spanish Navy Observatory SLR station (SFEL), San Fernando.

* Ángel Vera Herrera — avera@roa.es · angelvh.engr@gmail.com
* Jesús Relinque Madroñal — jrelinque@roa.es

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- ACKNOWLEDGMENTS -->
## Acknowledgments

* [ZWO ASI Camera SDK](https://www.zwoastro.com/software/) — the underlying camera-control SDK.
* Real Instituto y Observatorio de la Armada (ROA) and the SFEL SLR station, San Fernando.
* [Best-README-Template](https://github.com/othneildrew/Best-README-Template).

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- MARKDOWN LINKS & IMAGES -->
[cpp-shield]: https://img.shields.io/badge/C%2B%2B-17-00599C.svg?style=for-the-badge&logo=cplusplus
[cpp-url]: https://en.cppreference.com/w/cpp/17
[cmake-shield]: https://img.shields.io/badge/CMake-3.21%2B-064F8C.svg?style=for-the-badge&logo=cmake&logoColor=white
[cmake-url]: https://cmake.org/
[license-shield]: https://img.shields.io/badge/License-GPLv3-blue.svg?style=for-the-badge
[license-url]: https://www.gnu.org/licenses/gpl-3.0
[platform-shield]: https://img.shields.io/badge/platform-Windows%20|%20MinGW-lightgrey.svg?style=for-the-badge
[platform-url]: #getting-started
