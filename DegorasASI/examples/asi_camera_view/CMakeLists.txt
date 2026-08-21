# ----------------------------------------------------------------------------------------------------------------------
# EXAMPLE: asi_camera_view
#
# Writes what the camera produced to disk (BMP / PGM) and prints a terminal preview. No image-processing dependency:
# the writers are a header in this directory, deliberately NOT part of the library, which stays free of image
# pipelines, GUI toolkits, Qt and OpenCV.
# ----------------------------------------------------------------------------------------------------------------------

add_executable(Example_asi_camera_view main.cpp)
target_link_libraries(Example_asi_camera_view PRIVATE DegorasASI)

# **********************************************************************************************************************
