# ---- Why Catch2, not GoogleTest ---------------------------------------
#
# Both are fine, mainstream choices; the actual deciding factors here were:
#
#  1. Zero extra system dependencies. Catch2 v3 has no dependencies of its
#     own, so FetchContent-ing it is just "clone and build one library."
#     GoogleTest is also dependency-free, but Catch2's single responsibility
#     (just a test framework, no separate gmock library bundled in) keeps
#     the build graph smaller for a project this size.
#  2. TEST_CASE/SECTION reads naturally for this project's tests. A lot of
#     the interesting behavior here is "given a book already in state X,
#     what happens for scenario A vs scenario B vs scenario C" -- Catch2's
#     SECTION blocks let each scenario share setup code while still running
#     as an independent test (Catch2 re-enters the TEST_CASE from the top
#     for every SECTION, so state from one SECTION never leaks into
#     another). GoogleTest's fixture classes can do this too, but need a
#     separate class + TEST_F per scenario, which is more boilerplate for
#     the same coverage.
#  3. No separate main() to write. Catch2WithMain provides one; the test
#     binary here is just the TEST_CASE files linked against it.
#
# ------------------------------------------------------------------------

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(lob_tests
    test_order_book.cpp
    test_matching_engine.cpp
)
target_link_libraries(lob_tests PRIVATE lob Catch2::Catch2WithMain)

if(MSVC)
    target_compile_options(lob_tests PRIVATE /W4)
else()
    target_compile_options(lob_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Catch2 ships a CMake module (extras/Catch.cmake) that registers each
# TEST_CASE as its own individual CTest test, so `ctest` output shows
# pass/fail per case instead of one pass/fail for the whole binary.
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(lob_tests)
