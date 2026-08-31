#pragma once

#include "carla_ego_runtime/viss_access.hpp"

#include <boost/asio/io_context.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace carla_ego_runtime {

struct VissActiveRoleCounts {
  std::uint64_t selected_platform_unit = 0;
  std::uint64_t platform_update_runtime = 0;
  std::uint64_t engineering_dashboard = 0;
  std::uint64_t qualification_client = 0;
};

enum class VissAssignmentMutation { None, Selected, Detached };

struct VissAssignmentReply {
  std::string payload;
  VissAssignmentMutation mutation = VissAssignmentMutation::None;
};

class VissAssignmentState {
public:
  VissAssignmentState(
      std::uint64_t initial_generation,
      std::string engineering_dashboard_certificate_sha256,
      std::optional<std::string> qualification_certificate_sha256);

  VissAssignmentReply HandleRequest(std::string_view request,
                                    std::uint64_t current_frame,
                                    VissActiveRoleCounts active_counts);
  std::optional<VissSessionAccess>
  Authorize(const VissCertificateIdentity &identity) const;

  std::uint64_t generation() const;
  bool selected() const;

private:
  struct SelectedSource {
    std::string unit_id;
    std::string node_id;
    std::string selected_platform_unit_certificate_sha256;
    std::string platform_update_runtime_certificate_sha256;
  };
  std::uint64_t generation_;
  std::string engineering_dashboard_certificate_sha256_;
  std::optional<std::string> qualification_certificate_sha256_;
  std::optional<SelectedSource> selected_source_;
  std::uint64_t minimum_frame_exclusive_ = 0;
};

struct VissAssignmentControlConfig {
  std::string socket_file;
};

// The transport runs on the supplied Gateway io_context. It accepts exactly
// one bounded newline-delimited request per same-UID AF_UNIX connection.
class VissAssignmentControl {
public:
  using FrameProvider = std::function<std::uint64_t()>;
  using ActiveCountsProvider = std::function<VissActiveRoleCounts()>;
  using MutationHandler = std::function<void(VissAssignmentMutation)>;

  VissAssignmentControl(boost::asio::io_context &io_context,
                        VissAssignmentControlConfig config,
                        VissAssignmentState &state,
                        FrameProvider frame_provider,
                        ActiveCountsProvider active_counts_provider,
                        MutationHandler mutation_handler);
  ~VissAssignmentControl();

  VissAssignmentControl(const VissAssignmentControl &) = delete;
  VissAssignmentControl &operator=(const VissAssignmentControl &) = delete;

  void Start();
  void Stop();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace carla_ego_runtime
