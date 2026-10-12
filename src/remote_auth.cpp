#include "remote_auth.h"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <time.h>

#include "api.h"
#include "props.h"
#include "util.h"
#include "ws.h"

namespace {
int64_t mono_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
}  // namespace

struct RemoteAuth::Impl {
	RemoteAuth *self;
	EVP_PKEY *key = nullptr;
	std::string spki_der;
	WebSocket ws;
	int64_t hb_interval = 0, next_hb = 0;
	bool done = false;
	std::shared_ptr<bool> alive = std::make_shared<bool>(true);

	~Impl()
	{
		*alive = false;
		ws.close(1000);
		ws.join();
		if (key)
			EVP_PKEY_free(key);
	}

	bool make_key()
	{
		key = EVP_RSA_gen(2048);
		if (!key)
			return false;
		unsigned char *der = nullptr;
		int n = i2d_PUBKEY(key, &der);
		if (n <= 0)
			return false;
		spki_der.assign((const char *)der, n);
		OPENSSL_free(der);
		return true;
	}

	std::string decrypt(const std::string &b64)
	{
		std::string in = b64decode(b64);
		EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(key, nullptr);
		std::string out;
		size_t outlen = 0;
		if (ctx && EVP_PKEY_decrypt_init(ctx) > 0 &&
		    EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) > 0 &&
		    EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) > 0 &&
		    EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) > 0 &&
		    EVP_PKEY_decrypt(ctx, nullptr, &outlen, (const unsigned char *)in.data(), in.size()) > 0) {
			out.resize(outlen);
			if (EVP_PKEY_decrypt(ctx, (unsigned char *)out.data(), &outlen,
					     (const unsigned char *)in.data(), in.size()) > 0)
				out.resize(outlen);
			else
				out.clear();
		}
		EVP_PKEY_CTX_free(ctx);
		return out;
	}

	void fail(const std::string &msg)
	{
		auto a = alive;
		RemoteAuth *s = self;
		post_main([a, s, msg]() {
			if (*a && s->on_error)
				s->on_error(msg);
		});
	}

	void on_message(std::string &text)
	{
		json m = json::parse(text, nullptr, false);
		std::string op = jstr(m, "op");
		auto a = alive;
		RemoteAuth *s = self;
		if (op == "hello") {
			hb_interval = jint(m, "heartbeat_interval", 40000);
			next_hb = mono_ms() + hb_interval;
			ws.send_text(json{{"op", "init"}, {"encoded_public_key", b64encode(spki_der)}}.dump());
		} else if (op == "nonce_proof") {
			std::string nonce = decrypt(jstr(m, "encrypted_nonce"));
			if (nonce.empty()) {
				fail("could not decrypt the login nonce");
				ws.close(1000);
				return;
			}
			ws.send_text(json{{"op", "nonce_proof"}, {"nonce", b64encode(nonce, true, false)}}.dump());
		} else if (op == "pending_remote_init") {
			std::string fp = jstr(m, "fingerprint");
			unsigned char dig[32];
			unsigned int dl = 0;
			EVP_Digest(spki_der.data(), spki_der.size(), dig, &dl, EVP_sha256(), nullptr);
			std::string mine = b64encode(std::string((const char *)dig, dl), true, false);
			if (fp != mine)
				LOG("remote auth: fingerprint mismatch (%s vs %s)", fp.c_str(), mine.c_str());
			std::string url = "https://discord.com/ra/" + fp;
			post_main([a, s, url]() {
				if (*a && s->on_qr)
					s->on_qr(url);
			});
		} else if (op == "pending_ticket") {
			std::string payload = decrypt(jstr(m, "encrypted_user_payload"));
			// id:discriminator:avatar:username
			size_t c1 = payload.find(':');
			size_t c2 = c1 == std::string::npos ? c1 : payload.find(':', c1 + 1);
			size_t c3 = c2 == std::string::npos ? c2 : payload.find(':', c2 + 1);
			std::string name = c3 == std::string::npos ? payload : payload.substr(c3 + 1);
			post_main([a, s, name]() {
				if (*a && s->on_scanned)
					s->on_scanned(name);
			});
		} else if (op == "pending_login") {
			std::string ticket = jstr(m, "ticket");
			done = true;
			Impl *impl = this;
			post_main([a, s, impl, ticket]() {
				if (!*a)
					return;
				api_call("POST", "/users/@me/remote-auth/login", {{"ticket", ticket}}, [a, s, impl](ApiResult &r) {
					if (!*a)
						return;
					if (!r.ok()) {
						if (s->on_error)
							s->on_error("Ticket exchange failed: " + api_error_text(r));
						return;
					}
					std::string tok = impl->decrypt(jstr(r.body, "encrypted_token"));
					if (tok.empty()) {
						if (s->on_error)
							s->on_error("Could not decrypt the token.");
					} else if (s->on_token) {
						s->on_token(tok);
					}
				});
			});
		} else if (op == "cancel") {
			fail("Login was cancelled on the phone.");
		}
	}

	void on_tick()
	{
		if (hb_interval && mono_ms() >= next_hb) {
			ws.send_text(R"({"op":"heartbeat"})");
			next_hb = mono_ms() + hb_interval;
		}
	}
};

RemoteAuth::RemoteAuth() = default;
RemoteAuth::~RemoteAuth() = default;

void RemoteAuth::start()
{
	d = std::make_unique<Impl>();
	d->self = this;
	if (!d->make_key()) {
		if (on_error)
			on_error("Could not create an RSA key.");
		return;
	}
	WebSocket::Callbacks cb;
	Impl *impl = d.get();
	cb.on_message = [impl](std::string &t, bool) { impl->on_message(t); };
	cb.on_tick = [impl]() { impl->on_tick(); };
	cb.on_close = [impl](int code, const std::string &reason) {
		if (impl->done)
			return;
		LOG("remote auth closed: %d %s", code, reason.c_str());
		if (code == 4003)
			impl->fail("The QR code expired.");
		else if (code != 1000)
			impl->fail("Connection to the login server closed (" + std::to_string(code) + " " + reason + ").");
	};
	d->ws.connect("wss://remote-auth-gateway.discord.gg/?v=2",
		      {"Origin: https://discord.com", "User-Agent: " + props_user_agent()}, cb, 500);
}

void RemoteAuth::stop()
{
	d.reset();
}
