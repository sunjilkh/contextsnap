# Unit tests. Every test binary is self-contained (microtest.hpp) and runs
# headless, so `ctest` works on a CI runner without a display server.

enable_testing()

add_library(contextsnap_test_framework INTERFACE)
target_include_directories(contextsnap_test_framework INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(contextsnap_test_framework INTERFACE cxx_std_20)

set(CONTEXTSNAP_UNIT_TESTS
    test_json
    test_ulid
    test_matching
    test_sanitizer
    test_serialization
    test_restore_plan
    test_ipc_protocol)

foreach(test_name IN LISTS CONTEXTSNAP_UNIT_TESTS)
  add_executable(${test_name} unit/${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE contextsnap::core contextsnap_test_framework
                                             contextsnap::warnings)
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(
    ${test_name} PROPERTIES TIMEOUT 120 ENVIRONMENT
                            "CONTEXTSNAP_HEADLESS=1;CONTEXTSNAP_HOST=ci-runner")
endforeach()

# `cmake --build . --target check` runs the suite with output on failure.
add_custom_target(
  check
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
  DEPENDS ${CONTEXTSNAP_UNIT_TESTS}
  COMMENT "Running ContextSnap unit tests")
