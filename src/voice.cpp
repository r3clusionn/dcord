// Voice connection: voice gateway v8, UDP/RTP, transport AEAD, DAVE, Opus.
#include "voice.h"

#include <arpa/inet.h>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <dave/dave.h>
#include <glib.h>
#include <openssl/evp.h>
#include <opus/opus.h>

#include "app.h"
#include "audio.h"
#include "store.h"
#include "ws.h"

std::function<void()> g_voice_changed;
std::function<void()> g_voice_speaking;

namespace {

const int kRate = 48000;
const int kFrame = 960;  // 20 ms
const uint8_t kSilence[3] = {0xF8, 0xFF, 0xFE};

int64_t mono_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// --- float ring buffer -----------------------------------------------------------

struct Ring {
	std::vector<float> b;
	size_t r = 0, w = 0, n = 0;
	explicit Ring(size_t cap = 0) : b(cap) {}
	size_t size() const { return n; }
	void push(const float *s, size_t k)
	{
		for (size_t i = 0; i < k; i++) {
			if (n == b.size()) {  // full: drop oldest
				r = (r + 1) % b.size();
				n--;
			}
			b[w] = s[i];
			w = (w + 1) % b.size();
			n++;
		}
	}
	size_t pop(float *d, size_t k)
	{
		size_t m = std::min(k, n);
		for (size_t i = 0; i < m; i++) {
			d[i] = b[r];
			r = (r + 1) % b.size();
		}
		n -= m;
		return m;
	}
	void drop(size_t k)
	{
		size_t m = std::min(k, n);
		r = (r + m) % b.size();
		n -= m;
	}
};

// --- XChaCha20-Poly1305 (HChaCha20 + OpenSSL's ChaCha20-Poly1305) --------------------

inline uint32_t rotl(uint32_t v, int c)
{
	return (v << c) | (v >> (32 - c));
}

void hchacha20(const uint8_t key[32], const uint8_t nonce[16], uint8_t out[32])
{
	uint32_t x[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
	for (int i = 0; i < 8; i++)
		x[4 + i] = (uint32_t)key[i * 4] | (uint32_t)key[i * 4 + 1] << 8 | (uint32_t)key[i * 4 + 2] << 16 | (uint32_t)key[i * 4 + 3] << 24;
	for (int i = 0; i < 4; i++)
		x[12 + i] = (uint32_t)nonce[i * 4] | (uint32_t)nonce[i * 4 + 1] << 8 | (uint32_t)nonce[i * 4 + 2] << 16 |
			    (uint32_t)nonce[i * 4 + 3] << 24;
#define QR(a, b, c, d)                       \
	a += b; d ^= a; d = rotl(d, 16);     \
	c += d; b ^= c; b = rotl(b, 12);     \
	a += b; d ^= a; d = rotl(d, 8);      \
	c += d; b ^= c; b = rotl(b, 7);
	for (int i = 0; i < 10; i++) {
		QR(x[0], x[4], x[8], x[12]);
		QR(x[1], x[5], x[9], x[13]);
		QR(x[2], x[6], x[10], x[14]);
		QR(x[3], x[7], x[11], x[15]);
		QR(x[0], x[5], x[10], x[15]);
		QR(x[1], x[6], x[11], x[12]);
		QR(x[2], x[7], x[8], x[13]);
		QR(x[3], x[4], x[9], x[14]);
	}
#undef QR
	const int idx[8] = {0, 1, 2, 3, 12, 13, 14, 15};
	for (int i = 0; i < 8; i++)
		for (int k = 0; k < 4; k++)
			out[i * 4 + k] = (uint8_t)(x[idx[i]] >> (8 * k));
}

// Transport crypto for the *_rtpsize AEAD modes. The 4-byte nonce counter is
// appended to the packet; the RTP header (incl. CSRCs and the extension
// preamble) is the additional data.
struct Transport {
	enum Mode { AES, XCHACHA } mode = AES;
	uint8_t key[32];
	uint32_t counter = 0;
	EVP_CIPHER_CTX *enc = EVP_CIPHER_CTX_new(), *dec = EVP_CIPHER_CTX_new();
	~Transport()
	{
		EVP_CIPHER_CTX_free(enc);
		EVP_CIPHER_CTX_free(dec);
	}

	bool seal(const uint8_t *hdr, size_t hl, const uint8_t *pt, size_t pl, std::vector<uint8_t> &out)
	{
		uint8_t n4[4];
		uint32_t c = counter++;
		n4[0] = c >> 24;
		n4[1] = c >> 16;
		n4[2] = c >> 8;
		n4[3] = c;
		uint8_t nonce[12] = {0};
		const EVP_CIPHER *ci;
		uint8_t subkey[32];
		const uint8_t *k = key;
		if (mode == AES) {
			memcpy(nonce, n4, 4);
			ci = EVP_aes_256_gcm();
		} else {
			uint8_t n24[24] = {0};
			memcpy(n24, n4, 4);
			hchacha20(key, n24, subkey);
			memcpy(nonce + 4, n24 + 16, 8);
			k = subkey;
			ci = EVP_chacha20_poly1305();
		}
		out.resize(hl + pl + 16 + 4);
		memcpy(out.data(), hdr, hl);
		int len = 0;
		if (EVP_EncryptInit_ex(enc, ci, nullptr, k, nonce) != 1)
			return false;
		EVP_EncryptUpdate(enc, nullptr, &len, hdr, (int)hl);
		EVP_EncryptUpdate(enc, out.data() + hl, &len, pt, (int)pl);
		EVP_EncryptFinal_ex(enc, out.data() + hl + len, &len);
		EVP_CIPHER_CTX_ctrl(enc, EVP_CTRL_AEAD_GET_TAG, 16, out.data() + hl + pl);
		memcpy(out.data() + hl + pl + 16, n4, 4);
		return true;
	}

	bool open(const uint8_t *pkt, size_t n, size_t hl, std::vector<uint8_t> &out)
	{
		if (n < hl + 16 + 4)
			return false;
		const uint8_t *n4 = pkt + n - 4;
		const uint8_t *tag = pkt + n - 20;
		size_t cl = n - hl - 20;
		uint8_t nonce[12] = {0};
		uint8_t subkey[32];
		const uint8_t *k = key;
		const EVP_CIPHER *ci;
		if (mode == AES) {
			memcpy(nonce, n4, 4);
			ci = EVP_aes_256_gcm();
		} else {
			uint8_t n24[24] = {0};
			memcpy(n24, n4, 4);
			hchacha20(key, n24, subkey);
			memcpy(nonce + 4, n24 + 16, 8);
			k = subkey;
			ci = EVP_chacha20_poly1305();
		}
		out.resize(cl);
		int len = 0;
		if (EVP_DecryptInit_ex(dec, ci, nullptr, k, nonce) != 1)
			return false;
		EVP_DecryptUpdate(dec, nullptr, &len, pkt, (int)hl);
		EVP_DecryptUpdate(dec, out.data(), &len, pkt + hl, (int)cl);
		EVP_CIPHER_CTX_ctrl(dec, EVP_CTRL_AEAD_SET_TAG, 16, (void *)tag);
		return EVP_DecryptFinal_ex(dec, out.data() + len, &len) == 1;
	}
};

struct Peer {
	OpusDecoder *dec = nullptr;
	Ring ring{kRate * 2 / 2};  // 0.5 s stereo
	bool buffering = true;
	bool have_seq = false;
	uint16_t last_seq = 0;
	Snowflake uid = 0;
	int64_t last_voice = 0;
	~Peer()
	{
		if (dec)
			opus_decoder_destroy(dec);
	}
};

struct Params {
	Snowflake guild_id, channel_id, user_id;
	std::string session_id, token, endpoint;
};

VoiceStatus g_status;
std::mutex g_status_mu;
std::atomic<bool> g_mute{false}, g_deaf{false}, g_ptt_down{false};
std::atomic<float> g_level_db{-100};
std::map<Snowflake, double> g_user_vol;
std::mutex g_vol_mu;

void set_status(std::function<void(VoiceStatus &)> f)
{
	{
		std::lock_guard<std::mutex> lk(g_status_mu);
		f(g_status);
	}
	post_main([]() {
		if (g_voice_changed)
			g_voice_changed();
	});
}

class Conn;
std::unique_ptr<Conn> g_conn;

class Conn {
public:
	explicit Conn(Params p) : p_(std::move(p)) {}
	~Conn() { stop(); }

	void start()
	{
		std::string ep = p_.endpoint;
		if (ep.rfind("wss://", 0) != 0 && ep.rfind("ws://", 0) != 0)
			ep = "wss://" + ep;
		connect_ws(ep + "/?v=8");
	}

	void stop()
	{
		if (stopping_.exchange(true))
			return;
		audio_stop();
		ws_.close(1000);
		ws_.join();
		cv_.notify_all();
		if (udp_ >= 0)
			shutdown(udp_, SHUT_RDWR);
		if (recv_t_.joinable())
			recv_t_.join();
		if (send_t_.joinable())
			send_t_.join();
		if (tone_t_.joinable())
			tone_t_.join();
		LOG("voice: stopped (sent %llu frames, received %llu)", (unsigned long long)tx_frames_,
		    (unsigned long long)rx_frames_);
		if (udp_ >= 0)
			close(udp_);
		udp_ = -1;
		std::lock_guard<std::mutex> lk(dave_mu_);
		for (auto &[u, d] : decryptors_)
			daveDecryptorDestroy(d);
		decryptors_.clear();
		if (encryptor_)
			daveEncryptorDestroy(encryptor_);
		encryptor_ = nullptr;
		if (session_)
			daveSessionDestroy(session_);
		session_ = nullptr;
		if (enc_)
			opus_encoder_destroy(enc_);
		enc_ = nullptr;
	}

	std::set<Snowflake> speaking()
	{
		std::set<Snowflake> s;
		int64_t now = mono_ms();
		std::lock_guard<std::mutex> lk(peers_mu_);
		for (auto &[ssrc, p] : peers_)
			if (p->uid && now - p->last_voice < 250)
				s.insert(p->uid);
		if (transmitting_)
			s.insert(p_.user_id);
		return s;
	}

	Snowflake channel() const { return p_.channel_id; }
	void set_channel(Snowflake c) { p_.channel_id = c; }

private:
	Params p_;
	WebSocket ws_;
	std::atomic<bool> stopping_{false};
	int64_t hb_interval_ = 0, next_hb_ = 0;
	int64_t seq_ack_ = -1;
	bool resumed_once_ = false;
	uint32_t ssrc_ = 0;
	std::string ip_;
	int port_ = 0;
	int udp_ = -1;
	Transport tr_;
	bool have_key_ = false;
	std::thread recv_t_, send_t_, tone_t_;
	uint64_t rx_frames_ = 0, tx_frames_ = 0;
	double rx_energy_ = 0;
	int64_t rx_log_ = 0;

	// DAVE
	std::mutex dave_mu_;
	DAVESessionHandle session_ = nullptr;
	DAVEEncryptorHandle encryptor_ = nullptr;
	std::map<Snowflake, DAVEDecryptorHandle> decryptors_;
	std::map<uint16_t, uint16_t> prepared_;
	uint16_t last_prepared_version_ = 0;
	uint16_t dave_version_ = 0;
	bool dave_disabled_ = true;

	// peers
	std::mutex peers_mu_;
	std::map<uint32_t, std::unique_ptr<Peer>> peers_;
	std::map<uint32_t, Snowflake> ssrc_users_;

	// capture -> send
	std::mutex cap_mu_;
	std::condition_variable cv_;
	Ring cap_{kRate};
	OpusEncoder *enc_ = nullptr;
	std::atomic<bool> transmitting_{false};
	uint16_t rtp_seq_ = 0;
	uint32_t rtp_ts_ = 0;

	// --- websocket ------------------------------------------------------------------

	void connect_ws(const std::string &url)
	{
		WebSocket::Callbacks cb;
		cb.on_message = [this](std::string &d, bool bin) { on_message(d, bin); };
		cb.on_tick = [this]() { tick(); };
		cb.on_close = [this](int code, const std::string &r) { on_close(code, r); };
		ws_.connect(url, {"User-Agent: Mozilla/5.0", "Origin: https://discord.com"}, cb, 500);
		url_ = url;
	}
	std::string url_;

	void send_json(int op, const json &d)
	{
		ws_.send_text(json{{"op", op}, {"d", d}}.dump());
	}

	void send_binary(uint8_t op, const uint8_t *data, size_t len)
	{
		std::string s;
		s.push_back((char)op);
		s.append((const char *)data, len);
		ws_.send_binary(std::move(s));
	}

	void tick()
	{
		if (!hb_interval_ || mono_ms() < next_hb_)
			return;
		next_hb_ = mono_ms() + hb_interval_;
		send_json(3, {{"t", (int64_t)mono_ms()}, {"seq_ack", seq_ack_}});
	}

	void on_close(int code, const std::string &reason)
	{
		LOG("voice ws closed: %d %s", code, reason.c_str());
		if (stopping_)
			return;
		// 4006 session invalid, 4014 kicked/channel deleted, 4004 auth failed: stop.
		if (code == 4006 || code == 4014 || code == 4004 || code == 4011 || code == 4016 || resumed_once_) {
			set_status([code](VoiceStatus &s) {
				if (code == 4014) {
					s = VoiceStatus();
				} else {
					s.state = VoiceStatus::Failed;
					s.error = "voice server closed the connection (" + std::to_string(code) + ")";
				}
			});
			return;
		}
		// Try one resume (server restarts, network blips).
		resumed_once_ = true;
		std::string url = url_;
		post_main([this, url]() {
			g_timeout_add(500, [](gpointer p) -> gboolean {
				auto *pair = static_cast<std::pair<Conn *, std::string> *>(p);
				if (g_conn.get() == pair->first && !pair->first->stopping_) {
					pair->first->ws_.join();
					pair->first->resuming_ = true;
					pair->first->connect_ws(pair->second);
				}
				delete pair;
				return G_SOURCE_REMOVE;
			}, new std::pair<Conn *, std::string>(this, url));
		});
	}
	bool resuming_ = false;

	void on_message(std::string &data, bool binary)
	{
		if (binary) {
			if (data.size() < 3)
				return;
			const uint8_t *b = (const uint8_t *)data.data();
			seq_ack_ = (b[0] << 8) | b[1];
			on_binary(b[2], b + 3, data.size() - 3);
			return;
		}
		json m = json::parse(data, nullptr, false);
		if (m.is_discarded())
			return;
		if (m.contains("seq") && m["seq"].is_number())
			seq_ack_ = m["seq"].get<int64_t>();
		int op = (int)jint(m, "op", -1);
		const json &d = jget(m, "d");
		switch (op) {
		case 8:  // hello
			hb_interval_ = (int64_t)(jget(d, "heartbeat_interval").is_number() ? jget(d, "heartbeat_interval").get<double>() : 13750);
			next_hb_ = mono_ms() + hb_interval_ / 2;
			if (resuming_) {
				send_json(7, {{"server_id", sf_str(p_.guild_id ? p_.guild_id : p_.channel_id)},
					      {"session_id", p_.session_id}, {"token", p_.token}, {"seq_ack", seq_ack_}});
			} else {
				send_json(0, {{"server_id", sf_str(p_.guild_id ? p_.guild_id : p_.channel_id)},
					      {"user_id", sf_str(p_.user_id)}, {"session_id", p_.session_id},
					      {"token", p_.token}, {"video", false},
					      {"max_dave_protocol_version", (int)daveMaxSupportedProtocolVersion()}});
			}
			break;
		case 2:  // ready
			on_ready(d);
			break;
		case 4:  // session description
			on_session(d);
			break;
		case 5: {  // speaking
			uint32_t ssrc = (uint32_t)jint(d, "ssrc");
			Snowflake uid = jsf(d, "user_id");
			std::lock_guard<std::mutex> lk(peers_mu_);
			ssrc_users_[ssrc] = uid;
			if (auto it = peers_.find(ssrc); it != peers_.end())
				it->second->uid = uid;
			break;
		}
		case 6: {  // heartbeat ack
			int64_t t = jint(d, "t", 0);
			int ping = (int)(mono_ms() - t);
			if (t > 0 && ping >= 0 && ping < 60000)
				set_status([ping](VoiceStatus &s) { s.ping_ms = ping; });
			break;
		}
		case 9:
			LOG("voice: resumed");
			break;
		case 11: {  // clients connect
			std::lock_guard<std::mutex> lk(dave_mu_);
			for (auto &u : jget(d, "user_ids")) {
				Snowflake uid = sf_parse(u);
				if (!decryptors_.count(uid)) {
					decryptors_[uid] = daveDecryptorCreate();
					setup_ratchet(uid, last_prepared_version_);
				}
			}
			break;
		}
		case 13: {  // client disconnect
			Snowflake uid = jsf(d, "user_id");
			std::lock_guard<std::mutex> lk(dave_mu_);
			if (auto it = decryptors_.find(uid); it != decryptors_.end()) {
				daveDecryptorDestroy(it->second);
				decryptors_.erase(it);
			}
			break;
		}
		case 21: {  // prepare transition
			uint16_t tid = (uint16_t)jint(d, "transition_id"), ver = (uint16_t)jint(d, "protocol_version");
			std::lock_guard<std::mutex> lk(dave_mu_);
			prepare_transition(tid, ver);
			if (tid != 0)
				send_json(23, {{"transition_id", tid}});
			break;
		}
		case 22: {  // execute transition
			std::lock_guard<std::mutex> lk(dave_mu_);
			execute_transition((uint16_t)jint(d, "transition_id"));
			break;
		}
		case 24: {  // prepare epoch
			int epoch = (int)jint(d, "epoch");
			uint16_t ver = (uint16_t)jint(d, "protocol_version");
			std::lock_guard<std::mutex> lk(dave_mu_);
			if (epoch == 1) {
				daveSessionInit(session_, ver, p_.channel_id, sf_str(p_.user_id).c_str());
				send_key_package();
			}
			break;
		}
		default:
			break;
		}
	}

	void on_binary(uint8_t op, const uint8_t *b, size_t n)
	{
		std::lock_guard<std::mutex> lk(dave_mu_);
		if (!session_)
			return;
		switch (op) {
		case 25:  // external sender
			daveSessionSetExternalSender(session_, b, n);
			break;
		case 27: {  // proposals
			std::vector<std::string> ids = recognized();
			std::vector<const char *> cids;
			for (auto &s : ids)
				cids.push_back(s.c_str());
			uint8_t *out = nullptr;
			size_t outlen = 0;
			daveSessionProcessProposals(session_, b, n, cids.data(), cids.size(), &out, &outlen);
			if (out && outlen)
				send_binary(28, out, outlen);
			if (out)
				daveFree(out);
			break;
		}
		case 29: {  // announce commit transition
			if (n < 2)
				return;
			uint16_t tid = (b[0] << 8) | b[1];
			DAVECommitResultHandle r = daveSessionProcessCommit(session_, b + 2, n - 2);
			if (!r)
				return;
			if (daveCommitResultIsIgnored(r)) {
				daveCommitResultDestroy(r);
				return;
			}
			if (daveCommitResultIsFailed(r)) {
				daveCommitResultDestroy(r);
				LOG("dave: commit failed, resetting");
				send_json(31, {{"transition_id", tid}});
				protocol_init(daveSessionGetProtocolVersion(session_));
				return;
			}
			daveCommitResultDestroy(r);
			prepare_transition(tid, daveSessionGetProtocolVersion(session_));
			if (tid != 0)
				send_json(23, {{"transition_id", tid}});
			break;
		}
		case 30: {  // welcome
			if (n < 2)
				return;
			uint16_t tid = (b[0] << 8) | b[1];
			std::vector<std::string> ids = recognized();
			std::vector<const char *> cids;
			for (auto &s : ids)
				cids.push_back(s.c_str());
			DAVEWelcomeResultHandle r = daveSessionProcessWelcome(session_, b + 2, n - 2, cids.data(), cids.size());
			if (!r) {
				LOG("dave: welcome failed");
				send_json(31, {{"transition_id", tid}});
				send_key_package();
				return;
			}
			daveWelcomeResultDestroy(r);
			prepare_transition(tid, daveSessionGetProtocolVersion(session_));
			if (tid != 0)
				send_json(23, {{"transition_id", tid}});
			break;
		}
		default:
			break;
		}
	}

	// --- DAVE state machine (mirrors libdave's reference integration) -------------------
	// All of these run with dave_mu_ held.

	std::vector<std::string> recognized()
	{
		std::vector<std::string> v = {sf_str(p_.user_id)};
		for (auto &[u, d] : decryptors_)
			v.push_back(sf_str(u));
		return v;
	}

	void send_key_package()
	{
		uint8_t *kp = nullptr;
		size_t len = 0;
		daveSessionGetMarshalledKeyPackage(session_, &kp, &len);
		if (kp && len)
			send_binary(26, kp, len);
		if (kp)
			daveFree(kp);
	}

	void setup_ratchet(Snowflake uid, uint16_t version)
	{
		bool disabled = version == 0;
		if (uid == p_.user_id) {
			dave_disabled_ = disabled;
			daveEncryptorSetPassthroughMode(encryptor_, disabled);
			if (!disabled) {
				DAVEKeyRatchetHandle kr = daveSessionGetKeyRatchet(session_, sf_str(uid).c_str());
				daveEncryptorSetKeyRatchet(encryptor_, kr);
				if (kr)
					daveKeyRatchetDestroy(kr);
			}
			bool ready = disabled || daveEncryptorHasKeyRatchet(encryptor_);
			uint16_t v = version;
			set_status([ready, disabled, v](VoiceStatus &s) {
				s.e2ee_ready = ready && !disabled;
				s.dave_version = v;
			});
			return;
		}
		auto it = decryptors_.find(uid);
		if (it == decryptors_.end())
			return;
		daveDecryptorTransitionToPassthroughMode(it->second, disabled);
		if (!disabled) {
			DAVEKeyRatchetHandle kr = daveSessionGetKeyRatchet(session_, sf_str(uid).c_str());
			daveDecryptorTransitionToKeyRatchet(it->second, kr);
			if (kr)
				daveKeyRatchetDestroy(kr);
		}
	}

	void prepare_transition(uint16_t tid, uint16_t version)
	{
		for (auto &[u, d] : decryptors_)
			setup_ratchet(u, version);
		if (tid == 0)
			setup_ratchet(p_.user_id, version);
		else
			prepared_[tid] = version;
		last_prepared_version_ = version;
	}

	void execute_transition(uint16_t tid)
	{
		auto it = prepared_.find(tid);
		if (it == prepared_.end())
			return;
		uint16_t version = it->second;
		prepared_.erase(it);
		if (version == 0)
			daveSessionReset(session_);
		setup_ratchet(p_.user_id, version);
	}

	void protocol_init(uint16_t version)
	{
		dave_version_ = version;
		if (version > 0) {
			// E2EE is required: hold our audio until the group gives us a key.
			dave_disabled_ = false;
			daveSessionInit(session_, version, p_.channel_id, sf_str(p_.user_id).c_str());
			send_key_package();
		} else {
			prepare_transition(0, 0);
			execute_transition(0);
		}
	}

	// --- UDP setup ---------------------------------------------------------------------------

	void on_ready(const json &d)
	{
		ssrc_ = (uint32_t)jint(d, "ssrc");
		ip_ = jstr(d, "ip");
		port_ = (int)jint(d, "port");
		std::string mode = "aead_aes256_gcm_rtpsize";
		bool has_aes = false, has_x = false;
		for (auto &m : jget(d, "modes")) {
			if (m == "aead_aes256_gcm_rtpsize")
				has_aes = true;
			if (m == "aead_xchacha20_poly1305_rtpsize")
				has_x = true;
		}
		if (!has_aes && has_x) {
			mode = "aead_xchacha20_poly1305_rtpsize";
			tr_.mode = Transport::XCHACHA;
		}
		if (resuming_)
			return;  // UDP keeps running across a resume
		LOG("voice: ready ssrc %u %s:%d mode %s", ssrc_, ip_.c_str(), port_, mode.c_str());
		addrinfo hints = {}, *res = nullptr;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		if (getaddrinfo(ip_.c_str(), std::to_string(port_).c_str(), &hints, &res) != 0 || !res) {
			fail("could not resolve voice server");
			return;
		}
		udp_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		connect(udp_, res->ai_addr, res->ai_addrlen);
		freeaddrinfo(res);
		// IP discovery
		uint8_t pkt[74] = {0};
		pkt[1] = 1;
		pkt[3] = 70;
		pkt[4] = ssrc_ >> 24;
		pkt[5] = ssrc_ >> 16;
		pkt[6] = ssrc_ >> 8;
		pkt[7] = ssrc_;
		std::string my_ip;
		int my_port = 0;
		for (int attempt = 0; attempt < 5 && my_ip.empty() && !stopping_; attempt++) {
			send(udp_, pkt, sizeof pkt, 0);
			pollfd pf = {udp_, POLLIN, 0};
			if (poll(&pf, 1, 1000) <= 0)
				continue;
			uint8_t r[128];
			ssize_t n = recv(udp_, r, sizeof r, 0);
			if (n >= 74 && r[1] == 2) {
				r[71] = 0;
				my_ip = (const char *)r + 8;
				my_port = (r[72] << 8) | r[73];
			}
		}
		if (my_ip.empty()) {
			fail("UDP IP discovery failed (firewall?)");
			return;
		}
		LOG("voice: external address %s:%d", my_ip.c_str(), my_port);
		send_json(1, {{"protocol", "udp"},
			      {"data", {{"address", my_ip}, {"port", my_port}, {"mode", mode}}},
			      {"codecs", {{{"name", "opus"}, {"type", "audio"}, {"priority", 1000}, {"payload_type", 120}}}}});
	}

	void on_session(const json &d)
	{
		const json &k = jget(d, "secret_key");
		if (!k.is_array() || k.size() != 32) {
			fail("bad session key");
			return;
		}
		for (int i = 0; i < 32; i++)
			tr_.key[i] = (uint8_t)k[i].get<int>();
		have_key_ = true;
		uint16_t dv = (uint16_t)jint(d, "dave_protocol_version", 0);
		LOG("voice: session ready (mode %s, dave v%u)", jstr(d, "mode").c_str(), dv);
		if (resuming_)
			return;
		{
			std::lock_guard<std::mutex> lk(dave_mu_);
			session_ = daveSessionCreate(nullptr, "", [](const char *src, const char *why, void *) {
				LOG("dave failure: %s: %s", src, why);
			}, nullptr);
			encryptor_ = daveEncryptorCreate();
			daveEncryptorSetPassthroughMode(encryptor_, true);
			daveEncryptorAssignSsrcToCodec(encryptor_, ssrc_, DAVE_CODEC_OPUS);
			protocol_init(dv);
		}
		int err;
		// The mic is mono; a mono stream costs half the encoder time and every
		// Opus decoder plays it on both channels.
		enc_ = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
		if (enc_) {
			const Channel *c = g_store.channel(p_.channel_id);
			int br = c && c->bitrate ? std::min(c->bitrate, 128000) : 64000;
			opus_encoder_ctl(enc_, OPUS_SET_BITRATE(br));
			opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(1));
			opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(5));
			opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
			opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(8));
		}
		recv_t_ = std::thread(&Conn::recv_loop, this);
		send_t_ = std::thread(&Conn::send_loop, this);
		std::string in = g_settings.input_device, out = g_settings.output_device;
		bool tone = getenv("DCORD_TEST_TONE") != nullptr;
		if (tone) {
			// Test mode: a 440 Hz tone instead of the microphone, at real-time pace.
			tone_t_ = std::thread([this]() {
				float buf[kFrame];
				double ph = 0;
				int64_t next = mono_ms();
				while (!stopping_) {
					for (int i = 0; i < kFrame; i++) {
						buf[i] = 0.3f * (float)sin(ph);
						ph += 2 * M_PI * 440 / kRate;
					}
					on_capture(buf, kFrame);
					next += 20;
					int64_t d = next - mono_ms();
					if (d > 0)
						std::this_thread::sleep_for(std::chrono::milliseconds(d));
				}
			});
		}
		if (getenv("DCORD_NO_AUDIO")) {
			set_status([](VoiceStatus &s) { s.state = VoiceStatus::Connected; });
			return;
		}
		post_main([this, in, out, tone]() {
			bool ok = audio_start([this](const float *s, int n) { on_capture(s, n); },
					      [this](float *o, int f) { mix(o, f); }, in, out, !tone);
			if (!ok)
				LOG("voice: audio failed to start");
			audio_set_capture_active(!(g_mute || g_deaf));
		});
		set_status([](VoiceStatus &s) { s.state = VoiceStatus::Connected; });
	}

	void fail(const std::string &why)
	{
		LOG("voice: %s", why.c_str());
		set_status([why](VoiceStatus &s) {
			s.state = VoiceStatus::Failed;
			s.error = why;
		});
	}

	// --- receive -----------------------------------------------------------------------------

	void recv_loop()
	{
		std::vector<uint8_t> plain, frame;
		uint8_t buf[2048];
		float pcm[kFrame * 2 * 6];
		while (!stopping_) {
			pollfd pf = {udp_, POLLIN, 0};
			if (poll(&pf, 1, 250) <= 0)
				continue;
			ssize_t n = recv(udp_, buf, sizeof buf, 0);
			if (n < 12 || !have_key_)
				continue;
			if ((buf[0] >> 6) != 2 || (buf[1] & 0x7f) != 120)
				continue;  // not RTP/opus (RTCP etc.)
			int cc = buf[0] & 0x0f;
			bool ext = buf[0] & 0x10;
			bool padding = buf[0] & 0x20;
			size_t hl = 12 + 4 * cc;
			size_t extlen = 0;
			if (ext) {
				if ((size_t)n < hl + 4)
					continue;
				extlen = ((buf[hl + 2] << 8) | buf[hl + 3]) * 4;
				hl += 4;
			}
			if (!tr_.open(buf, n, hl, plain))
				continue;
			if (extlen > plain.size())
				continue;
			size_t off = extlen, len = plain.size() - extlen;
			if (padding && len > 0) {
				size_t pad = plain.back();
				if (pad > len)
					continue;
				len -= pad;
			}
			if (len == 0)
				continue;
			uint16_t seq = (buf[2] << 8) | buf[3];
			uint32_t ssrc = (uint32_t)buf[8] << 24 | buf[9] << 16 | buf[10] << 8 | buf[11];
			Snowflake uid = 0;
			{
				std::lock_guard<std::mutex> lk(peers_mu_);
				auto it = ssrc_users_.find(ssrc);
				if (it != ssrc_users_.end())
					uid = it->second;
			}
			// DAVE
			const uint8_t *opus = plain.data() + off;
			size_t opus_len = len;
			{
				std::lock_guard<std::mutex> lk(dave_mu_);
				auto d = decryptors_.find(uid);
				if (d != decryptors_.end()) {
					size_t cap = daveDecryptorGetMaxPlaintextByteSize(d->second, DAVE_MEDIA_TYPE_AUDIO, len);
					frame.resize(cap);
					size_t wrote = 0;
					auto rc = daveDecryptorDecrypt(d->second, DAVE_MEDIA_TYPE_AUDIO, opus, len, frame.data(), cap, &wrote);
					if (rc != DAVE_DECRYPTOR_RESULT_CODE_SUCCESS)
						continue;
					opus = frame.data();
					opus_len = wrote;
				} else if (!dave_disabled_) {
					continue;  // E2EE active but we don't know this sender yet
				}
			}
			if (g_deaf)
				continue;
			bool silent = opus_len == 3 && memcmp(opus, kSilence, 3) == 0;
			std::lock_guard<std::mutex> lk(peers_mu_);
			auto &pp = peers_[ssrc];
			if (!pp) {
				pp = std::make_unique<Peer>();
				int err;
				pp->dec = opus_decoder_create(kRate, 2, &err);
				pp->uid = uid;
			}
			Peer &p = *pp;
			if (!p.dec)
				continue;
			if (p.have_seq) {
				int16_t gap = (int16_t)(seq - p.last_seq);
				if (gap <= 0)
					continue;  // late or duplicate
				for (int i = 1; i < gap && i <= 3; i++) {
					int s = opus_decode_float(p.dec, nullptr, 0, pcm, kFrame, 0);  // concealment
					if (s > 0)
						p.ring.push(pcm, s * 2);
				}
			}
			p.have_seq = true;
			p.last_seq = seq;
			int s = opus_decode_float(p.dec, opus, (int)opus_len, pcm, kFrame * 6, 0);
			if (s > 0) {
				p.ring.push(pcm, s * 2);
				rx_frames_++;
				for (int i = 0; i < s * 2; i++)
					rx_energy_ += pcm[i] * pcm[i];
				if (mono_ms() - rx_log_ > 2000) {
					LOG("voice: rx %llu frames, last ssrc %u (user %llu), level %.1f dB", (unsigned long long)rx_frames_,
					    ssrc, (unsigned long long)p.uid, 10 * log10(rx_energy_ / (s * 2.0 * std::max<uint64_t>(1, rx_frames_)) + 1e-12));
					rx_log_ = mono_ms();
				}
			}
			// Keep latency bounded: drop if more than 200 ms is queued.
			if (p.ring.size() > (size_t)kRate * 2 / 5)
				p.ring.drop(p.ring.size() - (size_t)kRate * 2 / 10);
			if (!silent)
				p.last_voice = mono_ms();
		}
	}

	void mix(float *out, int frames)
	{
		size_t n = (size_t)frames * 2;
		memset(out, 0, n * sizeof(float));
		if (g_deaf)
			return;
		std::vector<float> tmp(n);
		double master = g_settings.output_volume;
		std::lock_guard<std::mutex> lk(peers_mu_);
		for (auto &[ssrc, pp] : peers_) {
			Peer &p = *pp;
			if (p.buffering) {
				if (p.ring.size() < (size_t)kFrame * 2 * 2)
					continue;
				p.buffering = false;
			}
			size_t got = p.ring.pop(tmp.data(), n);
			if (got < n)
				p.buffering = true;
			double v = master;
			{
				std::lock_guard<std::mutex> vl(g_vol_mu);
				auto it = g_user_vol.find(p.uid);
				if (it != g_user_vol.end())
					v *= it->second;
			}
			for (size_t i = 0; i < got; i++)
				out[i] += (float)(tmp[i] * v);
		}
		for (size_t i = 0; i < n; i++)
			out[i] = std::clamp(out[i], -1.0f, 1.0f);
	}

	// --- send --------------------------------------------------------------------------------

	void on_capture(const float *s, int n)
	{
		{
			std::lock_guard<std::mutex> lk(cap_mu_);
			cap_.push(s, n);
		}
		cv_.notify_one();
	}

	void send_speaking(bool on)
	{
		send_json(5, {{"speaking", on ? 1 : 0}, {"delay", 0}, {"ssrc", ssrc_}});
	}

	void send_opus(const uint8_t *data, size_t len)
	{
		uint8_t hdr[12];
		hdr[0] = 0x80;
		hdr[1] = 0x78;
		hdr[2] = rtp_seq_ >> 8;
		hdr[3] = rtp_seq_;
		hdr[4] = rtp_ts_ >> 24;
		hdr[5] = rtp_ts_ >> 16;
		hdr[6] = rtp_ts_ >> 8;
		hdr[7] = rtp_ts_;
		hdr[8] = ssrc_ >> 24;
		hdr[9] = ssrc_ >> 16;
		hdr[10] = ssrc_ >> 8;
		hdr[11] = ssrc_;
		rtp_seq_++;
		rtp_ts_ += kFrame;
		std::vector<uint8_t> enc;
		const uint8_t *payload = data;
		size_t plen = len;
		{
			std::lock_guard<std::mutex> lk(dave_mu_);
			if (!encryptor_)
				return;
			size_t cap = daveEncryptorGetMaxCiphertextByteSize(encryptor_, DAVE_MEDIA_TYPE_AUDIO, len);
			enc.resize(cap);
			size_t wrote = 0;
			auto rc = daveEncryptorEncrypt(encryptor_, DAVE_MEDIA_TYPE_AUDIO, ssrc_, data, len, enc.data(), cap, &wrote);
			if (rc != DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS)
				return;
			payload = enc.data();
			plen = wrote;
		}
		std::vector<uint8_t> pkt;
		if (tr_.seal(hdr, sizeof hdr, payload, plen, pkt)) {
			send(udp_, pkt.data(), pkt.size(), MSG_NOSIGNAL);
			tx_frames_++;
		}
	}

	bool e2ee_ok()
	{
		std::lock_guard<std::mutex> lk(dave_mu_);
		if (!encryptor_)
			return false;
		return daveEncryptorIsPassthroughMode(encryptor_) ? dave_disabled_ : daveEncryptorHasKeyRatchet(encryptor_);
	}

	void send_loop()
	{
		float mono[kFrame];
		uint8_t out[1500];
		int hang = 0;
		int64_t last_sent = mono_ms();
		uint64_t keepalive = 0;
		while (!stopping_) {
			{
				std::unique_lock<std::mutex> lk(cap_mu_);
				cv_.wait_for(lk, std::chrono::milliseconds(100), [&] { return stopping_ || cap_.size() >= (size_t)kFrame; });
				if (stopping_)
					break;
				if (cap_.size() < (size_t)kFrame) {
					lk.unlock();
					idle(last_sent, keepalive);
					if (g_mute || g_deaf)
						g_level_db = -100;
					continue;
				}
				cap_.pop(mono, kFrame);
			}
			double gain = g_settings.input_gain;
			double sum = 0;
			for (int i = 0; i < kFrame; i++) {
				float v = std::clamp((float)(mono[i] * gain), -1.0f, 1.0f);
				mono[i] = v;
				sum += v * v;
			}
			float db = sum > 0 ? (float)(10 * log10(sum / kFrame)) : -100.0f;
			g_level_db = db;
			bool voice;
			if (g_settings.ptt) {
				voice = g_ptt_down;
			} else {
				if (db >= g_settings.vad_threshold_db || !g_settings.noise_gate)
					hang = 15;  // 300 ms hangover
				else if (hang > 0)
					hang--;
				voice = hang > 0;
			}
			bool tx = voice && !g_mute && !g_deaf && e2ee_ok();
			if (tx) {
				if (!transmitting_) {
					send_speaking(true);
					transmitting_ = true;
					post_main([] { if (g_voice_speaking) g_voice_speaking(); });
				}
				int bytes = opus_encode_float(enc_, mono, kFrame, out, sizeof out);
				if (bytes > 0)
					send_opus(out, bytes);
				last_sent = mono_ms();
			} else if (transmitting_) {
				for (int i = 0; i < 5; i++)
					send_opus(kSilence, 3);
				send_speaking(false);
				transmitting_ = false;
				last_sent = mono_ms();
				post_main([] { if (g_voice_speaking) g_voice_speaking(); });
			} else {
				idle(last_sent, keepalive);
			}
		}
	}

	// NAT keepalive while silent, so incoming audio keeps flowing.
	void idle(int64_t &last_sent, uint64_t &counter)
	{
		if (transmitting_ && (g_mute || g_deaf)) {
			for (int i = 0; i < 5; i++)
				send_opus(kSilence, 3);
			send_speaking(false);
			transmitting_ = false;
			post_main([] { if (g_voice_speaking) g_voice_speaking(); });
		}
		if (udp_ < 0 || mono_ms() - last_sent < 5000)
			return;
		uint8_t ka[8];
		uint64_t c = counter++;
		for (int i = 0; i < 8; i++)
			ka[i] = (uint8_t)(c >> (8 * i));
		send(udp_, ka, sizeof ka, MSG_NOSIGNAL);
		last_sent = mono_ms();
	}
};

// --- session bookkeeping (main thread) --------------------------------------------------------

Snowflake g_want_guild = 0, g_want_channel = 0;
std::string g_vs_session;
json g_server;
guint g_speak_timer = 0;
std::set<Snowflake> g_last_speaking;

// PTT through XInput2 raw key events (works without focus, no grab).
std::thread g_ptt_thread;
std::atomic<bool> g_ptt_stop{false};
int g_ptt_pipe[2] = {-1, -1};

void ptt_stop()
{
	if (!g_ptt_thread.joinable())
		return;
	g_ptt_stop = true;
	char c = 1;
	ssize_t r = write(g_ptt_pipe[1], &c, 1);
	(void)r;
	g_ptt_thread.join();
	close(g_ptt_pipe[0]);
	close(g_ptt_pipe[1]);
	g_ptt_pipe[0] = g_ptt_pipe[1] = -1;
	g_ptt_down = false;
}

void ptt_start()
{
	ptt_stop();
	if (!g_settings.ptt || g_settings.ptt_key.empty())
		return;
	if (pipe2(g_ptt_pipe, O_CLOEXEC) != 0)
		return;
	g_ptt_stop = false;
	std::string key = g_settings.ptt_key;
	g_ptt_thread = std::thread([key]() {
		Display *dpy = XOpenDisplay(nullptr);
		if (!dpy)
			return;
		int xi_op, ev, er;
		if (!XQueryExtension(dpy, "XInputExtension", &xi_op, &ev, &er)) {
			XCloseDisplay(dpy);
			return;
		}
		int maj = 2, min = 0;
		XIQueryVersion(dpy, &maj, &min);
		KeyCode kc = XKeysymToKeycode(dpy, XStringToKeysym(key.c_str()));
		unsigned char mask[XIMaskLen(XI_LASTEVENT)] = {0};
		XISetMask(mask, XI_RawKeyPress);
		XISetMask(mask, XI_RawKeyRelease);
		XIEventMask em = {XIAllMasterDevices, sizeof mask, mask};
		XISelectEvents(dpy, DefaultRootWindow(dpy), &em, 1);
		XFlush(dpy);
		LOG("ptt: watching key %s (keycode %d)", key.c_str(), kc);
		int xfd = ConnectionNumber(dpy);
		while (!g_ptt_stop) {
			while (XPending(dpy)) {
				XEvent e;
				XNextEvent(dpy, &e);
				if (e.xcookie.type == GenericEvent && e.xcookie.extension == xi_op && XGetEventData(dpy, &e.xcookie)) {
					XIRawEvent *re = (XIRawEvent *)e.xcookie.data;
					if (re->detail == kc)
						g_ptt_down = e.xcookie.evtype == XI_RawKeyPress;
					XFreeEventData(dpy, &e.xcookie);
				}
			}
			pollfd pf[2] = {{xfd, POLLIN, 0}, {g_ptt_pipe[0], POLLIN, 0}};
			poll(pf, 2, 500);
		}
		XCloseDisplay(dpy);
	});
}

void send_voice_state(Snowflake gid, Snowflake cid)
{
	json d = {{"guild_id", gid ? json(sf_str(gid)) : json(nullptr)},
		  {"channel_id", cid ? json(sf_str(cid)) : json(nullptr)},
		  {"self_mute", (bool)g_mute}, {"self_deaf", (bool)g_deaf}, {"self_video", false}, {"flags", 2}};
	g_gateway.send(4, d);
}

void teardown()
{
	if (g_conn) {
		g_conn->stop();
		g_conn.reset();
	}
	g_server = nullptr;
	g_vs_session.clear();
	if (g_speak_timer) {
		g_source_remove(g_speak_timer);
		g_speak_timer = 0;
	}
	g_last_speaking.clear();
	ptt_stop();
}

void maybe_connect()
{
	if (g_vs_session.empty() || !g_server.is_object())
		return;
	std::string endpoint = jstr(g_server, "endpoint");
	if (endpoint.empty())
		return;
	if (g_conn) {
		g_conn->stop();
		g_conn.reset();
	}
	Params p;
	p.guild_id = g_want_guild;
	p.channel_id = g_want_channel;
	p.user_id = g_store.me.id;
	p.session_id = g_vs_session;
	p.token = jstr(g_server, "token");
	p.endpoint = endpoint;
	LOG("voice: connecting to %s", endpoint.c_str());
	set_status([](VoiceStatus &s) { s.state = VoiceStatus::Connecting; });
	g_conn = std::make_unique<Conn>(p);
	g_conn->start();
	ptt_start();
	if (!g_speak_timer)
		g_speak_timer = g_timeout_add(100, [](gpointer) -> gboolean {
			if (!g_conn)
				return G_SOURCE_CONTINUE;
			auto s = g_conn->speaking();
			if (s != g_last_speaking) {
				g_last_speaking = s;
				if (g_voice_speaking)
					g_voice_speaking();
			}
			return G_SOURCE_CONTINUE;
		}, nullptr);
}

}  // namespace

void voice_init()
{
	for (auto &[k, v] : g_settings.user_volumes.items())
		if (v.is_number())
			g_user_vol[sf_parse(std::string_view(k))] = v.get<double>();
}

void voice_join(Snowflake gid, Snowflake cid)
{
	if (g_want_channel == cid && g_conn)
		return;
	teardown();
	g_want_guild = gid;
	g_want_channel = cid;
	set_status([gid, cid](VoiceStatus &s) {
		s = VoiceStatus();
		s.state = VoiceStatus::Joining;
		s.guild_id = gid;
		s.channel_id = cid;
	});
	send_voice_state(gid, cid);
}

void voice_leave()
{
	if (!g_want_channel && !g_conn)
		return;
	send_voice_state(g_want_guild, 0);
	g_want_guild = g_want_channel = 0;
	teardown();
	set_status([](VoiceStatus &s) { s = VoiceStatus(); });
}

void voice_set_mute(bool m)
{
	if (g_mute == m)
		return;
	g_mute = m;
	if (!m && g_deaf)
		g_deaf = false;
	audio_set_capture_active(!(g_mute || g_deaf));
	if (g_want_channel)
		send_voice_state(g_want_guild, g_want_channel);
	if (g_voice_changed)
		g_voice_changed();
}

void voice_set_deaf(bool d)
{
	if (g_deaf == d)
		return;
	g_deaf = d;
	audio_set_capture_active(!(g_mute || g_deaf));
	if (g_want_channel)
		send_voice_state(g_want_guild, g_want_channel);
	if (g_voice_changed)
		g_voice_changed();
}

bool voice_self_mute() { return g_mute || g_deaf; }
bool voice_self_deaf() { return g_deaf; }
void voice_ptt(bool p) { g_ptt_down = p; }

void voice_on_state_update(const json &d)
{
	if (jstr(d, "session_id") != g_store.session_id)
		return;  // our account on another device
	Snowflake cid = jsf(d, "channel_id");
	if (!cid) {
		if (g_want_channel) {
			LOG("voice: disconnected by the server");
			g_want_channel = g_want_guild = 0;
			teardown();
			set_status([](VoiceStatus &s) { s = VoiceStatus(); });
		}
		return;
	}
	if (!g_want_channel)
		return;
	if (cid != g_want_channel) {
		// Moved by someone else.
		g_want_channel = cid;
		if (g_conn)
			g_conn->set_channel(cid);
		set_status([cid](VoiceStatus &s) { s.channel_id = cid; });
	}
	g_vs_session = jstr(d, "session_id");
	if (!g_conn)
		maybe_connect();
}

void voice_on_server_update(const json &d)
{
	if (!g_want_channel)
		return;
	g_server = d;
	if (jstr(d, "endpoint").empty())
		return;  // a new server is being allocated
	if (g_conn) {
		// Server migration: reconnect with the new endpoint.
		g_conn->stop();
		g_conn.reset();
	}
	maybe_connect();
}

VoiceStatus voice_status()
{
	std::lock_guard<std::mutex> lk(g_status_mu);
	return g_status;
}

std::set<Snowflake> voice_speaking()
{
	return g_last_speaking;
}

void voice_set_user_volume(Snowflake u, double v)
{
	{
		std::lock_guard<std::mutex> lk(g_vol_mu);
		g_user_vol[u] = v;
	}
	g_settings.user_volumes[sf_str(u)] = v;
	settings_save();
}

double voice_user_volume(Snowflake u)
{
	std::lock_guard<std::mutex> lk(g_vol_mu);
	auto it = g_user_vol.find(u);
	return it == g_user_vol.end() ? 1.0 : it->second;
}

float voice_input_level_db()
{
	return g_level_db;
}

void voice_reload_audio()
{
	if (g_conn)
		ptt_start();
}
