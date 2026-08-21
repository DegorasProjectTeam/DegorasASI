# ----------------------------------------------------------------------------------------------------------------------
# TESTING SUBGROUP: Devices
# One executable per .cpp in this subgroup (target name = file stem).
# ----------------------------------------------------------------------------------------------------------------------

file(GLOB_RECURSE ASI_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

foreach(test_src ${ASI_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE LibDegorasASI)
    # Warnings ON for test code too. The library target gets these; the tests did not, and that is how the whole
    # suite came to be a no-op in Release without anyone noticing: NDEBUG discarded every assert, which left 73
    # -Wunused-variable warnings' worth of evidence uncollected because nothing was asking for it. With the
    # assertions live again this costs nothing -- all eleven test translation units compile clean.
    target_compile_options(${test_name} PRIVATE -Wall -Wextra)
    # Register with CTest, except the long-running monitor and the hardware-gated tests. The Test_* executables here
    # self-skip when no camera is attached, so they are safe to register.
    if(NOT test_name MATCHES "Monitor|Hardware")
        add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
endforeach()

# **********************************************************************************************************************
