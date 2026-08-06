# ----------------------------------------------------------------------------------------------------------------------
# TESTING
#
# Assert-based, no-framework self-checks (UT_* unit tests, Test_* integration tests). Each subgroup — matching the
# library layers — is its own CMakeLists so the suite scales: add a new subgroup with add_subdirectory below, or drop
# a .cpp into an existing subgroup. A subgroup can later add its own sources, resources or link settings independently.
# ----------------------------------------------------------------------------------------------------------------------

add_subdirectory(Common)
add_subdirectory(ASI)
add_subdirectory(Devices)

# **********************************************************************************************************************
