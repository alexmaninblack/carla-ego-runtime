#include "carla_ego_runtime/runtime_options.hpp"

#include <charconv>
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
      parsed > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
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

}  // namespace

ParsedCommandLine ParseCommandLine(const std::vector<std::string> &arguments) {
  ParsedCommandLine result;

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
      result.options.port =
          ParseUnsigned<std::uint16_t>(RequireValue(arguments, index), "--port");
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
    } else if (argument == "--no-spawn") {
      result.options.spawn_if_missing = false;
    } else if (argument == "--allow-version-mismatch") {
      result.options.require_matching_versions = false;
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
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
      --run-seconds N           Keep the connection alive for N seconds
                                (default: 0, connect and validate only)
)";
}

}  // namespace carla_ego_runtime
