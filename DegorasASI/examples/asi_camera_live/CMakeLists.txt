# ----------------------------------------------------------------------------------------------------------------------
# EXAMPLE: asi_camera_live
#
# OPTIONAL. This is the only example with a third-party dependency, and it is built ONLY when OpenCV is present. The
# library itself depends on no imaging or GUI toolkit and never will -- see the note at the top of main.cpp -- so a
# missing OpenCV must cost nothing beyond this one example.
#
# QUIET, and a return() rather than a fatal error: a hard find_package here would abort the configure of a library
# that does not need OpenCV at all, which is precisely the failure this guard exists to prevent.
#
# Only the four modules actually used, never ${OpenCV_LIBS}: that variable expands to every installed module (48 of
# them in the reference environment -- aruco, dnn, stitching, videostab...) and each one would become a runtime DLL
# dependency of this example for nothing.
# ----------------------------------------------------------------------------------------------------------------------

# Silence Qt's "No qtpaths executable found for deployment purposes" warning. It has to be set BEFORE the OpenCV
# lookup, because OpenCV's config pulls Qt6 in transitively -- highgui here is built against Qt.
#
# The warning is spurious for this project twice over. In a Debug build Qt looks only for qtpaths6.debug.bat, and
# the triplet used by this environment is release-only (VCPKG_BUILD_TYPE release), so debug tooling is never
# installed and never can be found. And the warning exists to say qt_generate_deploy_app_script() will not work;
# this example does not use it, deploying instead through cmake/dp_deploy_runtime.cmake, which reads the binaries'
# own import tables and needs no Qt tooling at all.
#
# Plain variable, not CACHE: Qt only reads it, and a cache entry would become an unused-cli warning of its own.
set(QT_NO_QTPATHS_DEPLOYMENT_WARNING ON)

find_package(OpenCV QUIET CONFIG)

if(NOT OpenCV_FOUND OR NOT TARGET opencv_highgui)
    message(STATUS "asi_camera_live: SKIPPED (OpenCV not found). Nothing else is affected.")
    return()
endif()

message(STATUS "asi_camera_live: enabled with OpenCV ${OpenCV_VERSION}")

# Model, view, controller and the pixel layer, one pair of files each. See the comment at the top of main.cpp for
# why this example is split at all: the single-file version froze its own window for the length of every exposure.
add_executable(Example_asi_camera_live
    main.cpp
    live_image.cpp
    live_model.cpp
    live_view.cpp
    live_controller.cpp
    live_reticle.cpp)

target_link_libraries(Example_asi_camera_live PRIVATE
    DegorasASI
    opencv_core
    opencv_imgproc
    opencv_imgcodecs
    opencv_highgui)

# ----------------------------------------------------------------------------------------------------------------------
# RUNTIME DEPLOYMENT
#
# THIS IS NOT OPTIONAL POLISH -- without it the example either fails to start or HANGS. Two separate mechanisms are
# at work and both were measured rather than assumed:
#
#  1. THE WINDOWS LOADER searches the executable's own directory BEFORE anything on PATH. That is the only reliable
#     way to guarantee which Qt gets loaded. This machine has two complete Qt 6.11.1 installations -- vcpkg's, which
#     OpenCV's highgui is built against, and MSYS2's, which Qt Creator pulls in -- with identical DLL names and
#     identical version numbers, so picking the wrong one is SILENT. Relying on PATH order works right up until
#     something reorders PATH.
#
#  2. QT'S PLUGIN SEARCH is anchored to the location of Qt6Core.dll, because vcpkg builds Qt relocatable: it derives
#     its prefix from wherever that DLL turns out to live and then looks for <prefix>/Qt6/plugins. So copying the Qt
#     DLLs next to the executable MOVES the plugin search to <exedir>/.. -- where there is no plugin tree -- and the
#     only path left is the executable's own directory. Deploy the DLLs and forget the plugin and the platform
#     plugin becomes unfindable; the symptom is not an error but a PERMANENT HANG inside the first cv::namedWindow,
#     because OpenCV's Qt GUI thread dies in qFatal while the caller waits for it. Measured: removing
#     platforms/qwindows.dll hangs the process in every PATH configuration tested.
#
# So the two steps go together, always. Neither uses a hand-written file list: the plugin comes from Qt's own
# exported target, and the DLL closure is read out of the binaries' import tables. See cmake/dp_deploy_runtime.cmake
# for why $<TARGET_RUNTIME_DLLS> is not sufficient here.
#
# NOTE ON qt.conf: do not add one. A qt.conf whose [Paths] Plugins= names an absolute prefix OVERRIDES the
# app-local plugin directory, which puts the choice of Qt back at the mercy of an absolute path.

set(_LIVE_DEPLOY_DIR "$<TARGET_FILE_DIR:Example_asi_camera_live>")

# The Qt platform plugin first, into <exedir>/platforms/. Qt exports it as an imported target, so its location is
# whatever Qt was actually found -- no path guessing, and it follows a change of prefix by itself.
if(TARGET Qt6::QWindowsIntegrationPlugin)
    set(_LIVE_PLUGIN "${_LIVE_DEPLOY_DIR}/platforms/qwindows.dll")
    add_custom_command(TARGET Example_asi_camera_live POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_LIVE_DEPLOY_DIR}/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>" "${_LIVE_PLUGIN}"
        VERBATIM)
else()
    set(_LIVE_PLUGIN "")
    message(WARNING "[DEGORAS] asi_camera_live: Qt6::QWindowsIntegrationPlugin not found. OpenCV's highgui is built "
                    "against Qt here, so without the platform plugin beside the executable it will HANG on the "
                    "first window it opens. Check that find_package(OpenCV) pulled Qt6 in.")
endif()

# Then the DLL closure, resolved from the binaries' own import tables. Search order = CMAKE_PREFIX_PATH order, which
# the reference toolchain already sets to the package prefix before the compiler prefix, so provenance is decided
# here rather than by whatever PATH looks like at run time. The plugin is passed as a module so ITS dependencies are
# resolved too. See cmake/dp_deploy_runtime.cmake for why $<TARGET_RUNTIME_DLLS> is not enough.
set(_LIVE_SEARCH_DIRS "")
foreach(_pfx IN LISTS CMAKE_PREFIX_PATH)
    if(IS_DIRECTORY "${_pfx}/bin")
        list(APPEND _LIVE_SEARCH_DIRS "${_pfx}/bin")
    endif()
endforeach()
get_filename_component(_LIVE_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
list(APPEND _LIVE_SEARCH_DIRS "${_LIVE_MINGW_BIN}")

# Two more that belong to no prefix: the vendor SDK, which lives in thirdparty/, and the library itself, which is
# still in the build tree at this point. ASICamera_RUNTIME_DIR comes from cmake/FindASICamera.cmake.
if(ASICamera_RUNTIME_DIR)
    list(APPEND _LIVE_SEARCH_DIRS "${ASICamera_RUNTIME_DIR}")
endif()
list(APPEND _LIVE_SEARCH_DIRS "$<TARGET_FILE_DIR:DegorasASI>")

list(REMOVE_DUPLICATES _LIVE_SEARCH_DIRS)

add_custom_command(TARGET Example_asi_camera_live POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DDP_BINARY=$<TARGET_FILE:Example_asi_camera_live>
            -DDP_MODULES=${_LIVE_PLUGIN}
            -DDP_DEST=${_LIVE_DEPLOY_DIR}
            "-DDP_SEARCH_DIRS=${_LIVE_SEARCH_DIRS}"
            -P "${CMAKE_SOURCE_DIR}/cmake/dp_deploy_runtime.cmake"
    COMMENT "Deploying the runtime DLL closure for asi_camera_live"
    VERBATIM)

# **********************************************************************************************************************
