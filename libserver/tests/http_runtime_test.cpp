#include "http_runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <sys/socket.h>
#endif

namespace {

using libserver::http::ParseRequestHead;

bool Check(bool condition, const char* message) {
  if (!condition) std::cerr << "http_runtime_test: " << message << '\n';
  return condition;
}

bool ExpectError(std::string_view head, int status, std::string_view code,
                 const libserver::http::Limits& limits = {}) {
  const auto result = ParseRequestHead(head, limits);
  return Check(!result.request, "malformed request unexpectedly parsed") &&
         Check(result.error_status == status, "malformed request returned the wrong status") &&
         Check(result.error_code == code, "malformed request returned the wrong error code");
}

bool ParserTests() {
  bool passed = true;
  const auto valid = ParseRequestHead(
      "POST /api/v2/test?a=b HTTP/1.1\r\n"
      "Host: localhost:8080\r\n"
      "Content-Length: 2\r\n"
      "X-Test: one\r\n"
      "X-Test: two");
  passed &= Check(valid.request.has_value(), "valid request did not parse");
  if (valid.request) {
    passed &= Check(valid.request->method == "POST", "method was not preserved");
    passed &= Check(valid.request->path == "/api/v2/test", "path was not preserved");
    passed &= Check(valid.request->query.at("a") == "b", "query was not preserved");
    passed &= Check(valid.request->headers.at("x-test") == "one, two",
                    "repeatable headers were not combined");
  }

  passed &= ExpectError("GET / HTTP/1.1", 400, "missing_host");
  passed &= ExpectError("GET / HTTP/1.1\r\nHost:", 400, "invalid_host");
  passed &= ExpectError("GET / HTTP/1.1\r\nHost: one\r\nhOsT: two", 400,
                        "invalid_host");
  passed &= ExpectError("GET / HTTP/1.1\r\nHost : one", 400, "malformed_headers");
  passed &= ExpectError("GET / HTTP/1.1\r\nHost: user@example.com", 400,
                        "invalid_host");
  passed &= ExpectError("GET / HTTP/1.1\r\nHost: one\r\n folded", 400,
                        "malformed_headers");
  passed &= ExpectError("GET / HTTP/1.1\nHost: one", 400, "malformed_headers");
  passed &= ExpectError("GET / HTTP/1.0\r\nHost: one", 505,
                        "http_version_not_supported");
  passed &= ExpectError("GET http://one/path HTTP/1.1\r\nHost: one", 400,
                        "invalid_request_target");
  passed &= ExpectError("GET /path#fragment HTTP/1.1\r\nHost: one", 400,
                        "invalid_request_target");
  passed &= ExpectError("GET / HTTP/1.1 extra\r\nHost: one", 400,
                        "malformed_request_line");
  passed &= ExpectError("GE(T / HTTP/1.1\r\nHost: one", 400, "invalid_method");

  passed &= ExpectError(
      "POST / HTTP/1.1\r\nHost: one\r\nContent-Length: 2\r\nContent-Length: 2",
      400, "invalid_content_length");
  passed &= ExpectError(
      "POST / HTTP/1.1\r\nHost: one\r\nContent-Length: 2\r\nContent-Length: 3",
      400, "invalid_content_length");
  passed &= ExpectError("POST / HTTP/1.1\r\nHost: one\r\nContent-Length: 2, 2",
                        400, "invalid_content_length");
  passed &= ExpectError("POST / HTTP/1.1\r\nHost: one\r\nContent-Length: +2",
                        400, "invalid_content_length");
  passed &= ExpectError("POST / HTTP/1.1\r\nHost: one\r\nContent-Length: 2 2",
                        400, "invalid_content_length");
  passed &= ExpectError("POST / HTTP/1.1\r\nHost: one\r\nTransfer-Encoding: chunked",
                        501, "transfer_encoding_not_supported");
  passed &= ExpectError(
      "POST / HTTP/1.1\r\nHost: one\r\nTransfer-Encoding: chunked\r\nContent-Length: 2",
      400, "ambiguous_framing");
  passed &= ExpectError(
      "GET / HTTP/1.1\r\nHost: one\r\nAuthorization: one\r\nAuthorization: two",
      400, "duplicate_authorization");

  libserver::http::Limits body_limit;
  body_limit.maximum_body_bytes = 3;
  passed &= ExpectError("POST / HTTP/1.1\r\nHost: one\r\nContent-Length: 4",
                        413, "payload_too_large", body_limit);
  libserver::http::Limits target_limit;
  target_limit.maximum_request_target_bytes = 3;
  passed &= ExpectError("GET /abc HTTP/1.1\r\nHost: one", 414,
                        "request_target_too_long", target_limit);
  libserver::http::Limits field_limit;
  field_limit.maximum_header_fields = 1;
  passed &= ExpectError("GET / HTTP/1.1\r\nHost: one\r\nAccept: */*", 400,
                        "malformed_headers", field_limit);
  libserver::http::Limits head_limit;
  head_limit.maximum_header_bytes = 16;
  passed &= ExpectError("GET / HTTP/1.1\r\nHost: one", 431,
                        "headers_too_large", head_limit);
  return passed;
}

#if !defined(_WIN32)
bool MakeSocketPair(int sockets[2]) {
  return socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0;
}

bool FramingAndDeadlineTests() {
  bool passed = true;
  int complete[2]{};
  if (!Check(MakeSocketPair(complete), "could not create complete request socket pair")) {
    return false;
  }
  const std::string wire =
      "POST /submit HTTP/1.1\r\nHost: local\r\nContent-Length: 4\r\n\r\ndataNEXT";
  passed &= Check(send(complete[1], wire.data(), wire.size(), 0) ==
                      static_cast<ssize_t>(wire.size()),
                  "could not send complete request");
  libserver::http::Connection complete_connection(complete[0]);
  passed &= Check(complete_connection.Handshake(), "raw connection handshake failed");
  const auto request = libserver::http::ReadRequest(complete_connection);
  passed &= Check(request.request.has_value(), "complete framed request did not parse");
  if (request.request) {
    passed &= Check(request.request->body == "data", "parser consumed beyond Content-Length");
  }
  libserver::http::CloseSocket(complete[0]);
  libserver::http::CloseSocket(complete[1]);

  int partial[2]{};
  if (!Check(MakeSocketPair(partial), "could not create partial request socket pair")) {
    return false;
  }
  const std::string partial_wire =
      "POST /submit HTTP/1.1\r\nHost: local\r\nContent-Length: 4\r\n\r\nda";
  passed &= Check(send(partial[1], partial_wire.data(), partial_wire.size(), 0) ==
                      static_cast<ssize_t>(partial_wire.size()),
                  "could not send partial request");
  libserver::http::Connection partial_connection(partial[0]);
  passed &= Check(partial_connection.Handshake(), "partial raw connection handshake failed");
  const auto timed_out = libserver::http::ReadRequest(
      partial_connection, {}, std::chrono::milliseconds(50));
  passed &= Check(!timed_out.request && timed_out.error_status == 408,
                  "partial body did not hit the read deadline");
  libserver::http::CloseSocket(partial[0]);
  libserver::http::CloseSocket(partial[1]);

  int broken[2]{};
  if (!Check(MakeSocketPair(broken), "could not create broken pipe socket pair")) {
    return false;
  }
  libserver::http::CloseSocket(broken[1]);
  libserver::http::Connection broken_connection(broken[0]);
  passed &= Check(broken_connection.Handshake(), "broken raw connection handshake failed");
  passed &= Check(!libserver::http::SendAll(
                      broken_connection, "response", std::chrono::milliseconds(50)),
                  "send to a closed peer unexpectedly succeeded");
  libserver::http::CloseSocket(broken[0]);
  return passed;
}

bool BoundedPoolTest() {
  bool passed = true;
  std::mutex mutex;
  std::condition_variable condition;
  bool release = false;
  std::size_t processed = 0;
  libserver::http::WorkerPool pool(1, 1, [&](libserver::http::Connection&) {
    std::unique_lock lock(mutex);
    ++processed;
    condition.notify_all();
    condition.wait(lock, [&] { return release; });
  });
  passed &= Check(pool.Start(), "bounded worker pool did not start");
  int first[2]{};
  int second[2]{};
  int rejected[2]{};
  passed &= Check(MakeSocketPair(first) && MakeSocketPair(second) && MakeSocketPair(rejected),
                  "could not create bounded worker pool sockets");
  passed &= Check(pool.Enqueue(first[0]), "first connection was not accepted");
  {
    std::unique_lock lock(mutex);
    passed &= Check(condition.wait_for(lock, std::chrono::seconds(1), [&] {
                      return processed == 1;
                    }),
                    "worker did not begin the first connection");
  }
  passed &= Check(pool.Enqueue(second[0]), "queued connection was not accepted");
  passed &= Check(!pool.Enqueue(rejected[0]), "connection beyond queue capacity was accepted");
  libserver::http::CloseSocket(rejected[0]);
  libserver::http::CloseSocket(rejected[1]);
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
  pool.Shutdown();
  passed &= Check(processed == 1, "shutdown began queued work after stopping");
  char byte = 0;
  passed &= Check(recv(second[1], &byte, sizeof(byte), 0) == 0,
                  "shutdown did not close queued connection");
  libserver::http::CloseSocket(first[1]);
  libserver::http::CloseSocket(second[1]);
  return passed;
}

bool ActiveShutdownTest() {
  bool passed = true;
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  libserver::http::WorkerPool pool(1, 1, [&](libserver::http::Connection& connection) {
    {
      std::lock_guard lock(mutex);
      entered = true;
    }
    condition.notify_all();
    if (connection.Handshake()) (void)libserver::http::ReadRequest(connection);
  });
  passed &= Check(pool.Start(), "active-shutdown worker pool did not start");
  int sockets[2]{};
  passed &= Check(MakeSocketPair(sockets), "could not create active-shutdown socket pair");
  passed &= Check(pool.Enqueue(sockets[0]), "active-shutdown connection was not accepted");
  {
    std::unique_lock lock(mutex);
    passed &= Check(condition.wait_for(lock, std::chrono::seconds(1), [&] { return entered; }),
                    "active-shutdown handler did not start");
  }
  pool.Shutdown();
  passed &= Check(pool.active() == 0, "shutdown returned before active worker joined");
  libserver::http::CloseSocket(sockets[1]);
  return passed;
}
#endif

}  // namespace

int main() {
  bool passed = ParserTests();
#if !defined(_WIN32)
  passed &= FramingAndDeadlineTests();
  passed &= BoundedPoolTest();
  passed &= ActiveShutdownTest();
#endif
  if (passed) std::cout << "http_runtime_test passed\n";
  return passed ? 0 : 1;
}
