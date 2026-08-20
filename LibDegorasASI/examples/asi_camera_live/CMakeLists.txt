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
# On SDL2 rather than OpenCV, which was tried first: the OpenCV in this environment builds highgui against Qt, so its
# config runs find_dependency(Qt6 COMPONENTS ... Core5Compat ...). vcpkg's qtbase has no Core5Compat and the MSYS2
# Qt6 config fails here, and because that failure happens inside OpenCV's own cmake_policy(PUSH) block it surfaces as
# the thoroughly misleading "cmake_policy PUSH without matching POP". SDL2 needs none of it.
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
