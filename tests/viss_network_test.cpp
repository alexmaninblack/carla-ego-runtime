#include "carla_ego_runtime/viss_access.hpp"
#include "carla_ego_runtime/viss_server.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <sys/stat.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using LocalSocket = asio::local::stream_protocol;
using namespace std::chrono_literals;

int failures = 0;

void Check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <typename Type, auto Deleter>
using OpenSslPointer = std::unique_ptr<Type, decltype(Deleter)>;

using Key = OpenSslPointer<EVP_PKEY, EVP_PKEY_free>;
using Certificate = OpenSslPointer<X509, X509_free>;

struct PemIdentity {
  std::filesystem::path certificate;
  std::filesystem::path private_key;
  std::string fingerprint;
};

class EphemeralPki {
public:
  EphemeralPki() {
    std::random_device random;
    directory_ = std::filesystem::temp_directory_path() /
                 ("carla-viss-mtls-test-" +
                  std::to_string(static_cast<unsigned long long>(random())));
    std::filesystem::create_directory(directory_);
    if (chmod(directory_.c_str(), 0700) != 0) {
      throw std::runtime_error("could not secure temporary PKI directory");
    }
  }

  ~EphemeralPki() {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  EphemeralPki(const EphemeralPki &) = delete;
  EphemeralPki &operator=(const EphemeralPki &) = delete;

  void Initialize() {
    ca_key_ = GenerateKey();
    ca_certificate_ = MakeCertificate(
        ca_key_.get(), nullptr, nullptr, "test VISS root", -60, 3600,
        "critical,CA:TRUE", "critical,keyCertSign,cRLSign", {}, {});
    WriteCertificate(directory_ / "ca.pem", ca_certificate_.get());
  }

  PemIdentity Server() {
    return Leaf("server", "DNS:localhost", "serverAuth", ca_key_.get(),
                ca_certificate_.get(), -60, 3600);
  }

  PemIdentity Client(std::string name, std::string uri,
                     std::string eku = "clientAuth", long not_before = -60,
                     long not_after = 3600, std::string additional_san = {}) {
    auto san = "URI:" + std::move(uri);
    if (!additional_san.empty()) {
      san += "," + additional_san;
    }
    return Leaf(std::move(name), std::move(san), std::move(eku), ca_key_.get(),
                ca_certificate_.get(), not_before, not_after);
  }

  PemIdentity SelfSignedClient(std::string name, std::string uri) {
    auto key = GenerateKey();
    auto certificate = MakeCertificate(
        key.get(), key.get(), nullptr, name, -60, 3600, "critical,CA:FALSE",
        "critical,digitalSignature", "clientAuth", "URI:" + uri);
    return WriteIdentity(std::move(name), key.get(), certificate.get());
  }

  PemIdentity ForeignCaClient(std::string name, std::string uri) {
    auto foreign_ca_key = GenerateKey();
    auto foreign_ca = MakeCertificate(
        foreign_ca_key.get(), nullptr, nullptr, "foreign VISS root", -60, 3600,
        "critical,CA:TRUE", "critical,keyCertSign,cRLSign", {}, {});
    auto key = GenerateKey();
    auto certificate = MakeCertificate(
        key.get(), foreign_ca_key.get(), foreign_ca.get(), name, -60, 3600,
        "critical,CA:FALSE", "critical,digitalSignature", "clientAuth",
        "URI:" + uri);
    return WriteIdentity(std::move(name), key.get(), certificate.get());
  }

  PemIdentity ClientWithoutSan(std::string name) {
    return Leaf(std::move(name), {}, "clientAuth", ca_key_.get(),
                ca_certificate_.get(), -60, 3600);
  }

  const std::filesystem::path &directory() const { return directory_; }
  std::filesystem::path ca_file() const { return directory_ / "ca.pem"; }
  std::filesystem::path assignment_socket() const {
    return directory_ / "assignment.sock";
  }

private:
  static Key GenerateKey() {
    OpenSslPointer<EVP_PKEY_CTX, EVP_PKEY_CTX_free> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *raw = nullptr;
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 ||
        EVP_PKEY_keygen(context.get(), &raw) <= 0) {
      throw std::runtime_error("could not generate temporary RSA key");
    }
    return Key(raw, EVP_PKEY_free);
  }

  static void AddExtension(X509 *certificate, X509 *issuer, int nid,
                           const std::string &value) {
    if (value.empty()) {
      return;
    }
    X509V3_CTX context;
    X509V3_set_ctx(&context, issuer == nullptr ? certificate : issuer,
                   certificate, nullptr, nullptr, 0);
    OpenSslPointer<X509_EXTENSION, X509_EXTENSION_free> extension(
        X509V3_EXT_conf_nid(nullptr, &context, nid,
                            const_cast<char *>(value.c_str())),
        X509_EXTENSION_free);
    if (!extension || X509_add_ext(certificate, extension.get(), -1) != 1) {
      throw std::runtime_error("could not add temporary certificate extension");
    }
  }

  static Certificate MakeCertificate(
      EVP_PKEY *subject_key, EVP_PKEY *issuer_key, X509 *issuer,
      const std::string &common_name, long not_before, long not_after,
      const std::string &basic_constraints, const std::string &key_usage,
      const std::string &extended_key_usage, const std::string &san) {
    static long serial = 1;
    Certificate certificate(X509_new(), X509_free);
    if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial++) !=
            1 ||
        X509_gmtime_adj(X509_get_notBefore(certificate.get()), not_before) ==
            nullptr ||
        X509_gmtime_adj(X509_get_notAfter(certificate.get()), not_after) ==
            nullptr ||
        X509_set_pubkey(certificate.get(), subject_key) != 1) {
      throw std::runtime_error("could not initialize temporary certificate");
    }
    auto *name = X509_get_subject_name(certificate.get());
    if (X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char *>(common_name.c_str()), -1,
            -1, 0) != 1 ||
        X509_set_issuer_name(
            certificate.get(),
            issuer == nullptr ? name : X509_get_subject_name(issuer)) != 1) {
      throw std::runtime_error("could not name temporary certificate");
    }
    AddExtension(certificate.get(), issuer, NID_basic_constraints,
                 basic_constraints);
    AddExtension(certificate.get(), issuer, NID_key_usage, key_usage);
    AddExtension(certificate.get(), issuer, NID_ext_key_usage,
                 extended_key_usage);
    AddExtension(certificate.get(), issuer, NID_subject_alt_name, san);
    auto *signing_key = issuer_key == nullptr ? subject_key : issuer_key;
    if (X509_sign(certificate.get(), signing_key, EVP_sha256()) <= 0) {
      throw std::runtime_error("could not sign temporary certificate");
    }
    return certificate;
  }

  static std::string Fingerprint(X509 *certificate) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (X509_digest(certificate, EVP_sha256(), digest, &size) != 1) {
      throw std::runtime_error("could not fingerprint temporary certificate");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (unsigned int index = 0; index < size; ++index) {
      result.push_back(hex[digest[index] >> 4]);
      result.push_back(hex[digest[index] & 0x0f]);
    }
    return result;
  }

  static void WriteCertificate(const std::filesystem::path &path,
                               X509 *certificate) {
    std::unique_ptr<std::FILE, decltype(&std::fclose)> output(
        std::fopen(path.c_str(), "wb"), std::fclose);
    if (!output || PEM_write_X509(output.get(), certificate) != 1) {
      throw std::runtime_error("could not write temporary certificate");
    }
  }

  PemIdentity WriteIdentity(std::string name, EVP_PKEY *key,
                            X509 *certificate) {
    PemIdentity result{directory_ / (name + ".pem"),
                       directory_ / (name + "-key.pem"),
                       Fingerprint(certificate)};
    WriteCertificate(result.certificate, certificate);
    std::unique_ptr<std::FILE, decltype(&std::fclose)> output(
        std::fopen(result.private_key.c_str(), "wb"), std::fclose);
    if (!output ||
        PEM_write_PrivateKey(output.get(), key, nullptr, nullptr, 0, nullptr,
                             nullptr) != 1 ||
        chmod(result.private_key.c_str(), 0600) != 0) {
      throw std::runtime_error("could not write temporary private key");
    }
    return result;
  }

  PemIdentity Leaf(std::string name, std::string san, std::string eku,
                   EVP_PKEY *issuer_key, X509 *issuer, long not_before,
                   long not_after) {
    auto key = GenerateKey();
    auto certificate = MakeCertificate(
        key.get(), issuer_key, issuer, name, not_before, not_after,
        "critical,CA:FALSE", "critical,digitalSignature", eku, san);
    return WriteIdentity(std::move(name), key.get(), certificate.get());
  }

  std::filesystem::path directory_;
  Key ca_key_{nullptr, EVP_PKEY_free};
  Certificate ca_certificate_{nullptr, X509_free};
};

void Publish(carla_ego_runtime::LatestVssSignalStore &store,
             std::uint64_t frame, double speed) {
  carla_ego_runtime::VssSnapshot snapshot;
  snapshot.frame_id = frame;
  snapshot.simulation_time_s = static_cast<double>(frame) * 0.05;
  snapshot.timestamp = "2026-08-12T12:34:56.789Z";
  snapshot.data_points = {
      {"Vehicle.Speed", speed, snapshot.timestamp},
      {"Vehicle.CurrentLocation.Latitude", 52.520008, snapshot.timestamp},
      {"Vehicle.CarlaSimulation.FrameId", frame, snapshot.timestamp}};
  Check(store.Publish(std::move(snapshot)), "network snapshot published");
}

class Client {
public:
  Client(std::uint16_t port, const std::filesystem::path &ca_file,
         const std::optional<PemIdentity> &identity,
         std::string subprotocol = "VISSv3")
      : tls_context_(BuildContext(ca_file, identity)),
        websocket_(io_context_, tls_context_) {
    tcp::resolver resolver(io_context_);
    const auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));
    beast::get_lowest_layer(websocket_).connect(endpoints);
    if (SSL_set_tlsext_host_name(websocket_.next_layer().native_handle(),
                                 "localhost") != 1) {
      throw std::runtime_error("could not set TLS SNI");
    }
    websocket_.next_layer().set_verify_callback(
        ssl::host_name_verification("localhost"));
    websocket_.next_layer().handshake(ssl::stream_base::client);
    websocket_.set_option(websocket::stream_base::decorator(
        [subprotocol =
             std::move(subprotocol)](websocket::request_type &request) {
          request.set(http::field::sec_websocket_protocol, subprotocol);
        }));
    websocket_.handshake(handshake_response_, "localhost", "/");
  }

  std::string Request(std::string_view request) {
    websocket_.write(asio::buffer(request));
    return Read();
  }

  std::string Read() {
    beast::flat_buffer buffer;
    websocket_.read(buffer);
    return beast::buffers_to_string(buffer.data());
  }

  std::string tls_version() {
    return SSL_get_version(websocket_.next_layer().native_handle());
  }

  void Close() {
    boost::system::error_code ignored;
    websocket_.close(websocket::close_code::normal, ignored);
  }

private:
  static ssl::context BuildContext(const std::filesystem::path &ca_file,
                                   const std::optional<PemIdentity> &identity) {
    ssl::context context(ssl::context::tls_client);
    context.set_verify_mode(ssl::verify_peer);
    context.load_verify_file(ca_file.string());
    if (identity.has_value()) {
      context.use_certificate_chain_file(identity->certificate.string());
      context.use_private_key_file(identity->private_key.string(),
                                   ssl::context::pem);
      if (SSL_CTX_check_private_key(context.native_handle()) != 1) {
        throw std::runtime_error("temporary client key does not match leaf");
      }
    }
    return context;
  }

  asio::io_context io_context_;
  ssl::context tls_context_;
  websocket::stream<beast::ssl_stream<beast::tcp_stream>> websocket_;
  websocket::response_type handshake_response_;
};

bool ConnectionRejected(std::uint16_t port,
                        const std::filesystem::path &ca_file,
                        const std::optional<PemIdentity> &identity,
                        std::string subprotocol = "VISSv3") {
  try {
    Client client(port, ca_file, identity, std::move(subprotocol));
    client.Close();
    return false;
  } catch (const std::exception &) {
    std::this_thread::sleep_for(15ms);
    return true;
  }
}

json::object ParseObject(const std::string &payload) {
  boost::system::error_code error;
  auto parsed = json::parse(payload, error);
  Check(!error && parsed.is_object(), "network response is a JSON object");
  return !error && parsed.is_object() ? parsed.as_object() : json::object{};
}

std::string StringAt(const json::object &object, std::string_view key) {
  const auto *value = object.if_contains(key);
  Check(value != nullptr && value->is_string(),
        "network response has string field " + std::string(key));
  if (value == nullptr || !value->is_string()) {
    return {};
  }
  return {value->as_string().data(), value->as_string().size()};
}

std::string AssignmentRequest(const std::filesystem::path &socket_file,
                              std::string request) {
  asio::io_context io_context;
  LocalSocket::socket socket(io_context);
  socket.connect(LocalSocket::endpoint(socket_file.string()));
  request.push_back('\n');
  asio::write(socket, asio::buffer(request));
  boost::system::error_code error;
  socket.shutdown(LocalSocket::socket::shutdown_send, error);
  std::string response;
  std::array<char, 1024> chunk{};
  while (true) {
    const auto size = socket.read_some(asio::buffer(chunk), error);
    response.append(chunk.data(), size);
    if (error == asio::error::eof) {
      break;
    }
    if (error) {
      throw boost::system::system_error(error);
    }
  }
  return response;
}

std::string SelectRequest(std::uint64_t generation, std::string_view unit,
                          std::string_view node, const PemIdentity &selected,
                          const PemIdentity &runtime) {
  json::object source;
  source["unitId"] = unit;
  source["nodeId"] = node;
  source["selectedPlatformUnitCertificateSha256"] = selected.fingerprint;
  source["platformUpdateRuntimeCertificateSha256"] = runtime.fingerprint;
  json::object request;
  request["schemaVersion"] = 1;
  request["action"] = "select";
  request["requestId"] = "network-select";
  request["expectedAssignmentGeneration"] = generation;
  request["selectedSource"] = std::move(source);
  return json::serialize(request);
}

std::string DetachRequest(std::uint64_t generation, std::string_view unit,
                          std::string_view node) {
  json::object source;
  source["unitId"] = unit;
  source["nodeId"] = node;
  json::object request;
  request["schemaVersion"] = 1;
  request["action"] = "detach";
  request["requestId"] = "network-detach";
  request["expectedAssignmentGeneration"] = generation;
  request["selectedSource"] = std::move(source);
  return json::serialize(request);
}

bool IsUnavailable(const std::string &payload) {
  const auto response = ParseObject(payload);
  const auto *error = response.if_contains("error");
  return error != nullptr && error->is_object() &&
         StringAt(error->as_object(), "reason") == "unavailable_data";
}

} // namespace

int main() {
  try {
    constexpr auto unit1 = "11111111-2222-4333-8444-555555555555";
    constexpr auto node1 = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    constexpr auto unit2 = "22222222-3333-4444-8555-666666666666";
    constexpr auto node2 = "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff";
    const auto selected_uri = [](std::string_view role, std::string_view unit,
                                 std::string_view node) {
      return std::string("urn:aosedge:demo:viss-client:v1:") +
             std::string(role) + ":" + std::string(unit) + ":" +
             std::string(node);
    };

    EphemeralPki pki;
    pki.Initialize();
    const auto server_identity = pki.Server();
    const auto dashboard = pki.Client(
        "dashboard", "urn:aosedge:demo:viss-client:v1:engineering-dashboard");
    const auto qualification =
        pki.Client("qualification",
                   "urn:aosedge:demo:viss-client:v1:qualification-client");
    const auto selected1 = pki.Client(
        "selected-1", selected_uri("selected-platform-unit", unit1, node1));
    const auto runtime1 = pki.Client(
        "runtime-1", selected_uri("platform-update-runtime", unit1, node1));
    const auto selected2 = pki.Client(
        "selected-2", selected_uri("selected-platform-unit", unit2, node2));
    const auto runtime2 = pki.Client(
        "runtime-2", selected_uri("platform-update-runtime", unit2, node2));
    const auto unknown =
        pki.Client("unknown", "urn:aosedge:demo:viss-client:v1:unknown-role");
    const auto additional_san =
        pki.Client("additional-san",
                   "urn:aosedge:demo:viss-client:v1:engineering-dashboard",
                   "clientAuth", -60, 3600, "DNS:localhost");
    const auto wrong_eku = pki.Client(
        "wrong-eku", "urn:aosedge:demo:viss-client:v1:engineering-dashboard",
        "serverAuth");
    const auto expired = pki.Client(
        "expired", "urn:aosedge:demo:viss-client:v1:engineering-dashboard",
        "clientAuth", -3600, -60);
    const auto not_yet_valid =
        pki.Client("not-yet-valid",
                   "urn:aosedge:demo:viss-client:v1:engineering-dashboard",
                   "clientAuth", 3600, 7200);
    const auto self_signed = pki.SelfSignedClient(
        "self-signed", "urn:aosedge:demo:viss-client:v1:engineering-dashboard");
    const auto wrong_ca = pki.ForeignCaClient(
        "wrong-ca", "urn:aosedge:demo:viss-client:v1:engineering-dashboard");
    const auto missing_san = pki.ClientWithoutSan("missing-san");

    carla_ego_runtime::LatestVssSignalStore store;
    Publish(store, 1, 10.0);
    carla_ego_runtime::VissServerConfig config;
    config.port = 0;
    config.certificate_chain_file = server_identity.certificate.string();
    config.private_key_file = server_identity.private_key.string();
    config.strict_client_authentication = true;
    config.client_trust_bundle_file = pki.ca_file().string();
    config.assignment_socket_file = pki.assignment_socket().string();
    config.initial_assignment_generation = 0;
    config.engineering_dashboard_certificate_sha256 = dashboard.fingerprint;
    config.qualification_certificate_sha256 = qualification.fingerprint;
    config.max_clients = 4;
    config.max_pending_messages_per_client = 2;
    carla_ego_runtime::VissServer server(store, config);
    server.Start();
    Check(server.bound_port() != 0, "strict server binds ephemeral TLS port");

    Check(ConnectionRejected(server.bound_port(), pki.ca_file(), std::nullopt),
          "missing client certificate rejected");
    Check(ConnectionRejected(server.bound_port(), pki.ca_file(), self_signed),
          "self-signed client rejected");
    Check(ConnectionRejected(server.bound_port(), pki.ca_file(), wrong_ca),
          "client signed by wrong CA rejected");
    Check(ConnectionRejected(server.bound_port(), pki.ca_file(), expired),
          "expired client rejected");
    Check(ConnectionRejected(server.bound_port(), pki.ca_file(), not_yet_valid),
          "not-yet-valid client rejected");
    Check(ConnectionRejected(server.bound_port(), pki.ca_file(), wrong_eku),
          "wrong client EKU rejected");
    Check(ConnectionRejected(server.bound_port(), pki.ca_file(), unknown),
          "unknown role SAN rejected");
    Check(ConnectionRejected(server.bound_port(), pki.ca_file(), missing_san),
          "missing client identity SAN rejected");
    Check(
        ConnectionRejected(server.bound_port(), pki.ca_file(), additional_san),
        "additional SAN rejected");
    Check(ConnectionRejected(server.bound_port(), pki.ca_file(), selected1),
          "selected certificate cannot select itself while detached");

    auto assignment = ParseObject(
        AssignmentRequest(pki.assignment_socket(),
                          SelectRequest(0, unit1, node1, selected1, runtime1)));
    Check(StringAt(assignment, "result") == "ACCEPTED",
          "first Unit assignment accepted");

    Client dashboard_client(server.bound_port(), pki.ca_file(), dashboard);
    Check(dashboard_client.tls_version() == "TLSv1.2" ||
              dashboard_client.tls_version() == "TLSv1.3",
          "strict mTLS negotiates TLS 1.2 or newer");
    Client qualification_client(server.bound_port(), pki.ca_file(),
                                qualification);
    Client selected_client(server.bound_port(), pki.ca_file(), selected1);
    Client runtime_client(server.bound_port(), pki.ca_file(), runtime1);

    Check(
        IsUnavailable(selected_client.Request(
            R"({"action":"get","path":"Vehicle.Speed","requestId":"selected-floor"})")),
        "selected role cannot receive pre-assignment cached frame");
    Check(
        IsUnavailable(runtime_client.Request(
            R"({"action":"get","path":"Vehicle.CurrentLocation.Latitude","requestId":"runtime-deny"})")),
        "Runtime cannot read a selected-only path");
    auto response = ParseObject(dashboard_client.Request(
        R"({"action":"get","path":"Vehicle.Speed","requestId":"dashboard-continuity"})"));
    Check(StringAt(response, "action") == "get",
          "Dashboard remains independent of selected frame floor");
    response = ParseObject(qualification_client.Request(
        R"({"action":"get","path":"Vehicle.CarlaSimulation.Reset.Discontinuity","requestId":"qualification-read"})"));
    Check(StringAt(response, "action") == "get",
          "qualification role is admitted with its independent read policy");

    Publish(store, 2, 20.0);
    server.NotifySnapshot();
    response = ParseObject(selected_client.Request(
        R"({"action":"get","path":"Vehicle.Speed","requestId":"selected-new-frame"})"));
    Check(StringAt(response, "action") == "get",
          "selected role receives first later complete frame");
    response = ParseObject(runtime_client.Request(
        R"({"action":"get","path":"Vehicle.Speed","requestId":"runtime-allowed"})"));
    Check(StringAt(response, "action") == "get",
          "Runtime receives an allowed Safe Stop path");

    const auto advisory_now = std::chrono::system_clock::now();
    const json::object advisory_request{
        {"decisionId", "network-fixture-only"},
        {"expiresAt", carla_ego_runtime::FormatIso8601Utc(advisory_now + 30s)},
        {"issuedAt", carla_ego_runtime::FormatIso8601Utc(advisory_now)},
        {"modelVersion", "1.0.0"}, {"operation", "SET"},
        {"producerEpoch", "22222222-2222-4222-8222-222222222222"},
        {"reasonCode", "PREDICTED_BRAKE_DEGRADATION"},
        {"recommendation", "INSPECTION_RECOMMENDED"},
        {"requestId", "11111111-1111-4111-8111-111111111111"},
        {"schemaVersion", 1}, {"sequence", 1}, {"serviceVersion", "47.0.0"}};
    const auto advisory_set = json::serialize(json::object{
        {"action", "set"}, {"requestId", "advisory-network"},
        {"path", "Vehicle.OEM.BrakeHealth.Advisory.Request"},
        {"value", json::serialize(advisory_request)}});
    Check(ParseObject(dashboard_client.Request(advisory_set)).contains("error"),
          "authenticated dashboard cannot submit advisory Set");
    Check(ParseObject(runtime_client.Request(advisory_set)).contains("error"),
          "authenticated update runtime cannot submit advisory Set");
    Check(!ParseObject(selected_client.Request(advisory_set)).contains("error"),
          "selected VDP can submit one typed advisory over mTLS");
    const auto advisory_read = R"({"action":"get","path":"Vehicle.OEM.BrakeHealth.Advisory.GatewayStatus","requestId":"advisory-status"})";
    response = ParseObject(dashboard_client.Request(advisory_read));
    Check(json::serialize(response).find("APPLIED") != std::string::npos,
          "read-only dashboard observes Gateway application over mTLS");

    qualification_client.Close();
    std::this_thread::sleep_for(20ms);
    Check(ConnectionRejected(server.bound_port(), pki.ca_file(), dashboard),
          "second live session for one role is rejected");
    Check(ConnectionRejected(server.bound_port(), pki.ca_file(), qualification,
                             "not-viss"),
          "VISSv3 subprotocol remains mandatory");

    assignment = ParseObject(AssignmentRequest(pki.assignment_socket(),
                                               DetachRequest(1, unit1, node1)));
    Check(StringAt(assignment, "result") == "ACCEPTED",
          "explicit detach accepted");
    std::this_thread::sleep_for(30ms);
    bool old_selected_closed = false;
    try {
      selected_client.Request(
          R"({"action":"get","path":"Vehicle.Speed","requestId":"old-selected-session"})");
    } catch (const std::exception &) {
      old_selected_closed = true;
    }
    Check(old_selected_closed,
          "detach immediately closes the old selected Platform Unit session");
    bool old_runtime_closed = false;
    try {
      runtime_client.Request(
          R"({"action":"get","path":"Vehicle.Speed","requestId":"old-runtime-session"})");
    } catch (const std::exception &) {
      old_runtime_closed = true;
    }
    Check(old_runtime_closed,
          "detach immediately closes the old Platform Update Runtime session");
    Check(ConnectionRejected(server.bound_port(), pki.ca_file(), selected1),
          "old selected fingerprint cannot reconnect after detach");
    response = ParseObject(dashboard_client.Request(
        R"({"action":"get","path":"Vehicle.Speed","requestId":"dashboard-after-detach"})"));
    Check(StringAt(response, "action") == "get",
          "Dashboard stays live across selected detach");
    response = ParseObject(dashboard_client.Request(advisory_read));
    Check(!response.contains("error") &&
              response.at("data").as_object().at("dp").as_object().at("value") == "",
          "detach removes old vehicle advisory indication before next assignment");

    assignment = ParseObject(
        AssignmentRequest(pki.assignment_socket(),
                          SelectRequest(2, unit2, node2, selected2, runtime2)));
    Check(StringAt(assignment, "result") == "ACCEPTED",
          "new Unit assignment accepted after detach");
    Client selected2_client(server.bound_port(), pki.ca_file(), selected2);
    Check(
        IsUnavailable(selected2_client.Request(
            R"({"action":"get","path":"Vehicle.Speed","requestId":"new-selected-floor"})")),
        "new Unit cannot receive prior Unit cached frame");
    Publish(store, 3, 30.0);
    server.NotifySnapshot();
    response = ParseObject(selected2_client.Request(
        R"({"action":"get","path":"Vehicle.Speed","requestId":"new-selected-frame"})"));
    Check(StringAt(response, "action") == "get",
          "new Unit receives only a post-assignment frame");

    Client runtime2_client(server.bound_port(), pki.ca_file(), runtime2);
    Client qualification2_client(server.bound_port(), pki.ca_file(),
                                 qualification);
    Check(ConnectionRejected(server.bound_port(), pki.ca_file(), dashboard),
          "fifth strict client is rejected at the four-role cap");

    selected2_client.Close();
    runtime2_client.Close();
    qualification2_client.Close();
    dashboard_client.Close();
    std::this_thread::sleep_for(30ms);
    server.Stop();
    const auto metrics = server.metrics();
    Check(metrics.accepted_connections >= 7,
          "accepted metric includes valid strict role sessions");
    Check(metrics.rejected_connections >= 11,
          "rejected metric includes invalid mTLS identities and limits");
    Check(metrics.active_connections == 0,
          "no strict role sessions remain after shutdown");
  } catch (const std::exception &error) {
    std::cerr << "FAIL: unexpected network test error: " << error.what()
              << '\n';
    ++failures;
  }

  if (failures == 0) {
    std::cout << "VISS strict mTLS network tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
