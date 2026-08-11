cmake_minimum_required(VERSION 3.24)

project(
  carla_ego_runtime
  VERSION 0.2.0
  DESCRIPTION "Native runtime and telemetry gateway for a CARLA ego vehicle"
  LANGUAGES CXX
)

option(
  CARLA_EGO_WITH_CARLA
  "Build native CARLA connectivity using an installed LibCarla package"
  OFF
)

add_library(carla-ego-runtime-core STATIC src/runtime_options.cpp)
target_compile_features(carla-ego-runtime-core PUBLIC cxx_std_20)
target_include_directories(
  carla-ego-runtime-core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_executable(carla-ego-runtime src/main.cpp)
target_link_libraries(carla-ego-runtime PRIVATE carla-ego-runtime-core)

if(CARLA_EGO_WITH_CARLA)
  find_package(Carla CONFIG REQUIRED)
  target_sources(carla-ego-runtime PRIVATE src/runtime_carla.cpp)
  target_link_libraries(carla-ego-runtime PRIVATE Carla::carla-client)
  target_compile_definitions(carla-ego-runtime PRIVATE CARLA_EGO_WITH_CARLA=1)
else()
  target_sources(carla-ego-runtime PRIVATE src/runtime_stub.cpp)
endif()

function(carla_ego_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

carla_ego_enable_warnings(carla-ego-runtime-core)
carla_ego_enable_warnings(carla-ego-runtime)

include(CTest)
if(BUILD_TESTING)
  add_executable(runtime-options-test tests/runtime_options_test.cpp)
  target_link_libraries(runtime-options-test PRIVATE carla-ego-runtime-core)
  carla_ego_enable_warnings(runtime-options-test)

  add_test(NAME runtime_options COMMAND runtime-options-test)
  add_test(NAME runtime_reports_version COMMAND carla-ego-runtime --version)
  add_test(NAME runtime_reports_help COMMAND carla-ego-runtime --help)
  set_tests_properties(
    runtime_reports_version
    PROPERTIES PASS_REGULAR_EXPRESSION "carla-ego-runtime ${PROJECT_VERSION}"
  )
  set_tests_properties(
    runtime_reports_help
    PROPERTIES PASS_REGULAR_EXPRESSION "Usage: carla-ego-runtime"
  )
endif()

install(TARGETS carla-ego-runtime RUNTIME DESTINATION bin)
