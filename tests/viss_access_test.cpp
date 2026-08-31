#include "carla_ego_runtime/viss_access.hpp"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <typename Type, auto Deleter>
using OpenSslPointer = std::unique_ptr<Type, decltype(Deleter)>;

OpenSslPointer<X509, X509_free>
Certificate(std::string uri, std::string key_usage = "digitalSignature",
            std::string extended_key_usage = "clientAuth",
            std::optional<std::string> second_uri = std::nullopt) {
  OpenSslPointer<EVP_PKEY_CTX, EVP_PKEY_CTX_free> key_context(
      EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
  EVP_PKEY *raw_key = nullptr;
  if (!key_context || EVP_PKEY_keygen_init(key_context.get()) <= 0 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(key_context.get(), 2048) <= 0 ||
      EVP_PKEY_keygen(key_context.get(), &raw_key) <= 0) {
    throw std::runtime_error("could not generate access-test key");
  }
  OpenSslPointer<EVP_PKEY, EVP_PKEY_free> key(raw_key, EVP_PKEY_free);
  OpenSslPointer<X509, X509_free> certificate(X509_new(), X509_free);
  if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
      ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
      X509_gmtime_adj(X509_get_notBefore(certificate.get()), -60) == nullptr ||
      X509_gmtime_adj(X509_get_notAfter(certificate.get()), 3600) == nullptr ||
      X509_set_pubkey(certificate.get(), key.get()) != 1) {
    throw std::runtime_error("could not initialize access-test certificate");
  }
  auto *name = X509_get_subject_name(certificate.get());
  constexpr unsigned char common_name[] = "ignored.example";
  if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, common_name, -1, -1,
                                 0) != 1 ||
      X509_set_issuer_name(certificate.get(), name) != 1) {
    throw std::runtime_error("could not name access-test certificate");
  }

  X509V3_CTX extension_context;
  X509V3_set_ctx(&extension_context, certificate.get(), certificate.get(),
                 nullptr, nullptr, 0);
  const auto add_extension = [&](int nid, const std::string &value) {
    if (value.empty()) {
      return;
    }
    OpenSslPointer<X509_EXTENSION, X509_EXTENSION_free> extension(
        X509V3_EXT_conf_nid(nullptr, &extension_context, nid,
                            const_cast<char *>(value.c_str())),
        X509_EXTENSION_free);
    if (!extension ||
        X509_add_ext(certificate.get(), extension.get(), -1) != 1) {
      throw std::runtime_error("could not add access-test extension");
    }
  };
  add_extension(NID_key_usage, key_usage);
  add_extension(NID_ext_key_usage, extended_key_usage);
  auto san = "URI:" + uri;
  if (second_uri.has_value()) {
    san += ",URI:" + *second_uri;
  }
  add_extension(NID_subject_alt_name, san);
  if (X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
    throw std::runtime_error("could not sign access-test certificate");
  }
  return certificate;
}

} // namespace

int main() {
  using namespace carla_ego_runtime;
  constexpr auto unit = "11111111-2222-4333-8444-555555555555";
  constexpr auto node = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
  const auto selected_uri =
      std::string("urn:aosedge:demo:viss-client:v1:selected-platform-unit:") +
      unit + ":" + node;

  Check(IsCanonicalUuid(unit), "canonical lowercase UUID accepted");
  Check(!IsCanonicalUuid("11111111-2222-4333-8444-55555555555A"),
        "uppercase UUID rejected");
  Check(IsCanonicalSha256(std::string(64, 'a')),
        "canonical lowercase fingerprint accepted");
  Check(!IsCanonicalSha256(std::string(63, 'a')), "short fingerprint rejected");

  auto selected_certificate = Certificate(selected_uri);
  const auto selected =
      InspectVissClientCertificate(selected_certificate.get());
  Check(selected.has_value(), "selected certificate identity accepted");
  if (selected.has_value()) {
    Check(selected->role == VissClientRole::SelectedPlatformUnit,
          "selected role parsed from URI only");
    Check(selected->unit_id == unit && selected->node_id == node,
          "selected Unit and Node parsed canonically");
    Check(IsCanonicalSha256(selected->certificate_sha256),
          "leaf DER fingerprint is canonical SHA-256");
  }

  auto dashboard =
      Certificate("urn:aosedge:demo:viss-client:v1:engineering-dashboard");
  const auto dashboard_identity = InspectVissClientCertificate(dashboard.get());
  Check(dashboard_identity.has_value() &&
            dashboard_identity->role == VissClientRole::EngineeringDashboard,
        "independent dashboard URI accepted");

  auto wrong_usage = Certificate(selected_uri, "keyEncipherment");
  Check(!InspectVissClientCertificate(wrong_usage.get()).has_value(),
        "missing digitalSignature rejected");
  auto wrong_purpose =
      Certificate(selected_uri, "digitalSignature", "serverAuth");
  Check(!InspectVissClientCertificate(wrong_purpose.get()).has_value(),
        "missing clientAuth EKU rejected");
  auto absent_purpose = Certificate(selected_uri, "digitalSignature", "");
  Check(!InspectVissClientCertificate(absent_purpose.get()).has_value(),
        "absent client EKU rejected");
  auto multiple =
      Certificate(selected_uri, "digitalSignature", "clientAuth",
                  "urn:aosedge:demo:viss-client:v1:engineering-dashboard");
  Check(!InspectVissClientCertificate(multiple.get()).has_value(),
        "multiple identity SANs rejected");
  auto unknown = Certificate("urn:aosedge:demo:viss-client:v1:unknown");
  Check(!InspectVissClientCertificate(unknown.get()).has_value(),
        "unknown URI role rejected");

  Check(VissRoleMayRead(VissClientRole::PlatformUpdateRuntime,
                        "Vehicle.CarlaSimulation.Control.ActiveMode"),
        "Runtime reads Safe Stop control state");
  Check(!VissRoleMayRead(VissClientRole::PlatformUpdateRuntime,
                         "Vehicle.Acceleration.Longitudinal"),
        "Runtime cannot expand beyond exact ten paths");
  Check(VissRoleMayRead(VissClientRole::SelectedPlatformUnit,
                        "Vehicle.Acceleration.Longitudinal"),
        "selected VDP reads its physical path");
  Check(!VissRoleMayRead(VissClientRole::SelectedPlatformUnit,
                         "Vehicle.CarlaSimulation.Control.ActiveMode"),
        "selected VDP cannot read Runtime-only control path");
  Check(VissRoleMayRead(VissClientRole::EngineeringDashboard,
                        "Vehicle.CarlaSimulation.Reset.Reason"),
        "dashboard reads accepted engineering-only path");
  Check(!VissRoleMayRead(VissClientRole::EngineeringDashboard,
                         "Vehicle.Future.Secret"),
        "unknown strict path fails closed");
  Check(VissRoleMayRead(VissClientRole::QualificationClient,
                        "Vehicle.CarlaSimulation.Reset.Reason"),
        "qualification client receives the independent read-only policy");
  Check(!VissRoleMayRead(VissClientRole::QualificationClient,
                         "Vehicle.Future.Secret"),
        "qualification client also fails closed on unknown paths");
  Check(!VissRoleMayRead(VissClientRole::SelectedPlatformUnit,
                         "Vehicle.CarlaSimulation.Source.FutureSecret"),
        "unknown descendant of a known branch fails closed");
  Check(VissRoleMayRead(VissClientRole::Development,
                        "Vehicle.Future.DevelopmentFixture"),
        "explicit development profile retains generic loopback access");

  if (failures == 0) {
    std::cout << "VISS access tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
