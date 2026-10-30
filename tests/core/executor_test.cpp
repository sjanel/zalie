// Integration tests of the executor: real HTTP exchanges with a local aeronet server.

#include "zalie/executor.hpp"

#include <gtest/gtest.h>

#include <aeronet/http-method.hpp>
#include <aeronet/http-request-view.hpp>
#include <aeronet/http-response-writer.hpp>
#include <aeronet/http-response.hpp>
#include <aeronet/http-server-config.hpp>
#include <aeronet/router.hpp>
#include <aeronet/single-http-server.hpp>
#include <amc/vector.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "zalie/exchange.hpp"
#include "zalie/method.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/text-document.hpp"

namespace zalie {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t kBigBodySize = 4UL * 1024UL * 1024UL;

class ExecutorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    aeronet::Router router;
    router.setPath(aeronet::http::Method::GET, "/json", [](const aeronet::HttpRequestView& req) {
      return req.makeResponse(200, R"({"name":"Zalie","tags":["coton","de","tulear"]})", "application/json");
    });
    // Echoes the method, the body and a few request headers.
    const auto echo = [](const aeronet::HttpRequestView& req) {
      aeronet::HttpResponse resp = req.makeResponse(200, req.body(), "text/plain");
      resp.headerAddLine("x-echo-method", aeronet::http::MethodToStr(req.method()));
      resp.headerAddLine("x-echo-user-agent", req.headerValueOrEmpty("user-agent"));
      resp.headerAddLine("x-echo-custom", req.headerValueOrEmpty("x-custom"));
      resp.headerAddLine("x-echo-content-type", req.headerValueOrEmpty("content-type"));
      resp.headerAddLine("x-echo-query", req.queryParamValueOrEmpty("q"));
      return resp;
    };
    for (aeronet::http::Method method :
         {aeronet::http::Method::GET, aeronet::http::Method::POST, aeronet::http::Method::PUT,
          aeronet::http::Method::DELETE, aeronet::http::Method::PATCH}) {
      router.setPath(method, "/echo", echo);
    }
    router.setPath(aeronet::http::Method::GET, "/redirect", [](const aeronet::HttpRequestView& req) {
      aeronet::HttpResponse resp = req.makeResponse(302);
      resp.headerAddLine("location", "/json");
      return resp;
    });
    router.setPath(aeronet::http::Method::GET, "/slow", [](const aeronet::HttpRequestView& req) {
      std::this_thread::sleep_for(400ms);
      return req.makeResponse(200, "late", "text/plain");
    });
    router.setPath(aeronet::http::Method::GET, "/big",
                   [](const aeronet::HttpRequestView&, aeronet::HttpResponseWriter& writer) {
                     writer.status(200);
                     writer.contentType("application/octet-stream");
                     const std::string chunk(64UL * 1024UL, 'z');
                     for (std::size_t sent = 0; sent < kBigBodySize; sent += chunk.size()) {
                       writer.writeBody(chunk);
                     }
                     writer.end();
                   });
    _server = std::make_unique<aeronet::SingleHttpServer>(
        aeronet::HttpServerConfig{}.withPort(0).withKeepAliveTimeout(5s).withPollInterval(20ms), std::move(router));
    _port = _server->port();
    _server->start();
  }

  void TearDown() override {
    _executor.reset();
    _server.reset();
  }

  [[nodiscard]] std::string url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(_port) + std::string(path);
  }

  // A GET request to the local server (the URL is stored in the request's own pool).
  ResolvedRequest makeRequest(std::string_view path, Method method = Method::get) {
    ResolvedRequest req;
    req.method = method;
    req.url = req.strings.store(url(path));
    return req;
  }

  // Sends and waits for the exchange.
  Exchange send(ResolvedRequest req, std::uint64_t requestId = 1) {
    const Executor::Ticket ticket = executor().submit(std::move(req), requestId);
    EXPECT_TRUE(ticket.valid());
    return waitFor(ticket.id);
  }

  Exchange waitFor(std::uint64_t ticketId) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
      executor().waitForCompletion(50ms);
      executor().poll(_done);
      for (auto it = _done.begin(); it != _done.end(); ++it) {
        if (it->ticket == ticketId) {
          Exchange exchange = std::move(*it);
          _done.erase(it);
          return exchange;
        }
      }
    }
    ADD_FAILURE() << "exchange " << ticketId << " not completed in time";
    return {};
  }

  Executor& executor(std::uint32_t nbWorkers = 2) {
    if (!_executor) {
      _executor = std::make_unique<Executor>(
          Executor::Options{.wakeFn = [](void* pCount) { static_cast<std::atomic<int>*>(pCount)->fetch_add(1); },
                            .pWakeUserData = &_wakeCount,
                            .maxBodyBytes = Executor::Options::kDefaultMaxBodyBytes,
                            .nbWorkers = nbWorkers});
    }
    return *_executor;
  }

  std::unique_ptr<aeronet::SingleHttpServer> _server;
  std::unique_ptr<Executor> _executor;
  amc::vector<Exchange> _done;
  std::atomic<int> _wakeCount{0};
  std::uint16_t _port{0};
};

TEST_F(ExecutorTest, GetJson) {
  const Exchange exchange = send(makeRequest("/json"), 42);
  const Response& response = exchange.response;
  ASSERT_EQ(response.state, ExchangeState::completed) << response.error;
  EXPECT_TRUE(response.ok());
  EXPECT_EQ(exchange.requestId, 42U);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.reason, "OK");
  EXPECT_EQ(response.headerValue("content-type"), "application/json");
  EXPECT_GT(response.headBytes, 0U);
  EXPECT_EQ(View(response.body.raw), R"({"name":"Zalie","tags":["coton","de","tulear"]})");
  // Analyzed on the worker: kind, pretty-printed text and line index ready for the UI.
  EXPECT_EQ(response.body.kind, ContentKind::json);
  ASSERT_TRUE(response.body.hasPretty());
  EXPECT_TRUE(response.body.text(true).starts_with("{\n  \"name\": \"Zalie\",\n  \"tags\": [\n"));
  EXPECT_EQ(response.body.lines(true).size(), 8U);
  EXPECT_EQ(response.body.lines(false).size(), 1U);
  EXPECT_GT(response.timings.total.count(), 0);
  EXPECT_GE(response.timings.total, response.timings.head);
  // The worker wakes the loop right after publishing the exchange, so the wake may not have happened yet.
  const auto wakeDeadline = std::chrono::steady_clock::now() + 5s;
  while (_wakeCount.load() < 1 && std::chrono::steady_clock::now() < wakeDeadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_GE(_wakeCount.load(), 1);
  EXPECT_EQ(executor().pendingCount(), 0U);
}

TEST_F(ExecutorTest, PostBodyHeadersAndDefaultUserAgent) {
  ResolvedRequest req = makeRequest("/echo?q=zalie", Method::post);
  req.headers.push_back({"Content-Type", "application/json"});
  req.headers.push_back({"X-Custom", "Coton de Tulear"});
  req.body.assign(std::string_view{R"({"bark":true})"});
  const Exchange exchange = send(std::move(req));
  const Response& response = exchange.response;
  ASSERT_TRUE(response.ok()) << response.error;
  EXPECT_EQ(View(response.body.raw), R"({"bark":true})");
  EXPECT_EQ(response.headerValue("x-echo-method"), "POST");
  EXPECT_EQ(response.headerValue("x-echo-custom"), "Coton de Tulear");
  EXPECT_EQ(response.headerValue("x-echo-content-type"), "application/json");
  EXPECT_EQ(response.headerValue("x-echo-query"), "zalie");
  EXPECT_EQ(response.headerValue("x-echo-user-agent"), "zalie/" ZALIE_VERSION_STR);
  // The request as sent is kept with the exchange.
  EXPECT_EQ(View(exchange.request.body), R"({"bark":true})");
  EXPECT_TRUE(response.warnings.empty());
}

TEST_F(ExecutorTest, MethodsAndUserAgentOverride) {
  for (Method method : {Method::put, Method::patch, Method::del}) {
    ResolvedRequest req = makeRequest("/echo", method);
    req.headers.push_back({"User-Agent", "my-agent"});
    const Exchange exchange = send(std::move(req));
    ASSERT_TRUE(exchange.response.ok()) << exchange.response.error;
    EXPECT_EQ(exchange.response.headerValue("x-echo-method"), MethodToStr(method));
    EXPECT_EQ(exchange.response.headerValue("x-echo-user-agent"), "my-agent");
  }
}

TEST_F(ExecutorTest, ManagedAndInvalidHeadersAreNotSent) {
  ResolvedRequest req = makeRequest("/echo");
  req.headers.push_back({"Host", "elsewhere"});
  req.headers.push_back({"Content-Length", "3"});
  req.headers.push_back({"Bad Header", "x"});
  req.headers.push_back({"X-Custom", "line\r\nbreak"});
  req.headers.push_back({"Connection", "close"});
  const Exchange exchange = send(std::move(req));
  ASSERT_TRUE(exchange.response.ok()) << exchange.response.error;
  EXPECT_EQ(exchange.response.warnings.size(), 5U);
  EXPECT_TRUE(exchange.response.headerValue("x-echo-custom").empty());
}

TEST_F(ExecutorTest, Redirects) {
  Exchange exchange = send(makeRequest("/redirect"));
  ASSERT_TRUE(exchange.response.ok()) << exchange.response.error;
  EXPECT_EQ(exchange.response.status, 200);
  EXPECT_EQ(exchange.response.body.kind, ContentKind::json);

  ResolvedRequest noFollow = makeRequest("/redirect");
  noFollow.settings.followRedirects = false;
  exchange = send(std::move(noFollow));
  ASSERT_TRUE(exchange.response.ok()) << exchange.response.error;
  EXPECT_EQ(exchange.response.status, 302);
  EXPECT_EQ(exchange.response.headerValue("location"), "/json");
}

TEST_F(ExecutorTest, HeadRequest) {
  const Exchange exchange = send(makeRequest("/json", Method::head));
  ASSERT_TRUE(exchange.response.ok()) << exchange.response.error;
  EXPECT_EQ(exchange.response.status, 200);
  EXPECT_TRUE(exchange.response.body.raw.empty());
  EXPECT_EQ(exchange.response.body.kind, ContentKind::empty);
}

TEST_F(ExecutorTest, NotFound) {
  const Exchange exchange = send(makeRequest("/nope"));
  ASSERT_TRUE(exchange.response.ok());  // an error status is a completed exchange
  EXPECT_EQ(exchange.response.status, 404);
}

TEST_F(ExecutorTest, BigBodyAndProgress) {
  const Executor::Ticket ticket = executor().submit(makeRequest("/big"), 0);
  const Exchange exchange = waitFor(ticket.id);
  ASSERT_TRUE(exchange.response.ok()) << exchange.response.error;
  EXPECT_EQ(exchange.response.body.raw.size(), kBigBodySize);
  EXPECT_EQ(ticket.progress->bytesReceived.load(), kBigBodySize);
  EXPECT_TRUE(ticket.progress->headReceived.load());
  EXPECT_EQ(exchange.response.body.kind, ContentKind::text);  // no NUL byte: sniffed as text
}

TEST_F(ExecutorTest, BodyLimit) {
  _executor = std::make_unique<Executor>(Executor::Options{.maxBodyBytes = 1000, .nbWorkers = 1});
  const Exchange exchange = send(makeRequest("/big"));
  EXPECT_EQ(exchange.response.state, ExchangeState::failed);
  EXPECT_TRUE(exchange.response.error.contains("too large")) << exchange.response.error;
  EXPECT_LE(exchange.response.body.raw.size(), 1000U);
}

TEST_F(ExecutorTest, Timeout) {
  ResolvedRequest req = makeRequest("/slow");
  req.settings.timeoutMs = 100;
  const Exchange exchange = send(std::move(req));
  EXPECT_EQ(exchange.response.state, ExchangeState::failed);
  EXPECT_TRUE(exchange.response.error.contains("100 ms")) << exchange.response.error;
}

TEST_F(ExecutorTest, CancelInFlight) {
  const Executor::Ticket ticket = executor().submit(makeRequest("/slow"), 0);
  std::this_thread::sleep_for(50ms);
  Executor::Cancel(ticket);
  const Exchange exchange = waitFor(ticket.id);
  EXPECT_EQ(exchange.response.state, ExchangeState::cancelled);
  EXPECT_FALSE(exchange.response.error.empty());
}

TEST_F(ExecutorTest, CancelWhileQueued) {
  Executor& single = executor(1);
  const Executor::Ticket slow = single.submit(makeRequest("/slow"), 0);
  const Executor::Ticket queued = single.submit(makeRequest("/json"), 0);
  Executor::Cancel(queued);
  const Exchange cancelled = waitFor(queued.id);
  EXPECT_EQ(cancelled.response.state, ExchangeState::cancelled);
  EXPECT_EQ(cancelled.response.error, "Cancelled before being sent.");
  EXPECT_TRUE(waitFor(slow.id).response.ok());
  Executor::Cancel(Executor::Ticket{});  // no-op on an invalid ticket
}

TEST_F(ExecutorTest, ConnectionRefusedAndInvalidUrls) {
  ResolvedRequest refused;
  refused.url = "http://127.0.0.1:1/";
  refused.settings.timeoutMs = 2000;
  Exchange exchange = send(std::move(refused));
  EXPECT_EQ(exchange.response.state, ExchangeState::failed);
  EXPECT_TRUE(exchange.response.error.contains("Could not connect")) << exchange.response.error;

  for (std::string_view invalid : {"ftp://h/x", "http://user:pw@h/", "not a url"}) {
    ResolvedRequest req;
    req.url = invalid;
    exchange = send(std::move(req));
    EXPECT_EQ(exchange.response.state, ExchangeState::failed) << invalid;
    EXPECT_FALSE(exchange.response.error.empty());
  }
}

TEST_F(ExecutorTest, ManyConcurrentRequests) {
  amc::vector<std::uint64_t> tickets;
  for (int i = 0; i < 40; ++i) {
    tickets.push_back(executor(4).submit(makeRequest("/json"), static_cast<std::uint64_t>(i)).id);
  }
  for (std::uint64_t ticket : tickets) {
    ASSERT_TRUE(waitFor(ticket).response.ok());
  }
  EXPECT_EQ(executor().pendingCount(), 0U);
}

TEST_F(ExecutorTest, DestructionWithPendingRequests) {
  Executor& single = executor(1);
  (void)single.submit(makeRequest("/slow"), 0);
  (void)single.submit(makeRequest("/slow"), 0);
  _executor.reset();  // must not hang nor crash
  SUCCEED();
}

}  // namespace
}  // namespace zalie
