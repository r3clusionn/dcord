#include "gateway.h"

#include <random>
#include <time.h>

#include <glib.h>

#include "props.h"

namespace {
const char *kGatewayUrl = "wss://gateway.discord.gg/?v=9&encoding=json&compress=zlib-stream";

// LAZY_USER_NOTES | NO_AFFINE_USER_IDS | DEDUPE_USER_OBJECTS | PRIORITIZED_READY_PAYLOAD |
// USER_SETTINGS_PROTO | CLIENT_STATE_V2 | PASSIVE_GUILD_UPDATE_V2
const int64_t kCapabilities = (1 << 0) | (1 << 1) | (1 << 4) | (1 << 5) | (1 << 9) | (1 << 10) | (1 << 14);

int64_t mono_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
}  // namespace

Gateway::~Gateway()
{
	stop();
	if (zs_init_)
		inflateEnd(&zs_);
}

void Gateway::start(const std::string &token)
{
	token_ = token;
	stopped_ = false;
	seq_ = 0;
	session_id_.clear();
	can_resume_ = false;
	connect();
}

void Gateway::stop()
{
	stopped_ = true;
	if (reconnect_src_) {
		g_source_remove(reconnect_src_);
		reconnect_src_ = 0;
	}
	if (ws_) {
		ws_->close(1000);
		ws_->join();
		ws_.reset();
	}
	set_state(Disconnected);
}

void Gateway::set_state(State s)
{
	state_ = s;
	post_main([this, s]() {
		if (on_state)
			on_state(s);
	});
}

void Gateway::connect()
{
	if (ws_) {
		ws_->join();
		ws_.reset();
	}
	if (zs_init_)
		inflateEnd(&zs_);
	zs_ = {};
	inflateInit(&zs_);
	zs_init_ = true;
	zbuf_.clear();
	hb_interval_ = 0;
	hb_acked_ = true;
	set_state(Connecting);

	std::string url = kGatewayUrl;
	if (const char *g = getenv("DCORD_GATEWAY"))
		url = g;
	else if (can_resume_ && !resume_url_.empty())
		url = resume_url_ + "/?v=9&encoding=json&compress=zlib-stream";
	ws_ = std::make_unique<WebSocket>();
	WebSocket::Callbacks cb;
	cb.on_open = [this]() { on_open(); };
	cb.on_message = [this](std::string &d, bool b) { on_message(d, b); };
	cb.on_close = [this](int c, const std::string &r) { on_close(c, r); };
	cb.on_tick = [this]() { on_tick(); };
	ws_->connect(url, {"User-Agent: " + props_user_agent(), "Origin: https://discord.com"}, cb, 1000);
}

void Gateway::on_open()
{
	LOG("gateway: connected");
}

void Gateway::on_message(std::string &data, bool binary)
{
	std::string text;
	if (binary) {
		zbuf_.append(data);
		if (zbuf_.size() < 4 || memcmp(zbuf_.data() + zbuf_.size() - 4, "\x00\x00\xff\xff", 4) != 0)
			return;
		zs_.next_in = (Bytef *)zbuf_.data();
		zs_.avail_in = (uInt)zbuf_.size();
		char out[65536];
		do {
			zs_.next_out = (Bytef *)out;
			zs_.avail_out = sizeof out;
			int rc = inflate(&zs_, Z_SYNC_FLUSH);
			if (rc != Z_OK && rc != Z_BUF_ERROR) {
				LOG("gateway: inflate error %d", rc);
				zbuf_.clear();
				ws_->close(4000);
				return;
			}
			text.append(out, sizeof out - zs_.avail_out);
		} while (zs_.avail_out == 0);
		zbuf_.clear();
	} else {
		text.swap(data);
	}
	json msg = json::parse(text, nullptr, false);
	if (msg.is_discarded()) {
		LOG("gateway: bad json (%zu bytes)", text.size());
		return;
	}
	handle(msg);
}

void Gateway::handle(json &msg)
{
	int op = (int)jint(msg, "op", -1);
	if (msg.contains("s") && msg["s"].is_number())
		seq_ = msg["s"].get<int64_t>();
	switch (op) {
	case 0: {  // dispatch
		std::string t = jstr(msg, "t");
		if (t == "READY") {
			json &d = msg["d"];
			session_id_ = jstr(d, "session_id");
			resume_url_ = jstr(d, "resume_gateway_url");
			can_resume_ = true;
			backoff_ms_ = 1000;
			set_state(Connected);
			LOG("gateway: READY session %s", session_id_.c_str());
		} else if (t == "RESUMED") {
			backoff_ms_ = 1000;
			set_state(Connected);
			LOG("gateway: resumed at seq %lld", (long long)seq_);
		}
		auto d = std::make_shared<json>(std::move(msg["d"]));
		post_main([this, t, d]() {
			if (on_dispatch)
				on_dispatch(t, *d);
		});
		break;
	}
	case 1:
		heartbeat();
		break;
	case 7:  // reconnect
		LOG("gateway: server asked to reconnect");
		ws_->close(4900);
		break;
	case 9:  // invalid session
		LOG("gateway: invalid session (resumable=%d)", (int)jbool(msg, "d"));
		if (!jbool(msg, "d")) {
			can_resume_ = false;
			session_id_.clear();
			seq_ = 0;
		}
		ws_->close(4901);
		break;
	case 10: {  // hello
		hb_interval_ = jint(jget(msg, "d"), "heartbeat_interval", 41250);
		static std::mt19937 rng(std::random_device{}());
		next_hb_ = mono_ms() + (int64_t)(hb_interval_ * std::uniform_real_distribution<double>(0, 1)(rng));
		if (can_resume_ && !session_id_.empty())
			resume();
		else
			identify();
		break;
	}
	case 11:
		hb_acked_ = true;
		break;
	default:
		break;
	}
}

void Gateway::identify()
{
	json d = {
		{"token", token_},
		{"capabilities", kCapabilities},
		{"properties", props_object()},
		{"presence", {{"status", "unknown"}, {"since", 0}, {"activities", json::array()}, {"afk", false}}},
		{"compress", false},
		{"client_state", {{"guild_versions", json::object()}}},
	};
	ws_->send_text(json{{"op", 2}, {"d", d}}.dump());
	LOG("gateway: identify sent");
}

void Gateway::resume()
{
	json d = {{"token", token_}, {"session_id", session_id_}, {"seq", seq_}};
	ws_->send_text(json{{"op", 6}, {"d", d}}.dump());
	LOG("gateway: resume sent (seq %lld)", (long long)seq_);
}

void Gateway::heartbeat()
{
	json m = {{"op", 1}, {"d", seq_ ? json(seq_) : json(nullptr)}};
	ws_->send_text(m.dump());
}

void Gateway::on_tick()
{
	if (hb_interval_ <= 0)
		return;
	int64_t now = mono_ms();
	if (now < next_hb_)
		return;
	if (!hb_acked_) {
		LOG("gateway: heartbeat not acknowledged, reconnecting");
		ws_->close(4902);
		return;
	}
	hb_acked_ = false;
	heartbeat();
	next_hb_ = now + hb_interval_;
}

void Gateway::send(int op, const json &d)
{
	std::lock_guard<std::mutex> lk(mu_);
	if (ws_ && ws_->running())
		ws_->send_text(json{{"op", op}, {"d", d}}.dump());
}

void Gateway::on_close(int code, const std::string &reason)
{
	LOG("gateway: closed (%d %s)", code, reason.c_str());
	// Codes that mean the session can never work again.
	if (code == 4004 || code == 4010 || code == 4011 || code == 4012 || code == 4013 || code == 4014) {
		stopped_ = true;
		set_state(Disconnected);
		post_main([this, code, reason]() {
			if (on_fatal)
				on_fatal(code, code == 4004 ? "The token was rejected (logged out or invalid)." : reason);
		});
		return;
	}
	if (code == 4007 || code == 4009) {
		can_resume_ = false;
		session_id_.clear();
		seq_ = 0;
	}
	if (stopped_) {
		set_state(Disconnected);
		return;
	}
	set_state(Connecting);
	// 4900: server asked us to reconnect (fast). 4901: invalid session, Discord
	// asks clients to wait 1-5 s before identifying again.
	static std::mt19937 rng(std::random_device{}());
	int delay = code == 4900 ? 200 : code == 4901 ? 1000 + (int)(rng() % 4000) : backoff_ms_;
	backoff_ms_ = std::min(backoff_ms_ * 2, 60000);
	post_main([this, delay]() { schedule_reconnect(delay); });
}

void Gateway::schedule_reconnect(int delay_ms)
{
	if (stopped_ || reconnect_src_)
		return;
	reconnect_src_ = g_timeout_add(delay_ms, [](gpointer p) -> gboolean {
		Gateway *g = static_cast<Gateway *>(p);
		g->reconnect_src_ = 0;
		if (!g->stopped_)
			g->connect();
		return G_SOURCE_REMOVE;
	}, this);
}
