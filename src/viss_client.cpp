#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using SecureWebSocket = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

struct Options {
  std::string host = "localhost";
  std::uint16_t port = 6443;
  std::string ca_file;
  std::string request;
  std::size_t messages = 1;
  std::uint32_t monitor_period_ms = 250;
  bool monitor = false;
};

std::string Usage() {
  return R"(Usage: carla-viss-client --ca FILE (--request JSON | --monitor) [options]

Connect to a VISS 3.1 Secure WebSocket endpoint. A request prints raw JSON;
monitor mode continuously renders the basic vehicle signals as a dashboard.

Options:
  -h, --help                Show this help text
      --host HOST           TLS host name (default: localhost)
      --port PORT           Secure WebSocket port (default: 6443)
      --ca FILE             Trusted PEM certificate or CA bundle (required)
      --request JSON        One VISS request to send
      --messages N          Number of raw responses/events to read (default: 1)
      --monitor             Show the live basic-telemetry dashboard until Ctrl-C
      --monitor-period-ms N Dashboard refresh period (default: 250)
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
    } else if (argument == "--monitor") {
      options.monitor = true;
    } else if (argument == "--monitor-period-ms") {
      options.monitor_period_ms = ParseUnsigned<std::uint32_t>(
          RequireValue(arguments, index), "--monitor-period-ms");
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }
  if (options.host.empty() || options.ca_file.empty()) {
    throw std::invalid_argument(
        "--host must not be empty and --ca is required");
  }
  if (options.monitor == !options.request.empty()) {
    throw std::invalid_argument(
        "choose exactly one of --request and --monitor");
  }
  if (options.monitor_period_ms < 50 || options.monitor_period_ms > 60000) {
    throw std::invalid_argument(
        "--monitor-period-ms must be between 50 and 60000");
  }
  return options;
}

std::string ReadMessage(SecureWebSocket &client) {
  beast::flat_buffer buffer;
  client.read(buffer);
  return beast::buffers_to_string(buffer.data());
}

std::string AsString(const json::string &value) {
  return {value.data(), value.size()};
}

std::string BuildMonitorRequest(std::uint32_t period_ms) {
  json::array paths;
  for (const auto *path : {
           "Speed",
           "Acceleration.*",
           "Chassis.Accelerator.PedalPosition",
           "Chassis.Brake.PedalPosition",
           "Chassis.Axle.Row1.SteeringAngle",
           "Chassis.Axle.Row1.Wheel.Left.AngularSpeed",
           "Chassis.Axle.Row1.Wheel.Left.Speed",
           "Chassis.Axle.Row1.Wheel.Right.AngularSpeed",
           "Chassis.Axle.Row1.Wheel.Right.Speed",
           "Chassis.Axle.Row2.Wheel.Left.AngularSpeed",
           "Chassis.Axle.Row2.Wheel.Left.Speed",
           "Chassis.Axle.Row2.Wheel.Right.AngularSpeed",
           "Chassis.Axle.Row2.Wheel.Right.Speed",
           "Powertrain.Transmission.CurrentGear",
           "Powertrain.CombustionEngine.Speed",
           "CurrentLocation.*",
           "CarlaSimulation.FrameId",
           "CarlaSimulation.SimulationTime",
           "CarlaSimulation.ChaosWheel.Row1.Left.*",
           "CarlaSimulation.ChaosWheel.Row1.Right.*",
           "CarlaSimulation.ChaosWheel.Row2.Left.*",
           "CarlaSimulation.ChaosWheel.Row2.Right.*",
       }) {
    paths.emplace_back(path);
  }
  json::object path_filter;
  path_filter["variant"] = "paths";
  path_filter["parameter"] = std::move(paths);
  json::object period;
  period["period"] = std::to_string(period_ms);
  json::object time_filter;
  time_filter["variant"] = "timebased";
  time_filter["parameter"] = std::move(period);
  json::array filters;
  filters.emplace_back(std::move(path_filter));
  filters.emplace_back(std::move(time_filter));
  json::object request;
  request["action"] = "subscribe";
  request["path"] = "Vehicle";
  request["filter"] = std::move(filters);
  request["requestId"] = "carla-live-monitor";
  return json::serialize(request);
}

using SignalValues = std::map<std::string, std::string>;

struct MonitorHealth {
  std::optional<double> simulation_hz;
  std::optional<double> delivery_hz;
  std::optional<double> event_latency_ms;
  std::optional<double> previous_frame;
  std::optional<double> previous_simulation_time;
  std::optional<std::chrono::steady_clock::time_point> previous_received_at;
  std::size_t event_count = 0;
};

void CollectDataPoint(const json::object &object, SignalValues &signals) {
  const auto *path = object.if_contains("path");
  const auto *dp = object.if_contains("dp");
  if (path == nullptr || !path->is_string() || dp == nullptr ||
      !dp->is_object()) {
    return;
  }
  const auto *value = dp->as_object().if_contains("value");
  if (value != nullptr && value->is_string()) {
    signals[AsString(path->as_string())] = AsString(value->as_string());
  }
}

void CollectData(const json::value &data, SignalValues &signals) {
  if (data.is_object()) {
    CollectDataPoint(data.as_object(), signals);
  } else if (data.is_array()) {
    for (const auto &item : data.as_array()) {
      if (item.is_object()) {
        CollectDataPoint(item.as_object(), signals);
      }
    }
  }
}

std::string Value(const SignalValues &signals, std::string_view path,
                  std::string fallback = "--") {
  const auto iterator = signals.find(std::string(path));
  return iterator == signals.end() ? std::move(fallback) : iterator->second;
}

std::optional<double> Number(const SignalValues &signals,
                             std::string_view path) {
  const auto iterator = signals.find(std::string(path));
  if (iterator == signals.end()) {
    return std::nullopt;
  }
  std::size_t consumed = 0;
  try {
    const auto value = std::stod(iterator->second, &consumed);
    if (consumed == iterator->second.size() && std::isfinite(value)) {
      return value;
    }
  } catch (const std::exception &) {
  }
  return std::nullopt;
}

std::string NumberText(const SignalValues &signals, std::string_view path,
                       int precision) {
  const auto value = Number(signals, path);
  if (!value.has_value()) {
    return "--";
  }
  std::ostringstream output;
  output << std::fixed << std::setprecision(precision) << *value;
  return output.str();
}

std::string MetricText(const std::optional<double> &value, int precision,
                       std::string_view suffix) {
  if (!value.has_value() || !std::isfinite(*value)) {
    return "--";
  }
  std::ostringstream output;
  output << std::fixed << std::setprecision(precision) << *value << suffix;
  return output.str();
}

std::optional<std::chrono::system_clock::time_point>
ParseIso8601Utc(std::string_view text) {
  if (text.size() != 24 || text[4] != '-' || text[7] != '-' ||
      text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
      text[19] != '.' || text[23] != 'Z') {
    return std::nullopt;
  }
  std::tm utc{};
  std::istringstream input{std::string(text.substr(0, 19))};
  input >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%S");
  if (input.fail()) {
    return std::nullopt;
  }
  unsigned milliseconds = 0;
  const auto millisecond_text = text.substr(20, 3);
  const auto result = std::from_chars(millisecond_text.data(),
                                      millisecond_text.data() + 3,
                                      milliseconds);
  if (result.ec != std::errc{} || result.ptr != millisecond_text.data() + 3) {
    return std::nullopt;
  }
#if defined(_WIN32)
  const auto seconds = _mkgmtime(&utc);
#else
  const auto seconds = timegm(&utc);
#endif
  if (seconds < 0) {
    return std::nullopt;
  }
  return std::chrono::system_clock::from_time_t(seconds) +
         std::chrono::milliseconds(milliseconds);
}

void SmoothMetric(std::optional<double> &metric, double sample) {
  constexpr double response = 0.25;
  if (!std::isfinite(sample)) {
    return;
  }
  metric = metric.has_value() ? *metric + response * (sample - *metric)
                              : sample;
}

void UpdateHealth(const SignalValues &signals, std::string_view event_timestamp,
                  MonitorHealth &health) {
  const auto received_at = std::chrono::steady_clock::now();
  const auto frame = Number(signals, "Vehicle.CarlaSimulation.FrameId");
  const auto simulation_time =
      Number(signals, "Vehicle.CarlaSimulation.SimulationTime");
  if (frame.has_value() && simulation_time.has_value() &&
      health.previous_frame.has_value() &&
      health.previous_simulation_time.has_value()) {
    const double frame_delta = *frame - *health.previous_frame;
    const double simulation_delta =
        *simulation_time - *health.previous_simulation_time;
    if (frame_delta > 0.0 && simulation_delta > 0.0) {
      SmoothMetric(health.simulation_hz, frame_delta / simulation_delta);
    }
  }
  if (health.previous_received_at.has_value()) {
    const double seconds =
        std::chrono::duration<double>(received_at - *health.previous_received_at)
            .count();
    if (seconds > 0.0) {
      SmoothMetric(health.delivery_hz, 1.0 / seconds);
    }
  }
  if (const auto sent_at = ParseIso8601Utc(event_timestamp);
      sent_at.has_value()) {
    SmoothMetric(
        health.event_latency_ms,
        std::max(0.0, std::chrono::duration<double, std::milli>(
                          std::chrono::system_clock::now() - *sent_at)
                          .count()));
  }
  health.previous_frame = frame;
  health.previous_simulation_time = simulation_time;
  health.previous_received_at = received_at;
  ++health.event_count;
}

std::string Bar(const SignalValues &signals, std::string_view path) {
  constexpr int width = 20;
  const double percent =
      std::clamp(Number(signals, path).value_or(0.0), 0.0, 100.0);
  const int filled = static_cast<int>(std::lround(percent * width / 100.0));
  return "[" + std::string(filled, '#') + std::string(width - filled, '.') +
         "]";
}

void RenderDashboard(const Options &options, const SignalValues &signals,
                     std::string_view updated_at,
                     const MonitorHealth &health) {
  const double healthy_latency_ms =
      std::max(1000.0, static_cast<double>(options.monitor_period_ms) * 4.0);
  const bool live = health.event_latency_ms.has_value() &&
                    *health.event_latency_ms <= healthy_latency_ms;
  std::cout
      << "\033[2J\033[H"
      << "CARLA / VSS LIVE TELEMETRY\n"
      << "===========================\n"
      << "wss://" << options.host << ':' << options.port
      << "   VISSv3   TLS verified\n"
      << "Connection        CONNECTED\n"
      << "Data health       " << (live ? "LIVE" : "WAITING") << '\n'
      << "Simulation rate   "
      << MetricText(health.simulation_hz, 1, " Hz") << '\n'
      << "Dashboard rate    "
      << MetricText(health.delivery_hz, 1, " events/s") << '\n'
      << "VISS latency      "
      << MetricText(health.event_latency_ms, 1, " ms") << " (local)\n"
      << "Events received   " << health.event_count << "\n\n"
      << "Frame             "
      << Value(signals, "Vehicle.CarlaSimulation.FrameId") << '\n'
      << "Simulation time   "
      << NumberText(signals, "Vehicle.CarlaSimulation.SimulationTime", 2)
      << " s\n"
      << "Speed             " << std::setw(8)
      << NumberText(signals, "Vehicle.Speed", 1) << " km/h\n"
      << "Acceleration      " << std::setw(8)
      << NumberText(signals, "Vehicle.Acceleration.Longitudinal", 2)
      << " m/s2 (longitudinal)\n"
      << "Steering angle    " << std::setw(8)
      << NumberText(signals, "Vehicle.Chassis.Axle.Row1.SteeringAngle", 1)
      << " deg\n"
      << "Gear              "
      << Value(signals, "Vehicle.Powertrain.Transmission.CurrentGear") << '\n'
      << "Engine            " << std::setw(8)
      << NumberText(signals, "Vehicle.Powertrain.CombustionEngine.Speed", 0)
      << " rpm\n\n"
      << "Accelerator "
      << Bar(signals, "Vehicle.Chassis.Accelerator.PedalPosition") << ' '
      << std::setw(3)
      << Value(signals, "Vehicle.Chassis.Accelerator.PedalPosition", "0")
      << "%\n"
      << "Brake       " << Bar(signals, "Vehicle.Chassis.Brake.PedalPosition")
      << ' ' << std::setw(3)
      << Value(signals, "Vehicle.Chassis.Brake.PedalPosition", "0") << "%\n\n"
      << "WHEEL DYNAMICS     FL       FR       RL       RR\n"
      << "Speed km/h       " << std::setw(7)
      << NumberText(signals, "Vehicle.Chassis.Axle.Row1.Wheel.Left.Speed", 1)
      << "  " << std::setw(7)
      << NumberText(signals, "Vehicle.Chassis.Axle.Row1.Wheel.Right.Speed", 1)
      << "  " << std::setw(7)
      << NumberText(signals, "Vehicle.Chassis.Axle.Row2.Wheel.Left.Speed", 1)
      << "  " << std::setw(7)
      << NumberText(signals, "Vehicle.Chassis.Axle.Row2.Wheel.Right.Speed", 1)
      << '\n'
      << "Angular rad/s    " << std::setw(7)
      << NumberText(
             signals,
             "Vehicle.Chassis.Axle.Row1.Wheel.Left.AngularSpeed", 1)
      << "  " << std::setw(7)
      << NumberText(
             signals,
             "Vehicle.Chassis.Axle.Row1.Wheel.Right.AngularSpeed", 1)
      << "  " << std::setw(7)
      << NumberText(
             signals,
             "Vehicle.Chassis.Axle.Row2.Wheel.Left.AngularSpeed", 1)
      << "  " << std::setw(7)
      << NumberText(
             signals,
             "Vehicle.Chassis.Axle.Row2.Wheel.Right.AngularSpeed", 1)
      << '\n'
      << "Longitudinal slip" << std::setw(7)
      << NumberText(
             signals,
             "Vehicle.CarlaSimulation.ChaosWheel.Row1.Left.LongitudinalSlip",
             2)
      << "  " << std::setw(7)
      << NumberText(
             signals,
             "Vehicle.CarlaSimulation.ChaosWheel.Row1.Right.LongitudinalSlip",
             2)
      << "  " << std::setw(7)
      << NumberText(
             signals,
             "Vehicle.CarlaSimulation.ChaosWheel.Row2.Left.LongitudinalSlip",
             2)
      << "  " << std::setw(7)
      << NumberText(
             signals,
             "Vehicle.CarlaSimulation.ChaosWheel.Row2.Right.LongitudinalSlip",
             2)
      << '\n'
      << "Lateral slip deg " << std::setw(7)
      << NumberText(
             signals,
             "Vehicle.CarlaSimulation.ChaosWheel.Row1.Left.LateralSlipAngle",
             1)
      << "  " << std::setw(7)
      << NumberText(
             signals,
             "Vehicle.CarlaSimulation.ChaosWheel.Row1.Right.LateralSlipAngle",
             1)
      << "  " << std::setw(7)
      << NumberText(
             signals,
             "Vehicle.CarlaSimulation.ChaosWheel.Row2.Left.LateralSlipAngle",
             1)
      << "  " << std::setw(7)
      << NumberText(
             signals,
             "Vehicle.CarlaSimulation.ChaosWheel.Row2.Right.LateralSlipAngle",
             1)
      << "\n\n"
      << "GNSS latitude     "
      << NumberText(signals, "Vehicle.CurrentLocation.Latitude", 6) << '\n'
      << "GNSS longitude    "
      << NumberText(signals, "Vehicle.CurrentLocation.Longitude", 6) << '\n'
      << "GNSS altitude     "
      << NumberText(signals, "Vehicle.CurrentLocation.Altitude", 1) << " m\n\n"
      << "Last VISS event   " << updated_at << "\n"
      << "Press Ctrl-C to stop the monitor.\n"
      << std::flush;
}

void RunMonitor(SecureWebSocket &client, const Options &options) {
  const auto request = BuildMonitorRequest(options.monitor_period_ms);
  client.write(asio::buffer(request));
  const auto response = json::parse(ReadMessage(client));
  if (!response.is_object() ||
      response.as_object().if_contains("subscriptionId") == nullptr) {
    throw std::runtime_error("VISS subscription was rejected: " +
                             json::serialize(response));
  }

  SignalValues signals;
  MonitorHealth health;
  while (true) {
    const auto event = json::parse(ReadMessage(client));
    if (!event.is_object()) {
      continue;
    }
    const auto &object = event.as_object();
    const auto *data = object.if_contains("data");
    if (data != nullptr) {
      CollectData(*data, signals);
    }
    std::string updated_at = "--";
    if (const auto *timestamp = object.if_contains("ts");
        timestamp != nullptr && timestamp->is_string()) {
      updated_at = AsString(timestamp->as_string());
    }
    UpdateHealth(signals, updated_at, health);
    RenderDashboard(options, signals, updated_at, health);
  }
}

int Run(const Options &options) {
  asio::io_context io_context;
  ssl::context tls_context(ssl::context::tls_client);
  tls_context.set_verify_mode(ssl::verify_peer);
  tls_context.load_verify_file(options.ca_file);

  tcp::resolver resolver(io_context);
  SecureWebSocket client(io_context, tls_context);
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

  if (options.monitor) {
    RunMonitor(client, options);
  } else {
    client.write(asio::buffer(options.request));
    for (std::size_t index = 0; index < options.messages; ++index) {
      std::cout << ReadMessage(client) << '\n';
    }
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
