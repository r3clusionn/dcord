#include "props.h"

#include <ctime>
#include <regex>
#include <unistd.h>

#include "http.h"

// DCORD_API_BASE / DCORD_GATEWAY point the client at a test server (tests/mock_discord.py).
const char *kApiBase = getenv("DCORD_API_BASE") ? getenv("DCORD_API_BASE") : "https://discord.com/api/v9";

namespace {
long g_build = 634304;  // seen 2026-10-12; refreshed at startup
std::string g_ua, g_chrome, g_launch_id, g_heartbeat_sid;

// Chrome ships a major version roughly every four weeks; derive a current-looking
// version so the user agent doesn't freeze at whatever was true at build time.
std::string chrome_major()
{
	const time_t base = 1745884800;  // 2025-04-29, Chrome 136 stable
	long weeks = (long)((time(nullptr) - base) / (7 * 86400));
	long v = 136 + (weeks > 0 ? weeks / 4 : 0);
	return std::to_string(v);
}

std::string timezone_name()
{
	if (const char *tz = getenv("TZ"); tz && *tz)
		return *tz == ':' ? tz + 1 : tz;
	char buf[256];
	ssize_t n = readlink("/etc/localtime", buf, sizeof buf - 1);
	if (n > 0) {
		buf[n] = 0;
		std::string s(buf);
		size_t p = s.find("zoneinfo/");
		if (p != std::string::npos)
			return s.substr(p + 9);
	}
	std::string t = trim(read_file("/etc/timezone"));
	return t.empty() ? "UTC" : t;
}

std::string cache_path()
{
	return config_dir() + "/build_number";
}
}  // namespace

void props_init(std::function<void()> done)
{
	g_chrome = chrome_major();
	g_ua = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" +
	       g_chrome + ".0.0.0 Safari/537.36";
	g_launch_id = uuid4();
	g_heartbeat_sid = uuid4();
	std::string cached = trim(read_file(cache_path()));
	if (!cached.empty())
		g_build = atol(cached.c_str());

	HttpRequest r;
	r.url = getenv("DCORD_APP_URL") ? getenv("DCORD_APP_URL") : "https://discord.com/app";
	r.headers = {"User-Agent: " + g_ua, "Accept: text/html"};
	r.timeout_s = 8;
	http_request(std::move(r), [done](HttpResponse &resp) {
		static const std::regex re("\"BUILD_NUMBER\":\"?(\\d+)");
		std::smatch m;
		if (resp.ok() && std::regex_search(resp.body, m, re)) {
			long b = atol(m[1].str().c_str());
			if (b > 0 && b != g_build) {
				LOG("client build number %ld -> %ld", g_build, b);
				g_build = b;
				mkdirs(config_dir());
				write_file(cache_path(), std::to_string(b) + "\n");
			}
		} else {
			LOG("could not refresh build number (status %ld %s), using %ld", resp.status,
			    resp.error.c_str(), g_build);
		}
		done();
	});
}

json props_object()
{
	return json{
		{"os", "Linux"},
		{"browser", "Chrome"},
		{"device", ""},
		{"system_locale", "en-US"},
		{"has_client_mods", false},
		{"browser_user_agent", g_ua},
		{"browser_version", g_chrome + ".0.0.0"},
		{"os_version", ""},
		{"referrer", ""},
		{"referring_domain", ""},
		{"referrer_current", ""},
		{"referring_domain_current", ""},
		{"release_channel", "stable"},
		{"client_build_number", g_build},
		{"client_event_source", nullptr},
		{"client_launch_id", g_launch_id},
		{"client_heartbeat_session_id", g_heartbeat_sid},
	};
}

const std::string &props_user_agent()
{
	return g_ua;
}

std::string props_super_b64()
{
	return b64encode(props_object().dump());
}

std::vector<std::string> api_headers(const std::string &token, bool json_body)
{
	static std::string tz = timezone_name();
	std::vector<std::string> h = {
		"User-Agent: " + g_ua,
		"X-Super-Properties: " + props_super_b64(),
		"X-Discord-Locale: en-US",
		"X-Discord-Timezone: " + tz,
		"X-Debug-Options: bugReporterEnabled",
		"Origin: https://discord.com",
		"Referer: https://discord.com/channels/@me",
		"Accept: */*",
		"Accept-Language: en-US,en;q=0.9",
	};
	if (!token.empty())
		h.push_back("Authorization: " + token);
	if (json_body)
		h.push_back("Content-Type: application/json");
	return h;
}
