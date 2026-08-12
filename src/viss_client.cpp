#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <openssl/ssl.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

struct Options {
  std::string host = "localhost";
  std::uint16_t port = 6443;
  std::string ca_file;
  std::string request;
  std::size_t messages = 1;
};

std::string Usage() {
  return R"(Usage: carla-viss-client --ca FILE --request JSON [options]

Connect to a VISS 3.1 Secure WebSocket endpoint and print JSON messages.

Options:
  -h, --help          Show this help text
      --host HOST     TLS host name (default: localhost)
      --port PORT     Secure WebSocket port (default: 6443)
      --ca FILE       Trusted PEM certificate or CA bundle (required)
      --request JSON  One VISS request to send (required)
      --messages N    Number of responses/events to read (default: 1)
)";
}

const std::string &RequireValue(const std::vector<std::string> &arguments,
                                std::size_t &index) {
  if (index + 1 >= arguments.size()) {
    throw std::invalid_argument(arguments[index] + " requires a value");
  }
  return arguments[++index];
}

template <typename Integer>
Integer ParseUnsigned(const std::string &text, std::string_view option) {
  std::uint64_t parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (text.empty() || result.ec != std::errc{} ||
      result.ptr != text.data() + text.size() || parsed == 0 ||
      parsed >
          static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
    throw std::invalid_argument("invalid value for " + std::string(option));
  }
  return static_cast<Integer>(parsed);
}

Options Parse(const std::vector<std::string> &arguments) {
  Options options;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto &argument = arguments[index];
    if (argument == "--host") {
      options.host = RequireValue(arguments, index);
    } else if (argument == "--port") {
      options.port = ParseUnsigned<std::uint16_t>(
          RequireValue(arguments, index), "--port");
    } else if (argument == "--ca") {
      options.ca_file = RequireValue(arguments, index);
    } else if (argument == "--request") {
      options.request = RequireValue(arguments, index);
    } else if (argument == "--messages") {
      options.messages = ParseUnsigned<std::size_t>(
          RequireValue(arguments, index), "--messages");
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }
  if (options.host.empty() || options.ca_file.empty() ||
      options.request.empty()) {
    throw std::invalid_argument(
        "--host must not be empty and --ca/--request are required");
  }
  return options;
}

int Run(const Options &options) {
  asio::io_context io_context;
  ssl::context tls_context(ssl::context::tls_client);
  tls_context.set_verify_mode(ssl::verify_peer);
  tls_context.load_verify_file(options.ca_file);

  tcp::resolver resolver(io_context);
  websocket::stream<beast::ssl_stream<beast::tcp_stream>> client(io_context,
                                                                 tls_context);
  const auto endpoints =
      resolver.resolve(options.host, std::to_string(options.port));
  beast::get_lowest_layer(client).connect(endpoints);
  if (SSL_set_tlsext_host_name(client.next_layer().native_handle(),
                               options.host.c_str()) != 1) {
    throw std::runtime_error("could not set the TLS server name");
  }
  client.next_layer().set_verify_callback(
      ssl::host_name_verification(options.host));
  client.next_layer().handshake(ssl::stream_base::client);
  client.set_option(
      websocket::stream_base::decorator([](websocket::request_type &request) {
        request.set(http::field::sec_websocket_protocol, "VISSv3");
      }));
  websocket::response_type handshake_response;
  client.handshake(handshake_response, options.host, "/");
  const auto negotiated =
      handshake_response[http::field::sec_websocket_protocol];
  if (negotiated != "VISSv3") {
    throw std::runtime_error("server did not negotiate VISSv3");
  }

  client.write(asio::buffer(options.request));
  for (std::size_t index = 0; index < options.messages; ++index) {
    beast::flat_buffer buffer;
    client.read(buffer);
    std::cout << beast::buffers_to_string(buffer.data()) << '\n';
  }
  boost::system::error_code ignored;
  client.close(websocket::close_code::normal, ignored);
  return 0;
}

} // namespace

int main(int argc, char *argv[]) {
  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  if (arguments.size() == 1 &&
      (arguments.front() == "--help" || arguments.front() == "-h")) {
    std::cout << Usage();
    return 0;
  }
  try {
    return Run(Parse(arguments));
  } catch (const std::exception &error) {
    std::cerr << "VISS client error: " << error.what() << "\n\n" << Usage();
    return 2;
  }
}
