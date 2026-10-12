// Markdown parser and emoji detection tests.
#include <cstdio>
#include <string>

#include "markdown.h"

static int fails;

static std::string dump(const std::vector<MdSpan> &v)
{
	std::string s;
	for (auto &x : v) {
		switch (x.kind) {
		case MdSpan::Text: {
			s += "[";
			if (x.style & MD_BOLD) s += "b";
			if (x.style & MD_ITALIC) s += "i";
			if (x.style & MD_UNDERLINE) s += "u";
			if (x.style & MD_STRIKE) s += "s";
			if (x.style & MD_CODE) s += "c";
			if (x.style & MD_CODEBLOCK) s += "C" + x.lang;
			if (x.style & MD_SPOILER) s += "S";
			if (x.style & MD_QUOTE) s += "q";
			if (x.style & MD_H1) s += "h1";
			if (x.style & MD_SUBTEXT) s += "sub";
			if (x.style & MD_LINK) s += "L(" + x.url + ")";
			s += ":" + x.text + "]";
			break;
		}
		case MdSpan::Newline: s += "\\n"; break;
		case MdSpan::CustomEmoji: s += "<E:" + x.text + ":" + std::to_string(x.id) + (x.animated ? "a" : "") + ">"; break;
		case MdSpan::UnicodeEmoji: s += "<U:" + twemoji_name(x.text) + ">"; break;
		case MdSpan::UserMention: s += "<@" + std::to_string(x.id) + ">"; break;
		case MdSpan::RoleMention: s += "<@&" + std::to_string(x.id) + ">"; break;
		case MdSpan::ChannelMention: s += "<#" + std::to_string(x.id) + ">"; break;
		case MdSpan::Everyone: s += "<" + x.text + ">"; break;
		case MdSpan::Timestamp: s += "<t:" + std::to_string(x.ts) + ":" + x.ts_style + ">"; break;
		}
	}
	return s;
}

static void check(const char *in, const char *want)
{
	std::string got = dump(md_parse(in));
	if (got != want) {
		printf("FAIL: %s\n  want %s\n  got  %s\n", in, want, got.c_str());
		fails++;
	}
}

int main()
{
	check("hello", "[:hello]");
	check("**bold** and *it*", "[b:bold][: and ][i:it]");
	check("***both***", "[bi:both]");
	check("__u__ ~~s~~", "[u:u][: ][s:s]");
	check("snake_case_name", "[:snake_case_name]");
	check("_it_", "[i:it]");
	check("a * b * c", "[:a * b * c]");
	check("`code *x*`", "[c:code *x*]");
	check("||secret||", "[S:secret]");
	check("\\*not\\*", "[:*not*]");
	check("line1\nline2", "[:line1]\\n[:line2]");
	check("> quoted\nnot", "[q:quoted]\\n[:not]");
	check(">>> all\nquoted", "[q:all]\\n[q:quoted]");
	check("# Big", "[h1:Big]");
	check("-# small", "[sub:small]");
	check("- item", "[:• item]");
	check("```py\nprint(1)\n```", "[Cpy:print(1)]");
	check("before\n```\nx = 1\n```\nafter", "[:before]\\n[C:x = 1]\\n[:after]");
	check("hi <@123> <@!45> <@&6> <#7>", "[:hi ]<@123>[: ]<@45>[: ]<@&6>[: ]<#7>");
	check("<:pog:999> <a:dance:1000>", "<E:pog:999>[: ]<E:dance:1000a>");
	check("<t:1700000000:R>", "<t:1700000000:R>");
	check("see https://x.com/a_b_c.", "[:see ][L(https://x.com/a_b_c):https://x.com/a_b_c][:.]");
	check("[site](https://e.com) ok", "[L(https://e.com):site][: ok]");
	check("<https://nolink.com>", "[L(https://nolink.com):https://nolink.com]");
	check("@everyone hi", "<@everyone>[: hi]");
	check("smile \xF0\x9F\x98\x80!", "[:smile ]<U:1f600>[:!]");
	check("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x92\xBB", "<U:1f468-200d-1f4bb>");
	check("\xF0\x9F\x87\xB3\xF0\x9F\x87\xB1", "<U:1f1f3-1f1f1>");
	check("\xE2\x9D\xA4\xEF\xB8\x8F", "<U:2764>");
	check("\xE2\x9D\xA4 text heart", "[:\xE2\x9D\xA4 text heart]");
	check("1\xEF\xB8\x8F\xE2\x83\xA3", "<U:31-20e3>");
	check("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", "<U:1f44d-1f3fd>");
	check("price 100", "[:price 100]");
	if (!fails)
		printf("markdown: all tests passed\n");
	return fails ? 1 : 0;
}
