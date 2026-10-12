#include "ws.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#include "util.h"

WebSocket::~WebSocket()
{
	close(1000);
	join();
}

void WebSocket::connect(const std::string &url, std::vector<std::string> headers, Callbacks cb, int tick_ms)
{
	join();
	cb_ = std::move(cb);
	tick_ms_ = tick_ms;
	close_req_ = 0;
	outq_.clear();
	if (wake_fd_[0] < 0 && pipe2(wake_fd_, O_CLOEXEC | O_NONBLOCK) != 0)
		LOG("ws: pipe failed");
	running_ = true;
	thread_ = std::thread(&WebSocket::run, this, url, std::move(headers));
}

void WebSocket::join()
{
	if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
		thread_.join();
}

void WebSocket::wake()
{
	if (wake_fd_[1] >= 0) {
		char c = 1;
		ssize_t r = write(wake_fd_[1], &c, 1);
		(void)r;
	}
}

void WebSocket::send_text(std::string s)
{
	{
		std::lock_guard<std::mutex> lk(mu_);
		outq_.emplace_back(std::move(s), false);
	}
	wake();
}

void WebSocket::send_binary(std::string s)
{
	{
		std::lock_guard<std::mutex> lk(mu_);
		outq_.emplace_back(std::move(s), true);
	}
	wake();
}

void WebSocket::close(int code)
{
	if (!running_)
		return;
	close_req_ = code ? code : 1000;
	wake();
}

static int64_t mono_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Sends the whole buffer; curl_ws_send may write partially on a full socket.
static CURLcode ws_send_all(CURL *c, const char *data, size_t len, unsigned flags, curl_socket_t fd)
{
	size_t off = 0;
	bool first = true;
	while (off < len || first) {
		size_t sent = 0;
		CURLcode rc = curl_ws_send(c, data + off, len - off, &sent, first ? 0 : 0, flags);
		if (rc == CURLE_AGAIN) {
			struct pollfd p = {fd, POLLOUT, 0};
			poll(&p, 1, 1000);
			continue;
		}
		if (rc != CURLE_OK)
			return rc;
		off += sent;
		first = false;
		if (len == 0)
			break;
	}
	return CURLE_OK;
}

void WebSocket::run(std::string url, std::vector<std::string> headers)
{
	CURL *c = curl_easy_init();
	curl_slist *hl = nullptr;
	for (auto &h : headers)
		hl = curl_slist_append(hl, h.c_str());
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	CURLcode rc = curl_easy_perform(c);
	int close_code = 0;
	std::string reason;
	if (rc != CURLE_OK) {
		reason = curl_easy_strerror(rc);
		LOG("ws: connect %s failed: %s", url.c_str(), reason.c_str());
	} else {
		curl_socket_t fd = CURL_SOCKET_BAD;
		curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &fd);
		if (cb_.on_open)
			cb_.on_open();
		std::string msg;
		bool msg_binary = false;
		int64_t next_tick = mono_ms() + tick_ms_;
		char buf[65536];
		bool alive = true;
		while (alive) {
			// Outgoing frames.
			std::deque<std::pair<std::string, bool>> q;
			{
				std::lock_guard<std::mutex> lk(mu_);
				q.swap(outq_);
			}
			for (auto &m : q) {
				rc = ws_send_all(c, m.first.data(), m.first.size(), m.second ? CURLWS_BINARY : CURLWS_TEXT, fd);
				if (rc != CURLE_OK) {
					reason = curl_easy_strerror(rc);
					alive = false;
					break;
				}
			}
			if (!alive)
				break;
			if (int code = close_req_.load()) {
				char cb2[2] = {(char)(code >> 8), (char)(code & 0xff)};
				size_t sent;
				curl_ws_send(c, cb2, 2, &sent, 0, CURLWS_CLOSE);
				close_code = code;
				reason = "closed locally";
				break;
			}
			// Incoming frames: drain everything that is buffered.
			while (true) {
				size_t n = 0;
				const struct curl_ws_frame *meta = nullptr;
				rc = curl_ws_recv(c, buf, sizeof buf, &n, &meta);
				if (rc == CURLE_AGAIN)
					break;
				if (rc != CURLE_OK) {
					reason = curl_easy_strerror(rc);
					alive = false;
					break;
				}
				if (meta->flags & CURLWS_CLOSE) {
					if (n >= 2)
						close_code = ((uint8_t)buf[0] << 8) | (uint8_t)buf[1];
					else
						close_code = 1005;
					if (n > 2)
						reason.assign(buf + 2, n - 2);
					alive = false;
					break;
				}
				if (meta->flags & CURLWS_PING)
					continue;  // curl answers pings itself
				if (msg.empty())
					msg_binary = (meta->flags & CURLWS_BINARY) != 0;
				msg.append(buf, n);
				if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
					if (cb_.on_message)
						cb_.on_message(msg, msg_binary);
					msg.clear();
				}
			}
			if (!alive)
				break;
			int64_t now = mono_ms();
			if (now >= next_tick) {
				if (cb_.on_tick)
					cb_.on_tick();
				next_tick = now + tick_ms_;
			}
			struct pollfd p[2] = {{fd, POLLIN, 0}, {wake_fd_[0], POLLIN, 0}};
			int to = (int)std::max<int64_t>(0, next_tick - mono_ms());
			if (poll(p, 2, to) < 0 && errno != EINTR) {
				reason = strerror(errno);
				break;
			}
			if (p[1].revents & POLLIN) {
				char tmp[64];
				while (read(wake_fd_[0], tmp, sizeof tmp) > 0) {
				}
			}
			if (p[0].revents & (POLLERR | POLLHUP) && !(p[0].revents & POLLIN)) {
				reason = "socket hangup";
				break;
			}
		}
	}
	curl_easy_cleanup(c);
	curl_slist_free_all(hl);
	running_ = false;
	if (cb_.on_close)
		cb_.on_close(close_code, reason);
}
