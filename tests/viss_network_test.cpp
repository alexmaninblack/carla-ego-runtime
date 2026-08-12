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

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
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

struct TemporaryCertificate {
  std::filesystem::path directory;
  std::filesystem::path certificate;
  std::filesystem::path private_key;

  TemporaryCertificate(std::filesystem::path directory_value,
                       std::filesystem::path certificate_value,
                       std::filesystem::path private_key_value)
      : directory(std::move(directory_value)),
        certificate(std::move(certificate_value)),
        private_key(std::move(private_key_value)) {}

  TemporaryCertificate(const TemporaryCertificate &) = delete;
  TemporaryCertificate &operator=(const TemporaryCertificate &) = delete;
  TemporaryCertificate(TemporaryCertificate &&other) noexcept
      : directory(std::move(other.directory)),
        certificate(std::move(other.certificate)),
        private_key(std::move(other.private_key)) {
    other.directory.clear();
  }

  ~TemporaryCertificate() {
    if (directory.empty()) {
      return;
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

TemporaryCertificate GenerateCertificate() {
  std::random_device random;
  auto directory = std::filesystem::temp_directory_path() /
                   ("carla-viss-network-test-" +
                    std::to_string(static_cast<unsigned long long>(random())));
  std::filesystem::create_directory(directory);
  TemporaryCertificate result{directory, directory / "certificate.pem",
                              directory / "private-key.pem"};

  OpenSslPointer<EVP_PKEY_CTX, EVP_PKEY_CTX_free> key_context(
      EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
  if (!key_context || EVP_PKEY_keygen_init(key_context.get()) <= 0 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(key_context.get(), 2048) <= 0) {
    throw std::runtime_error("could not initialize temporary RSA key");
  }
  EVP_PKEY *raw_key = nullptr;
  if (EVP_PKEY_keygen(key_context.get(), &raw_key) <= 0) {
    throw std::runtime_error("could not generate temporary RSA key");
  }
  OpenSslPointer<EVP_PKEY, EVP_PKEY_free> key(raw_key, EVP_PKEY_free);
  OpenSslPointer<X509, X509_free> certificate(X509_new(), X509_free);
  if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
      ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
      X509_gmtime_adj(X509_get_notBefore(certificate.get()), -60) == nullptr ||
      X509_gmtime_adj(X509_get_notAfter(certificate.get()), 3600) == nullptr ||
      X509_set_pubkey(certificate.get(), key.get()) != 1) {
    throw std::runtime_error("could not initialize temporary certificate");
  }
  auto *name = X509_get_subject_name(certificate.get());
  constexpr unsigned char common_name[] = "localhost";
  if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, common_name, -1, -1,
                                 0) != 1 ||
      X509_set_issuer_name(certificate.get(), name) != 1 ||
      X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
    throw std::runtime_error("could not sign temporary certificate");
  }

  std::unique_ptr<std::FILE, decltype(&std::fclose)> certificate_file(
      std::fopen(result.certificate.c_str(), "wb"), std::fclose);
  std::unique_ptr<std::FILE, decltype(&std::fclose)> key_file(
      std::fopen(result.private_key.c_str(), "wb"), std::fclose);
  if (!certificate_file || !key_file ||
      PEM_write_X509(certificate_file.get(), certificate.get()) != 1 ||
      PEM_write_PrivateKey(key_file.get(), key.get(), nullptr, nullptr, 0,
                           nullptr, nullptr) != 1) {
    throw std::runtime_error("could not write temporary TLS material");
  }
  return result;
}

void PopulateStore(carla_ego_runtime::LatestVssSignalStore &store) {
  carla_ego_runtime::VssSnapshot snapshot;
  snapshot.frame_id = 1;
  snapshot.simulation_time_s = 0.05;
  snapshot.timestamp = "2026-08-12T12:34:56.789Z";
  snapshot.data_points = {{"Vehicle.Speed", 22.5, snapshot.timestamp},
                          {"Vehicle.CurrentLocation.Latitude", 52.520008,
                           "2026-08-12T12:34:56.700Z"},
                          {"Vehicle.CarlaSimulation.FrameId", std::uint64_t{1},
                           snapshot.timestamp}};
  Check(store.Publish(std::move(snapshot)), "network test snapshot published");
}

class Client {
public:
  Client(std::uint16_t port, std::string subprotocol = "VISSv3")
      : tls_context_(ssl::context::tls_client),
        websocket_(io_context_, tls_context_) {
    tls_context_.set_verify_mode(ssl::verify_none);
    tcp::resolver resolver(io_context_);
    const auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));
    beast::get_lowest_layer(websocket_).connect(endpoints);
    if (SSL_set_tlsext_host_name(websocket_.next_layer().native_handle(),
                                 "localhost") != 1) {
      throw std::runtime_error("could not set TLS SNI");
    }
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

  std::string negotiated_subprotocol() const {
    const auto header =
        handshake_response_[http::field::sec_websocket_protocol];
    return {header.data(), header.size()};
  }

  void Close() {
    boost::system::error_code ignored;
    websocket_.close(websocket::close_code::normal, ignored);
  }

private:
  asio::io_context io_context_;
  ssl::context tls_context_;
  websocket::stream<beast::ssl_stream<beast::tcp_stream>> websocket_;
  websocket::response_type handshake_response_;
};

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

} // namespace

int main() {
  try {
    auto tls_material = GenerateCertificate();
    carla_ego_runtime::LatestVssSignalStore store;
    PopulateStore(store);
    carla_ego_runtime::VissServerConfig config;
    config.port = 0;
    config.certificate_chain_file = tls_material.certificate.string();
    config.private_key_file = tls_material.private_key.string();
    config.max_pending_messages_per_client = 2;
    carla_ego_runtime::VissServer server(store, config);
    server.Start();
    Check(server.bound_port() != 0, "server binds an ephemeral TLS port");

    bool wrong_protocol_rejected = false;
    try {
      Client wrong_protocol(server.bound_port(), "not-viss");
    } catch (const std::exception &) {
      wrong_protocol_rejected = true;
    }
    Check(wrong_protocol_rejected, "non-VISS WebSocket subprotocol rejected");

    Client first(server.bound_port());
    Check(first.negotiated_subprotocol() == "VISSv3",
          "VISSv3 subprotocol negotiated");
    Check(first.tls_version() == "TLSv1.2" || first.tls_version() == "TLSv1.3",
          "TLS 1.2 or newer negotiated");

    auto response = ParseObject(first.Request(
        R"({"action":"get","path":"Vehicle.Speed","requestId":"network-get"})"));
    Check(StringAt(response, "action") == "get", "network get succeeds");
    Check(StringAt(response.at("data").as_object().at("dp").as_object(),
                   "value") == "22.5",
          "network get carries string value");

    response = ParseObject(first.Request("not-json"));
    Check(StringAt(response.at("error").as_object(), "reason") == "bad_request",
          "malformed network request receives a VISS error");

    response = ParseObject(first.Request(
        R"({"action":"set","path":"Vehicle.Speed","value":"0","requestId":"network-set"})"));
    Check(StringAt(response.at("error").as_object(), "reason") ==
              "invalid_data",
          "read-only Update receives a VISS error over the network");

    response = ParseObject(first.Request(
        R"({"action":"subscribe","path":"Vehicle","filter":{"variant":"timebased","parameter":{"period":"50"}},"requestId":"network-sub"})"));
    const auto subscription_id = StringAt(response, "subscriptionId");
    Check(!subscription_id.empty(), "network subscription id returned");
    const auto event = ParseObject(first.Read());
    Check(StringAt(event, "action") == "subscription",
          "server pushes subscription event");
    Check(StringAt(event, "subscriptionId") == subscription_id,
          "pushed event uses subscription id");
    first.Close();

    Client reconnected(server.bound_port());
    response = ParseObject(reconnected.Request(
        std::string(R"({"action":"unsubscribe","subscriptionId":")") +
        subscription_id + R"(","requestId":"old-subscription"})"));
    Check(StringAt(response.at("error").as_object(), "reason") ==
              "unavailable_data",
          "subscription state is cleared on reconnect");
    response = ParseObject(reconnected.Request(
        R"({"action":"get","path":"Vehicle.CurrentLocation.Latitude","requestId":"network-gnss"})"));
    Check(StringAt(response, "action") == "get",
          "GNSS path readable after reconnect");
    reconnected.Close();

    std::this_thread::sleep_for(50ms);
    server.Stop();
    const auto metrics = server.metrics();
    Check(metrics.accepted_connections == 2,
          "accepted connection metric counts reconnect");
    Check(metrics.rejected_connections >= 1,
          "rejected connection metric counts wrong subprotocol");
    Check(metrics.requests == 6, "request metric crosses the network boundary");
    Check(metrics.protocol_errors == 3,
          "protocol error metric crosses the network boundary");
    Check(metrics.subscription_events >= 1,
          "subscription event metric crosses the network boundary");
    Check(metrics.active_connections == 0,
          "no active clients remain after shutdown");
  } catch (const std::exception &error) {
    std::cerr << "FAIL: unexpected network test error: " << error.what()
              << '\n';
    ++failures;
  }

  if (failures == 0) {
    std::cout << "VISS TLS network tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
