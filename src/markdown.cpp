#include "markdown.h"

#include <cstring>
#include <ctime>

namespace {

// Decodes one UTF-8 code point; returns its byte length (1 on bad input).
int utf8_dec(std::string_view s, size_t i, uint32_t *cp)
{
	unsigned char c = (unsigned char)s[i];
	int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
	if (i + n > s.size())
		n = 1;
	uint32_t v = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f) : (c & 0x07);
	for (int k = 1; k < n; k++)
		v = (v << 6) | ((unsigned char)s[i + k] & 0x3f);
	*cp = v;
	return n;
}

bool is_default_emoji_bmp(uint32_t c)
{
	static const uint32_t list[] = {0x231A, 0x231B, 0x23E9, 0x23EA, 0x23EB, 0x23EC, 0x23F0, 0x23F3, 0x25FD, 0x25FE,
		0x2614, 0x2615, 0x267F, 0x2693, 0x26A1, 0x26AA, 0x26AB, 0x26BD, 0x26BE, 0x26C4, 0x26C5, 0x26CE,
		0x26D4, 0x26EA, 0x26F2, 0x26F3, 0x26F5, 0x26FA, 0x26FD, 0x2705, 0x270A, 0x270B, 0x2728, 0x274C,
		0x274E, 0x2753, 0x2754, 0x2755, 0x2757, 0x2795, 0x2796, 0x2797, 0x27B0, 0x27BF, 0x2B1B, 0x2B1C,
		0x2B50, 0x2B55};
	if (c >= 0x2648 && c <= 0x2653)
		return true;
	for (uint32_t x : list)
		if (x == c)
			return true;
	return false;
}

bool is_emoji_capable_bmp(uint32_t c)
{
	return c == 0xA9 || c == 0xAE || c == 0x203C || c == 0x2049 || c == 0x2122 || c == 0x2139 ||
	       (c >= 0x2194 && c <= 0x21AA) || (c >= 0x2300 && c <= 0x23FF) || c == 0x24C2 ||
	       (c >= 0x25AA && c <= 0x25FE) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x2934 && c <= 0x2935) ||
	       (c >= 0x2B05 && c <= 0x2B55) || c == 0x3030 || c == 0x303D || c == 0x3297 || c == 0x3299;
}

bool is_supp_emoji(uint32_t c)
{
	return (c >= 0x1F000 && c <= 0x1FAFF) && !(c >= 0x1F1E6 && c <= 0x1F1FF) && !(c >= 0x1F3FB && c <= 0x1F3FF);
}

}  // namespace

size_t emoji_seq_len(std::string_view s)
{
	if (s.empty())
		return 0;
	uint32_t c;
	size_t i = 0;
	int n = utf8_dec(s, 0, &c);
	// Keycap: [0-9#*] FE0F? 20E3
	if ((c >= '0' && c <= '9') || c == '#' || c == '*') {
		size_t j = 1;
		uint32_t d;
		if (j < s.size()) {
			int m = utf8_dec(s, j, &d);
			if (d == 0xFE0F) {
				j += m;
				if (j < s.size())
					m = utf8_dec(s, j, &d);
			}
			if (d == 0x20E3)
				return j + m;
		}
		return 0;
	}
	// Flags: two regional indicators.
	if (c >= 0x1F1E6 && c <= 0x1F1FF) {
		uint32_t d;
		if ((size_t)n < s.size()) {
			int m = utf8_dec(s, n, &d);
			if (d >= 0x1F1E6 && d <= 0x1F1FF)
				return n + m;
		}
		return 0;
	}
	auto peek = [&](size_t at, uint32_t *out) -> int { return at < s.size() ? utf8_dec(s, at, out) : 0; };
	auto base_ok = [&](uint32_t cp, size_t after) {
		if (is_supp_emoji(cp) || is_default_emoji_bmp(cp))
			return true;
		uint32_t nx = 0;
		return is_emoji_capable_bmp(cp) && peek(after, &nx) && nx == 0xFE0F;
	};
	if (!base_ok(c, n))
		return 0;
	i = n;
	while (true) {
		uint32_t d = 0;
		int m = peek(i, &d);
		if (!m)
			break;
		if (d == 0xFE0F || (d >= 0x1F3FB && d <= 0x1F3FF) || (d >= 0xE0020 && d <= 0xE007F)) {
			i += m;
			continue;
		}
		if (d == 0x200D) {
			uint32_t e = 0;
			int k = peek(i + m, &e);
			if (k && (is_supp_emoji(e) || is_emoji_capable_bmp(e))) {
				i += m + k;
				continue;
			}
		}
		break;
	}
	return i;
}

std::string twemoji_name(std::string_view seq)
{
	bool zwj = seq.find("‍") != std::string_view::npos;
	std::string out;
	for (size_t i = 0; i < seq.size();) {
		uint32_t c;
		i += utf8_dec(seq, i, &c);
		if (c == 0xFE0F && !zwj)
			continue;
		char b[12];
		snprintf(b, sizeof b, "%x", c);
		if (!out.empty())
			out += '-';
		out += b;
	}
	return out;
}

std::string format_timestamp(int64_t t, char style)
{
	time_t tt = (time_t)t;
	struct tm tm;
	localtime_r(&tt, &tm);
	char buf[128];
	switch (style) {
	case 't':
		strftime(buf, sizeof buf, "%H:%M", &tm);
		break;
	case 'T':
		strftime(buf, sizeof buf, "%H:%M:%S", &tm);
		break;
	case 'd':
		strftime(buf, sizeof buf, "%d/%m/%Y", &tm);
		break;
	case 'D':
		strftime(buf, sizeof buf, "%e %B %Y", &tm);
		break;
	case 'F':
		strftime(buf, sizeof buf, "%A, %e %B %Y %H:%M", &tm);
		break;
	case 'R': {
		int64_t d = t - time(nullptr);
		int64_t a = d < 0 ? -d : d;
		const char *unit = "second";
		int64_t v = a;
		if (a >= 86400 * 365) {
			v = a / (86400 * 365);
			unit = "year";
		} else if (a >= 86400 * 30) {
			v = a / (86400 * 30);
			unit = "month";
		} else if (a >= 86400) {
			v = a / 86400;
			unit = "day";
		} else if (a >= 3600) {
			v = a / 3600;
			unit = "hour";
		} else if (a >= 60) {
			v = a / 60;
			unit = "minute";
		}
		snprintf(buf, sizeof buf, d < 0 ? "%lld %s%s ago" : "in %lld %s%s", (long long)v, unit, v == 1 ? "" : "s");
		break;
	}
	default:
		strftime(buf, sizeof buf, "%e %B %Y %H:%M", &tm);
	}
	return trim(buf);
}

namespace {

struct Parser {
	std::vector<MdSpan> out;
	int spoilers = 0;

	void text(std::string_view s, uint32_t style, int spoiler)
	{
		if (s.empty())
			return;
		// Split out unicode emoji and newlines.
		size_t start = 0;
		for (size_t i = 0; i < s.size();) {
			if (s[i] == '\n') {
				push_text(s.substr(start, i - start), style, spoiler);
				MdSpan nl;
				nl.kind = MdSpan::Newline;
				nl.style = style;
				out.push_back(nl);
				i++;
				start = i;
				continue;
			}
			if ((unsigned char)s[i] >= 0x80 || s[i] == '#' || s[i] == '*' || (s[i] >= '0' && s[i] <= '9')) {
				size_t n = (unsigned char)s[i] >= 0x80 || i + 1 < s.size() ? emoji_seq_len(s.substr(i)) : 0;
				if (n) {
					push_text(s.substr(start, i - start), style, spoiler);
					MdSpan e;
					e.kind = MdSpan::UnicodeEmoji;
					e.text = std::string(s.substr(i, n));
					e.style = style;
					e.spoiler = spoiler;
					out.push_back(e);
					i += n;
					start = i;
					continue;
				}
			}
			i++;
		}
		push_text(s.substr(start), style, spoiler);
	}

	void push_text(std::string_view s, uint32_t style, int spoiler, const std::string &url = "")
	{
		if (s.empty())
			return;
		if (!out.empty() && out.back().kind == MdSpan::Text && out.back().style == style &&
		    out.back().url == url && out.back().spoiler == spoiler) {
			out.back().text += s;
			return;
		}
		MdSpan t;
		t.text = std::string(s);
		t.style = style;
		t.url = url;
		t.spoiler = spoiler;
		out.push_back(t);
	}

	static bool is_url_char(char c)
	{
		return !isspace((unsigned char)c) && c != '<' && c != '>' && c != '"';
	}

	// Finds a closing delimiter `d` after position i (non-empty content).
	static size_t find_close(std::string_view s, size_t from, std::string_view d)
	{
		size_t p = from;
		while (true) {
			p = s.find(d, p);
			if (p == std::string_view::npos)
				return p;
			if (p > from && s[p - 1] == '\\') {
				p++;
				continue;
			}
			// "**" inside "***": prefer the last run.
			if (d.size() == 1 && p + 1 < s.size() && s[p + 1] == d[0]) {
				p += 2;
				continue;
			}
			return p;
		}
	}

	void inline_(std::string_view s, uint32_t style, int spoiler)
	{
		size_t plain = 0;
		size_t i = 0;
		auto flush = [&](size_t upto) {
			if (upto > plain)
				text(s.substr(plain, upto - plain), style, spoiler);
		};
		while (i < s.size()) {
			char c = s[i];
			// Escapes
			if (c == '\\' && i + 1 < s.size() && ispunct((unsigned char)s[i + 1])) {
				flush(i);
				text(s.substr(i + 1, 1), style, spoiler);
				i += 2;
				plain = i;
				continue;
			}
			// Inline code
			if (c == '`') {
				size_t ticks = s.compare(i, 2, "``") == 0 ? 2 : 1;
				size_t close = s.find(std::string(ticks, '`'), i + ticks);
				if (close != std::string_view::npos && close > i + ticks) {
					flush(i);
					std::string_view inner = s.substr(i + ticks, close - i - ticks);
					if (ticks == 2 && inner.size() >= 2 && inner.front() == ' ' && inner.back() == ' ')
						inner = inner.substr(1, inner.size() - 2);
					push_text(inner, style | MD_CODE, spoiler);
					i = close + ticks;
					plain = i;
					continue;
				}
			}
			// Angle-bracket tokens: mentions, emoji, timestamps, suppressed links.
			if (c == '<') {
				size_t close = s.find('>', i);
				if (close != std::string_view::npos) {
					std::string_view tok = s.substr(i + 1, close - i - 1);
					MdSpan sp;
					sp.style = style;
					sp.spoiler = spoiler;
					bool matched = true;
					if (tok.rfind("@&", 0) == 0 && sf_parse(tok.substr(2))) {
						sp.kind = MdSpan::RoleMention;
						sp.id = sf_parse(tok.substr(2));
					} else if (tok.rfind("@!", 0) == 0 && sf_parse(tok.substr(2))) {
						sp.kind = MdSpan::UserMention;
						sp.id = sf_parse(tok.substr(2));
					} else if (tok.rfind("@", 0) == 0 && sf_parse(tok.substr(1))) {
						sp.kind = MdSpan::UserMention;
						sp.id = sf_parse(tok.substr(1));
					} else if (tok.rfind("#", 0) == 0 && sf_parse(tok.substr(1))) {
						sp.kind = MdSpan::ChannelMention;
						sp.id = sf_parse(tok.substr(1));
					} else if ((tok.rfind(":", 0) == 0 || tok.rfind("a:", 0) == 0) &&
						   std::count(tok.begin(), tok.end(), ':') >= 2) {
						bool anim = tok[0] == 'a';
						std::string_view rest = tok.substr(anim ? 2 : 1);
						size_t col = rest.rfind(':');
						Snowflake id = sf_parse(rest.substr(col + 1));
						if (id) {
							sp.kind = MdSpan::CustomEmoji;
							sp.text = std::string(rest.substr(0, col));
							sp.id = id;
							sp.animated = anim;
						} else {
							matched = false;
						}
					} else if (tok.rfind("t:", 0) == 0) {
						std::string_view rest = tok.substr(2);
						size_t col = rest.find(':');
						std::string num(rest.substr(0, col));
						char *end;
						long long v = strtoll(num.c_str(), &end, 10);
						if (*end == 0 && !num.empty()) {
							sp.kind = MdSpan::Timestamp;
							sp.ts = v;
							sp.ts_style = col == std::string_view::npos ? 'f' : rest[col + 1];
						} else {
							matched = false;
						}
					} else if (tok.rfind("http://", 0) == 0 || tok.rfind("https://", 0) == 0) {
						flush(i);
						push_text(tok, style | MD_LINK, spoiler, std::string(tok));
						i = close + 1;
						plain = i;
						continue;
					} else {
						matched = false;
					}
					if (matched) {
						flush(i);
						out.push_back(sp);
						i = close + 1;
						plain = i;
						continue;
					}
				}
			}
			// Masked links [text](url)
			if (c == '[') {
				size_t rb = s.find("](", i);
				if (rb != std::string_view::npos && s.substr(i, rb - i).find('\n') == std::string_view::npos) {
					size_t rp = s.find(')', rb + 2);
					std::string_view url = rp == std::string_view::npos ? std::string_view() : s.substr(rb + 2, rp - rb - 2);
					if (url.front() == '<' && url.back() == '>')
						url = url.substr(1, url.size() - 2);
					if (rp != std::string_view::npos && (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)) {
						flush(i);
						std::string_view label = s.substr(i + 1, rb - i - 1);
						size_t before = out.size();
						inline_(label, style | MD_LINK, spoiler);
						for (size_t k = before; k < out.size(); k++)
							out[k].url = std::string(url);
						i = rp + 1;
						plain = i;
						continue;
					}
				}
			}
			// Autolinks
			if ((c == 'h') && (s.compare(i, 8, "https://") == 0 || s.compare(i, 7, "http://") == 0) &&
			    (i == 0 || !isalnum((unsigned char)s[i - 1]))) {
				size_t e = i;
				while (e < s.size() && is_url_char(s[e]))
					e++;
				// Trailing punctuation is usually not part of the URL.
				while (e > i && strchr(".,:;!?)'", s[e - 1]))
					e--;
				flush(i);
				std::string_view url = s.substr(i, e - i);
				push_text(url, style | MD_LINK, spoiler, std::string(url));
				i = e;
				plain = i;
				continue;
			}
			// @everyone / @here
			if (c == '@' && (s.compare(i, 9, "@everyone") == 0 || s.compare(i, 5, "@here") == 0)) {
				flush(i);
				MdSpan sp;
				sp.kind = MdSpan::Everyone;
				sp.text = s.compare(i, 9, "@everyone") == 0 ? "@everyone" : "@here";
				sp.style = style;
				sp.spoiler = spoiler;
				out.push_back(sp);
				i += sp.text.size();
				plain = i;
				continue;
			}
			// Paired delimiters
			struct Delim {
				const char *d;
				uint32_t flag;
			};
			static const Delim delims[] = {
				{"||", MD_SPOILER}, {"***", MD_BOLD | MD_ITALIC}, {"**", MD_BOLD}, {"__", MD_UNDERLINE},
				{"~~", MD_STRIKE}, {"*", MD_ITALIC}, {"_", MD_ITALIC},
			};
			bool took = false;
			for (const Delim &dl : delims) {
				size_t n = strlen(dl.d);
				if (s.compare(i, n, dl.d) != 0)
					continue;
				if (i + n >= s.size())
					break;
				if (n == 1 && isspace((unsigned char)s[i + 1]))
					continue;
				if (dl.d[0] == '_' && n == 1 && i > 0 && isalnum((unsigned char)s[i - 1]))
					continue;
				size_t close = find_close(s, i + n, dl.d);
				if (close == std::string_view::npos || close == i + n)
					continue;
				if (dl.d[0] == '_' && n == 1 && close + 1 < s.size() && isalnum((unsigned char)s[close + 1]))
					continue;
				if (n == 1 && isspace((unsigned char)s[close - 1]))
					continue;
				flush(i);
				int sp = spoiler;
				if (dl.flag == MD_SPOILER)
					sp = ++spoilers;
				inline_(s.substr(i + n, close - i - n), style | dl.flag, sp);
				i = close + n;
				plain = i;
				took = true;
				break;
			}
			if (took)
				continue;
			i++;
		}
		flush(s.size());
	}

	void newline(uint32_t style)
	{
		MdSpan nl;
		nl.kind = MdSpan::Newline;
		nl.style = style;
		out.push_back(nl);
	}

	void blocks(std::string_view s)
	{
		size_t i = 0;
		bool first = true;
		bool quote_rest = false;
		while (i <= s.size()) {
			// Code block
			if (s.compare(i, 3, "```") == 0) {
				size_t close = s.find("```", i + 3);
				if (close != std::string_view::npos) {
					std::string_view body = s.substr(i + 3, close - i - 3);
					std::string lang;
					size_t nl = body.find('\n');
					if (nl != std::string_view::npos) {
						std::string_view l = body.substr(0, nl);
						bool word = l.find(' ') == std::string_view::npos;
						if (word) {
							lang = std::string(l);
							body = body.substr(nl + 1);
						}
					}
					if (!body.empty() && body.back() == '\n')
						body.remove_suffix(1);
					if (!first && !out.empty() && out.back().kind != MdSpan::Newline)
						newline(0);
					MdSpan cb;
					cb.kind = MdSpan::Text;
					cb.text = std::string(body);
					cb.style = MD_CODEBLOCK | (quote_rest ? (uint32_t)MD_QUOTE : 0u);
					cb.lang = lang;
					out.push_back(cb);
					i = close + 3;
					if (i < s.size() && s[i] == '\n')
						i++;
					if (i < s.size())
						newline(0);
					first = true;  // the newline after the block is already emitted
					continue;
				}
			}
			size_t e = s.find('\n', i);
			if (e == std::string_view::npos)
				e = s.size();
			std::string_view line = s.substr(i, e - i);
			// A code block may start mid-line; handle the prefix as a line.
			size_t cbpos = line.find("```");
			if (cbpos != std::string_view::npos && cbpos > 0 && s.find("```", i + cbpos + 3) != std::string_view::npos) {
				e = i + cbpos;
				line = s.substr(i, cbpos);
			}
			uint32_t style = quote_rest ? (uint32_t)MD_QUOTE : 0u;
			if (line.rfind(">>> ", 0) == 0) {
				quote_rest = true;
				style = MD_QUOTE;
				line.remove_prefix(4);
			} else if (line.rfind("> ", 0) == 0) {
				style |= MD_QUOTE;
				line.remove_prefix(2);
			}
			if (line.rfind("# ", 0) == 0) {
				style |= MD_H1;
				line.remove_prefix(2);
			} else if (line.rfind("## ", 0) == 0) {
				style |= MD_H2;
				line.remove_prefix(3);
			} else if (line.rfind("### ", 0) == 0) {
				style |= MD_H3;
				line.remove_prefix(4);
			} else if (line.rfind("-# ", 0) == 0) {
				style |= MD_SUBTEXT;
				line.remove_prefix(3);
			}
			std::string bullet;
			size_t indent = 0;
			while (indent < line.size() && line[indent] == ' ')
				indent++;
			if (line.size() > indent + 2 && (line[indent] == '-' || line[indent] == '*') && line[indent + 1] == ' ') {
				bullet = std::string(indent, ' ') + "• ";
				line.remove_prefix(indent + 2);
			}
			if (!first && !(cbpos != std::string_view::npos && cbpos == 0))
				newline(style);
			if (!bullet.empty())
				push_text(bullet, style, 0);
			inline_(line, style, 0);
			first = false;
			if (e >= s.size())
				break;
			i = (s[e] == '\n') ? e + 1 : e;
			if (s[e] != '\n')
				first = true;  // continuing into a code block on the same line
		}
	}
};

}  // namespace

std::vector<MdSpan> md_parse(std::string_view text)
{
	Parser p;
	p.blocks(text);
	// Drop a trailing newline span.
	while (!p.out.empty() && p.out.back().kind == MdSpan::Newline)
		p.out.pop_back();
	return std::move(p.out);
}
