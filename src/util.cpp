#include "util.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <random>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>

static FILE *g_logfile;
static std::mutex g_log_mu;

void log_init()
{
	std::string dir = cache_dir();
	mkdirs(dir);
	std::string path = dir + "/log";
	std::string old = dir + "/log.1";
	rename(path.c_str(), old.c_str());
	g_logfile = fopen(path.c_str(), "w");
	if (g_logfile)
		fcntl(fileno(g_logfile), F_SETFD, FD_CLOEXEC);
}

void logf(const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	localtime_r(&ts.tv_sec, &tm);
	std::lock_guard<std::mutex> lk(g_log_mu);
	char head[32];
	snprintf(head, sizeof head, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
	fprintf(stderr, "%s%s\n", head, buf);
	if (g_logfile) {
		fprintf(g_logfile, "%s%s\n", head, buf);
		fflush(g_logfile);
	}
}

void post_main(std::function<void()> f)
{
	auto *p = new std::function<void()>(std::move(f));
	g_idle_add_full(G_PRIORITY_DEFAULT,
		[](gpointer d) -> gboolean {
			(*static_cast<std::function<void()> *>(d))();
			return G_SOURCE_REMOVE;
		},
		p, [](gpointer d) { delete static_cast<std::function<void()> *>(d); });
}

static std::string env_or(const char *var, const std::string &fallback)
{
	const char *v = getenv(var);
	return (v && *v) ? std::string(v) : fallback;
}

static std::string home()
{
	return env_or("HOME", "/tmp");
}

std::string config_dir()
{
	return env_or("XDG_CONFIG_HOME", home() + "/.config") + "/dcord";
}

std::string cache_dir()
{
	return env_or("XDG_CACHE_HOME", home() + "/.cache") + "/dcord";
}

std::string read_file(const std::string &path, bool *ok)
{
	std::string out;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) {
		if (ok)
			*ok = false;
		return out;
	}
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		out.append(buf, n);
	fclose(f);
	if (ok)
		*ok = true;
	return out;
}

bool write_file(const std::string &path, std::string_view data, int mode)
{
	std::string tmp = path + ".tmp";
	int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0)
		return false;
	size_t off = 0;
	while (off < data.size()) {
		ssize_t w = write(fd, data.data() + off, data.size() - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(tmp.c_str());
			return false;
		}
		off += w;
	}
	fchmod(fd, mode);
	close(fd);
	return rename(tmp.c_str(), path.c_str()) == 0;
}

void mkdirs(const std::string &path)
{
	std::string cur;
	for (size_t i = 0; i <= path.size(); i++) {
		if (i == path.size() || path[i] == '/') {
			if (!cur.empty())
				mkdir(cur.c_str(), 0755);
		}
		if (i < path.size())
			cur += path[i];
	}
}

static const json kNull = nullptr;

const json &jget(const json &j, const char *key)
{
	if (!j.is_object())
		return kNull;
	auto it = j.find(key);
	return it == j.end() ? kNull : *it;
}

std::string jstr(const json &j, const char *key, const std::string &def)
{
	const json &v = jget(j, key);
	return v.is_string() ? v.get<std::string>() : def;
}

int64_t jint(const json &j, const char *key, int64_t def)
{
	const json &v = jget(j, key);
	if (v.is_number_integer())
		return v.get<int64_t>();
	if (v.is_number())
		return (int64_t)v.get<double>();
	if (v.is_string()) {
		const std::string &s = v.get_ref<const std::string &>();
		char *end;
		long long r = strtoll(s.c_str(), &end, 10);
		return end != s.c_str() ? r : def;
	}
	return def;
}

bool jbool(const json &j, const char *key, bool def)
{
	const json &v = jget(j, key);
	return v.is_boolean() ? v.get<bool>() : def;
}

Snowflake sf_parse(std::string_view s)
{
	Snowflake r = 0;
	for (char c : s) {
		if (c < '0' || c > '9')
			return 0;
		r = r * 10 + (c - '0');
	}
	return r;
}

Snowflake sf_parse(const json &v)
{
	if (v.is_string())
		return sf_parse(std::string_view(v.get_ref<const std::string &>()));
	if (v.is_number_unsigned())
		return v.get<uint64_t>();
	if (v.is_number_integer())
		return (Snowflake)v.get<int64_t>();
	return 0;
}

Snowflake jsf(const json &j, const char *key)
{
	return sf_parse(jget(j, key));
}

std::string sf_str(Snowflake s)
{
	return std::to_string(s);
}

static const int64_t kDiscordEpoch = 1420070400000LL;

int64_t sf_unix_ms(Snowflake s)
{
	return (int64_t)(s >> 22) + kDiscordEpoch;
}

Snowflake sf_from_unix_ms(int64_t ms)
{
	return (Snowflake)(ms - kDiscordEpoch) << 22;
}

int64_t iso8601_ms(const std::string &s)
{
	// 2024-01-02T03:04:05.123456+00:00
	struct tm tm = {};
	int ms = 0;
	if (s.size() < 19)
		return 0;
	if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
		   &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
		return 0;
	size_t p = 19;
	if (p < s.size() && s[p] == '.') {
		int digits = 0;
		p++;
		while (p < s.size() && isdigit((unsigned char)s[p])) {
			if (digits < 3) {
				ms = ms * 10 + (s[p] - '0');
				digits++;
			}
			p++;
		}
		while (digits++ < 3)
			ms *= 10;
	}
	int off = 0;
	if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
		int hh = 0, mm = 0;
		sscanf(s.c_str() + p + 1, "%d:%d", &hh, &mm);
		off = (hh * 60 + mm) * 60 * (s[p] == '-' ? -1 : 1);
	}
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	time_t t = timegm(&tm) - off;
	return (int64_t)t * 1000 + ms;
}

int64_t now_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

std::string b64encode(std::string_view in, bool url, bool pad)
{
	static const char *std_tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	static const char *url_tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	const char *t = url ? url_tab : std_tab;
	std::string out;
	out.reserve((in.size() + 2) / 3 * 4);
	size_t i = 0;
	for (; i + 2 < in.size(); i += 3) {
		uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
		out += t[v >> 18];
		out += t[(v >> 12) & 63];
		out += t[(v >> 6) & 63];
		out += t[v & 63];
	}
	if (i < in.size()) {
		uint32_t v = (uint8_t)in[i] << 16;
		if (i + 1 < in.size())
			v |= (uint8_t)in[i + 1] << 8;
		out += t[v >> 18];
		out += t[(v >> 12) & 63];
		if (i + 1 < in.size())
			out += t[(v >> 6) & 63];
		else if (pad)
			out += '=';
		if (pad)
			out += '=';
	}
	return out;
}

std::string b64decode(std::string_view in)
{
	std::string out;
	uint32_t acc = 0;
	int bits = 0;
	for (char c : in) {
		int v;
		if (c >= 'A' && c <= 'Z')
			v = c - 'A';
		else if (c >= 'a' && c <= 'z')
			v = c - 'a' + 26;
		else if (c >= '0' && c <= '9')
			v = c - '0' + 52;
		else if (c == '+' || c == '-')
			v = 62;
		else if (c == '/' || c == '_')
			v = 63;
		else
			continue;
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += (char)((acc >> bits) & 0xff);
		}
	}
	return out;
}

std::string uuid4()
{
	static std::mt19937_64 rng(std::random_device{}());
	uint64_t a = rng(), b = rng();
	a = (a & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
	b = (b & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;
	char buf[40];
	snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx",
		 (unsigned)(a >> 32), (unsigned)((a >> 16) & 0xffff), (unsigned)(a & 0xffff),
		 (unsigned)(b >> 48), (unsigned long long)(b & 0xffffffffffffULL));
	return buf;
}

std::string url_escape(std::string_view s)
{
	std::string out;
	for (unsigned char c : s) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out += (char)c;
		else {
			char b[4];
			snprintf(b, sizeof b, "%%%02X", c);
			out += b;
		}
	}
	return out;
}

std::string markup_escape(std::string_view s)
{
	char *e = g_markup_escape_text(s.data(), (gssize)s.size());
	std::string r(e);
	g_free(e);
	return r;
}

std::string human_size(uint64_t b)
{
	char buf[32];
	if (b < 1024)
		snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
	else if (b < 1024 * 1024)
		snprintf(buf, sizeof buf, "%.1f KB", b / 1024.0);
	else
		snprintf(buf, sizeof buf, "%.1f MB", b / 1048576.0);
	return buf;
}

std::string trim(std::string_view s)
{
	size_t a = 0, b = s.size();
	while (a < b && isspace((unsigned char)s[a]))
		a++;
	while (b > a && isspace((unsigned char)s[b - 1]))
		b--;
	return std::string(s.substr(a, b - a));
}

void spawn_detached(const std::vector<std::string> &argv)
{
	if (argv.empty())
		return;
	pid_t pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		// Double fork so the grandchild is reparented and never becomes a zombie.
		if (fork() != 0)
			_exit(0);
		setsid();
		// Don't leak our sockets (PipeWire, X11, gateway) into the child.
		closefrom(3);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
			dup2(nul, 2);
			if (nul > 2)
				close(nul);
		}
		std::vector<char *> av;
		for (auto &a : argv)
			av.push_back(const_cast<char *>(a.c_str()));
		av.push_back(nullptr);
		execvp(av[0], av.data());
		_exit(127);
	}
	int st;
	waitpid(pid, &st, 0);
}

void open_external(const std::string &target)
{
	// $DCORD_BROWSER, then $BROWSER, then xdg-open, then the owner's thorium wrapper.
	const char *b = getenv("DCORD_BROWSER");
	if (!b || !*b)
		b = getenv("BROWSER");
	if (b && *b) {
		spawn_detached({b, target});
		return;
	}
	gchar *xdg = g_find_program_in_path("xdg-open");
	if (xdg) {
		spawn_detached({xdg, target});
		g_free(xdg);
		return;
	}
	gchar *th = g_find_program_in_path("thorium");
	if (th) {
		spawn_detached({th, target});
		g_free(th);
		return;
	}
	LOG("no browser found to open %s", target.c_str());
}

int run_wait(const std::vector<std::string> &argv)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		closefrom(3);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
			if (nul > 2)
				close(nul);
		}
		std::vector<char *> av;
		for (auto &a : argv)
			av.push_back(const_cast<char *>(a.c_str()));
		av.push_back(nullptr);
		execvp(av[0], av.data());
		_exit(127);
	}
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
	}
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
