#include "carla_ego_runtime/viss_assignment_control.hpp"

#include <boost/asio.hpp>
#include <boost/json.hpp>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/socket.h>
#endif

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <istream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" __attribute__((weak, noinline)) bool
carla_ego_runtime_assignment_fail_after_bind_for_test() {
  return false;
}

namespace carla_ego_runtime {
namespace {

namespace asio = boost::asio;
namespace json = boost::json;
using LocalSocket = asio::local::stream_protocol;

constexpr std::size_t kMaximumRequestBytes = 4096;
constexpr std::size_t kMaximumRequestIdBytes = 128;

std::string AsString(const json::string &value) {
  return {value.data(), value.size()};
}

bool ExactKeys(const json::object &object,
               std::initializer_list<std::string_view> expected) {
  if (object.size() != expected.size()) {
    return false;
  }
  for (const auto key : expected) {
    if (object.if_contains(key) == nullptr) {
      return false;
    }
  }
  return true;
}

bool IsBoundedRequestId(std::string_view value) {
  if (value.empty() || value.size() > kMaximumRequestIdBytes) {
    return false;
  }
  for (const auto character : value) {
    const bool allowed = (character >= 'a' && character <= 'z') ||
                         (character >= 'A' && character <= 'Z') ||
                         (character >= '0' && character <= '9') ||
                         character == '-' || character == '_' ||
                         character == '.';
    if (!allowed) {
      return false;
    }
  }
  return true;
}

std::optional<std::uint64_t> UnsignedInteger(const json::value *value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  if (value->is_uint64()) {
    return value->as_uint64();
  }
  if (value->is_int64() && value->as_int64() >= 0) {
    return static_cast<std::uint64_t>(value->as_int64());
  }
  return std::nullopt;
}

// Accepted member names are plain ASCII. Rejecting escaped object keys keeps
// duplicate detection closed even for aliases such as "action"/"a\u0063tion".
bool HasDuplicateOrEscapedObjectKey(std::string_view input) {
  struct ObjectFrame {
    bool object = false;
    bool expecting_key = false;
    std::set<std::string> keys;
  };
  std::vector<ObjectFrame> stack;
  for (std::size_t index = 0; index < input.size();) {
    const auto character = input[index];
    if (character == '{') {
      stack.push_back({true, true, {}});
      ++index;
      continue;
    }
    if (character == '[') {
      stack.push_back({false, false, {}});
      ++index;
      continue;
    }
    if (character == '}' || character == ']') {
      if (!stack.empty()) {
        stack.pop_back();
      }
      ++index;
      continue;
    }
    if (character == ',' && !stack.empty() && stack.back().object) {
      stack.back().expecting_key = true;
      ++index;
      continue;
    }
    if (character != '"') {
      ++index;
      continue;
    }

    std::string value;
    bool escaped = false;
    ++index;
    while (index < input.size()) {
      const auto item = input[index++];
      if (item == '\\') {
        escaped = true;
        if (index < input.size()) {
          ++index;
        }
        continue;
      }
      if (item == '"') {
        break;
      }
      value.push_back(item);
    }
    if (!stack.empty() && stack.back().object && stack.back().expecting_key) {
      if (escaped || !stack.back().keys.insert(value).second) {
        return true;
      }
      stack.back().expecting_key = false;
    }
  }
  return false;
}

json::object BaseResponse(std::string_view request_id, std::string_view result,
                          std::string_view reason, bool selected,
                          std::uint64_t generation) {
  json::object response;
  response["schemaVersion"] = 1;
  response["requestId"] = request_id;
  response["result"] = result;
  response["reason"] = reason;
  response["state"] = selected ? "SELECTED" : "DETACHED";
  response["assignmentGeneration"] = generation;
  return response;
}

std::string ErrorPayload(std::string_view request_id, std::string_view reason,
                         bool selected, std::uint64_t generation) {
  return json::serialize(
      BaseResponse(request_id, "REJECTED", reason, selected, generation));
}

bool SameUid(int socket) {
#if defined(__APPLE__)
  uid_t effective_uid = 0;
  gid_t effective_gid = 0;
  return getpeereid(socket, &effective_uid, &effective_gid) == 0 &&
         effective_uid == geteuid();
#elif defined(__linux__)
  struct ucred credentials{};
  socklen_t size = sizeof(credentials);
  return getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &size) ==
             0 &&
         size == sizeof(credentials) && credentials.uid == geteuid();
#else
#error "Selected-Unit assignment requires getpeereid or SO_PEERCRED"
#endif
}

void ValidateSocketParent(const std::string &socket_file) {
  if (socket_file.empty() ||
      socket_file.size() >= sizeof(sockaddr_un::sun_path)) {
    throw std::invalid_argument("invalid VISS assignment socket path");
  }
  const auto parent = std::filesystem::path(socket_file).parent_path();
  struct stat status{};
  if (parent.empty() || lstat(parent.c_str(), &status) != 0 ||
      !S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode) ||
      (status.st_mode & 0777) != 0700 || status.st_uid != geteuid()) {
    throw std::invalid_argument(
        "VISS assignment socket parent must be owner-only mode 0700");
  }
  if (lstat(socket_file.c_str(), &status) == 0 || errno != ENOENT) {
    throw std::invalid_argument("VISS assignment socket path already exists");
  }
}

} // namespace

VissAssignmentState::VissAssignmentState(
    std::uint64_t initial_generation,
    std::string engineering_dashboard_certificate_sha256,
    std::optional<std::string> qualification_certificate_sha256)
    : generation_(initial_generation),
      engineering_dashboard_certificate_sha256_(
          std::move(engineering_dashboard_certificate_sha256)),
      qualification_certificate_sha256_(
          std::move(qualification_certificate_sha256)) {
  if (!IsCanonicalSha256(engineering_dashboard_certificate_sha256_) ||
      (qualification_certificate_sha256_.has_value() &&
       !IsCanonicalSha256(*qualification_certificate_sha256_))) {
    throw std::invalid_argument(
        "invalid independent VISS certificate fingerprint");
  }
}

VissAssignmentReply
VissAssignmentState::HandleRequest(std::string_view request,
                                   std::uint64_t current_frame,
                                   VissActiveRoleCounts active_counts) {
  if (request.empty() || request.size() > kMaximumRequestBytes ||
      HasDuplicateOrEscapedObjectKey(request)) {
    return {ErrorPayload("", "MALFORMED_REQUEST", selected(), generation_),
            VissAssignmentMutation::None};
  }
  boost::system::error_code error;
  const auto parsed =
      json::parse(json::string_view(request.data(), request.size()), error);
  if (error || !parsed.is_object()) {
    return {ErrorPayload("", "MALFORMED_REQUEST", selected(), generation_),
            VissAssignmentMutation::None};
  }
  const auto &object = parsed.as_object();
  const auto *schema = object.if_contains("schemaVersion");
  const auto *action_value = object.if_contains("action");
  const auto *request_id_value = object.if_contains("requestId");
  if (UnsignedInteger(schema) != 1 || action_value == nullptr ||
      !action_value->is_string() || request_id_value == nullptr ||
      !request_id_value->is_string()) {
    return {ErrorPayload("", "MALFORMED_REQUEST", selected(), generation_),
            VissAssignmentMutation::None};
  }
  const auto action = AsString(action_value->as_string());
  const auto request_id = AsString(request_id_value->as_string());
  if (!IsBoundedRequestId(request_id)) {
    return {ErrorPayload("", "MALFORMED_REQUEST", selected(), generation_),
            VissAssignmentMutation::None};
  }

  if (action == "status") {
    if (!ExactKeys(object, {"schemaVersion", "action", "requestId"})) {
      return {ErrorPayload(request_id, "MALFORMED_REQUEST", selected(),
                           generation_),
              VissAssignmentMutation::None};
    }
    auto response =
        BaseResponse(request_id, "ACCEPTED", "NONE", selected(), generation_);
    json::object counts;
    counts["selectedPlatformUnit"] = active_counts.selected_platform_unit;
    counts["platformUpdateRuntime"] = active_counts.platform_update_runtime;
    counts["engineeringDashboard"] = active_counts.engineering_dashboard;
    counts["qualificationClient"] = active_counts.qualification_client;
    response["activeRoleCounts"] = std::move(counts);
    if (selected_source_.has_value()) {
      json::object source;
      source["unitId"] = selected_source_->unit_id;
      source["nodeId"] = selected_source_->node_id;
      source["selectedPlatformUnitCertificateSha256"] =
          selected_source_->selected_platform_unit_certificate_sha256;
      source["platformUpdateRuntimeCertificateSha256"] =
          selected_source_->platform_update_runtime_certificate_sha256;
      response["selectedSource"] = std::move(source);
    }
    return {json::serialize(response), VissAssignmentMutation::None};
  }

  if (!ExactKeys(object, {"schemaVersion", "action", "requestId",
                          "expectedAssignmentGeneration", "selectedSource"})) {
    return {
        ErrorPayload(request_id, "MALFORMED_REQUEST", selected(), generation_),
        VissAssignmentMutation::None};
  }
  const auto expected =
      UnsignedInteger(object.if_contains("expectedAssignmentGeneration"));
  const auto *source_value = object.if_contains("selectedSource");
  if (!expected.has_value() || source_value == nullptr ||
      !source_value->is_object()) {
    return {
        ErrorPayload(request_id, "MALFORMED_REQUEST", selected(), generation_),
        VissAssignmentMutation::None};
  }
  if (*expected != generation_) {
    return {
        ErrorPayload(request_id, "STALE_GENERATION", selected(), generation_),
        VissAssignmentMutation::None};
  }
  if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
    return {ErrorPayload(request_id, "GENERATION_OVERFLOW", selected(),
                         generation_),
            VissAssignmentMutation::None};
  }

  const auto &source = source_value->as_object();
  const auto *unit_value = source.if_contains("unitId");
  const auto *node_value = source.if_contains("nodeId");
  if (unit_value == nullptr || !unit_value->is_string() ||
      node_value == nullptr || !node_value->is_string()) {
    return {
        ErrorPayload(request_id, "MALFORMED_REQUEST", selected(), generation_),
        VissAssignmentMutation::None};
  }
  const auto unit_id = AsString(unit_value->as_string());
  const auto node_id = AsString(node_value->as_string());
  if (!IsCanonicalUuid(unit_id) || !IsCanonicalUuid(node_id)) {
    return {
        ErrorPayload(request_id, "MALFORMED_REQUEST", selected(), generation_),
        VissAssignmentMutation::None};
  }

  if (action == "select") {
    if (selected_source_.has_value()) {
      return {ErrorPayload(request_id, "DETACH_REQUIRED", true, generation_),
              VissAssignmentMutation::None};
    }
    if (!ExactKeys(source,
                   {"unitId", "nodeId", "selectedPlatformUnitCertificateSha256",
                    "platformUpdateRuntimeCertificateSha256"})) {
      return {ErrorPayload(request_id, "MALFORMED_REQUEST", false, generation_),
              VissAssignmentMutation::None};
    }
    const auto *selected_fingerprint =
        source.if_contains("selectedPlatformUnitCertificateSha256");
    const auto *runtime_fingerprint =
        source.if_contains("platformUpdateRuntimeCertificateSha256");
    if (selected_fingerprint == nullptr || !selected_fingerprint->is_string() ||
        runtime_fingerprint == nullptr || !runtime_fingerprint->is_string()) {
      return {ErrorPayload(request_id, "MALFORMED_REQUEST", false, generation_),
              VissAssignmentMutation::None};
    }
    const auto selected_sha = AsString(selected_fingerprint->as_string());
    const auto runtime_sha = AsString(runtime_fingerprint->as_string());
    if (!IsCanonicalSha256(selected_sha) || !IsCanonicalSha256(runtime_sha) ||
        selected_sha == runtime_sha) {
      return {
          ErrorPayload(request_id, "INVALID_ENROLLMENT", false, generation_),
          VissAssignmentMutation::None};
    }
    selected_source_ =
        SelectedSource{unit_id, node_id, selected_sha, runtime_sha};
    minimum_frame_exclusive_ = current_frame;
    ++generation_;
    auto response =
        BaseResponse(request_id, "ACCEPTED", "NONE", true, generation_);
    return {json::serialize(response), VissAssignmentMutation::Selected};
  }

  if (action == "detach") {
    if (!ExactKeys(source, {"unitId", "nodeId"})) {
      return {ErrorPayload(request_id, "MALFORMED_REQUEST", selected(),
                           generation_),
              VissAssignmentMutation::None};
    }
    if (!selected_source_.has_value() || selected_source_->unit_id != unit_id ||
        selected_source_->node_id != node_id) {
      return {ErrorPayload(request_id, "IDENTITY_MISMATCH", selected(),
                           generation_),
              VissAssignmentMutation::None};
    }
    selected_source_.reset();
    minimum_frame_exclusive_ = current_frame;
    ++generation_;
    auto response =
        BaseResponse(request_id, "ACCEPTED", "NONE", false, generation_);
    return {json::serialize(response), VissAssignmentMutation::Detached};
  }
  return {
      ErrorPayload(request_id, "MALFORMED_REQUEST", selected(), generation_),
      VissAssignmentMutation::None};
}

std::optional<VissSessionAccess>
VissAssignmentState::Authorize(const VissCertificateIdentity &identity) const {
  if (identity.role == VissClientRole::EngineeringDashboard) {
    if (identity.certificate_sha256 ==
        engineering_dashboard_certificate_sha256_) {
      return VissSessionAccess{identity.role, generation_, 0};
    }
    return std::nullopt;
  }
  if (identity.role == VissClientRole::QualificationClient) {
    if (qualification_certificate_sha256_.has_value() &&
        identity.certificate_sha256 == *qualification_certificate_sha256_) {
      return VissSessionAccess{identity.role, generation_, 0};
    }
    return std::nullopt;
  }
  if (!IsSelectedBoundRole(identity.role) || !selected_source_.has_value() ||
      identity.unit_id != selected_source_->unit_id ||
      identity.node_id != selected_source_->node_id) {
    return std::nullopt;
  }
  const auto &expected =
      identity.role == VissClientRole::SelectedPlatformUnit
          ? selected_source_->selected_platform_unit_certificate_sha256
          : selected_source_->platform_update_runtime_certificate_sha256;
  if (identity.certificate_sha256 != expected) {
    return std::nullopt;
  }
  return VissSessionAccess{identity.role, generation_,
                           minimum_frame_exclusive_};
}

std::uint64_t VissAssignmentState::generation() const { return generation_; }
bool VissAssignmentState::selected() const {
  return selected_source_.has_value();
}

class VissAssignmentControl::Impl {
public:
  Impl(asio::io_context &io_context, VissAssignmentControlConfig config,
       VissAssignmentState &state, FrameProvider frame_provider,
       ActiveCountsProvider active_counts_provider,
       MutationHandler mutation_handler)
      : config_(std::move(config)), state_(state),
        frame_provider_(std::move(frame_provider)),
        active_counts_provider_(std::move(active_counts_provider)),
        mutation_handler_(std::move(mutation_handler)), acceptor_(io_context) {
    if (!frame_provider_ || !active_counts_provider_ || !mutation_handler_) {
      throw std::invalid_argument("VISS assignment callbacks are required");
    }
  }

  ~Impl() { Stop(); }

  void Start() {
    if (started_) {
      throw std::logic_error("VISS assignment control is already running");
    }
    ValidateSocketParent(config_.socket_file);
    try {
      boost::system::error_code error;
      acceptor_.open(LocalSocket(), error);
      ThrowOnError(error, "open VISS assignment socket");
      acceptor_.bind(LocalSocket::endpoint(config_.socket_file), error);
      ThrowOnError(error, "bind VISS assignment socket");

      struct stat bound_socket{};
      if (lstat(config_.socket_file.c_str(), &bound_socket) != 0 ||
          !S_ISSOCK(bound_socket.st_mode) || bound_socket.st_uid != geteuid()) {
        throw std::runtime_error("could not identify VISS assignment socket");
      }
      owned_socket_ = bound_socket;
      owns_socket_ = true;

      // Product binaries use the weak default-false definition above. The
      // test links a strong definition to exercise the otherwise
      // non-deterministic failure window after bind and before security/listen
      // completion.
      if (carla_ego_runtime_assignment_fail_after_bind_for_test()) {
        throw std::runtime_error("injected VISS assignment post-bind failure");
      }

      struct stat secured_socket{};
      if (chmod(config_.socket_file.c_str(), 0600) != 0 ||
          lstat(config_.socket_file.c_str(), &secured_socket) != 0 ||
          !S_ISSOCK(secured_socket.st_mode) ||
          secured_socket.st_uid != geteuid() ||
          (secured_socket.st_mode & 0777) != 0600 ||
          secured_socket.st_dev != owned_socket_.st_dev ||
          secured_socket.st_ino != owned_socket_.st_ino) {
        throw std::runtime_error("could not secure VISS assignment socket");
      }
      acceptor_.listen(asio::socket_base::max_listen_connections, error);
      ThrowOnError(error, "listen on VISS assignment socket");
      started_ = true;
      AcceptNext();
    } catch (...) {
      started_ = false;
      boost::system::error_code ignored;
      acceptor_.cancel(ignored);
      acceptor_.close(ignored);
      RemoveOwnedSocket();
      throw;
    }
  }

  void Stop() {
    if (!started_ && !owns_socket_) {
      return;
    }
    started_ = false;
    boost::system::error_code ignored;
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);
    const auto connections = connections_;
    for (const auto &connection : connections) {
      connection->Stop();
    }
    RemoveOwnedSocket();
  }

private:
  class Connection : public std::enable_shared_from_this<Connection> {
  public:
    Connection(LocalSocket::socket socket, Impl &owner)
        : socket_(std::move(socket)), input_(kMaximumRequestBytes + 2),
          owner_(owner) {}

    void Start() {
      if (!SameUid(socket_.native_handle())) {
        Stop();
        return;
      }
      asio::async_read_until(
          socket_, input_, '\n',
          [self = shared_from_this()](boost::system::error_code error,
                                      std::size_t bytes) {
            self->OnRead(error, bytes);
          });
    }

    void Stop() {
      if (finished_) {
        return;
      }
      finished_ = true;
      boost::system::error_code ignored;
      socket_.cancel(ignored);
      socket_.shutdown(LocalSocket::socket::shutdown_both, ignored);
      socket_.close(ignored);
      owner_.Remove(shared_from_this());
    }

  private:
    void OnRead(boost::system::error_code error, std::size_t bytes) {
      if (error || bytes == 0 || bytes > kMaximumRequestBytes + 1 ||
          input_.size() != bytes) {
        Stop();
        return;
      }
      std::istream stream(&input_);
      std::string request;
      std::getline(stream, request);
      const auto reply = owner_.state_.HandleRequest(
          request, owner_.frame_provider_(), owner_.active_counts_provider_());
      if (reply.mutation != VissAssignmentMutation::None) {
        owner_.mutation_handler_(reply.mutation);
      }
      output_ = reply.payload + "\n";
      asio::async_write(
          socket_, asio::buffer(output_),
          [self = shared_from_this()](boost::system::error_code, std::size_t) {
            self->Stop();
          });
    }

    LocalSocket::socket socket_;
    asio::streambuf input_;
    Impl &owner_;
    std::string output_;
    bool finished_ = false;
  };

  static void ThrowOnError(const boost::system::error_code &error,
                           std::string_view operation) {
    if (error) {
      throw std::runtime_error(std::string(operation) + ": " + error.message());
    }
  }

  void AcceptNext() {
    acceptor_.async_accept(
        [this](boost::system::error_code error, LocalSocket::socket socket) {
          if (!error) {
            auto connection =
                std::make_shared<Connection>(std::move(socket), *this);
            connections_.insert(connection);
            connection->Start();
          }
          if (started_) {
            AcceptNext();
          }
        });
  }

  void Remove(const std::shared_ptr<Connection> &connection) {
    connections_.erase(connection);
  }

  void RemoveOwnedSocket() {
    if (!owns_socket_) {
      return;
    }
    struct stat current{};
    if (lstat(config_.socket_file.c_str(), &current) == 0 &&
        S_ISSOCK(current.st_mode) && current.st_dev == owned_socket_.st_dev &&
        current.st_ino == owned_socket_.st_ino) {
      std::error_code ignored;
      std::filesystem::remove(config_.socket_file, ignored);
    }
    owns_socket_ = false;
  }

  VissAssignmentControlConfig config_;
  VissAssignmentState &state_;
  FrameProvider frame_provider_;
  ActiveCountsProvider active_counts_provider_;
  MutationHandler mutation_handler_;
  LocalSocket::acceptor acceptor_;
  std::set<std::shared_ptr<Connection>> connections_;
  struct stat owned_socket_{};
  bool started_ = false;
  bool owns_socket_ = false;
};

VissAssignmentControl::VissAssignmentControl(
    boost::asio::io_context &io_context, VissAssignmentControlConfig config,
    VissAssignmentState &state, FrameProvider frame_provider,
    ActiveCountsProvider active_counts_provider,
    MutationHandler mutation_handler)
    : impl_(std::make_unique<Impl>(
          io_context, std::move(config), state, std::move(frame_provider),
          std::move(active_counts_provider), std::move(mutation_handler))) {}

VissAssignmentControl::~VissAssignmentControl() = default;
void VissAssignmentControl::Start() { impl_->Start(); }
void VissAssignmentControl::Stop() { impl_->Stop(); }

} // namespace carla_ego_runtime
