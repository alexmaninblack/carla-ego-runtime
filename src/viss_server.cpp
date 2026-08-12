#include "carla_ego_runtime/viss_server.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace carla_ego_runtime {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

bool OffersVissV3(std::string_view offered) {
  while (!offered.empty()) {
    const auto comma = offered.find(',');
    auto token = offered.substr(0, comma);
    const auto first = token.find_first_not_of(" \t");
    const auto last = token.find_last_not_of(" \t");
    if (first != std::string_view::npos &&
        token.substr(first, last - first + 1) == "VISSv3") {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    offered.remove_prefix(comma + 1);
  }
  return false;
}

} // namespace

class VissServer::Impl {
public:
  Impl(const LatestVssSignalStore &signal_store, VissServerConfig config)
      : signal_store_(signal_store), config_(std::move(config)),
        tls_context_(ssl::context::tls_server), acceptor_(io_context_) {
    if (config_.certificate_chain_file.empty() ||
        config_.private_key_file.empty()) {
      throw std::invalid_argument(
          "VISS TLS certificate chain and private key are required");
    }
    if (config_.max_clients == 0 ||
        config_.max_pending_messages_per_client == 0) {
      throw std::invalid_argument(
          "VISS server limits must be greater than zero");
    }
  }

  ~Impl() { Stop(); }

  void Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
      throw std::logic_error("VISS server is already running");
    }

    try {
      tls_context_.set_options(ssl::context::default_workarounds |
                               ssl::context::no_sslv2 | ssl::context::no_sslv3 |
                               ssl::context::no_tlsv1 |
                               ssl::context::no_tlsv1_1);
      if (SSL_CTX_set_min_proto_version(tls_context_.native_handle(),
                                        TLS1_2_VERSION) != 1) {
        throw std::runtime_error("failed to require TLS 1.2 or newer");
      }
      tls_context_.use_certificate_chain_file(config_.certificate_chain_file);
      tls_context_.use_private_key_file(config_.private_key_file,
                                        ssl::context::pem);
      if (SSL_CTX_check_private_key(tls_context_.native_handle()) != 1) {
        throw std::runtime_error(
            "VISS TLS private key does not match the certificate");
      }

      boost::system::error_code error;
      const auto address = asio::ip::make_address(config_.bind_address, error);
      if (error) {
        throw std::invalid_argument("invalid VISS bind address: " +
                                    config_.bind_address);
      }
      const tcp::endpoint endpoint(address, config_.port);
      acceptor_.open(endpoint.protocol(), error);
      ThrowOnError(error, "open VISS listener");
      acceptor_.set_option(asio::socket_base::reuse_address(true), error);
      ThrowOnError(error, "configure VISS listener");
      acceptor_.bind(endpoint, error);
      ThrowOnError(error, "bind VISS listener");
      acceptor_.listen(asio::socket_base::max_listen_connections, error);
      ThrowOnError(error, "listen for VISS clients");
      bound_port_.store(acceptor_.local_endpoint().port());

      work_guard_.emplace(asio::make_work_guard(io_context_));
      AcceptNext();
      network_thread_ = std::thread([this] { io_context_.run(); });
    } catch (...) {
      running_.store(false);
      boost::system::error_code ignored;
      acceptor_.close(ignored);
      work_guard_.reset();
      throw;
    }
  }

  void Stop() {
    if (!running_.exchange(false)) {
      return;
    }
    asio::post(io_context_, [this] {
      boost::system::error_code ignored;
      acceptor_.cancel(ignored);
      acceptor_.close(ignored);
      const auto sessions = sessions_;
      for (const auto &session : sessions) {
        session->Stop();
      }
    });
    work_guard_.reset();
    if (network_thread_.joinable()) {
      network_thread_.join();
    }
    io_context_.stop();
  }

  void NotifySnapshot() {
    if (!running_.load()) {
      return;
    }
    asio::post(io_context_, [this] {
      const auto sessions = sessions_;
      for (const auto &session : sessions) {
        session->PollSubscriptions();
      }
    });
  }

  std::uint16_t bound_port() const { return bound_port_.load(); }

  VissServerMetrics metrics() const {
    return {accepted_connections_.load(),
            rejected_connections_.load(),
            active_connections_.load(),
            requests_.load(),
            protocol_errors_.load(),
            subscription_events_.load(),
            dropped_subscription_events_.load(),
            coalesced_subscription_intervals_.load()};
  }

private:
  class Session : public std::enable_shared_from_this<Session> {
  public:
    Session(tcp::socket socket, ssl::context &tls_context,
            const LatestVssSignalStore &signal_store, VissServerConfig config,
            Impl &owner)
        : websocket_(beast::tcp_stream(std::move(socket)), tls_context),
          timer_(websocket_.get_executor()), signal_store_(signal_store),
          config_(std::move(config)), protocol_(config_.protocol_limits),
          owner_(owner) {}

    void Start() {
      beast::get_lowest_layer(websocket_)
          .expires_after(std::chrono::seconds(10));
      websocket_.next_layer().async_handshake(
          ssl::stream_base::server,
          [self = shared_from_this()](boost::system::error_code error) {
            self->OnTlsHandshake(error);
          });
    }

    void Stop() {
      if (finished_) {
        return;
      }
      boost::system::error_code ignored;
      timer_.cancel();
      beast::get_lowest_layer(websocket_).socket().cancel(ignored);
      beast::get_lowest_layer(websocket_)
          .socket()
          .shutdown(tcp::socket::shutdown_both, ignored);
      beast::get_lowest_layer(websocket_).socket().close(ignored);
      Finish();
    }

    void PollSubscriptions() {
      if (!accepted_ || finished_) {
        return;
      }
      const auto before = protocol_.metrics();
      const auto events = protocol_.CollectDueSubscriptionEvents(signal_store_);
      const auto after = protocol_.metrics();
      owner_.protocol_errors_.fetch_add(after.errors - before.errors);
      owner_.subscription_events_.fetch_add(after.subscription_events -
                                            before.subscription_events);
      owner_.coalesced_subscription_intervals_.fetch_add(
          after.coalesced_intervals - before.coalesced_intervals);
      for (const auto &event : events) {
        Enqueue(event, true);
      }
    }

  private:
    struct OutboundMessage {
      std::string payload;
      bool subscription_event = false;
    };

    void OnTlsHandshake(boost::system::error_code error) {
      if (error) {
        if (error != asio::error::operation_aborted) {
          ++owner_.rejected_connections_;
        }
        Finish();
        return;
      }
      http::async_read(websocket_.next_layer(), input_buffer_, upgrade_request_,
                       [self = shared_from_this()](
                           boost::system::error_code read_error, std::size_t) {
                         self->OnUpgradeRequest(read_error);
                       });
    }

    void OnUpgradeRequest(boost::system::error_code error) {
      if (error) {
        ++owner_.rejected_connections_;
        Finish();
        return;
      }
      const auto offered =
          upgrade_request_[http::field::sec_websocket_protocol];
      if (!websocket::is_upgrade(upgrade_request_) ||
          !OffersVissV3(std::string_view(offered.data(), offered.size()))) {
        RejectUpgrade();
        return;
      }

      websocket_.set_option(
          websocket::stream_base::timeout::suggested(beast::role_type::server));
      websocket_.set_option(websocket::stream_base::decorator(
          [](websocket::response_type &response) {
            response.set(http::field::sec_websocket_protocol, "VISSv3");
          }));
      websocket_.read_message_max(64 * 1024);
      beast::get_lowest_layer(websocket_).expires_never();
      websocket_.async_accept(
          upgrade_request_,
          [self = shared_from_this()](boost::system::error_code accept_error) {
            self->OnWebSocketAccept(accept_error);
          });
    }

    void RejectUpgrade() {
      ++owner_.rejected_connections_;
      rejection_.emplace(http::status::bad_request, upgrade_request_.version());
      rejection_->set(http::field::server, "carla-ego-runtime");
      rejection_->set(http::field::content_type, "text/plain");
      rejection_->keep_alive(false);
      rejection_->body() = "WebSocket subprotocol VISSv3 is required\n";
      rejection_->prepare_payload();
      http::async_write(
          websocket_.next_layer(), *rejection_,
          [self = shared_from_this()](boost::system::error_code, std::size_t) {
            self->Finish();
          });
    }

    void OnWebSocketAccept(boost::system::error_code error) {
      if (error) {
        ++owner_.rejected_connections_;
        Finish();
        return;
      }
      accepted_ = true;
      ++owner_.accepted_connections_;
      ++owner_.active_connections_;
      ReadNext();
      SchedulePoll();
    }

    void ReadNext() {
      if (finished_) {
        return;
      }
      websocket_.async_read(
          input_buffer_,
          [self = shared_from_this()](boost::system::error_code error,
                                      std::size_t) { self->OnRead(error); });
    }

    void OnRead(boost::system::error_code error) {
      if (error) {
        Finish();
        return;
      }
      const auto request = beast::buffers_to_string(input_buffer_.data());
      input_buffer_.consume(input_buffer_.size());
      ++owner_.requests_;
      const auto before = protocol_.metrics();
      const auto response = protocol_.HandleRequest(
          websocket_.got_text() ? request : std::string_view{}, signal_store_);
      const auto after = protocol_.metrics();
      owner_.protocol_errors_.fetch_add(after.errors - before.errors);
      if (!Enqueue(response.payload, false)) {
        return;
      }
      ReadNext();
    }

    bool Enqueue(std::string payload, bool subscription_event) {
      if (finished_) {
        return false;
      }
      if (outbound_.size() >= config_.max_pending_messages_per_client) {
        if (subscription_event) {
          ++owner_.dropped_subscription_events_;
          return true;
        }
        const auto event =
            std::find_if(std::next(outbound_.begin()), outbound_.end(),
                         [](const OutboundMessage &message) {
                           return message.subscription_event;
                         });
        if (event != outbound_.end()) {
          outbound_.erase(event);
          ++owner_.dropped_subscription_events_;
        } else {
          Stop();
          return false;
        }
      }
      const bool write_in_progress = !outbound_.empty();
      outbound_.push_back({std::move(payload), subscription_event});
      if (!write_in_progress) {
        WriteNext();
      }
      return true;
    }

    void WriteNext() {
      if (finished_ || outbound_.empty()) {
        return;
      }
      websocket_.text(true);
      websocket_.async_write(
          asio::buffer(outbound_.front().payload),
          [self = shared_from_this()](boost::system::error_code error,
                                      std::size_t) { self->OnWrite(error); });
    }

    void OnWrite(boost::system::error_code error) {
      if (error) {
        Finish();
        return;
      }
      outbound_.pop_front();
      WriteNext();
    }

    void SchedulePoll() {
      if (finished_) {
        return;
      }
      timer_.expires_after(std::chrono::milliseconds(25));
      timer_.async_wait(
          [self = shared_from_this()](boost::system::error_code error) {
            if (!error) {
              self->PollSubscriptions();
              self->SchedulePoll();
            }
          });
    }

    void Finish() {
      if (finished_) {
        return;
      }
      finished_ = true;
      timer_.cancel();
      if (accepted_) {
        --owner_.active_connections_;
      }
      owner_.Remove(shared_from_this());
    }

    websocket::stream<beast::ssl_stream<beast::tcp_stream>> websocket_;
    beast::flat_buffer input_buffer_;
    http::request<http::string_body> upgrade_request_;
    std::optional<http::response<http::string_body>> rejection_;
    asio::steady_timer timer_;
    const LatestVssSignalStore &signal_store_;
    VissServerConfig config_;
    VissSessionProtocol protocol_;
    Impl &owner_;
    std::deque<OutboundMessage> outbound_;
    bool accepted_ = false;
    bool finished_ = false;
  };

  static void ThrowOnError(const boost::system::error_code &error,
                           std::string_view operation) {
    if (error) {
      throw std::runtime_error(std::string(operation) + ": " + error.message());
    }
  }

  void AcceptNext() {
    acceptor_.async_accept([this](boost::system::error_code error,
                                  tcp::socket socket) {
      if (!error) {
        if (sessions_.size() >= config_.max_clients) {
          ++rejected_connections_;
          boost::system::error_code ignored;
          socket.shutdown(tcp::socket::shutdown_both, ignored);
          socket.close(ignored);
        } else {
          auto session = std::make_shared<Session>(
              std::move(socket), tls_context_, signal_store_, config_, *this);
          sessions_.insert(session);
          session->Start();
        }
      }
      if (running_.load()) {
        AcceptNext();
      }
    });
  }

  void Remove(const std::shared_ptr<Session> &session) {
    sessions_.erase(session);
  }

  const LatestVssSignalStore &signal_store_;
  VissServerConfig config_;
  asio::io_context io_context_;
  ssl::context tls_context_;
  tcp::acceptor acceptor_;
  using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;
  std::optional<WorkGuard> work_guard_;
  std::thread network_thread_;
  std::set<std::shared_ptr<Session>> sessions_;
  std::atomic<bool> running_{false};
  std::atomic<std::uint16_t> bound_port_{0};
  std::atomic<std::uint64_t> accepted_connections_{0};
  std::atomic<std::uint64_t> rejected_connections_{0};
  std::atomic<std::uint64_t> active_connections_{0};
  std::atomic<std::uint64_t> requests_{0};
  std::atomic<std::uint64_t> protocol_errors_{0};
  std::atomic<std::uint64_t> subscription_events_{0};
  std::atomic<std::uint64_t> dropped_subscription_events_{0};
  std::atomic<std::uint64_t> coalesced_subscription_intervals_{0};
};

VissServer::VissServer(const LatestVssSignalStore &signal_store,
                       VissServerConfig config)
    : impl_(std::make_unique<Impl>(signal_store, std::move(config))) {}

VissServer::~VissServer() = default;

void VissServer::Start() { impl_->Start(); }
void VissServer::Stop() { impl_->Stop(); }
void VissServer::NotifySnapshot() { impl_->NotifySnapshot(); }
std::uint16_t VissServer::bound_port() const { return impl_->bound_port(); }
VissServerMetrics VissServer::metrics() const { return impl_->metrics(); }

} // namespace carla_ego_runtime
