if(NOT EXISTS "${OVERLAY_FILE}")
  message(FATAL_ERROR "VSS overlay does not exist: ${OVERLAY_FILE}")
endif()

file(READ "${OVERLAY_FILE}" overlay)

foreach(path IN ITEMS
    "Vehicle.CarlaSimulation:"
    "Vehicle.CarlaSimulation.ProfileVersion:"
    "Vehicle.CarlaSimulation.RunId:"
    "Vehicle.CarlaSimulation.EgoVehicleId:"
    "Vehicle.CarlaSimulation.FrameId:"
    "Vehicle.CarlaSimulation.SimulationTime:"
    "Vehicle.CarlaSimulation.GnssFrameId:"
    "Vehicle.CarlaSimulation.GnssSimulationTime:")
  string(FIND "${overlay}" "${path}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "VSS overlay is missing ${path}")
  endif()
endforeach()

foreach(fragment IN ITEMS
    "datatype: string"
    "datatype: uint64"
    "datatype: double"
    "type: attribute"
    "type: sensor"
    "default: \"0.1\""
    "unit: s")
  string(FIND "${overlay}" "${fragment}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "VSS overlay is missing required field: ${fragment}")
  endif()
endforeach()

message(STATUS "Validated CARLA simulation VSS overlay")
