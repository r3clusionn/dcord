// Minimal WebSocket client on top of libcurl's ws support. One thread per
// connection; the owner's callbacks run on that thread.
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class WebSocket {
public:
	struct Callbacks {
		std::function<void()> on_open;
		std::function<void(std::string &data, bool binary)> on_message;
		// code is the close code from the server, or 0 for a transport error.
		std::function<void(int code, const std::string &reason)> on_close;
		// Called every `tick_ms` on the socket thread (heartbeats).
		std::function<void()> on_tick;
	};

	WebSocket() = default;
	~WebSocket();
	WebSocket(const WebSocket &) = delete;
	WebSocket &operator=(const WebSocket &) = delete;

	void connect(const std::string &url, std::vector<std::string> headers, Callbacks cb, int tick_ms = 250);
	// Thread-safe. Queued and sent on the socket thread.
	void send_text(std::string s);
	void send_binary(std::string s);
	// Thread-safe; on_close is still called (with the given code).
	void close(int code = 1000);
	// Waits for the thread to end. Must not be called from a callback.
	void join();
	bool running() const { return running_; }

private:
	void run(std::string url, std::vector<std::string> headers);
	void wake();

	Callbacks cb_;
	int tick_ms_ = 250;
	std::thread thread_;
	std::mutex mu_;
	std::deque<std::pair<std::string, bool>> outq_;
	std::atomic<bool> running_{false};
	std::atomic<int> close_req_{0};
	int wake_fd_[2] = {-1, -1};
};
