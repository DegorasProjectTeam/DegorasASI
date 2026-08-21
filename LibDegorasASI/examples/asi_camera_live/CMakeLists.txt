# ----------------------------------------------------------------------------------------------------------------------
# EXAMPLE: asi_camera_live
#
# OPTIONAL. This is the only example with a third-party dependency, and it is built ONLY when SDL2 is present. The
# library itself depends on no imaging or GUI toolkit and never will -- see the note at the top of main.cpp -- so a
# missing SDL2 must cost nothing beyond this one example.
#
# QUIET, and a return() rather than a fatal error: a hard find_package here would abort the configure of a library
# that does not need SDL2 at all, which is precisely the failure this guard exists to prevent.
#
# ON SDL2 RATHER THAN OPENCV, which was tried first. The OpenCV in this environment builds highgui against Qt, so
# OpenCVModules.cmake runs find_dependency(Qt6 COMPONENTS ... Core5Compat ...). At the time this example was written
# the qt5compat port was not installed, that find_dependency failed, and because the failure happens inside OpenCV's
# own cmake_policy(PUSH) block it surfaced as the thoroughly misleading "cmake_policy PUSH without matching POP".
#
# That specific obstacle is gone: qt5compat 6.11.1 is installed now and find_package(OpenCV) resolves cleanly (see
# HelloWorlds/HelloWorldOpenCV, which exercises it). SDL2 nevertheless remains the right dependency HERE, and for a
# better reason than the one above: linking the OpenCV GUI would drag Qt into an example of a library whose whole
# point is that it has no Qt and no GUI toolkit anywhere near it. SDL2 needs none of that -- it is a window, a
# texture and a blit.
# ----------------------------------------------------------------------------------------------------------------------

find_package(SDL2 QUIET)

if(NOT SDL2_FOUND OR NOT TARGET SDL2::SDL2)
    message(STATUS "asi_camera_live: SKIPPED (SDL2 not found). Nothing else is affected.")
    return()
endif()

message(STATUS "asi_camera_live: enabled with SDL2 ${SDL2_VERSION}")

add_executable(Example_asi_camera_live main.cpp)

# SDL2::SDL2main is intentionally NOT linked. It redefines main() as WinMain, which is what a windowed application
# wants; this example is a console tool that also opens a window, and its stdout is half the interface.
target_link_libraries(Example_asi_camera_live PRIVATE
    LibDegorasASI
    SDL2::SDL2)

# **********************************************************************************************************************
