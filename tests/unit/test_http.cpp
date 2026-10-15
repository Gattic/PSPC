// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/host.h"
#include "net/host_net.h"
#include "net/http_client.h"
#include "testing.h"
#include <atomic>
#include <future>
#include <stdexcept>
#include <thread>

namespace {
// A real loopback peer that can stall before headers or before the body. Every wait is bounded.
class StalledServer {
public:
	explicit StalledServer(bool headers, const std::string &body = {}) {
		int error;
		listener_ = hostnet::OpenSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, &error);
		if (!listener_.Valid()) throw std::runtime_error("HTTP fixture socket failed");
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(listener_.s, (sockaddr *)&address, sizeof(address)) || listen(listener_.s, 1))
			throw std::runtime_error("HTTP fixture listen failed");
		int length = sizeof(address);
		if (hostnet::GetSockName(listener_.s, (sockaddr *)&address, &length))
			throw std::runtime_error("HTTP fixture address failed");
		url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/";
		worker_ = std::thread([this, headers, body] {
			if (!Ready(listener_.s)) return;
			auto accepted = hostnet::Accept(listener_.s, nullptr, nullptr);
			if (accepted == hostnet::InvalidSocket) return;
			hostnet::Socket client;
			int error;
			if (!hostnet::AdoptSocket(accepted, &client, &error)) {
				hostnet::CloseSocketHandle(accepted); return;
			}
			std::string request;
			while (request.find("\r\n\r\n") == std::string::npos && Ready(client.s)) {
				char data[4096];
				int n = recv(client.s, data, sizeof(data), 0);
				if (n <= 0) break;
				request.append(data, n);
			}
			requested = request.find("\r\n\r\n") != std::string::npos;
			if (headers && requested) {
				const char *response = "HTTP/1.1 200 OK\r\nContent-Length: 8\r\nConnection: close\r\n\r\n";
				hostnet::Send(client.s, response, (int)strlen(response), 0);
				if (!body.empty()) hostnet::Send(client.s, body.data(), (int)body.size(), 0);
			}
			u64 until = hostsys::MonotonicUs() + 3000000;
			while (!stop_ && hostsys::MonotonicUs() < until) hostsys::SleepMs(1);
			hostnet::CloseSocket(client);
		});
	}
	~StalledServer() { stop_ = true; worker_.join(); hostnet::CloseSocket(listener_); }
	std::string url;
	std::atomic<bool> requested{false};
private:
	bool Ready(hostnet::SocketHandle socket) {
		u64 until = hostsys::MonotonicUs() + 3000000;
		while (!stop_ && hostsys::MonotonicUs() < until) {
			hostnet::PollFd fd{socket, POLLRDNORM, 0};
			if (hostnet::Poll(&fd, 1, 20) > 0) return true;
		}
		return false;
	}
	hostnet::Socket listener_;
	std::atomic<bool> stop_{false};
	std::thread worker_;
};

hostnet::HttpRequestSpec Request(const StalledServer &server, u32 timeout = 1000) {
	hostnet::HttpRequestSpec spec;
	spec.url = server.url;
	spec.connectTimeoutMs = spec.resolveTimeoutMs = spec.sendTimeoutMs = 1000;
	spec.recvTimeoutMs = timeout;
	return spec;
}
}

TEST(http_headers_return_before_body_and_read_timeout) {
	StalledServer server(true);
	hostnet::HttpExchange exchange;
	CHECK_EQ(exchange.Send(Request(server, 100)), 0u);
	CHECK_EQ(exchange.status, 200);
	CHECK(exchange.haveContentLength && exchange.contentLength == 8);
	u8 byte = 0;
	u32 got = 99;
	u64 started = hostsys::MonotonicUs();
	CHECK_EQ(exchange.Read(&byte, 0, &got), 0u);
	CHECK_EQ(got, 0u);
	CHECK(hostsys::MonotonicUs() - started < 500000);
	started = hostsys::MonotonicUs();
	u32 error = exchange.Read(&byte, 1, &got);
	if (hostnet::HttpExchange::ClassifyError(error) != hostnet::HttpFailure::Timeout)
		printf("  body read returned %s (%u), %u bytes\n", hostnet::HttpExchange::ErrorName(error), error, got);
	CHECK(hostnet::HttpExchange::ClassifyError(error) == hostnet::HttpFailure::Timeout);
	CHECK(hostsys::MonotonicUs() - started < 1000000);
	CHECK_EQ(got, 0u);
	CHECK(hostnet::HttpExchange::ClassifyError(exchange.Read(&byte, 1, &got)) == hostnet::HttpFailure::Timeout);
}

TEST(http_receive_headers_timeout) {
	StalledServer server(false);
	hostnet::HttpExchange exchange;
	u64 started = hostsys::MonotonicUs();
	u32 error = exchange.Send(Request(server, 100));
	if (hostnet::HttpExchange::ClassifyError(error) != hostnet::HttpFailure::Timeout)
		printf("  headers returned %s (%u) after %llu us\n", hostnet::HttpExchange::ErrorName(error), error, hostsys::MonotonicUs() - started);
	CHECK(server.requested);
	CHECK(hostnet::HttpExchange::ClassifyError(error) == hostnet::HttpFailure::Timeout);
	CHECK(hostsys::MonotonicUs() - started < 1000000);
}

TEST(http_reuse_after_timeout_and_partial_reads) {
	hostnet::HttpExchange exchange;
	u8 byte = 0;
	u32 got = 0;
	{
		StalledServer server(true);
		CHECK_EQ(exchange.Send(Request(server, 50)), 0u);
		CHECK(hostnet::HttpExchange::ClassifyError(exchange.Read(&byte, 1, &got)) == hostnet::HttpFailure::Timeout);
	}
	StalledServer server(true, "payload!");
	CHECK_EQ(exchange.Send(Request(server)), 0u);
	std::string body;
	for (int i = 0; i < 8; ++i) {
		CHECK_EQ(exchange.Read(&byte, 1, &got), 0u);
		CHECK_EQ(got, 1u);
		body += (char)byte;
	}
	CHECK(body == "payload!");
	CHECK_EQ(exchange.Read(&byte, 1, &got), 0u);
	CHECK_EQ(got, 0u);
}

TEST(http_cancel_during_send_and_read_is_persistent) {
	for (bool headers : {false, true}) {
		StalledServer server(headers);
		hostnet::HttpExchange exchange;
		auto spec = Request(server);
		if (headers) CHECK_EQ(exchange.Send(spec), 0u);
		auto result = std::async(std::launch::async, [&] {
			if (!headers) return exchange.Send(spec);
			u8 byte; u32 got;
			return exchange.Read(&byte, 1, &got);
		});
		u64 until = hostsys::MonotonicUs() + 1000000;
		while (!server.requested && hostsys::MonotonicUs() < until) hostsys::SleepMs(1);
		CHECK(server.requested);
		CHECK(result.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
		exchange.Cancel();
		CHECK(result.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
		CHECK(hostnet::HttpExchange::ClassifyError(result.get()) == hostnet::HttpFailure::Cancelled);
		CHECK(hostnet::HttpExchange::ClassifyError(exchange.Send(spec)) == hostnet::HttpFailure::Cancelled);
		u8 byte; u32 got = 99;
		CHECK(hostnet::HttpExchange::ClassifyError(exchange.Read(&byte, 1, &got)) == hostnet::HttpFailure::Cancelled);
		CHECK_EQ(got, 0u);
	}
}
