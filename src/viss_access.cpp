#include "carla_ego_runtime/viss_access.hpp"

#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace carla_ego_runtime {
namespace {

template <typename Type, auto Deleter>
using OpenSslPointer = std::unique_ptr<Type, decltype(Deleter)>;

constexpr std::string_view kUriPrefix = "urn:aosedge:demo:viss-client:v1:";

bool IsLowerHex(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

bool HasDigitalSignature(X509 *certificate) {
  OpenSslPointer<ASN1_BIT_STRING, ASN1_BIT_STRING_free> usage(
      static_cast<ASN1_BIT_STRING *>(
          X509_get_ext_d2i(certificate, NID_key_usage, nullptr, nullptr)),
      ASN1_BIT_STRING_free);
  return usage && ASN1_BIT_STRING_get_bit(usage.get(), 0) == 1;
}

bool HasClientAuthPurpose(X509 *certificate) {
  OpenSslPointer<EXTENDED_KEY_USAGE, EXTENDED_KEY_USAGE_free> purposes(
      static_cast<EXTENDED_KEY_USAGE *>(
          X509_get_ext_d2i(certificate, NID_ext_key_usage, nullptr, nullptr)),
      EXTENDED_KEY_USAGE_free);
  if (!purposes) {
    return false;
  }
  for (int index = 0; index < sk_ASN1_OBJECT_num(purposes.get()); ++index) {
    if (OBJ_obj2nid(sk_ASN1_OBJECT_value(purposes.get(), index)) ==
        NID_client_auth) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> ExactIdentityUri(X509 *certificate) {
  OpenSslPointer<GENERAL_NAMES, GENERAL_NAMES_free> names(
      static_cast<GENERAL_NAMES *>(X509_get_ext_d2i(
          certificate, NID_subject_alt_name, nullptr, nullptr)),
      GENERAL_NAMES_free);
  if (!names || sk_GENERAL_NAME_num(names.get()) != 1) {
    return std::nullopt;
  }
  const auto *name = sk_GENERAL_NAME_value(names.get(), 0);
  if (name == nullptr || name->type != GEN_URI ||
      name->d.uniformResourceIdentifier == nullptr) {
    return std::nullopt;
  }
  const auto *data = ASN1_STRING_get0_data(name->d.uniformResourceIdentifier);
  const auto size = ASN1_STRING_length(name->d.uniformResourceIdentifier);
  if (data == nullptr || size <= 0 ||
      std::memchr(data, '\0', static_cast<std::size_t>(size)) != nullptr) {
    return std::nullopt;
  }
  return std::string(reinterpret_cast<const char *>(data),
                     static_cast<std::size_t>(size));
}

std::optional<VissCertificateIdentity> ParseIdentityUri(std::string_view uri) {
  if (!uri.starts_with(kUriPrefix)) {
    return std::nullopt;
  }
  uri.remove_prefix(kUriPrefix.size());
  if (uri == "engineering-dashboard") {
    return VissCertificateIdentity{
        VissClientRole::EngineeringDashboard, {}, {}, {}};
  }
  if (uri == "qualification-client") {
    return VissCertificateIdentity{
        VissClientRole::QualificationClient, {}, {}, {}};
  }

  const auto role_end = uri.find(':');
  if (role_end == std::string_view::npos) {
    return std::nullopt;
  }
  const auto role = uri.substr(0, role_end);
  uri.remove_prefix(role_end + 1);
  const auto unit_end = uri.find(':');
  if (unit_end == std::string_view::npos) {
    return std::nullopt;
  }
  const auto unit = uri.substr(0, unit_end);
  const auto node = uri.substr(unit_end + 1);
  if (!IsCanonicalUuid(unit) || !IsCanonicalUuid(node)) {
    return std::nullopt;
  }

  VissClientRole parsed_role;
  if (role == "selected-platform-unit") {
    parsed_role = VissClientRole::SelectedPlatformUnit;
  } else if (role == "platform-update-runtime") {
    parsed_role = VissClientRole::PlatformUpdateRuntime;
  } else {
    return std::nullopt;
  }
  return VissCertificateIdentity{
      parsed_role, std::string(unit), std::string(node), {}};
}

std::optional<std::string> CertificateFingerprint(X509 *certificate) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_size = 0;
  if (X509_digest(certificate, EVP_sha256(), digest, &digest_size) != 1 ||
      digest_size != SHA256_DIGEST_LENGTH) {
    return std::nullopt;
  }
  std::string result;
  result.reserve(digest_size * 2);
  for (unsigned int index = 0; index < digest_size; ++index) {
    char encoded[3];
    std::snprintf(encoded, sizeof(encoded), "%02x", digest[index]);
    result.append(encoded, 2);
  }
  return result;
}

bool IsAnyOf(std::string_view path,
             const std::initializer_list<std::string_view> &values) {
  for (const auto value : values) {
    if (path == value) {
      return true;
    }
  }
  return false;
}

bool IsSelectedPlatformPath(std::string_view path) {
  return IsAnyOf(
      path, {"Vehicle.Speed",
             "Vehicle.Acceleration.Longitudinal",
             "Vehicle.Acceleration.Lateral",
             "Vehicle.Acceleration.Vertical",
             "Vehicle.Chassis.Accelerator.PedalPosition",
             "Vehicle.Chassis.Brake.PedalPosition",
             "Vehicle.Chassis.Axle.Row1.SteeringAngle",
             "Vehicle.Powertrain.Transmission.CurrentGear",
             "Vehicle.Powertrain.CombustionEngine.Speed",
             "Vehicle.CurrentLocation.Latitude",
             "Vehicle.CurrentLocation.Longitude",
             "Vehicle.CurrentLocation.Altitude",
             "Vehicle.Chassis.Axle.Row1.Wheel.Left.AngularSpeed",
             "Vehicle.Chassis.Axle.Row1.Wheel.Right.AngularSpeed",
             "Vehicle.Chassis.Axle.Row2.Wheel.Left.AngularSpeed",
             "Vehicle.Chassis.Axle.Row2.Wheel.Right.AngularSpeed",
             "Vehicle.Chassis.Axle.Row1.Wheel.Left.Speed",
             "Vehicle.Chassis.Axle.Row1.Wheel.Right.Speed",
             "Vehicle.Chassis.Axle.Row2.Wheel.Left.Speed",
             "Vehicle.Chassis.Axle.Row2.Wheel.Right.Speed",
             "Vehicle.CarlaSimulation.ProfileVersion",
             "Vehicle.CarlaSimulation.RunId",
             "Vehicle.CarlaSimulation.EgoVehicleId",
             "Vehicle.CarlaSimulation.FrameId",
             "Vehicle.CarlaSimulation.SimulationTime",
             "Vehicle.CarlaSimulation.GnssFrameId",
             "Vehicle.CarlaSimulation.GnssSimulationTime",
             "Vehicle.CarlaSimulation.Source.State",
             "Vehicle.CarlaSimulation.Source.Reason",
             "Vehicle.CarlaSimulation.Source.LastGoodFrameId",
             "Vehicle.CarlaSimulation.Source.LastGoodSimulationTime",
             "Vehicle.CarlaSimulation.Source.DataAgeMilliseconds",
             "Vehicle.CarlaSimulation.Pose.X",
             "Vehicle.CarlaSimulation.Pose.Y",
             "Vehicle.CarlaSimulation.Pose.Z",
             "Vehicle.CarlaSimulation.Pose.Roll",
             "Vehicle.CarlaSimulation.Pose.Pitch",
             "Vehicle.CarlaSimulation.Pose.Yaw",
             "Vehicle.CarlaSimulation.BoundingBox.Center.X",
             "Vehicle.CarlaSimulation.BoundingBox.Center.Y",
             "Vehicle.CarlaSimulation.BoundingBox.Center.Z",
             "Vehicle.CarlaSimulation.BoundingBox.Extent.X",
             "Vehicle.CarlaSimulation.BoundingBox.Extent.Y",
             "Vehicle.CarlaSimulation.BoundingBox.Extent.Z",
             "Vehicle.CarlaSimulation.BoundingBox.Rotation.Roll",
             "Vehicle.CarlaSimulation.BoundingBox.Rotation.Pitch",
             "Vehicle.CarlaSimulation.BoundingBox.Rotation.Yaw",
             "Vehicle.CarlaSimulation.Velocity.World.X",
             "Vehicle.CarlaSimulation.Velocity.World.Y",
             "Vehicle.CarlaSimulation.Velocity.World.Z",
             "Vehicle.CarlaSimulation.AngularVelocity.World.X",
             "Vehicle.CarlaSimulation.AngularVelocity.World.Y",
             "Vehicle.CarlaSimulation.AngularVelocity.World.Z",
             "Vehicle.CarlaSimulation.Control.Applied.SteeringCommand",
             "Vehicle.CarlaSimulation.Control.Applied.Handbrake",
             "Vehicle.CarlaSimulation.Control.Applied.Reverse",
             "Vehicle.CarlaSimulation.Control.Applied.ManualShift",
             "Vehicle.CarlaSimulation.VehicleLights.Mask",
             "Vehicle.CarlaSimulation.FrontWheel.Left.SteeringAngle",
             "Vehicle.CarlaSimulation.FrontWheel.Right.SteeringAngle",
             "Vehicle.CarlaSimulation.ChaosWheel.Row1.Left.Radius",
             "Vehicle.CarlaSimulation.ChaosWheel.Row1.Right.Radius",
             "Vehicle.CarlaSimulation.ChaosWheel.Row2.Left.Radius",
             "Vehicle.CarlaSimulation.ChaosWheel.Row2.Right.Radius",
             "Vehicle.CarlaSimulation.ChaosWheel.Row1.Left.LateralSlipAngle",
             "Vehicle.CarlaSimulation.ChaosWheel.Row1.Right.LateralSlipAngle",
             "Vehicle.CarlaSimulation.ChaosWheel.Row2.Left.LateralSlipAngle",
             "Vehicle.CarlaSimulation.ChaosWheel.Row2.Right.LateralSlipAngle",
             "Vehicle.CarlaSimulation.ChaosWheel.Row1.Left.LongitudinalSlip",
             "Vehicle.CarlaSimulation.ChaosWheel.Row1.Right.LongitudinalSlip",
             "Vehicle.CarlaSimulation.ChaosWheel.Row2.Left.LongitudinalSlip",
             "Vehicle.CarlaSimulation.ChaosWheel.Row2.Right.LongitudinalSlip"});
}

bool IsRuntimePath(std::string_view path) {
  return IsAnyOf(path,
                 {"Vehicle.Speed", "Vehicle.Chassis.Accelerator.PedalPosition",
                  "Vehicle.Chassis.Brake.PedalPosition",
                  "Vehicle.CarlaSimulation.FrameId",
                  "Vehicle.CarlaSimulation.Control.ActiveMode",
                  "Vehicle.CarlaSimulation.Control.TransitionState",
                  "Vehicle.CarlaSimulation.Control.Generation",
                  "Vehicle.CarlaSimulation.Reset.Generation",
                  "Vehicle.CarlaSimulation.Reset.InProgress",
                  "Vehicle.CarlaSimulation.Reset.Discontinuity"});
}

bool IsIndependentOnlyPath(std::string_view path) {
  return IsAnyOf(path, {"Vehicle.CarlaSimulation.Control.RequestedMode",
                        "Vehicle.CarlaSimulation.Control.LastReason",
                        "Vehicle.CarlaSimulation.World.Context",
                        "Vehicle.CarlaSimulation.Scenario.Id",
                        "Vehicle.CarlaSimulation.Scenario.State",
                        "Vehicle.CarlaSimulation.Scenario.Phase",
                        "Vehicle.CarlaSimulation.Scenario.Result",
                        "Vehicle.CarlaSimulation.Scenario.Generation",
                        "Vehicle.CarlaSimulation.Reset.Reason",
                        "Vehicle.CarlaSimulation.Reset.FrameId"});
}

bool IsKnownPath(std::string_view path) {
  return IsSelectedPlatformPath(path) || IsRuntimePath(path) ||
         IsIndependentOnlyPath(path);
}

} // namespace

bool IsSelectedBoundRole(VissClientRole role) {
  return role == VissClientRole::SelectedPlatformUnit ||
         role == VissClientRole::PlatformUpdateRuntime;
}

std::string_view VissRoleName(VissClientRole role) {
  switch (role) {
  case VissClientRole::Development:
    return "DEVELOPMENT";
  case VissClientRole::SelectedPlatformUnit:
    return "SELECTED_PLATFORM_UNIT";
  case VissClientRole::PlatformUpdateRuntime:
    return "PLATFORM_UPDATE_RUNTIME";
  case VissClientRole::EngineeringDashboard:
    return "ENGINEERING_DASHBOARD";
  case VissClientRole::QualificationClient:
    return "QUALIFICATION_CLIENT";
  }
  return "UNKNOWN";
}

bool IsCanonicalUuid(std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      continue;
    }
    if (!IsLowerHex(value[index])) {
      return false;
    }
  }
  return true;
}

bool IsCanonicalSha256(std::string_view value) {
  if (value.size() != SHA256_DIGEST_LENGTH * 2) {
    return false;
  }
  for (const auto character : value) {
    if (!IsLowerHex(character)) {
      return false;
    }
  }
  return true;
}

std::optional<VissCertificateIdentity>
InspectVissClientCertificate(X509 *certificate) {
  if (certificate == nullptr || !HasDigitalSignature(certificate) ||
      !HasClientAuthPurpose(certificate)) {
    return std::nullopt;
  }
  const auto uri = ExactIdentityUri(certificate);
  const auto fingerprint = CertificateFingerprint(certificate);
  if (!uri.has_value() || !fingerprint.has_value()) {
    return std::nullopt;
  }
  auto identity = ParseIdentityUri(*uri);
  if (!identity.has_value()) {
    return std::nullopt;
  }
  identity->certificate_sha256 = *fingerprint;
  return identity;
}

bool VissRoleMayRead(VissClientRole role, std::string_view path) {
  switch (role) {
  case VissClientRole::Development:
    return true;
  case VissClientRole::SelectedPlatformUnit:
    return IsSelectedPlatformPath(path);
  case VissClientRole::PlatformUpdateRuntime:
    return IsRuntimePath(path);
  case VissClientRole::EngineeringDashboard:
  case VissClientRole::QualificationClient:
    return IsKnownPath(path);
  }
  return false;
}

} // namespace carla_ego_runtime
