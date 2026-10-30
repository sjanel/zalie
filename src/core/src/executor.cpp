#include "zalie/executor.hpp"

#include <aeronet/aeronet-client.hpp>
#include <aeronet/client-connection.hpp>
#include <aeronet/client-protocol.hpp>
#include <aeronet/http-constants.hpp>
#include <aeronet/http-method.hpp>
#include <aeronet/lower-ascii-key.hpp>
#include <aeronet/reserved-headers.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

#include "zalie/exchange.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/request.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/string-pool.hpp"
#include "zalie/string-utils.hpp"

namespace zalie {

namespace {

using std::chrono::steady_clock;

constexpr std::string_view kUserAgent = "user-agent: zalie/" ZALIE_VERSION_STR;

aeronet::http::Method ToAeronet(Method method) noexcept {
  switch (method) {
    case Method::get:
      return aeronet::http::Method::GET;
    case Method::post:
      return aeronet::http::Method::POST;
    case Method::put:
      return aeronet::http::Method::PUT;
    case Method::patch:
      return aeronet::http::Method::PATCH;
    case Method::del:
      return aeronet::http::Method::DELETE;
    case Method::head:
      return aeronet::http::Method::HEAD;
    case Method::options:
      return aeronet::http::Method::OPTIONS;
    case Method::trace:
      return aeronet::http::Method::TRACE;
    case Method::connect:
      return aeronet::http::Method::CONNECT;
  }
  return aeronet::http::Method::GET;
}

// The client settings an HttpClient is configured with: requests with equal settings share a client (and its pool).
struct ClientKey {
  bool operator==(const ClientKey&) const = default;

  std::uint32_t timeoutMs;
  HttpVersion httpVersion;
  bool followRedirects;
  bool verifyTls;
  bool decompress;
};

ClientKey KeyOf(const RequestSettings& settings) noexcept {
  return {.timeoutMs = std::max(settings.timeoutMs, 1U),
          .httpVersion = settings.httpVersion,
          .followRedirects = settings.followRedirects,
          .verifyTls = settings.verifyTls,
          .decompress = settings.decompress};
}

aeronet::HttpClientConfig MakeConfig(const ClientKey& key, std::size_t maxBodyBytes) {
  using Duration = aeronet::HttpClientConfig::Duration;
  aeronet::HttpClientConfig config;
  config.connectTimeout = Duration{key.timeoutMs};
  config.requestTimeout = Duration{key.timeoutMs};
  config.inactivityTimeout = Duration{key.timeoutMs};
  config.followRedirects = key.followRedirects;
  config.maxRedirects = 10;
  config.globalHeaders = aeronet::ConcatenatedHeaders{kUserAgent};
  config.withDecompression(key.decompress);
  // A debugging tool must show what the server sends: no "compression bomb" ratio guard (highly repetitive test
  // payloads compress more than 1000x), only the body size limit.
  config.decompression.maxExpansionRatio = 1e12;
  const std::size_t decompressionLimit = std::max<std::size_t>(maxBodyBytes, config.decompression.decoderChunkSize);
  config.decompression.maxDecompressedBytes = decompressionLimit;
  config.decompression.maxCompressedBytes = decompressionLimit;
  switch (key.httpVersion) {
    case HttpVersion::http1:
      config.httpVersion = aeronet::HttpVersionMode::Http1_1;
      break;
    case HttpVersion::http2:
#ifdef AERONET_ENABLE_HTTP2
      config.httpVersion = aeronet::HttpVersionMode::Http2;
#endif
      break;
    case HttpVersion::automatic:
      config.httpVersion = aeronet::HttpVersionMode::Auto;
      break;
  }
#ifdef AERONET_ENABLE_OPENSSL
  config.tlsVerifyPeer = key.verifyTls;
#endif
  return config;
}

// The HttpClients of one worker thread, by settings. A handful at most: a linear scan is the fastest lookup.
class ClientCache {
 public:
  explicit ClientCache(std::size_t maxBodyBytes) noexcept : _maxBodyBytes(maxBodyBytes) {}

  aeronet::HttpClient& get(const RequestSettings& settings) {
    const ClientKey key = KeyOf(settings);
    for (Entry& entry : _entries) {
      if (entry.key == key) {
        return *entry.client;
      }
    }
    static constexpr std::size_t kMaxClients = 8;
    if (_entries.size() == kMaxClients) {
      _entries.erase(_entries.begin());  // least recently created
    }
    _entries.push_back({key, std::make_unique<aeronet::HttpClient>(MakeConfig(key, _maxBodyBytes))});
    return *_entries.back().client;
  }

 private:
  struct Entry {
    ClientKey key;
    std::unique_ptr<aeronet::HttpClient> client;
  };

  amc::vector<Entry> _entries;
  std::size_t _maxBodyBytes;
};

// Copies the status line and headers of 'head' into the string pool of the response, in one allocation.
// The client consumes Content-Type and Content-Length while parsing (they describe the body it hands over), so they are
// given back by 'info' for a streamed body. aeronet servers send no reason phrase: the standard one is shown instead.
void CopyHead(const aeronet::HttpResponse& head, const aeronet::ResponseBodyInfo* pInfo, Response& response) {
  StringPool strings(head.headSize() + head.reason().size() + (pInfo == nullptr ? 0 : pInfo->contentType.size()) + 64);
  response.status = head.status();
  const std::string_view reason = head.reason();
  response.reason = strings.store(reason.empty() ? aeronet::http::ReasonPhraseFor(head.status()) : reason);
  response.headers.clear();
  for (const auto& [name, value] : head.headers()) {
    response.headers.push_back({strings.store(name), strings.store(value)});
  }
  if (pInfo != nullptr) {
    if (!pInfo->contentType.empty()) {
      response.headers.push_back({"content-type", strings.store(pInfo->contentType)});
    }
    if (pInfo->sizeKnown()) {
      RawChars32 length;
      AppendInt(length, pInfo->size);
      response.headers.push_back({"content-length", strings.store(length)});
    }
  } else if (const std::string_view contentType = head.headerValueOrEmpty("content-type");
             !contentType.empty() && !HasHeader(response.headers, "content-type")) {
    response.headers.push_back({"content-type", strings.store(contentType)});
  }
  response.headBytes = head.headSize();
  // Keep the strings stored earlier (warnings) alive: splice the new pool into the response one.
  response.strings.splice(strings);
}

// Collects the body of the final response, follows the progress and cancels the transfer when asked.
class CollectingSink final : public aeronet::ResponseBodySink {
 public:
  CollectingSink(Response& response, Progress& progress, steady_clock::time_point start, std::size_t maxBodyBytes)
      : _response(response), _progress(progress), _start(start), _maxBodyBytes(maxBodyBytes) {}

  bool onHead(const aeronet::HttpResponse& head, const aeronet::ResponseBodyInfo& info) override {
    if (_progress.cancelRequested.load(std::memory_order_relaxed)) {
      return false;
    }
    _response.timings.head = steady_clock::now() - _start;
    CopyHead(head, &info, _response);
    _headCopied = true;
    if (info.sizeKnown()) {
      _response.body.raw.reserve(std::min(info.size, _maxBodyBytes));  // the only allocation of the body
      _progress.expectedBytes.store(info.size, std::memory_order_relaxed);
    }
    _progress.headReceived.store(true, std::memory_order_release);
    return true;
  }

  bool onBody(std::string_view chunk) override {
    if (_progress.cancelRequested.load(std::memory_order_relaxed)) {
      return false;
    }
    if (_response.body.raw.size() + chunk.size() > _maxBodyBytes) {
      _tooLarge = true;
      return false;
    }
    _response.body.raw.append(chunk);
    _progress.bytesReceived.fetch_add(chunk.size(), std::memory_order_relaxed);
    return true;
  }

  [[nodiscard]] bool headCopied() const noexcept { return _headCopied; }
  [[nodiscard]] bool tooLarge() const noexcept { return _tooLarge; }

 private:
  Response& _response;
  Progress& _progress;
  steady_clock::time_point _start;
  std::size_t _maxBodyBytes;
  bool _headCopied{false};
  bool _tooLarge{false};
};

std::string_view ErrorMessage(aeronet::HttpClientErrc errc, const ResolvedRequest& request, Response& response) {
  switch (errc) {
    case aeronet::HttpClientErrc::connectFailed:
      return "Could not connect: unknown host, connection refused or connect timeout.";
    case aeronet::HttpClientErrc::tlsError:
      return request.settings.verifyTls
                 ? "TLS handshake failed. For a self-signed certificate, disable 'Verify TLS certificate' in the "
                   "request settings."
                 : "TLS handshake failed.";
    case aeronet::HttpClientErrc::timeout: {
      RawChars32 message(std::string_view{"No complete response within "});
      AppendInt(message, request.settings.timeoutMs);
      message.append(std::string_view{" ms (timeout of the request settings)."});
      return response.strings.store(message);
    }
    case aeronet::HttpClientErrc::invalidUrl:
      return "Invalid URL: only absolute http:// and https:// URLs are supported (no user:password@ part).";
    default:
      return response.strings.concat({aeronet::ErrcToStr(errc), "."});
  }
}

// Adds the headers of 'request' to 'req', skipping (with a warning) the ones the client manages or that are invalid.
void AddHeaders(const ResolvedRequest& request, aeronet::HttpRequest& req, Response& response) {
  RawChars32 lowerName;
  for (const HeaderView& header : request.headers) {
    if (EqualsIgnoreCase(header.name, "content-type")) {
      continue;  // given with the body
    }
    if (!IsHttpToken(header.name) || header.value.find_first_of("\r\n", 0) != std::string_view::npos ||
        header.value.contains('\0')) {
      response.warnings.push_back(response.strings.concat({"Invalid header not sent: ", header.name}));
      continue;
    }
    lowerName.clear();
    lowerName.ensureAvailableCapacity(header.name.size());
    for (char ch : header.name) {
      lowerName.unchecked_push_back(ToLowerAscii(ch));
    }
    const aeronet::LowerAsciiKey key{std::string_view(lowerName)};
    if (aeronet::http::IsReservedOrForbiddenRequestHeader(key) || EqualsIgnoreCase(header.name, "connection")) {
      response.warnings.push_back(response.strings.concat({"Header managed by the client, not sent: ", header.name}));
      continue;
    }
    if (std::string_view(lowerName) == "user-agent") {
      req.header(key, header.value);  // replaces Zalie's default one
    } else {
      req.headerAddLine(key, header.value);  // duplicates are legitimate (several Cookie or X-Forwarded-For lines)
    }
  }
}

void Execute(ClientCache& clients, Exchange& exchange, Progress& progress, std::size_t maxBodyBytes) {
  const ResolvedRequest& request = exchange.request;
  Response& response = exchange.response;
  response.timings.startedAt = std::chrono::system_clock::now();
  const steady_clock::time_point start = steady_clock::now();
  if (progress.cancelRequested.load(std::memory_order_relaxed)) {
    response.state = ExchangeState::cancelled;
    response.error = "Cancelled before being sent.";
    return;
  }
  CollectingSink sink(response, progress, start, maxBodyBytes);
  try {
    aeronet::HttpClient& client = clients.get(request.settings);
    aeronet::HttpRequest req = client.makeRequest(ToAeronet(request.method), request.url);
    AddHeaders(request, req, response);
    if (!request.body.empty()) {
      const std::string_view contentType = request.headerValue("content-type");
      // The body stays owned by the exchange, alive for the whole call: no copy.
      req.bodyStatic(std::string_view(request.body), contentType.empty() ? "text/plain" : contentType);
    }
    aeronet::HttpClientResult result = client.request(std::move(req), sink);
    response.timings.total = steady_clock::now() - start;
    if (result) {
      if (!sink.headCopied()) {
        CopyHead(*result, nullptr, response);
      }
      response.state = ExchangeState::completed;
    } else if (progress.cancelRequested.load(std::memory_order_relaxed)) {
      response.state = ExchangeState::cancelled;
      response.error = "Cancelled.";
    } else if (sink.tooLarge()) {
      response.state = ExchangeState::failed;
      response.error = "Response body too large: it was cut (see the maximum body size in the settings).";
    } else {
      response.state = ExchangeState::failed;
      response.error = ErrorMessage(result.error(), request, response);
    }
  } catch (const std::exception& ex) {
    response.timings.total = steady_clock::now() - start;
    response.state = ExchangeState::failed;
    response.error = response.strings.store(ex.what());
  }
  response.body.analyze(response.headerValue("content-type"));
}

}  // namespace

Executor::Executor(Options options) : _options(options) {
  const std::uint32_t nbWorkers = std::max(options.nbWorkers, 1U);
  _workers.reserve(nbWorkers);
  for (std::uint32_t i = 0; i < nbWorkers; ++i) {
    _workers.emplace_back([this](const std::stop_token& stopToken) { workerLoop(stopToken); });
  }
}

Executor::~Executor() {
  {
    std::scoped_lock lock(_jobsMutex);
    for (Job& job : _jobs) {
      job.progress->cancelRequested.store(true, std::memory_order_relaxed);
    }
  }
  for (std::jthread& worker : _workers) {
    worker.request_stop();
  }
  _jobsCondition.notify_all();
  _workers.clear();  // joins
}

Executor::Ticket Executor::submit(ResolvedRequest request, std::uint64_t requestId) {
  Ticket ticket{.progress = std::make_shared<Progress>(), .id = _nextTicket.fetch_add(1, std::memory_order_relaxed)};
  ticket.progress->startedAt = steady_clock::now();
  _pending.fetch_add(1, std::memory_order_relaxed);
  {
    std::scoped_lock lock(_jobsMutex);
    _jobs.push_back(
        {.request = std::move(request), .progress = ticket.progress, .ticket = ticket.id, .requestId = requestId});
  }
  _jobsCondition.notify_one();
  return ticket;
}

void Executor::Cancel(const Ticket& ticket) noexcept {
  if (ticket.progress) {
    ticket.progress->cancelRequested.store(true, std::memory_order_relaxed);
  }
}

std::size_t Executor::poll(amc::vector<Exchange>& out) {
  std::scoped_lock lock(_completedMutex);
  const std::size_t count = _completed.size();
  for (Exchange& exchange : _completed) {
    out.push_back(std::move(exchange));
  }
  _completed.clear();
  _pending.fetch_sub(count, std::memory_order_relaxed);
  return count;
}

bool Executor::waitForCompletion(std::chrono::milliseconds timeout) {
  std::unique_lock lock(_completedMutex);
  return _completedCondition.wait_for(lock, timeout, [this] { return !_completed.empty(); });
}

void Executor::complete(Exchange exchange) {
  {
    std::scoped_lock lock(_completedMutex);
    _completed.push_back(std::move(exchange));
  }
  _completedCondition.notify_all();
  if (_options.wakeFn != nullptr) {
    _options.wakeFn(_options.pWakeUserData);
  }
}

void Executor::workerLoop(const std::stop_token& stopToken) {
  ClientCache clients(_options.maxBodyBytes);  // thread-local by construction: an HttpClient is used by one thread only
  while (true) {
    Job job;
    {
      std::unique_lock lock(_jobsMutex);
      if (!_jobsCondition.wait(lock, stopToken, [this] { return !_jobs.empty(); })) {
        return;  // stop requested
      }
      job = std::move(_jobs.front());
      _jobs.pop_front();
    }
    Exchange exchange{
        .request = std::move(job.request), .response = {}, .ticket = job.ticket, .requestId = job.requestId};
    Execute(clients, exchange, *job.progress, _options.maxBodyBytes);
    complete(std::move(exchange));
  }
}

}  // namespace zalie
