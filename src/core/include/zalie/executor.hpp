#pragma once

#include <amc/vector.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

#include "zalie/exchange.hpp"
#include "zalie/resolved-request.hpp"

namespace zalie {

// Live state of a request being sent, shared between its worker and the UI (lock-free reads every frame).
struct Progress {
  std::chrono::steady_clock::time_point startedAt;
  std::atomic<std::uint64_t> bytesReceived{0};
  // From Content-Length, 0 when unknown.
  std::atomic<std::uint64_t> expectedBytes{0};
  std::atomic<bool> headReceived{false};
  std::atomic<bool> cancelRequested{false};
};

// Sends requests on a pool of worker threads, each with its own aeronet::HttpClient instances (an HttpClient is not
// thread-safe), so that the UI thread never blocks. Connections are kept alive per worker and reused.
// Completed exchanges are collected with poll(); the optional wake callback is invoked from the worker thread after
// each completion (the app uses it to wake its event loop).
class Executor {
 public:
  using WakeFn = void (*)(void* pUserData);

  struct Options {
    static constexpr std::size_t kDefaultMaxBodyBytes = 512UL * 1024UL * 1024UL;

    WakeFn wakeFn{nullptr};
    void* pWakeUserData{nullptr};
    // Bodies larger than this are cut, and the exchange reported as failed.
    std::size_t maxBodyBytes{kDefaultMaxBodyBytes};
    std::uint32_t nbWorkers{4};
  };

  // Handle on a submitted request.
  struct Ticket {
    [[nodiscard]] bool valid() const noexcept { return id != 0; }

    std::shared_ptr<Progress> progress;
    std::uint64_t id{0};
  };

  explicit Executor(Options options);
  Executor() : Executor(Options{}) {}

  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;
  Executor(Executor&&) = delete;
  Executor& operator=(Executor&&) = delete;

  // Stops the workers. Requests in flight end with their timeouts (they are cancelled as soon as data arrives).
  ~Executor();

  // Queues a request. 'requestId' is reported back in the Exchange.
  [[nodiscard]] Ticket submit(ResolvedRequest request, std::uint64_t requestId);

  // Asks for the cancellation of a request: effective at the next received data (or immediately if still queued).
  static void Cancel(const Ticket& ticket) noexcept;

  // Moves the completed exchanges at the end of 'out'. Never blocks. Returns the number of moved exchanges.
  std::size_t poll(amc::vector<Exchange>& out);

  // Waits until at least one exchange is completed, or the timeout. Returns false on timeout.
  bool waitForCompletion(std::chrono::milliseconds timeout);

  [[nodiscard]] const Options& options() const noexcept { return _options; }

  // Number of submitted requests not polled yet.
  [[nodiscard]] std::size_t pendingCount() const noexcept { return _pending.load(std::memory_order_relaxed); }

 private:
  struct Job {
    ResolvedRequest request;
    std::shared_ptr<Progress> progress;
    std::uint64_t ticket{0};
    std::uint64_t requestId{0};
  };

  void workerLoop(const std::stop_token& stopToken);
  void complete(Exchange exchange);

  Options _options;
  std::mutex _jobsMutex;
  std::condition_variable_any _jobsCondition;
  std::deque<Job> _jobs;
  std::mutex _completedMutex;
  std::condition_variable _completedCondition;
  amc::vector<Exchange> _completed;
  std::atomic<std::uint64_t> _nextTicket{1};
  std::atomic<std::size_t> _pending{0};
  amc::vector<std::jthread> _workers;
};

}  // namespace zalie
