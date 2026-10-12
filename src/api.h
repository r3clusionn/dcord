// Discord REST API calls. Callbacks run on the main thread.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "http.h"
#include "util.h"

struct ApiResult {
	long status = 0;
	json body;            // parsed JSON (null if none)
	std::string error;    // human-readable error when !ok()
	bool ok() const { return status >= 200 && status < 300; }
};
using ApiCallback = std::function<void(ApiResult &)>;

void api_set_token(const std::string &token);
const std::string &api_token();

void api_call(const std::string &method, const std::string &path, const json &body, ApiCallback cb);
inline void api_get(const std::string &path, ApiCallback cb) { api_call("GET", path, nullptr, std::move(cb)); }

struct UploadFile {
	std::string path;      // file on disk, or
	std::string data;      // in-memory bytes (pasted images)
	std::string filename;
	std::string content_type;
	bool spoiler = false;
};

void api_fetch_messages(Snowflake channel, Snowflake before, int limit, ApiCallback cb);
void api_send_message(Snowflake channel, const std::string &content, const std::string &nonce,
		      Snowflake reply_to, bool mention_reply, const std::vector<UploadFile> &files,
		      ApiCallback cb, HttpProgress progress = nullptr);
void api_edit_message(Snowflake channel, Snowflake id, const std::string &content, ApiCallback cb);
void api_delete_message(Snowflake channel, Snowflake id, ApiCallback cb);
void api_ack(Snowflake channel, Snowflake message, int flags, int64_t last_viewed);
void api_typing(Snowflake channel);
// emoji: "👍" or "name:id"
void api_react(Snowflake channel, Snowflake message, const std::string &emoji, bool add, ApiCallback cb);
void api_pins(Snowflake channel, ApiCallback cb);
void api_gifs(const std::string &query, ApiCallback cb);  // empty query = trending
void api_open_dm(Snowflake user, ApiCallback cb);
void api_ring(Snowflake channel);

// Unauthenticated login flow.
void api_login(const std::string &login, const std::string &password, ApiCallback cb);
void api_login_mfa(const std::string &kind, const std::string &ticket, const std::string &code, ApiCallback cb);
void api_send_mfa_sms(const std::string &ticket, ApiCallback cb);

std::string api_error_text(const ApiResult &r);
