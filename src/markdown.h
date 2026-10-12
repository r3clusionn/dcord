// Discord-flavoured markdown -> flat list of styled spans.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "util.h"

enum MdStyle : uint32_t {
	MD_BOLD = 1u << 0,
	MD_ITALIC = 1u << 1,
	MD_UNDERLINE = 1u << 2,
	MD_STRIKE = 1u << 3,
	MD_CODE = 1u << 4,
	MD_CODEBLOCK = 1u << 5,
	MD_SPOILER = 1u << 6,
	MD_QUOTE = 1u << 7,
	MD_H1 = 1u << 8,
	MD_H2 = 1u << 9,
	MD_H3 = 1u << 10,
	MD_SUBTEXT = 1u << 11,
	MD_LINK = 1u << 12,
};

struct MdSpan {
	enum Kind { Text, Newline, CustomEmoji, UnicodeEmoji, UserMention, RoleMention, ChannelMention,
		    Everyone, Timestamp } kind = Text;
	std::string text;     // display text (emoji: name / the emoji itself)
	uint32_t style = 0;
	std::string url;      // links
	Snowflake id = 0;     // mentions, custom emoji
	bool animated = false;
	int spoiler = 0;      // spoiler group (1-based) when MD_SPOILER
	int64_t ts = 0;       // Timestamp: unix seconds
	char ts_style = 'f';
	std::string lang;     // code block language
};

std::vector<MdSpan> md_parse(std::string_view text);

// Length in bytes of the emoji sequence starting at s, or 0.
size_t emoji_seq_len(std::string_view s);
// Twemoji asset name ("1f468-200d-1f4bb") for an emoji sequence.
std::string twemoji_name(std::string_view seq);
// Formats a <t:..:style> timestamp.
std::string format_timestamp(int64_t unix_s, char style);
