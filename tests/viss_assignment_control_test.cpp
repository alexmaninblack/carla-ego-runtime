#include "carla_ego_runtime/viss_assignment_control.hpp"

#include <boost/asio.hpp>
#include <boost/json.hpp>

#include <sys/stat.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <thread>

namespace {

namespace asio = boost::asio;
namespace json = boost::json;
using LocalSocket = asio::local::stream_protocol;

int failures = 0;
std::atomic<bool> fail_after_bind{false};

void Check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

json::object Parse(const std::string &payload) {
  boost::system::error_code error;
  auto value = json::parse(payload, error);
  Check(!error && value.is_object(), "assignment response is JSON object");
  return !error && value.is_object() ? value.as_object() : json::object{};
}

std::string StringAt(const json::object &object, std::string_view key) {
  const auto *value = object.if_contains(key);
  Check(value != nullptr && value->is_string(),
        "assignment response has string " + std::string(key));
  return value != nullptr && value->is_string()
             ? std::string(value->as_string().data(), value->as_string().size())
             : std::string{};
}

std::uint64_t Generation(const json::object &object) {
  const auto *value = object.if_contains("assignmentGeneration");
  Check(value != nullptr && (value->is_uint64() || value->is_int64()),
        "assignment response has generation");
  if (value == nullptr) {
    return 0;
  }
  return value->is_uint64() ? value->as_uint64()
                            : static_cast<std::uint64_t>(value->as_int64());
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::random_device random;
    path_ = std::filesystem::temp_directory_path() /
            ("carla-viss-assignment-test-" +
             std::to_string(static_cast<unsigned long long>(random())));
    std::filesystem::create_directory(path_);
    if (chmod(path_.c_str(), 0700) != 0) {
      throw std::runtime_error("could not secure assignment test directory");
    }
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

} // namespace

extern "C" bool carla_ego_runtime_assignment_fail_after_bind_for_test() {
  return fail_after_bind.exchange(false);
}

int main() {
  using namespace carla_ego_runtime;
  const std::string dashboard_sha(64, 'a');
  const std::string qualification_sha(64, 'b');
  const std::string selected_sha(64, 'c');
  const std::string runtime_sha(64, 'd');
  constexpr auto unit = "11111111-2222-4333-8444-555555555555";
  constexpr auto node = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";

  VissAssignmentState state(7, dashboard_sha, qualification_sha);
  auto response = Parse(
      state
          .HandleRequest(
              R"({"schemaVersion":1,"action":"status","requestId":"status-1"})",
              41, {})
          .payload);
  Check(StringAt(response, "state") == "DETACHED",
        "assignment starts detached");
  Check(Generation(response) == 7, "journal generation is restored");

  response = Parse(
      state
          .HandleRequest(
              R"({"schemaVersion":1,"action":"status","action":"select","requestId":"duplicate"})",
              41, {})
          .payload);
  Check(StringAt(response, "reason") == "MALFORMED_REQUEST",
        "duplicate JSON member rejected");

  const auto select =
      std::string(
          R"({"schemaVersion":1,"action":"select","requestId":"select-1","expectedAssignmentGeneration":7,"selectedSource":{"unitId":")") +
      unit + R"(","nodeId":")" + node +
      R"(","selectedPlatformUnitCertificateSha256":")" + selected_sha +
      R"(","platformUpdateRuntimeCertificateSha256":")" + runtime_sha +
      R"("}})";
  const auto selected_reply = state.HandleRequest(select, 42, {});
  response = Parse(selected_reply.payload);
  Check(selected_reply.mutation == VissAssignmentMutation::Selected,
        "select mutation reported");
  Check(StringAt(response, "state") == "SELECTED" && Generation(response) == 8,
        "select advances generation atomically");

  response = Parse(
      state
          .HandleRequest(
              R"({"schemaVersion":1,"action":"status","requestId":"lost-response-reconcile"})",
              42, {1, 1, 1, 0})
          .payload);
  Check(StringAt(response, "state") == "SELECTED" &&
            Generation(response) == 8 &&
            response.if_contains("selectedSource") != nullptr,
        "lost select response is reconciled by status without retry");

  VissCertificateIdentity selected_identity{
      VissClientRole::SelectedPlatformUnit, unit, node, selected_sha};
  const auto access = state.Authorize(selected_identity);
  Check(access.has_value() && access->assignment_generation == 8 &&
            access->minimum_frame_exclusive == 42,
        "selected identity receives generation and exclusive frame floor");
  selected_identity.certificate_sha256 = std::string(64, 'e');
  Check(!state.Authorize(selected_identity).has_value(),
        "wrong selected fingerprint denied");
  selected_identity.certificate_sha256 = selected_sha;
  selected_identity.unit_id = "22222222-3333-4444-8555-666666666666";
  Check(!state.Authorize(selected_identity).has_value(),
        "wrong selected Unit denied");
  selected_identity.unit_id = unit;
  selected_identity.node_id = "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff";
  Check(!state.Authorize(selected_identity).has_value(),
        "wrong selected Node denied");
  selected_identity.node_id = node;
  selected_identity.role = VissClientRole::PlatformUpdateRuntime;
  Check(!state.Authorize(selected_identity).has_value(),
        "certificate enrolled for the other selected role denied");
  selected_identity.role = VissClientRole::SelectedPlatformUnit;

  const auto replacement =
      std::string(
          R"({"schemaVersion":1,"action":"select","requestId":"replace","expectedAssignmentGeneration":8,"selectedSource":{"unitId":")") +
      unit + R"(","nodeId":")" + node +
      R"(","selectedPlatformUnitCertificateSha256":")" + selected_sha +
      R"(","platformUpdateRuntimeCertificateSha256":")" + runtime_sha +
      R"("}})";
  response = Parse(state.HandleRequest(replacement, 43, {}).payload);
  Check(StringAt(response, "reason") == "DETACH_REQUIRED",
        "direct replacement explicitly requires detach");
  Check(state.generation() == 8 && state.selected(),
        "rejected replacement retains assignment");

  const auto stale_detach =
      std::string(
          R"({"schemaVersion":1,"action":"detach","requestId":"detach-stale","expectedAssignmentGeneration":7,"selectedSource":{"unitId":")") +
      unit + R"(","nodeId":")" + node + R"("}})";
  response = Parse(state.HandleRequest(stale_detach, 44, {}).payload);
  Check(StringAt(response, "reason") == "STALE_GENERATION" &&
            state.generation() == 8,
        "stale detach has no side effect");

  const auto detach =
      std::string(
          R"({"schemaVersion":1,"action":"detach","requestId":"detach-1","expectedAssignmentGeneration":8,"selectedSource":{"unitId":")") +
      unit + R"(","nodeId":")" + node + R"("}})";
  const auto detached_reply = state.HandleRequest(detach, 45, {});
  response = Parse(detached_reply.payload);
  Check(detached_reply.mutation == VissAssignmentMutation::Detached &&
            StringAt(response, "state") == "DETACHED" &&
            Generation(response) == 9,
        "detach advances generation and clears assignment");
  selected_identity.certificate_sha256 = selected_sha;
  Check(!state.Authorize(selected_identity).has_value(),
        "detached selected certificate cannot reconnect");

  Check(state
            .Authorize(
                {VissClientRole::EngineeringDashboard, {}, {}, dashboard_sha})
            .has_value(),
        "independent dashboard enrollment accepted");
  Check(
      state
          .Authorize(
              {VissClientRole::QualificationClient, {}, {}, qualification_sha})
          .has_value(),
      "qualification enrollment accepted");

  VissAssignmentState malformed(0, dashboard_sha, std::nullopt);
  response =
      Parse(malformed.HandleRequest(std::string(4097, 'x'), 0, {}).payload);
  Check(StringAt(response, "reason") == "MALFORMED_REQUEST",
        "oversize assignment request rejected");
  std::string invalid_utf8 =
      R"({"schemaVersion":1,"action":"status","requestId":")";
  invalid_utf8.push_back(static_cast<char>(0xff));
  invalid_utf8 += R"("})";
  response = Parse(malformed.HandleRequest(invalid_utf8, 0, {}).payload);
  Check(StringAt(response, "reason") == "MALFORMED_REQUEST",
        "invalid UTF-8 assignment request rejected");
  response = Parse(
      malformed
          .HandleRequest(
              R"({"schema\u0056ersion":1,"action":"status","requestId":"escaped-key"})",
              0, {})
          .payload);
  Check(StringAt(response, "reason") == "MALFORMED_REQUEST",
        "escaped assignment object key rejected before mutation");
  response = Parse(
      malformed
          .HandleRequest(
              R"({"schemaVersion":1,"action":"status","requestId":"unknown-key","extra":true})",
              0, {})
          .payload);
  Check(StringAt(response, "reason") == "MALFORMED_REQUEST",
        "unknown assignment object key rejected");
  response = Parse(
      malformed
          .HandleRequest(
              R"({"schemaVersion":1,"action":"status","requestId":"trailing"} trailing)",
              0, {})
          .payload);
  Check(StringAt(response, "reason") == "MALFORMED_REQUEST",
        "assignment trailing content rejected");
  const auto equal_fingerprints =
      std::string(
          R"({"schemaVersion":1,"action":"select","requestId":"same-sha","expectedAssignmentGeneration":0,"selectedSource":{"unitId":")") +
      unit + R"(","nodeId":")" + node +
      R"(","selectedPlatformUnitCertificateSha256":")" + selected_sha +
      R"(","platformUpdateRuntimeCertificateSha256":")" + selected_sha +
      R"("}})";
  response = Parse(malformed.HandleRequest(equal_fingerprints, 0, {}).payload);
  Check(StringAt(response, "reason") == "INVALID_ENROLLMENT" &&
            malformed.generation() == 0,
        "selected role fingerprints must be distinct");

  VissAssignmentState overflow(std::numeric_limits<std::uint64_t>::max(),
                               dashboard_sha, std::nullopt);
  const auto overflow_select =
      std::string(
          R"({"schemaVersion":1,"action":"select","requestId":"overflow","expectedAssignmentGeneration":18446744073709551615,"selectedSource":{"unitId":")") +
      unit + R"(","nodeId":")" + node +
      R"(","selectedPlatformUnitCertificateSha256":")" + selected_sha +
      R"(","platformUpdateRuntimeCertificateSha256":")" + runtime_sha +
      R"("}})";
  response = Parse(overflow.HandleRequest(overflow_select, 0, {}).payload);
  Check(StringAt(response, "reason") == "GENERATION_OVERFLOW" &&
            !overflow.selected(),
        "generation overflow fails without mutation");

  try {
    TemporaryDirectory directory;
    const auto socket_file = (directory.path() / "assign.sock").string();
    asio::io_context io_context;
    VissAssignmentState socket_state(2, dashboard_sha, std::nullopt);
    bool mutation_called = false;
    VissAssignmentControl control(
        io_context, {socket_file}, socket_state, [] { return 0; },
        [] { return VissActiveRoleCounts{}; },
        [&mutation_called](VissAssignmentMutation) { mutation_called = true; });
    control.Start();
    struct stat socket_status{};
    Check(lstat(socket_file.c_str(), &socket_status) == 0 &&
              S_ISSOCK(socket_status.st_mode) &&
              (socket_status.st_mode & 0777) == 0600,
          "assignment socket is owner-only mode 0600");
    std::thread server_thread([&io_context] { io_context.run(); });

    asio::io_context client_context;
    LocalSocket::socket socket(client_context);
    socket.connect(LocalSocket::endpoint(socket_file));
    const std::string request =
        R"({"schemaVersion":1,"action":"status","requestId":"socket-status"})"
        "\n";
    asio::write(socket, asio::buffer(request));
    asio::streambuf buffer;
    asio::read_until(socket, buffer, '\n');
    std::istream stream(&buffer);
    std::string payload;
    std::getline(stream, payload);
    response = Parse(payload);
    Check(StringAt(response, "result") == "ACCEPTED" &&
              Generation(response) == 2,
          "same-UID Unix socket status succeeds");
    Check(!mutation_called, "status does not mutate assignment");

    asio::post(io_context, [&control] { control.Stop(); });
    server_thread.join();
    Check(!std::filesystem::exists(socket_file),
          "owned assignment socket is removed on stop");
  } catch (const std::exception &error) {
    Check(false, std::string("assignment socket fixture: ") + error.what());
  }

  try {
    TemporaryDirectory directory;
    const auto socket_file = (directory.path() / "retry.sock").string();
    asio::io_context io_context;
    VissAssignmentState retry_state(3, dashboard_sha, std::nullopt);
    VissAssignmentControl control(
        io_context, {socket_file}, retry_state, [] { return 0; },
        [] { return VissActiveRoleCounts{}; }, [](VissAssignmentMutation) {});

    fail_after_bind.store(true);
    bool post_bind_failure = false;
    try {
      control.Start();
    } catch (const std::exception &) {
      post_bind_failure = true;
    }
    Check(post_bind_failure, "post-bind start failure is exercised");
    Check(!std::filesystem::exists(socket_file),
          "post-bind start failure removes the owned socket");

    control.Start();
    Check(std::filesystem::is_socket(socket_file),
          "same assignment-control object retries after partial start");
    control.Stop();
    Check(!std::filesystem::exists(socket_file),
          "retried assignment socket is removed on stop");
  } catch (const std::exception &error) {
    Check(false, std::string("assignment retry fixture: ") + error.what());
  }

  try {
    TemporaryDirectory directory;
    const auto socket_path = directory.path() / "replace.sock";
    const auto moved_socket = directory.path() / "moved-owned.sock";
    asio::io_context io_context;
    VissAssignmentState replace_state(4, dashboard_sha, std::nullopt);
    VissAssignmentControl control(
        io_context, {socket_path.string()}, replace_state, [] { return 0; },
        [] { return VissActiveRoleCounts{}; }, [](VissAssignmentMutation) {});

    control.Start();
    std::filesystem::rename(socket_path, moved_socket);
    {
      std::ofstream replacement(socket_path);
      replacement << "non-owned sentinel";
    }
    control.Stop();
    Check(std::filesystem::is_regular_file(socket_path),
          "stop does not delete a replacement non-owned path");
    Check(std::filesystem::is_socket(moved_socket),
          "inode-safe cleanup does not follow the moved owned socket");
  } catch (const std::exception &error) {
    Check(false,
          std::string("assignment replacement fixture: ") + error.what());
  }

  if (failures == 0) {
    std::cout << "VISS assignment control tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
