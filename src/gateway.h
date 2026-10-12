// The main Discord gateway connection (wss://gateway.discord.gg, zlib-stream).
// Handles heartbeats, identify/resume and reconnects. Dispatches arrive on the
// main thread through on_dispatch.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <zlib.h>

#include "util.h"
#include "ws.h"

class Gateway {
public:
	enum State { Disconnected, Connecting, Connected };

	std::function<void(const std::string &type, json &d)> on_dispatch;
	std::function<void(State)> on_state;
	// The token was rejected (4004) or the session can't continue.
	std::function<void(int code, const std::string &why)> on_fatal;

	~Gateway();
	void start(const std::string &token);
	void stop();
	// Thread-safe.
	void send(int op, const json &d);
	State state() const { return state_; }

private:
	void connect();
	void on_open();
	void on_message(std::string &data, bool binary);
	void on_close(int code, const std::string &reason);
	void on_tick();
	void handle(json &msg);
	void identify();
	void resume();
	void heartbeat();
	void set_state(State s);
	void schedule_reconnect(int delay_ms);

	std::string token_;
	std::unique_ptr<WebSocket> ws_;
	z_stream zs_{};
	bool zs_init_ = false;
	std::string zbuf_;

	// Owned by the socket thread while connected.
	int64_t seq_ = 0;
	std::string session_id_, resume_url_;
	int64_t hb_interval_ = 0, next_hb_ = 0;
	bool hb_acked_ = true;
	bool can_resume_ = false;
	std::atomic<State> state_{Disconnected};
	std::atomic<bool> stopped_{true};
	int backoff_ms_ = 1000;
	std::mutex mu_;
	unsigned reconnect_src_ = 0;
};
