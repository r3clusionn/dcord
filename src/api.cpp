#include "api.h"

#include <glib.h>

#include "props.h"

static std::string g_token;

void api_set_token(const std::string &t)
{
	g_token = t;
}

const std::string &api_token()
{
	return g_token;
}

std::string api_error_text(const ApiResult &r)
{
	if (!r.error.empty())
		return r.error;
	std::string msg = jstr(r.body, "message");
	// Field errors: {"errors": {"login": {"_errors": [{"message": ...}]}}}
	std::function<void(const json &)> walk = [&](const json &e) {
		if (!e.is_object())
			return;
		if (const json &l = jget(e, "_errors"); l.is_array())
			for (auto &x : l)
				msg += (msg.empty() ? "" : "\n") + jstr(x, "message");
		for (auto &[k, v] : e.items())
			if (k != "_errors")
				walk(v);
	};
	walk(jget(r.body, "errors"));
	if (msg.empty())
		msg = "HTTP " + std::to_string(r.status);
	return msg;
}

static void do_request(HttpRequest req, ApiCallback cb, int attempt)
{
	HttpRequest copy = req;
	http_request(std::move(req), [copy, cb, attempt](HttpResponse &resp) mutable {
		ApiResult r;
		r.status = resp.status;
		if (resp.status == 0)
			r.error = "network error: " + resp.error;
		if (!resp.body.empty())
			r.body = json::parse(resp.body, nullptr, false);
		if (r.body.is_discarded())
			r.body = nullptr;
		if (resp.status == 429 && attempt < 4) {
			double after = 1.0;
			if (const json &ra = jget(r.body, "retry_after"); ra.is_number())
				after = ra.get<double>();
			LOG("rate limited on %s, retrying in %.2fs", copy.url.c_str(), after);
			auto *p = new std::pair<HttpRequest, ApiCallback>(copy, cb);
			struct Ctx { std::pair<HttpRequest, ApiCallback> *p; int attempt; };
			auto *ctx = new Ctx{p, attempt + 1};
			g_timeout_add((guint)(after * 1000) + 50, [](gpointer d) -> gboolean {
				Ctx *c = static_cast<Ctx *>(d);
				do_request(c->p->first, c->p->second, c->attempt);
				delete c->p;
				delete c;
				return G_SOURCE_REMOVE;
			}, ctx);
			return;
		}
		if (!r.ok() && r.error.empty())
			r.error = api_error_text(r);
		if (!r.ok())
			LOG("api %s -> %ld: %s", copy.url.c_str(), r.status, r.error.c_str());
		if (cb)
			cb(r);
	});
}

void api_call(const std::string &method, const std::string &path, const json &body, ApiCallback cb)
{
	HttpRequest req;
	req.method = method;
	req.url = std::string(kApiBase) + path;
	req.headers = api_headers(g_token, !body.is_null());
	if (!body.is_null())
		req.body = body.dump();
	do_request(std::move(req), std::move(cb), 0);
}

void api_fetch_messages(Snowflake channel, Snowflake before, int limit, ApiCallback cb)
{
	std::string path = "/channels/" + sf_str(channel) + "/messages?limit=" + std::to_string(limit);
	if (before)
		path += "&before=" + sf_str(before);
	api_get(path, std::move(cb));
}

void api_send_message(Snowflake channel, const std::string &content, const std::string &nonce,
		      Snowflake reply_to, bool mention_reply, const std::vector<UploadFile> &files,
		      ApiCallback cb, HttpProgress progress)
{
	json body = {{"content", content}, {"nonce", nonce}, {"tts", false}, {"flags", 0}};
	if (reply_to) {
		body["message_reference"] = {{"message_id", sf_str(reply_to)}, {"channel_id", sf_str(channel)}};
		if (!mention_reply)
			body["allowed_mentions"] = {{"parse", {"users", "roles", "everyone"}}, {"replied_user", false}};
	}
	std::string path = "/channels/" + sf_str(channel) + "/messages";
	if (files.empty()) {
		api_call("POST", path, body, std::move(cb));
		return;
	}
	json atts = json::array();
	HttpRequest req;
	req.method = "POST";
	req.url = std::string(kApiBase) + path;
	req.headers = api_headers(g_token, false);
	req.timeout_s = 600;
	req.progress = std::move(progress);
	for (size_t i = 0; i < files.size(); i++) {
		const UploadFile &f = files[i];
		std::string name = f.spoiler && f.filename.rfind("SPOILER_", 0) != 0 ? "SPOILER_" + f.filename : f.filename;
		atts.push_back({{"id", (int)i}, {"filename", name}});
		HttpPart part;
		part.name = "files[" + std::to_string(i) + "]";
		part.filename = name;
		part.content_type = f.content_type.empty() ? "application/octet-stream" : f.content_type;
		part.path = f.path;
		part.data = f.data;
		req.parts.push_back(std::move(part));
	}
	body["attachments"] = atts;
	HttpPart pj;
	pj.name = "payload_json";
	pj.content_type = "application/json";
	pj.data = body.dump();
	req.parts.insert(req.parts.begin(), std::move(pj));
	do_request(std::move(req), std::move(cb), 0);
}

void api_edit_message(Snowflake channel, Snowflake id, const std::string &content, ApiCallback cb)
{
	api_call("PATCH", "/channels/" + sf_str(channel) + "/messages/" + sf_str(id), {{"content", content}}, std::move(cb));
}

void api_delete_message(Snowflake channel, Snowflake id, ApiCallback cb)
{
	api_call("DELETE", "/channels/" + sf_str(channel) + "/messages/" + sf_str(id), nullptr, std::move(cb));
}

void api_ack(Snowflake channel, Snowflake message, int flags, int64_t last_viewed)
{
	json body = {{"token", nullptr}, {"last_viewed", last_viewed}, {"flags", flags}};
	api_call("POST", "/channels/" + sf_str(channel) + "/messages/" + sf_str(message) + "/ack", body, nullptr);
}

void api_typing(Snowflake channel)
{
	HttpRequest req;
	req.method = "POST";
	req.url = std::string(kApiBase) + "/channels/" + sf_str(channel) + "/typing";
	req.headers = api_headers(g_token, false);
	req.headers.push_back("Content-Length: 0");
	do_request(std::move(req), nullptr, 0);
}

void api_react(Snowflake channel, Snowflake message, const std::string &emoji, bool add, ApiCallback cb)
{
	std::string path = "/channels/" + sf_str(channel) + "/messages/" + sf_str(message) + "/reactions/" +
			   url_escape(emoji) + "/@me?location=Message&type=0";
	api_call(add ? "PUT" : "DELETE", path, nullptr, std::move(cb));
}

void api_pins(Snowflake channel, ApiCallback cb)
{
	api_get("/channels/" + sf_str(channel) + "/pins", std::move(cb));
}

void api_gifs(const std::string &query, ApiCallback cb)
{
	if (query.empty())
		api_get("/gifs/trending-gifs?media_format=mp4&provider=tenor&locale=en-US", std::move(cb));
	else
		api_get("/gifs/search?q=" + url_escape(query) + "&media_format=mp4&provider=tenor&locale=en-US&limit=50",
			std::move(cb));
}

void api_open_dm(Snowflake user, ApiCallback cb)
{
	api_call("POST", "/users/@me/channels", {{"recipients", {sf_str(user)}}}, std::move(cb));
}

void api_ring(Snowflake channel)
{
	api_call("POST", "/channels/" + sf_str(channel) + "/call/ring", {{"recipients", nullptr}}, nullptr);
}

void api_login(const std::string &login, const std::string &password, ApiCallback cb)
{
	json body = {{"login", login}, {"password", password}, {"undelete", false},
		     {"login_source", nullptr}, {"gift_code_sku_id", nullptr}};
	HttpRequest req;
	req.method = "POST";
	req.url = std::string(kApiBase) + "/auth/login";
	req.headers = api_headers("", true);
	req.headers.push_back("Referer: https://discord.com/login");
	req.body = body.dump();
	do_request(std::move(req), std::move(cb), 0);
}

void api_login_mfa(const std::string &kind, const std::string &ticket, const std::string &code, ApiCallback cb)
{
	json body = {{"code", code}, {"ticket", ticket}, {"login_source", nullptr}, {"gift_code_sku_id", nullptr}};
	HttpRequest req;
	req.method = "POST";
	req.url = std::string(kApiBase) + "/auth/mfa/" + kind;
	req.headers = api_headers("", true);
	req.headers.push_back("Referer: https://discord.com/login");
	req.body = body.dump();
	do_request(std::move(req), std::move(cb), 0);
}

void api_send_mfa_sms(const std::string &ticket, ApiCallback cb)
{
	HttpRequest req;
	req.method = "POST";
	req.url = std::string(kApiBase) + "/auth/mfa/sms/send";
	req.headers = api_headers("", true);
	req.body = json{{"ticket", ticket}}.dump();
	do_request(std::move(req), std::move(cb), 0);
}
