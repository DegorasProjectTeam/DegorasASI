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
    # testing/ is on the include path so the subgroups can share small test-only headers, such as the hardware gate.
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    # Register with CTest, except the long-running monitor and the hardware-gated tests.
    if(NOT test_name MATCHES "Monitor|Hardware")
        add_test(NAME ${test_name} COMMAND ${test_name})
        # By convention in this suite UT_* are unit tests and Test_* are integration tests that drive real hardware.
        # A Test_* with no camera attached exits 77, which SKIP_RETURN_CODE turns into a "Skipped" line of its own in
        # the ctest summary -- not a pass, which is what returning 0 used to make it, and not a failure either.
        # Set DPASI_REQUIRE_CAMERA=1 in the environment to make a missing camera fail instead; see
        # testing/dpasi_test_camera.h.
        if(test_name MATCHES "^Test_")
            set_tests_properties(${test_name} PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endif()
endforeach()

# **********************************************************************************************************************
