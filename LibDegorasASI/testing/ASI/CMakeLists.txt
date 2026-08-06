# ----------------------------------------------------------------------------------------------------------------------
# TESTING SUBGROUP: ASI
# One executable per .cpp in this subgroup (target name = file stem).
# ----------------------------------------------------------------------------------------------------------------------

file(GLOB_RECURSE ASI_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

foreach(test_src ${ASI_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE LibDegorasASI)
    # Register with CTest, except the long-running monitor and the hardware-gated tests.
    if(NOT test_name MATCHES "Monitor|Hardware")
        add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
endforeach()

# **********************************************************************************************************************
