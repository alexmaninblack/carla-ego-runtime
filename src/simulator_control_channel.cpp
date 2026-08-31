#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "carla_ego_runtime/simulator_control_channel.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace carla_ego_runtime {
namespace {

void Increment(std::uint64_t &value) {
  if (value != std::numeric_limits<std::uint64_t>::max()) {
    ++value;
  }
}

bool ValidUtf8(std::string_view value) {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    if (first <= 0x7f) {
      length = 1;
      code_point = first;
    } else if ((first & 0xe0) == 0xc0) {
      length = 2;
      code_point = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
      length = 3;
      code_point = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
      length = 4;
      code_point = first & 0x07;
    } else {
      return false;
    }
    if (index + length > value.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xc0) != 0x80) {
        return false;
      }
      code_point = (code_point << 6) | (next & 0x3f);
    }
    if ((length == 2 && code_point < 0x80) ||
        (length == 3 && code_point < 0x800) ||
        (length == 4 && code_point < 0x10000) || code_point > 0x10ffff ||
        (code_point >= 0xd800 && code_point <= 0xdfff)) {
      return false;
    }
    index += length;
  }
  return true;
}

void AppendUtf8(std::string &output, std::uint32_t code_point) {
  if (code_point <= 0x7f) {
    output.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else if (code_point <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else {
    output.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
}

class JsonReader {
 public:
  explicit JsonReader(std::string_view input) : input_(input) {}

  void Whitespace() {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\n' ||
            input_[position_] == '\r' || input_[position_] == '\t')) {
      ++position_;
    }
  }

  bool Consume(char expected) {
    Whitespace();
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  bool End() {
    Whitespace();
    return position_ == input_.size();
  }

  std::optional<std::string> String() {
    Whitespace();
    if (position_ >= input_.size() || input_[position_++] != '"') {
      return std::nullopt;
    }
    std::string output;
    while (position_ < input_.size()) {
      const auto character = static_cast<unsigned char>(input_[position_++]);
      if (character == '"') {
        return ValidUtf8(output) ? std::optional<std::string>(std::move(output))
                                 : std::nullopt;
      }
      if (character < 0x20) {
        return std::nullopt;
      }
      if (character != '\\') {
        output.push_back(static_cast<char>(character));
        continue;
      }
      if (position_ >= input_.size()) {
        return std::nullopt;
      }
      const char escape = input_[position_++];
      switch (escape) {
        case '"': output.push_back('"'); break;
        case '\\': output.push_back('\\'); break;
        case '/': output.push_back('/'); break;
        case 'b': output.push_back('\b'); break;
        case 'f': output.push_back('\f'); break;
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        case 'u': {
          auto code_point = HexQuad();
          if (!code_point.has_value()) {
            return std::nullopt;
          }
          if (*code_point >= 0xd800 && *code_point <= 0xdbff) {
            if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                input_[position_ + 1] != 'u') {
              return std::nullopt;
            }
            position_ += 2;
            const auto low = HexQuad();
            if (!low.has_value() || *low < 0xdc00 || *low > 0xdfff) {
              return std::nullopt;
            }
            code_point = 0x10000 + ((*code_point - 0xd800) << 10) +
                         (*low - 0xdc00);
          } else if (*code_point >= 0xdc00 && *code_point <= 0xdfff) {
            return std::nullopt;
          }
          AppendUtf8(output, *code_point);
          break;
        }
        default: return std::nullopt;
      }
    }
    return std::nullopt;
  }

  std::optional<std::uint64_t> Unsigned() {
    Whitespace();
    const auto begin = position_;
    if (position_ >= input_.size() || input_[position_] < '0' ||
        input_[position_] > '9') {
      return std::nullopt;
    }
    if (input_[position_] == '0') {
      ++position_;
      if (position_ < input_.size() && input_[position_] >= '0' &&
          input_[position_] <= '9') {
        return std::nullopt;
      }
    } else {
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(input_.data() + begin,
                                        input_.data() + position_, value);
    if (result.ec != std::errc{} || result.ptr != input_.data() + position_) {
      return std::nullopt;
    }
    return value;
  }

  std::optional<double> Number() {
    Whitespace();
    const auto begin = position_;
    if (position_ < input_.size() && input_[position_] == '-') {
      ++position_;
    }
    if (position_ >= input_.size() || input_[position_] < '0' ||
        input_[position_] > '9') {
      return std::nullopt;
    }
    if (input_[position_] == '0') {
      ++position_;
    } else {
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      const auto fraction = position_;
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
      if (position_ == fraction) {
        return std::nullopt;
      }
    }
    if (position_ < input_.size() &&
        (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() &&
          (input_[position_] == '+' || input_[position_] == '-')) {
        ++position_;
      }
      const auto exponent = position_;
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
      if (position_ == exponent) {
        return std::nullopt;
      }
    }
    double value = 0.0;
    const auto result = std::from_chars(input_.data() + begin,
                                        input_.data() + position_, value);
    if (result.ec != std::errc{} || result.ptr != input_.data() + position_ ||
        !std::isfinite(value)) {
      return std::nullopt;
    }
    return value;
  }

  std::optional<bool> Boolean() {
    Whitespace();
    if (input_.substr(position_, 4) == "true") {
      position_ += 4;
      return true;
    }
    if (input_.substr(position_, 5) == "false") {
      position_ += 5;
      return false;
    }
    return std::nullopt;
  }

 private:
  std::optional<std::uint32_t> HexQuad() {
    if (position_ + 4 > input_.size()) {
      return std::nullopt;
    }
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
      const char digit = input_[position_++];
      value <<= 4;
      if (digit >= '0' && digit <= '9') {
        value |= static_cast<std::uint32_t>(digit - '0');
      } else if (digit >= 'a' && digit <= 'f') {
        value |= static_cast<std::uint32_t>(digit - 'a' + 10);
      } else if (digit >= 'A' && digit <= 'F') {
        value |= static_cast<std::uint32_t>(digit - 'A' + 10);
      } else {
        return std::nullopt;
      }
    }
    return value;
  }

  std::string_view input_;
  std::size_t position_ = 0;
};

std::optional<SimulatorDriveMode> ParseMode(const std::string &value) {
  if (value == "SAFE_STOP") return SimulatorDriveMode::kSafeStop;
  if (value == "SCENARIO") return SimulatorDriveMode::kScenario;
  if (value == "MANUAL") return SimulatorDriveMode::kManual;
  if (value == "AUTOPILOT") return SimulatorDriveMode::kAutopilot;
  return std::nullopt;
}

std::optional<SimulatorTransitionState> ParseTransition(
    const std::string &value) {
  if (value == "STABLE") return SimulatorTransitionState::kStable;
  if (value == "PREPARING") return SimulatorTransitionState::kPreparing;
  if (value == "FAILED") return SimulatorTransitionState::kFailed;
  return std::nullopt;
}

template <typename Queue>
auto OldestIterator(Queue &queue) {
  return std::min_element(queue.begin(), queue.end(),
                          [](const auto &left, const auto &right) {
                            return left.received < right.received;
                          });
}

bool SetNonBlockingCloseOnExec(int descriptor) {
  const int status_flags = ::fcntl(descriptor, F_GETFL, 0);
  const int descriptor_flags = ::fcntl(descriptor, F_GETFD, 0);
  return status_flags >= 0 && descriptor_flags >= 0 &&
         ::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) == 0 &&
         ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
}

bool PeerHasSameEffectiveUid(int descriptor) {
#if defined(__APPLE__)
  uid_t peer_uid = 0;
  gid_t peer_gid = 0;
  return ::getpeereid(descriptor, &peer_uid, &peer_gid) == 0 &&
         peer_uid == ::geteuid();
#elif defined(__linux__)
  struct ucred credentials {};
  socklen_t credentials_size = sizeof(credentials);
  return ::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials,
                      &credentials_size) == 0 &&
         credentials_size == sizeof(credentials) &&
         credentials.uid == ::geteuid();
#else
  (void)descriptor;
  return false;
#endif
}

}  // namespace

SimulatorControlDecodeResult DecodeSimulatorControlRecord(
    std::string_view payload) {
  if (payload.empty() || payload.size() > kMaximumControlFrameBodyBytes ||
      !ValidUtf8(payload)) {
    return {std::nullopt, "payload is empty, oversize, or invalid UTF-8"};
  }
  JsonReader reader(payload);
  if (!reader.Consume('{')) {
    return {std::nullopt, "record must be a JSON object"};
  }

  SimulatorControlRecord record;
  std::array<bool, 11> seen{};
  auto fail = [](std::string error) {
    return SimulatorControlDecodeResult{std::nullopt, std::move(error)};
  };
  bool first = true;
  while (true) {
    reader.Whitespace();
    if (reader.Consume('}')) {
      break;
    }
    if (!first && !reader.Consume(',')) {
      return fail("object fields must be comma-separated");
    }
    first = false;
    const auto key = reader.String();
    if (!key.has_value() || !reader.Consume(':')) {
      return fail("invalid object field");
    }
    std::size_t field = seen.size();
    if (*key == "schemaVersion") field = 0;
    else if (*key == "runId") field = 1;
    else if (*key == "egoActorId") field = 2;
    else if (*key == "frameId") field = 3;
    else if (*key == "simulationTime") field = 4;
    else if (*key == "activeMode") field = 5;
    else if (*key == "transitionState") field = 6;
    else if (*key == "controlGeneration") field = 7;
    else if (*key == "resetGeneration") field = 8;
    else if (*key == "resetInProgress") field = 9;
    else if (*key == "resetDiscontinuity") field = 10;
    if (field == seen.size()) {
      return fail("unknown field: " + *key);
    }
    if (seen[field]) {
      return fail("duplicate field: " + *key);
    }
    seen[field] = true;

    switch (field) {
      case 0: {
        const auto value = reader.Unsigned();
        if (!value.has_value() || *value != 1) {
          return fail("schemaVersion must equal 1");
        }
        break;
      }
      case 1: {
        const auto value = reader.String();
        if (!value.has_value() || value->empty() ||
            value->find('\0') != std::string::npos) {
          return fail("runId must be a non-empty string");
        }
        record.run_id = *value;
        break;
      }
      case 2: {
        const auto value = reader.Unsigned();
        if (!value.has_value()) return fail("egoActorId must be uint64");
        record.ego_actor_id = *value;
        break;
      }
      case 3: {
        const auto value = reader.Unsigned();
        if (!value.has_value()) return fail("frameId must be uint64");
        record.frame_id = *value;
        break;
      }
      case 4: {
        const auto value = reader.Number();
        if (!value.has_value() || *value < 0.0) {
          return fail("simulationTime must be finite and non-negative");
        }
        record.simulation_time_s = *value;
        break;
      }
      case 5: {
        const auto value = reader.String();
        const auto parsed = value.has_value() ? ParseMode(*value) : std::nullopt;
        if (!parsed.has_value()) return fail("activeMode is invalid");
        record.active_mode = *parsed;
        break;
      }
      case 6: {
        const auto value = reader.String();
        const auto parsed =
            value.has_value() ? ParseTransition(*value) : std::nullopt;
        if (!parsed.has_value()) return fail("transitionState is invalid");
        record.transition_state = *parsed;
        break;
      }
      case 7: {
        const auto value = reader.Unsigned();
        if (!value.has_value()) return fail("controlGeneration must be uint64");
        record.control_generation = *value;
        break;
      }
      case 8: {
        const auto value = reader.Unsigned();
        if (!value.has_value()) return fail("resetGeneration must be uint64");
        record.reset_generation = *value;
        break;
      }
      case 9: {
        const auto value = reader.Boolean();
        if (!value.has_value()) return fail("resetInProgress must be boolean");
        record.reset_in_progress = *value;
        break;
      }
      case 10: {
        const auto value = reader.Boolean();
        if (!value.has_value()) return fail("resetDiscontinuity must be boolean");
        record.reset_discontinuity = *value;
        break;
      }
      default: return fail("unreachable field");
    }
  }
  if (!reader.End() ||
      std::any_of(seen.begin(), seen.end(), [](bool value) { return !value; })) {
    return fail("record is incomplete or has trailing input");
  }
  if (record.reset_in_progress && record.reset_discontinuity) {
    return fail("reset cannot be in progress and discontinuous simultaneously");
  }
  return {record, {}};
}

SimulatorControlJoin::SimulatorControlJoin(std::string expected_run_id,
                                           std::uint64_t expected_ego_actor_id)
    : expected_run_id_(std::move(expected_run_id)),
      expected_ego_actor_id_(expected_ego_actor_id) {
  if (expected_run_id_.empty()) {
    throw std::invalid_argument("expected simulator run ID must not be empty");
  }
}

void SimulatorControlJoin::OfferPhysical(SimulatorPhysicalFrame frame,
                                         Clock::time_point received) {
  Expire(received);
  if (frame.run_id != expected_run_id_ ||
      frame.ego_actor_id != expected_ego_actor_id_ ||
      !std::isfinite(frame.simulation_time_s) || frame.simulation_time_s < 0.0) {
    Increment(diagnostics_.wrong_identity_records);
    return;
  }
  if (last_physical_frame_id_.has_value() &&
      frame.frame_id <= *last_physical_frame_id_) {
    Increment(diagnostics_.duplicate_or_out_of_order_records);
    return;
  }
  last_physical_frame_id_ = frame.frame_id;
  physical_.push_back({std::move(frame), received});
  MatchAvailable();
  EnforceCapacity();
}

void SimulatorControlJoin::OfferControl(SimulatorControlRecord record,
                                        Clock::time_point received) {
  Expire(received);
  if (record.run_id != expected_run_id_ ||
      record.ego_actor_id != expected_ego_actor_id_) {
    Increment(diagnostics_.wrong_identity_records);
    return;
  }
  if (last_control_frame_id_.has_value() &&
      record.frame_id <= *last_control_frame_id_) {
    Increment(diagnostics_.duplicate_or_out_of_order_records);
    return;
  }
  if ((last_control_generation_.has_value() &&
       record.control_generation < *last_control_generation_) ||
      (last_reset_generation_.has_value() &&
       record.reset_generation < *last_reset_generation_)) {
    Increment(diagnostics_.generation_regressions);
    return;
  }
  if (record.reset_discontinuity && last_reset_generation_.has_value() &&
      record.reset_generation <= *last_reset_generation_) {
    Increment(diagnostics_.generation_regressions);
    return;
  }
  last_control_frame_id_ = record.frame_id;
  last_control_generation_ = record.control_generation;
  last_reset_generation_ = record.reset_generation;
  control_.push_back({std::move(record), received});
  Increment(diagnostics_.frames_accepted);
  MatchAvailable();
  EnforceCapacity();
}

std::optional<SimulatorControlFacts> SimulatorControlJoin::TakeMatch(
    std::uint64_t frame_id, double simulation_time_s) {
  const auto match = std::find_if(matches_.begin(), matches_.end(),
                                  [frame_id, simulation_time_s](const auto &item) {
                                    return item.frame_id == frame_id &&
                                           item.simulation_time_s == simulation_time_s;
                                  });
  if (match == matches_.end()) {
    return std::nullopt;
  }
  auto facts = match->facts;
  matches_.erase(match);
  return facts;
}

void SimulatorControlJoin::Expire(Clock::time_point now) {
  const auto expired = [now](const auto &item) {
    return now - item.received >= kMaximumSimulatorRecordResidence;
  };
  while (!physical_.empty() && expired(physical_.front())) {
    physical_.pop_front();
    Increment(diagnostics_.expired_records);
  }
  while (!control_.empty() && expired(control_.front())) {
    control_.pop_front();
    Increment(diagnostics_.expired_records);
  }
}

void SimulatorControlJoin::NoteFrameReceived() {
  Increment(diagnostics_.frames_received);
}

void SimulatorControlJoin::NoteConnectionAccepted() {
  Increment(diagnostics_.connections_accepted);
}

void SimulatorControlJoin::NoteMalformedRecord() {
  Increment(diagnostics_.malformed_records);
}

void SimulatorControlJoin::NotePeerRejection() {
  Increment(diagnostics_.peer_rejections);
}

const SimulatorControlDiagnostics &SimulatorControlJoin::diagnostics() const {
  return diagnostics_;
}

void SimulatorControlJoin::MatchAvailable() {
  for (auto physical = physical_.begin(); physical != physical_.end();) {
    const auto control = std::find_if(
        control_.begin(), control_.end(), [&physical](const auto &candidate) {
          return candidate.record.frame_id == physical->frame.frame_id &&
                 candidate.record.simulation_time_s ==
                     physical->frame.simulation_time_s;
        });
    if (control == control_.end()) {
      ++physical;
      continue;
    }
    matches_.push_back(
        {control->record.frame_id, control->record.simulation_time_s,
         SimulatorControlFacts{
             control->record.frame_id, control->record.active_mode,
             control->record.transition_state,
             control->record.control_generation,
             control->record.reset_generation,
             control->record.reset_in_progress,
             control->record.reset_discontinuity}});
    control_.erase(control);
    physical = physical_.erase(physical);
    Increment(diagnostics_.records_matched);
  }
}

void SimulatorControlJoin::EnforceCapacity() {
  while (physical_.size() > kMaximumUnmatchedSimulatorRecords) {
    physical_.erase(OldestIterator(physical_));
    Increment(diagnostics_.capacity_evictions);
  }
  while (control_.size() > kMaximumUnmatchedSimulatorRecords) {
    control_.erase(OldestIterator(control_));
    Increment(diagnostics_.capacity_evictions);
  }
  while (matches_.size() > kMaximumUnmatchedSimulatorRecords) {
    matches_.pop_front();
    Increment(diagnostics_.capacity_evictions);
  }
}

SimulatorControlChannel::SimulatorControlChannel(
    SimulatorControlChannelConfig config)
    : config_(std::move(config)),
      join_(config_.expected_run_id, config_.expected_ego_actor_id) {
#if !defined(__APPLE__) && !defined(__linux__)
  throw std::runtime_error(
      "controller facts channel requires Darwin or Linux peer credentials");
#else
  if (config_.socket_path.empty()) {
    throw std::invalid_argument("controller facts socket path must not be empty");
  }
  const std::filesystem::path path(config_.socket_path);
  const auto parent = path.parent_path();
  struct stat parent_status {};
  if (parent.empty() || ::stat(parent.c_str(), &parent_status) != 0 ||
      !S_ISDIR(parent_status.st_mode) ||
      (parent_status.st_mode & 0777) != 0700 ||
      parent_status.st_uid != ::geteuid()) {
    throw std::runtime_error("controller facts runtime directory is not owner-only");
  }
  sockaddr_un address{};
  if (config_.socket_path.size() >= sizeof(address.sun_path)) {
    throw std::invalid_argument("controller facts socket path is too long");
  }
  struct stat existing {};
  if (::lstat(config_.socket_path.c_str(), &existing) == 0 || errno != ENOENT) {
    throw std::runtime_error("controller facts socket path already exists");
  }

  listener_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener_fd_ < 0) {
    throw std::runtime_error("failed to create controller facts socket: " +
                             std::string(std::strerror(errno)));
  }
  if (!SetNonBlockingCloseOnExec(listener_fd_)) {
    const auto error = std::string(std::strerror(errno));
    ::close(listener_fd_);
    listener_fd_ = -1;
    throw std::runtime_error(
        "failed to make controller facts listener non-blocking: " + error);
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, config_.socket_path.c_str(),
              config_.socket_path.size() + 1);
  if (::bind(listener_fd_, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0) {
    const auto error = std::string(std::strerror(errno));
    ::close(listener_fd_);
    listener_fd_ = -1;
    throw std::runtime_error("failed to bind private controller facts socket: " +
                             error);
  }
  if (::chmod(config_.socket_path.c_str(), 0600) != 0) {
    const auto error = std::string(std::strerror(errno));
    ::close(listener_fd_);
    listener_fd_ = -1;
    ::unlink(config_.socket_path.c_str());
    throw std::runtime_error(
        "failed to restrict controller facts socket: " + error);
  }
  struct stat socket_status {};
  if (::lstat(config_.socket_path.c_str(), &socket_status) != 0 ||
      !S_ISSOCK(socket_status.st_mode) ||
      (socket_status.st_mode & 0777) != 0600 ||
      socket_status.st_uid != ::geteuid()) {
    ::close(listener_fd_);
    listener_fd_ = -1;
    ::unlink(config_.socket_path.c_str());
    throw std::runtime_error("controller facts socket permissions are invalid");
  }
  socket_device_ = static_cast<std::uint64_t>(socket_status.st_dev);
  socket_inode_ = static_cast<std::uint64_t>(socket_status.st_ino);
  if (::listen(listener_fd_, 1) != 0) {
    const auto error = std::string(std::strerror(errno));
    RemoveOwnedSocket();
    throw std::runtime_error("failed to listen on controller facts socket: " +
                             error);
  }
#endif
}

SimulatorControlChannel::~SimulatorControlChannel() { RemoveOwnedSocket(); }

std::optional<SimulatorControlFacts> SimulatorControlChannel::WaitFor(
    const SimulatorPhysicalFrame &frame,
    std::chrono::milliseconds maximum_wait) {
  const auto started = SimulatorControlJoin::Clock::now();
  join_.OfferPhysical(frame, started);
  while (true) {
    if (auto match = join_.TakeMatch(frame.frame_id, frame.simulation_time_s)) {
      return match;
    }
    if (state_ == State::kListening) {
      (void)AcceptOne();
    }
    while (state_ == State::kConnected && ReceiveOne()) {
      if (auto match =
              join_.TakeMatch(frame.frame_id, frame.simulation_time_s)) {
        return match;
      }
    }
    if (auto match = join_.TakeMatch(frame.frame_id, frame.simulation_time_s)) {
      return match;
    }
    if (state_ == State::kUnavailable) {
      return std::nullopt;
    }
    const auto now = SimulatorControlJoin::Clock::now();
    join_.Expire(now);
    if (now - started >= maximum_wait) {
      return std::nullopt;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(maximum_wait -
                                                               (now - started));
    const int descriptor_fd = state_ == State::kListening ? listener_fd_
                                                           : connection_fd_;
    pollfd descriptor{descriptor_fd, POLLIN, 0};
    const auto result = ::poll(
        &descriptor, 1,
        std::max(1, static_cast<int>(remaining.count())));
    if (result < 0 && errno != EINTR) {
      MakeUnavailable();
      return std::nullopt;
    }
  }
}

SimulatorControlDiagnostics SimulatorControlChannel::diagnostics() const {
  return join_.diagnostics();
}

bool SimulatorControlChannel::AcceptOne() {
#if !defined(__APPLE__) && !defined(__linux__)
  return false;
#else
  if (state_ != State::kListening) {
    return false;
  }
  const int accepted = ::accept(listener_fd_, nullptr, nullptr);
  if (accepted < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return false;
    }
    MakeUnavailable();
    return false;
  }
  ::close(listener_fd_);
  listener_fd_ = -1;
  if (!SetNonBlockingCloseOnExec(accepted) ||
      !PeerHasSameEffectiveUid(accepted)) {
    join_.NotePeerRejection();
    ::close(accepted);
    state_ = State::kUnavailable;
    return true;
  }
  connection_fd_ = accepted;
  state_ = State::kConnected;
  join_.NoteConnectionAccepted();
  return true;
#endif
}

bool SimulatorControlChannel::ReceiveOne() {
#if !defined(__APPLE__) && !defined(__linux__)
  return false;
#else
  if (state_ != State::kConnected ||
      receive_size_ >= receive_buffer_.size()) {
    return false;
  }
  const auto received = ::recv(connection_fd_,
                               receive_buffer_.data() + receive_size_,
                               receive_buffer_.size() - receive_size_,
                               MSG_DONTWAIT);
  if (received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return false;
    }
    MakeUnavailable();
    return false;
  }
  if (received == 0) {
    MakeUnavailable(receive_size_ != 0);
    return false;
  }
  receive_size_ += static_cast<std::size_t>(received);
  (void)ConsumeFrames();
  return true;
#endif
}

bool SimulatorControlChannel::ConsumeFrames() {
  while (receive_size_ >= kControlFrameLengthBytes) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(
        receive_buffer_.data());
    const auto body_size =
        (static_cast<std::uint32_t>(bytes[0]) << 24U) |
        (static_cast<std::uint32_t>(bytes[1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[2]) << 8U) |
        static_cast<std::uint32_t>(bytes[3]);
    if (body_size == 0 || body_size > kMaximumControlFrameBodyBytes) {
      join_.NoteMalformedRecord();
      MakeUnavailable();
      return false;
    }
    const auto frame_size = kControlFrameLengthBytes + body_size;
    if (receive_size_ < frame_size) {
      return true;
    }
    join_.NoteFrameReceived();
    const auto decoded = DecodeSimulatorControlRecord(std::string_view(
        receive_buffer_.data() + kControlFrameLengthBytes, body_size));
    if (decoded.record.has_value()) {
      join_.OfferControl(*decoded.record, SimulatorControlJoin::Clock::now());
    } else {
      join_.NoteMalformedRecord();
    }
    const auto remaining = receive_size_ - frame_size;
    if (remaining != 0) {
      std::memmove(receive_buffer_.data(),
                   receive_buffer_.data() + frame_size, remaining);
    }
    receive_size_ = remaining;
  }
  return true;
}

void SimulatorControlChannel::MakeUnavailable(bool malformed_partial) noexcept {
  if (malformed_partial) {
    join_.NoteMalformedRecord();
  }
  if (connection_fd_ >= 0) {
    ::close(connection_fd_);
    connection_fd_ = -1;
  }
  if (listener_fd_ >= 0) {
    ::close(listener_fd_);
    listener_fd_ = -1;
  }
  receive_size_ = 0;
  state_ = State::kUnavailable;
}

void SimulatorControlChannel::RemoveOwnedSocket() noexcept {
  MakeUnavailable();
  if (config_.socket_path.empty() || socket_inode_ == 0) {
    return;
  }
  struct stat status {};
  if (::lstat(config_.socket_path.c_str(), &status) == 0 &&
      S_ISSOCK(status.st_mode) &&
      static_cast<std::uint64_t>(status.st_dev) == socket_device_ &&
      static_cast<std::uint64_t>(status.st_ino) == socket_inode_) {
    ::unlink(config_.socket_path.c_str());
  }
  socket_inode_ = 0;
}

}  // namespace carla_ego_runtime
