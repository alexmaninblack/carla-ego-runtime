#pragma once

#include "carla_ego_runtime/viss_protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace carla_ego_runtime {

struct VissServerConfig {
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 6443;
  std::string certificate_chain_file;
  std::string private_key_file;
  std::size_t max_clients = 8;
  std::size_t max_pending_messages_per_client = 8;
  VissProtocolLimits protocol_limits;
};

struct VissServerMetrics {
  std::uint64_t accepted_connections = 0;
  std::uint64_t rejected_connections = 0;
  std::uint64_t active_connections = 0;
  std::uint64_t requests = 0;
  std::uint64_t protocol_errors = 0;
  std::uint64_t subscription_events = 0;
  std::uint64_t dropped_subscription_events = 0;
  std::uint64_t coalesced_subscription_intervals = 0;
};

// A TLS-only WebSocket endpoint. The server never exposes an unencrypted
// listener and accepts only the VISSv3 WebSocket subprotocol.
class VissServer {
public:
  VissServer(const LatestVssSignalStore &signal_store, VissServerConfig config);
  ~VissServer();

  VissServer(const VissServer &) = delete;
  VissServer &operator=(const VissServer &) = delete;

  void Start();
  void Stop();
  void NotifySnapshot();

  std::uint16_t bound_port() const;
  VissServerMetrics metrics() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace carla_ego_runtime
