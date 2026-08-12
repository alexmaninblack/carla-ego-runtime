cmake_minimum_required(VERSION 3.24)

project(
  carla_ego_runtime
  VERSION 0.8.0
  DESCRIPTION "Native runtime and telemetry gateway for a CARLA ego vehicle"
  LANGUAGES CXX
)

option(
  CARLA_EGO_WITH_CARLA
  "Build native CARLA connectivity using an installed LibCarla package"
  OFF
)
option(
  CARLA_EGO_WITH_VISS
  "Build the TLS WebSocket VISS 3.1 endpoint"
  OFF
)
set(
  CARLA_EGO_BOOST_INCLUDE_DIR
  ""
  CACHE PATH
  "Directory containing Boost headers; the installed LibCarla include tree is supported"
)

if(CARLA_EGO_WITH_CARLA)
  find_package(Carla CONFIG REQUIRED)
endif()

add_library(
  carla-ego-runtime-core STATIC
  src/runtime_options.cpp
  src/gnss.cpp
  src/vehicle_state.cpp
  src/vss.cpp
)
target_compile_features(carla-ego-runtime-core PUBLIC cxx_std_20)
target_include_directories(
  carla-ego-runtime-core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_executable(carla-ego-runtime src/main.cpp)
if(NOT CARLA_EGO_WITH_VISS)
  target_link_libraries(carla-ego-runtime PRIVATE carla-ego-runtime-core)
endif()

if(CARLA_EGO_WITH_CARLA)
  target_sources(carla-ego-runtime PRIVATE src/runtime_carla.cpp)
  target_link_libraries(carla-ego-runtime PRIVATE Carla::carla-client)
  target_compile_definitions(carla-ego-runtime PRIVATE CARLA_EGO_WITH_CARLA=1)
else()
  target_sources(carla-ego-runtime PRIVATE src/runtime_stub.cpp)
endif()

if(CARLA_EGO_WITH_VISS)
  find_package(OpenSSL 3.0 REQUIRED)
  find_package(Threads REQUIRED)
  if(NOT CARLA_EGO_BOOST_INCLUDE_DIR)
    if(DEFINED Carla_INCLUDE_DIR AND
       EXISTS "${Carla_INCLUDE_DIR}/boost/beast.hpp" AND
       EXISTS "${Carla_INCLUDE_DIR}/boost/json.hpp")
      set(CARLA_EGO_BOOST_INCLUDE_DIR "${Carla_INCLUDE_DIR}")
    else()
      find_path(
        CARLA_EGO_DISCOVERED_BOOST_INCLUDE_DIR
        NAMES boost/beast.hpp boost/json.hpp
      )
      set(
        CARLA_EGO_BOOST_INCLUDE_DIR
        "${CARLA_EGO_DISCOVERED_BOOST_INCLUDE_DIR}"
      )
    endif()
  endif()
  if(NOT CARLA_EGO_BOOST_INCLUDE_DIR)
    message(FATAL_ERROR "Boost.Beast and Boost.JSON headers were not found")
  endif()

  add_library(
    carla-ego-runtime-viss STATIC
    src/boost_json.cpp
    src/viss_protocol.cpp
    src/viss_server.cpp
  )
  target_compile_features(carla-ego-runtime-viss PUBLIC cxx_std_20)
  target_include_directories(
    carla-ego-runtime-viss
    PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
      "${CARLA_EGO_BOOST_INCLUDE_DIR}"
  )
  target_compile_definitions(
    carla-ego-runtime-viss
    PUBLIC
      BOOST_ERROR_CODE_HEADER_ONLY=1
      BOOST_SYSTEM_NO_LIB=1
  )
  target_link_libraries(
    carla-ego-runtime-viss
    PUBLIC
      carla-ego-runtime-core
    PRIVATE
      OpenSSL::SSL
      OpenSSL::Crypto
      Threads::Threads
  )
  target_link_libraries(carla-ego-runtime PRIVATE carla-ego-runtime-viss)
  target_compile_definitions(carla-ego-runtime PRIVATE CARLA_EGO_WITH_VISS=1)

  add_executable(carla-viss-client src/viss_client.cpp)
  target_compile_features(carla-viss-client PRIVATE cxx_std_20)
  target_include_directories(
    carla-viss-client PRIVATE "${CARLA_EGO_BOOST_INCLUDE_DIR}"
  )
  target_compile_definitions(
    carla-viss-client
    PRIVATE
      BOOST_ERROR_CODE_HEADER_ONLY=1
      BOOST_SYSTEM_NO_LIB=1
  )
  target_link_libraries(
    carla-viss-client
    PRIVATE
      carla-ego-runtime-viss
      OpenSSL::SSL
      OpenSSL::Crypto
      Threads::Threads
  )
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
if(CARLA_EGO_WITH_VISS)
  carla_ego_enable_warnings(carla-ego-runtime-viss)
  carla_ego_enable_warnings(carla-viss-client)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(runtime-options-test tests/runtime_options_test.cpp)
  target_link_libraries(runtime-options-test PRIVATE carla-ego-runtime-core)
  carla_ego_enable_warnings(runtime-options-test)

  add_executable(vehicle-state-test tests/vehicle_state_test.cpp)
  target_link_libraries(vehicle-state-test PRIVATE carla-ego-runtime-core)
  carla_ego_enable_warnings(vehicle-state-test)

  add_executable(vss-projection-test tests/vss_projection_test.cpp)
  target_link_libraries(vss-projection-test PRIVATE carla-ego-runtime-core)
  carla_ego_enable_warnings(vss-projection-test)

  add_test(NAME runtime_options COMMAND runtime-options-test)
  add_test(NAME vehicle_state COMMAND vehicle-state-test)
  add_test(NAME vss_projection COMMAND vss-projection-test)

  add_executable(gnss-test tests/gnss_test.cpp)
  target_link_libraries(gnss-test PRIVATE carla-ego-runtime-core)
  carla_ego_enable_warnings(gnss-test)
  add_test(NAME gnss COMMAND gnss-test)

  if(CARLA_EGO_WITH_VISS)
    add_executable(viss-protocol-test tests/viss_protocol_test.cpp)
    target_link_libraries(viss-protocol-test PRIVATE carla-ego-runtime-viss)
    target_include_directories(
      viss-protocol-test PRIVATE "${CARLA_EGO_BOOST_INCLUDE_DIR}"
    )
    carla_ego_enable_warnings(viss-protocol-test)
    add_test(NAME viss_protocol COMMAND viss-protocol-test)

    add_executable(viss-network-test tests/viss_network_test.cpp)
    target_link_libraries(
      viss-network-test
      PRIVATE
        carla-ego-runtime-viss
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )
    target_include_directories(
      viss-network-test PRIVATE "${CARLA_EGO_BOOST_INCLUDE_DIR}"
    )
    carla_ego_enable_warnings(viss-network-test)
    add_test(NAME viss_network COMMAND viss-network-test)
    set_tests_properties(viss_network PROPERTIES TIMEOUT 20)
  endif()
  add_test(
    NAME vss_overlay
    COMMAND
      "${CMAKE_COMMAND}"
      -DOVERLAY_FILE=${CMAKE_CURRENT_SOURCE_DIR}/vss/Vehicle.CarlaSimulation.vspec
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ValidateVssOverlay.cmake
  )
  add_test(NAME runtime_reports_version COMMAND carla-ego-runtime --version)
  add_test(NAME runtime_reports_help COMMAND carla-ego-runtime --help)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME m5_tools
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/m5_tools_test.py"
    )
    add_test(
      NAME external_control_protocol
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/external_control_protocol_test.py"
    )
  endif()
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
if(CARLA_EGO_WITH_VISS)
  install(TARGETS carla-viss-client RUNTIME DESTINATION bin)
endif()
