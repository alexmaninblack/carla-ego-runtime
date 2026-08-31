#pragma once

#include <openssl/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace carla_ego_runtime {

enum class VissClientRole {
  Development,
  SelectedPlatformUnit,
  PlatformUpdateRuntime,
  EngineeringDashboard,
  QualificationClient,
};

struct VissCertificateIdentity {
  VissClientRole role = VissClientRole::Development;
  std::string unit_id;
  std::string node_id;
  std::string certificate_sha256;
};

struct VissSessionAccess {
  VissClientRole role = VissClientRole::Development;
  std::uint64_t assignment_generation = 0;
  std::uint64_t minimum_frame_exclusive = 0;
};

bool IsSelectedBoundRole(VissClientRole role);
std::string_view VissRoleName(VissClientRole role);
bool IsCanonicalUuid(std::string_view value);
bool IsCanonicalSha256(std::string_view value);

// The caller owns the X509 object. Failure is intentionally reason-free at the
// network boundary so rejected identities do not reveal the accepted policy.
std::optional<VissCertificateIdentity>
InspectVissClientCertificate(X509 *certificate);

// This is the compiled D4-006 read policy. Unknown paths fail closed in every
// strict role; Development is the explicit loopback-only compatibility mode.
bool VissRoleMayRead(VissClientRole role, std::string_view path);

} // namespace carla_ego_runtime
