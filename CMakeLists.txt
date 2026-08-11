cmake_minimum_required(VERSION 3.24)

project(
  carla_ego_runtime
  VERSION 0.1.0
  DESCRIPTION "Native runtime and telemetry gateway for a CARLA ego vehicle"
  LANGUAGES CXX
)

add_executable(carla-ego-runtime src/main.cpp)
target_compile_features(carla-ego-runtime PRIVATE cxx_std_17)
target_include_directories(
  carla-ego-runtime
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(MSVC)
  target_compile_options(carla-ego-runtime PRIVATE /W4 /permissive-)
else()
  target_compile_options(carla-ego-runtime PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
  add_test(NAME runtime_reports_version COMMAND carla-ego-runtime --version)
endif()

install(TARGETS carla-ego-runtime RUNTIME DESTINATION bin)
