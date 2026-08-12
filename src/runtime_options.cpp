#include "carla_ego_runtime/runtime_options.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace carla_ego_runtime {
namespace {

template <typename Integer>
Integer ParseUnsigned(std::string_view text, std::string_view option) {
  if (text.empty()) {
    throw std::invalid_argument(std::string(option) + " requires a value");
  }

  std::uint64_t parsed = 0;
  const auto *begin = text.data();
  const auto *end = begin + text.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end ||
      parsed >
          static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
    throw std::invalid_argument("invalid value for " + std::string(option) +
                                ": " + std::string(text));
  }
  return static_cast<Integer>(parsed);
}

const std::string &RequireValue(const std::vector<std::string> &arguments,
                                std::size_t &index) {
  if (index + 1 >= arguments.size()) {
    throw std::invalid_argument(arguments[index] + " requires a value");
  }
  if (arguments[index + 1].starts_with('-')) {
    throw std::invalid_argument(arguments[index] + " requires a value");
  }
  ++index;
  return arguments[index];
}

void RequireNonEmpty(const std::string &value, std::string_view option) {
  if (value.empty()) {
    throw std::invalid_argument(std::string(option) + " must not be empty");
  }
}

double ParsePositiveDouble(const std::string &text, std::string_view option) {
  std::size_t consumed = 0;
  double value = 0.0;
  try {
    value = std::stod(text, &consumed);
  } catch (const std::exception &) {
    throw std::invalid_argument("invalid value for " + std::string(option) +
                                ": " + text);
  }
  if (consumed != text.size() || !std::isfinite(value) || value <= 0.0) {
    throw std::invalid_argument("invalid value for " + std::string(option) +
                                ": " + text);
  }
  return value;
}

double ParseFiniteDouble(const std::string &text, std::string_view option) {
  std::size_t consumed = 0;
  double value = 0.0;
  try {
    value = std::stod(text, &consumed);
  } catch (const std::exception &) {
    throw std::invalid_argument("invalid value for " + std::string(option) +
                                ": " + text);
  }
  if (consumed != text.size() || !std::isfinite(value)) {
    throw std::invalid_argument("invalid value for " + std::string(option) +
                                ": " + text);
  }
  return value;
}

const std::string &RequireSignedValue(const std::vector<std::string> &arguments,
                                      std::size_t &index) {
  if (index + 1 >= arguments.size()) {
    throw std::invalid_argument(arguments[index] + " requires a value");
  }
  ++index;
  return arguments[index];
}

} // namespace

ParsedCommandLine ParseCommandLine(const std::vector<std::string> &arguments) {
  ParsedCommandLine result;
  bool max_frames_was_set = false;

  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto &argument = arguments[index];
    if (argument == "--help" || argument == "-h") {
      result.command = Command::kHelp;
    } else if (argument == "--version") {
      result.command = Command::kVersion;
    } else if (argument == "--host") {
      result.options.host = RequireValue(arguments, index);
      RequireNonEmpty(result.options.host, "--host");
    } else if (argument == "--port") {
      result.options.port = ParseUnsigned<std::uint16_t>(
          RequireValue(arguments, index), "--port");
      if (result.options.port == 0) {
        throw std::invalid_argument("--port must be between 1 and 65535");
      }
    } else if (argument == "--timeout-ms") {
      result.options.timeout_ms = ParseUnsigned<std::uint32_t>(
          RequireValue(arguments, index), "--timeout-ms");
      if (result.options.timeout_ms == 0) {
        throw std::invalid_argument("--timeout-ms must be greater than zero");
      }
    } else if (argument == "--role-name") {
      result.options.role_name = RequireValue(arguments, index);
      RequireNonEmpty(result.options.role_name, "--role-name");
    } else if (argument == "--blueprint") {
      result.options.blueprint_id = RequireValue(arguments, index);
      RequireNonEmpty(result.options.blueprint_id, "--blueprint");
    } else if (argument == "--spawn-point-index") {
      result.options.spawn_point_index = ParseUnsigned<std::size_t>(
          RequireValue(arguments, index), "--spawn-point-index");
    } else if (argument == "--run-seconds") {
      result.options.run_seconds = ParseUnsigned<std::uint32_t>(
          RequireValue(arguments, index), "--run-seconds");
    } else if (argument == "--max-frames") {
      result.options.max_frames = ParseUnsigned<std::uint64_t>(
          RequireValue(arguments, index), "--max-frames");
      max_frames_was_set = true;
    } else if (argument == "--fixed-delta-seconds") {
      result.options.fixed_delta_seconds = ParsePositiveDouble(
          RequireValue(arguments, index), "--fixed-delta-seconds");
      if (result.options.fixed_delta_seconds > 1.0) {
        throw std::invalid_argument(
            "--fixed-delta-seconds must be no greater than 1.0");
      }
    } else if (argument == "--log-every-frames") {
      result.options.log_every_frames = ParseUnsigned<std::uint64_t>(
          RequireValue(arguments, index), "--log-every-frames");
      if (result.options.log_every_frames == 0) {
        throw std::invalid_argument(
            "--log-every-frames must be greater than zero");
      }
    } else if (argument == "--gnss-sensor-tick-seconds") {
      result.options.gnss_sensor_tick_seconds = ParsePositiveDouble(
          RequireValue(arguments, index), "--gnss-sensor-tick-seconds");
      if (result.options.gnss_sensor_tick_seconds > 10.0) {
        throw std::invalid_argument(
            "--gnss-sensor-tick-seconds must be no greater than 10.0");
      }
    } else if (argument == "--gnss-max-age-seconds") {
      result.options.gnss_max_age_seconds = ParsePositiveDouble(
          RequireValue(arguments, index), "--gnss-max-age-seconds");
      if (result.options.gnss_max_age_seconds > 60.0) {
        throw std::invalid_argument(
            "--gnss-max-age-seconds must be no greater than 60.0");
      }
    } else if (argument == "--viss") {
      result.options.viss_enabled = true;
    } else if (argument == "--viss-bind-address") {
      result.options.viss_bind_address = RequireValue(arguments, index);
      RequireNonEmpty(result.options.viss_bind_address, "--viss-bind-address");
    } else if (argument == "--viss-port") {
      result.options.viss_port = ParseUnsigned<std::uint16_t>(
          RequireValue(arguments, index), "--viss-port");
      if (result.options.viss_port == 0) {
        throw std::invalid_argument("--viss-port must be between 1 and 65535");
      }
    } else if (argument == "--viss-cert") {
      result.options.viss_certificate_chain_file =
          RequireValue(arguments, index);
      RequireNonEmpty(result.options.viss_certificate_chain_file,
                      "--viss-cert");
    } else if (argument == "--viss-key") {
      result.options.viss_private_key_file = RequireValue(arguments, index);
      RequireNonEmpty(result.options.viss_private_key_file, "--viss-key");
    } else if (argument == "--viss-max-clients") {
      result.options.viss_max_clients = ParseUnsigned<std::size_t>(
          RequireValue(arguments, index), "--viss-max-clients");
      if (result.options.viss_max_clients == 0 ||
          result.options.viss_max_clients > 128) {
        throw std::invalid_argument(
            "--viss-max-clients must be between 1 and 128");
      }
    } else if (argument == "--viss-max-subscriptions") {
      result.options.viss_max_subscriptions_per_client =
          ParseUnsigned<std::size_t>(RequireValue(arguments, index),
                                     "--viss-max-subscriptions");
      if (result.options.viss_max_subscriptions_per_client == 0 ||
          result.options.viss_max_subscriptions_per_client > 1024) {
        throw std::invalid_argument(
            "--viss-max-subscriptions must be between 1 and 1024");
      }
    } else if (argument == "--viss-max-pending-messages") {
      result.options.viss_max_pending_messages_per_client =
          ParseUnsigned<std::size_t>(RequireValue(arguments, index),
                                     "--viss-max-pending-messages");
      if (result.options.viss_max_pending_messages_per_client == 0 ||
          result.options.viss_max_pending_messages_per_client > 1024) {
        throw std::invalid_argument(
            "--viss-max-pending-messages must be between 1 and 1024");
      }
    } else if (argument == "--observe-ticks") {
      result.options.tick_owner = false;
    } else if (argument == "--real-time") {
      result.options.real_time = true;
    } else if (argument == "--autopilot") {
      result.options.autopilot = true;
    } else if (argument == "--chase-camera") {
      result.options.chase_camera = true;
    } else if (argument == "--chase-camera-response") {
      result.options.chase_camera_response = ParsePositiveDouble(
          RequireValue(arguments, index), "--chase-camera-response");
      if (result.options.chase_camera_response > 100.0) {
        throw std::invalid_argument(
            "--chase-camera-response must be no greater than 100");
      }
    } else if (argument == "--chase-camera-update-hz") {
      result.options.chase_camera_update_hz = ParseUnsigned<std::uint32_t>(
          RequireValue(arguments, index), "--chase-camera-update-hz");
      if (result.options.chase_camera_update_hz < 20 ||
          result.options.chase_camera_update_hz > 240) {
        throw std::invalid_argument(
            "--chase-camera-update-hz must be between 20 and 240");
      }
    } else if (argument == "--exposure-offset") {
      result.options.exposure_offset = ParseFiniteDouble(
          RequireSignedValue(arguments, index), "--exposure-offset");
      if (result.options.exposure_offset < -5.0 ||
          result.options.exposure_offset > 5.0) {
        throw std::invalid_argument(
            "--exposure-offset must be between -5 and 5 EV");
      }
    } else if (argument == "--no-spawn") {
      result.options.spawn_if_missing = false;
    } else if (argument == "--allow-version-mismatch") {
      result.options.require_matching_versions = false;
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }

  // A requested wall-clock run should not be cut short by the safe one-frame
  // default unless the caller explicitly supplies both limits.
  if (result.options.run_seconds > 0 && !max_frames_was_set) {
    result.options.max_frames = 0;
  }
  if (result.options.viss_enabled &&
      (result.options.viss_certificate_chain_file.empty() ||
       result.options.viss_private_key_file.empty())) {
    throw std::invalid_argument(
        "--viss requires both --viss-cert and --viss-key");
  }

  return result;
}

std::string Usage() {
  return R"(Usage: carla-ego-runtime [options]

Connect to CARLA, select an ego vehicle by role_name, and spawn one if needed.

Options:
  -h, --help                    Show this help text
      --version                 Show the runtime version
      --host HOST               CARLA RPC host (default: 127.0.0.1)
      --port PORT               CARLA RPC port (default: 2000)
      --timeout-ms MS           RPC timeout in milliseconds (default: 10000)
      --role-name NAME          Ego vehicle role_name (default: hero)
      --blueprint ID            Blueprint used when spawning is needed
                                (default: vehicle.lincoln.mkz)
      --spawn-point-index N     First recommended spawn point to try (default: 0)
      --no-spawn                Fail instead of spawning when the role is absent
      --allow-version-mismatch  Warn instead of failing on client/server mismatch
      --max-frames N            Stop after N telemetry frames (default: 1;
                                0 means unlimited)
      --run-seconds N           Maximum wall-clock run time (default: 0,
                                no time limit)
      --fixed-delta-seconds S   Synchronous simulation step (default: 0.05)
      --real-time               Pace owned ticks against the wall clock
      --autopilot               Drive the ego vehicle with Traffic Manager
      --chase-camera            Follow the ego vehicle with the spectator
      --chase-camera-response N Camera smoothing response (default: 10;
                                lower values are smoother but add more lag)
      --chase-camera-update-hz N
                                Camera interpolation rate (default: 60)
      --exposure-offset EV      Unreal exposure compensation for this run
                                (default: 0; restored on exit)
      --gnss-sensor-tick-seconds S
                                GNSS measurement period (default: 0.1)
      --gnss-max-age-seconds S  Omit older retained GNSS fixes (default: 0.25)
      --log-every-frames N      Print one sample summary every N frames
                                (default: 1)
      --viss                    Enable the TLS-only VISS 3.1 endpoint
      --viss-bind-address IP    VISS listener address (default: 127.0.0.1)
      --viss-port PORT          VISS Secure WebSocket port (default: 6443)
      --viss-cert FILE          PEM TLS certificate chain (required by --viss)
      --viss-key FILE           PEM TLS private key (required by --viss)
      --viss-max-clients N      Concurrent client cap (default: 8)
      --viss-max-subscriptions N
                                Subscription cap per client (default: 16)
      --viss-max-pending-messages N
                                Outbound queue cap per client (default: 8)
      --observe-ticks           Do not own or advance the simulation clock;
                                wait for another designated tick owner
)";
}

} // namespace carla_ego_runtime
