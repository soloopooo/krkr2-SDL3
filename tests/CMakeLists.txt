# Host-executable unit tests for pure-logic engine components.
# No cocos2d-x, no Android, no JNI — just the .c/.cpp files under test + gtest.
# Invoked from the root CMakeLists when -DKRKR2_BUILD_TESTS=ON on a non-Android host.

include(FetchContent)
FetchContent_Declare(
	googletest
	GIT_REPOSITORY https://github.com/google/googletest.git
	GIT_TAG        release-1.12.1   # last release supporting C++11
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

set(SRC_ROOT ${CMAKE_SOURCE_DIR}/src)

# --- MathAlgorithms (signal deinterleave + window) ---------------------------
add_executable(test_math_algorithms
	test_math_algorithms.cpp
	${SRC_ROOT}/core/utils/MathAlgorithms_Default.cpp
)
target_link_libraries(test_math_algorithms PRIVATE gtest_main)
gtest_discover_tests(test_math_algorithms)

# --- CharacterSet (UTF-8 <-> UTF-16 round-trip) ------------------------------
add_executable(test_character_set
	test_character_set.cpp
)
target_include_directories(test_character_set PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/stubs      # override MsgIntf.h with stub
	${SRC_ROOT}/core
	${SRC_ROOT}/core/base
	${SRC_ROOT}/core/tjs2
	${SRC_ROOT}/core/utils
)
target_link_libraries(test_character_set PRIVATE gtest_main)
gtest_discover_tests(test_character_set)

# --- MD5 (RFC 1321 known-answer vectors) -------------------------------------
# src/core/utils/md5.c is a standalone C translation unit (no engine deps).
add_executable(test_md5
	test_md5.cpp
	${SRC_ROOT}/core/utils/md5.c
)
target_include_directories(test_md5 PRIVATE ${SRC_ROOT}/core/utils)
target_link_libraries(test_md5 PRIVATE gtest_main)
gtest_discover_tests(test_md5)

# --- Bitap fuzzy match (header-only template) --------------------------------
# src/plugins/bitap_fuzzy.hpp is a pure template; just include it.
add_executable(test_bitap_fuzzy
	test_bitap_fuzzy.cpp
)
target_include_directories(test_bitap_fuzzy PRIVATE ${SRC_ROOT}/plugins)
target_link_libraries(test_bitap_fuzzy PRIVATE gtest_main)
gtest_discover_tests(test_bitap_fuzzy)
