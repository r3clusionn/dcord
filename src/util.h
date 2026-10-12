// Small shared helpers: logging, paths, main-thread dispatch, JSON access,
// snowflakes, base64.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using Snowflake = uint64_t;

void log_init();
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logf(__VA_ARGS__)

// Runs f on the GTK main thread (safe to call from any thread).
void post_main(std::function<void()> f);

std::string config_dir();   // ~/.config/dcord
std::string cache_dir();    // ~/.cache/dcord
std::string read_file(const std::string &path, bool *ok = nullptr);
bool write_file(const std::string &path, std::string_view data, int mode = 0644);
void mkdirs(const std::string &path);

// JSON field access that tolerates missing/null/wrong-typed fields.
std::string jstr(const json &j, const char *key, const std::string &def = "");
int64_t jint(const json &j, const char *key, int64_t def = 0);
bool jbool(const json &j, const char *key, bool def = false);
Snowflake jsf(const json &j, const char *key);   // string or number snowflake
Snowflake sf_parse(const json &v);
Snowflake sf_parse(std::string_view s);
inline Snowflake sf_parse(const std::string &s) { return sf_parse(std::string_view(s)); }
inline Snowflake sf_parse(const char *s) { return sf_parse(std::string_view(s)); }
std::string sf_str(Snowflake s);
const json &jget(const json &j, const char *key);  // null json if absent

// Snowflake <-> time. Discord epoch is 2015-01-01.
int64_t sf_unix_ms(Snowflake s);
Snowflake sf_from_unix_ms(int64_t ms);
int64_t iso8601_ms(const std::string &s);
int64_t now_ms();

std::string b64encode(std::string_view in, bool url = false, bool pad = true);
std::string b64decode(std::string_view in);
std::string uuid4();
std::string url_escape(std::string_view s);
std::string markup_escape(std::string_view s);
std::string human_size(uint64_t bytes);
std::string trim(std::string_view s);

// Opens a URL or file with the user's browser / opener.
void open_external(const std::string &target);
// Starts a detached child process (closes inherited fds, no zombies).
void spawn_detached(const std::vector<std::string> &argv);
// Runs a program and waits for it (fds above 2 are not inherited). Returns exit status.
int run_wait(const std::vector<std::string> &argv);
