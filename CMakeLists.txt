# kb-c is C17 with no runtime dependency beyond libc, libsqlite3 and pthreads.
cmake_minimum_required(VERSION 3.20)
project(kb_c VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(KBC_SANITIZE "build with ASan + UBSan (no -O3)" OFF)
option(KBC_TSAN "build with ThreadSanitizer (no -O3)" OFF)
option(KBC_WERROR "treat warnings as errors" ON)
option(KBC_NATIVE "tune for the build machine (-march=native); off for release" OFF)
option(KBC_BUILD_TESTS "build the test suite" ON)

set(KBC_WARNINGS
  -Wall -Wextra -Wpedantic
  -Wshadow -Wpointer-arith -Wcast-qual -Wcast-align
  -Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings
  -Wvla -Wpointer-to-int-cast -Wformat=2 -Wundef)

if(KBC_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=address,undefined)
elseif(KBC_TSAN)
  # The index shares one cached scoring accumulator across query threads, so
  # the concurrency claim needs a lane that can actually see a race.
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=thread)
else()
  add_compile_options(-O3 -fno-plt)
  if(KBC_NATIVE)
    add_compile_options(-march=native)
  endif()
endif()

if(KBC_WERROR)
  list(APPEND KBC_WARNINGS -Werror)
endif()

# CMAKE_C_EXTENSIONS OFF compiles with -std=c17, which defines __STRICT_ANSI__
# and hides opendir/fstatat/inotify/strdup. kb-c is a Linux daemon, so the
# feature test macros go here once, centrally, instead of per translation unit.
add_compile_definitions(_POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
add_compile_options(${KBC_WARNINGS})

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
find_package(Threads REQUIRED)

# ------------------------------------------------------------------ library
file(GLOB KBC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
list(LENGTH KBC_SOURCES KBC_SOURCE_COUNT)
if(KBC_SOURCE_COUNT EQUAL 0)
  message(FATAL_ERROR "no sources in src/ — nothing to build")
endif()

add_library(kbc STATIC ${KBC_SOURCES})
target_include_directories(kbc PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(kbc PUBLIC PkgConfig::SQLITE3 Threads::Threads m)
set_target_properties(kbc PROPERTIES OUTPUT_NAME kbc POSITION_INDEPENDENT_CODE ON)

# ----------------------------------------------------------------------- CLI
add_executable(kbc_cli cli/main.c)
target_link_libraries(kbc_cli PRIVATE kbc)
set_target_properties(kbc_cli PROPERTIES OUTPUT_NAME kbc)

# --------------------------------------------------------------------- tests
if(KBC_BUILD_TESTS)
  enable_testing()
  add_library(kbc_testing STATIC tests/kbc_test.c)
  target_link_libraries(kbc_testing PUBLIC kbc)
  target_include_directories(kbc_testing PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)

  file(GLOB KBC_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.c")
  foreach(test_src ${KBC_TEST_SOURCES})
    get_filename_component(test_name "${test_src}" NAME_WE)
    add_executable(${test_name} "${test_src}")
    target_link_libraries(${test_name} PRIVATE kbc_testing)
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 300)
  endforeach()
  # kbc_parity.c is deliberately NOT named test_*.c, so the glob above does
  # not take it. It is not a unit test of kb-c; it is the gate that runs the
  # cross-implementation diff against the Rust daemon, and it takes minutes
  # because it starts both daemons and waits for both to finish indexing.
  # Sweeping it into the fast lane would hide exactly the thing it exists to
  # surface. The name is the separation.
  set(KBCPARITY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/kbcparity")
  add_executable(kbc_parity tests/kbc_parity.c)
  target_link_libraries(kbc_parity PRIVATE kbc_testing)
  target_compile_definitions(kbc_parity PRIVATE
    KBCPARITY_DEFAULT_DIR="${KBCPARITY_DIR}")
  # Exit 77 is the automake skip convention and means the comparison could
  # not be performed: no Rust binary, no kb-c binary, a daemon that never
  # came up, a corpus whose SHA256SUMS does not verify. SKIP_RETURN_CODE
  # makes ctest print "***Skipped" with the harness's own explanation above
  # it, so a gate that did not run can never be read as one that passed.
  add_test(NAME kbc_parity COMMAND kbc_parity "${KBCPARITY_DIR}")
  set_tests_properties(kbc_parity PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 1800
    # The suite brings up its own daemons on ephemeral loopback ports in its
    # own sandbox. Without the lock, `ctest -j` starts a second copy
    # underneath the first and the two daemons race for the corpus.
    RESOURCE_LOCK kbc_parity)
  list(LENGTH KBC_TEST_SOURCES KBC_TEST_COUNT)
  message(STATUS "kb-c: ${KBC_SOURCE_COUNT} sources, ${KBC_TEST_COUNT} test binaries + kbc_parity")
endif()

install(TARGETS kbc_cli RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
