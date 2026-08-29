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
    "Vehicle.CarlaSimulation.GnssSimulationTime:"
    "Vehicle.CarlaSimulation.Control:"
    "Vehicle.CarlaSimulation.Control.ActiveMode:"
    "Vehicle.CarlaSimulation.Control.TransitionState:"
    "Vehicle.CarlaSimulation.Control.Generation:"
    "Vehicle.CarlaSimulation.Reset:"
    "Vehicle.CarlaSimulation.Reset.Generation:"
    "Vehicle.CarlaSimulation.Reset.InProgress:"
    "Vehicle.CarlaSimulation.Reset.Discontinuity:"
    "Vehicle.CarlaSimulation.ChaosWheel:"
    "Vehicle.CarlaSimulation.ChaosWheel.Row1.Left.LateralSlipAngle:"
    "Vehicle.CarlaSimulation.ChaosWheel.Row1.Left.LongitudinalSlip:"
    "Vehicle.CarlaSimulation.ChaosWheel.Row1.Right.LateralSlipAngle:"
    "Vehicle.CarlaSimulation.ChaosWheel.Row1.Right.LongitudinalSlip:"
    "Vehicle.CarlaSimulation.ChaosWheel.Row2.Left.LateralSlipAngle:"
    "Vehicle.CarlaSimulation.ChaosWheel.Row2.Left.LongitudinalSlip:"
    "Vehicle.CarlaSimulation.ChaosWheel.Row2.Right.LateralSlipAngle:"
    "Vehicle.CarlaSimulation.ChaosWheel.Row2.Right.LongitudinalSlip:")
  string(FIND "${overlay}" "${path}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "VSS overlay is missing ${path}")
  endif()
endforeach()

foreach(fragment IN ITEMS
    "datatype: string"
    "datatype: uint64"
    "datatype: double"
    "datatype: boolean"
    "type: attribute"
    "type: sensor"
    "default: \"0.2\""
    "unit: s")
  string(FIND "${overlay}" "${fragment}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "VSS overlay is missing required field: ${fragment}")
  endif()
endforeach()

function(require_exact_sensor path datatype)
  string(REPLACE "." "\\." escaped_path "${path}")
  string(REGEX MATCH
    "${escaped_path}:[ \t]*[\r\n]+[ \t]+datatype:[ \t]+${datatype}[ \t]*[\r\n]+[ \t]+type:[ \t]+sensor([ \t]*[\r\n]|$)"
    sensor_definition
    "${overlay}")
  if(NOT sensor_definition)
    message(FATAL_ERROR
      "VSS overlay path ${path} must have datatype ${datatype} and type sensor")
  endif()
endfunction()

require_exact_sensor("Vehicle.CarlaSimulation.Control.ActiveMode" "string")
require_exact_sensor("Vehicle.CarlaSimulation.Control.TransitionState" "string")
require_exact_sensor("Vehicle.CarlaSimulation.Control.Generation" "uint64")
require_exact_sensor("Vehicle.CarlaSimulation.Reset.Generation" "uint64")
require_exact_sensor("Vehicle.CarlaSimulation.Reset.InProgress" "boolean")
require_exact_sensor("Vehicle.CarlaSimulation.Reset.Discontinuity" "boolean")

foreach(value IN ITEMS
    "SAFE_STOP"
    "SCENARIO"
    "MANUAL"
    "AUTOPILOT"
    "STABLE"
    "PREPARING"
    "FAILED")
  string(FIND "${overlay}" "\"${value}\"" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "VSS overlay is missing accepted control value: ${value}")
  endif()
endforeach()

message(STATUS "Validated CARLA simulation VSS overlay")
