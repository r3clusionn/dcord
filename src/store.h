// In-memory model of everything the client knows: users, guilds, channels,
// roles, read states, voice states and loaded messages. Main thread only.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "util.h"

enum ChannelType {
	CH_TEXT = 0, CH_DM = 1, CH_VOICE = 2, CH_GROUP_DM = 3, CH_CATEGORY = 4,
	CH_NEWS = 5, CH_NEWS_THREAD = 10, CH_PUBLIC_THREAD = 11, CH_PRIVATE_THREAD = 12,
	CH_STAGE = 13, CH_DIRECTORY = 14, CH_FORUM = 15, CH_MEDIA = 16,
};

namespace perm {
constexpr uint64_t CREATE_INSTANT_INVITE = 1ull << 0;
constexpr uint64_t KICK = 1ull << 1;
constexpr uint64_t ADMINISTRATOR = 1ull << 3;
constexpr uint64_t MANAGE_CHANNELS = 1ull << 4;
constexpr uint64_t ADD_REACTIONS = 1ull << 6;
constexpr uint64_t VIEW_CHANNEL = 1ull << 10;
constexpr uint64_t SEND_MESSAGES = 1ull << 11;
constexpr uint64_t MANAGE_MESSAGES = 1ull << 13;
constexpr uint64_t EMBED_LINKS = 1ull << 14;
constexpr uint64_t ATTACH_FILES = 1ull << 15;
constexpr uint64_t READ_HISTORY = 1ull << 16;
constexpr uint64_t MENTION_EVERYONE = 1ull << 17;
constexpr uint64_t EXTERNAL_EMOJIS = 1ull << 18;
constexpr uint64_t CONNECT = 1ull << 20;
constexpr uint64_t SPEAK = 1ull << 21;
constexpr uint64_t MUTE_MEMBERS = 1ull << 22;
constexpr uint64_t USE_VAD = 1ull << 25;
constexpr uint64_t SEND_IN_THREADS = 1ull << 38;
constexpr uint64_t ALL = ~0ull;
}  // namespace perm

struct User {
	Snowflake id = 0;
	std::string username, global_name, avatar;
	int discriminator = 0;
	bool bot = false;
	std::string display() const { return global_name.empty() ? username : global_name; }
};

struct Role {
	Snowflake id = 0;
	std::string name;
	uint32_t color = 0;
	int position = 0;
	uint64_t permissions = 0;
	bool hoist = false;
};

struct Overwrite {
	Snowflake id = 0;
	int type = 0;  // 0 role, 1 member
	uint64_t allow = 0, deny = 0;
};

struct Emoji {
	Snowflake id = 0;
	std::string name;
	bool animated = false;
	bool available = true;
};

struct Member {
	std::vector<Snowflake> roles;
	std::string nick, avatar;
	bool loaded = false;
};

struct Channel {
	Snowflake id = 0, guild_id = 0, parent_id = 0, last_message_id = 0, owner_id = 0;
	int type = 0, position = 0;
	std::string name, topic, icon;
	std::vector<Overwrite> overwrites;
	std::vector<Snowflake> recipients;
	int64_t last_pin_ms = 0;
	int bitrate = 64000, user_limit = 0, rate_limit = 0;
	bool nsfw = false;
	bool archived = false;
	bool joined = false;  // threads: we are a member
	bool is_voice() const { return type == CH_VOICE || type == CH_STAGE; }
	bool is_private() const { return type == CH_DM || type == CH_GROUP_DM; }
	bool is_thread() const { return type == CH_NEWS_THREAD || type == CH_PUBLIC_THREAD || type == CH_PRIVATE_THREAD; }
	bool is_text() const { return type == CH_TEXT || type == CH_NEWS || is_private() || is_thread() || type == CH_VOICE || type == CH_STAGE; }
};

struct Guild {
	Snowflake id = 0, owner_id = 0;
	std::string name, icon;
	bool unavailable = false;
	int member_count = 0;
	std::vector<Snowflake> channels;   // all channel ids incl. categories
	std::vector<Snowflake> threads;
	std::map<Snowflake, Role> roles;
	std::map<Snowflake, Emoji> emojis;
	std::unordered_map<Snowflake, Member> members;  // members seen so far
	std::set<Snowflake> requested_members;           // already asked the gateway for
};

struct VoiceState {
	Snowflake user_id = 0, channel_id = 0, guild_id = 0;
	std::string session_id;
	bool mute = false, deaf = false, self_mute = false, self_deaf = false;
	bool self_video = false, self_stream = false, suppress = false;
};

struct ReadState {
	Snowflake last_message_id = 0;
	int mention_count = 0;
	int flags = 0;
	int64_t last_viewed = 0;
};

struct NotifyOverride {
	bool muted = false;
	int level = -1;  // -1 inherit, 0 all, 1 mentions, 2 nothing
};

struct Attachment {
	Snowflake id = 0;
	std::string filename, url, proxy_url, content_type, description;
	uint64_t size = 0;
	int width = 0, height = 0;
	double duration = 0;
	bool spoiler = false;
	bool is_image() const;
	bool is_video() const;
	bool is_audio() const;
};

struct EmbedMedia {
	std::string url, proxy_url;
	int width = 0, height = 0;
	bool empty() const { return url.empty() && proxy_url.empty(); }
};

struct EmbedField {
	std::string name, value;
	bool inline_ = false;
};

struct Embed {
	std::string type, title, description, url, author, author_url, footer, provider;
	uint32_t color = 0;
	bool has_color = false;
	EmbedMedia image, thumbnail, video;
	std::vector<EmbedField> fields;
};

struct Reaction {
	std::string name;   // unicode emoji, or custom emoji name
	Snowflake emoji_id = 0;
	bool animated = false;
	int count = 0;
	bool me = false;
	std::string key() const { return emoji_id ? name + ":" + sf_str(emoji_id) : name; }
};

struct StickerItem {
	Snowflake id = 0;
	std::string name;
	int format = 1;  // 1 png, 2 apng, 3 lottie, 4 gif
};

struct Message;
using MessagePtr = std::shared_ptr<Message>;

struct Message {
	Snowflake id = 0, channel_id = 0, guild_id = 0, author_id = 0, webhook_id = 0;
	int type = 0, flags = 0;
	std::string content;
	int64_t ts = 0, edited_ts = 0;
	std::vector<Attachment> attachments;
	std::vector<Embed> embeds;
	std::vector<Reaction> reactions;
	std::vector<StickerItem> stickers;
	std::vector<Snowflake> mentions, mention_roles;
	bool mention_everyone = false, pinned = false;
	Snowflake ref_message_id = 0, ref_channel_id = 0;
	MessagePtr ref;                 // replied-to message, when known
	bool forwarded = false;         // content comes from message_snapshots
	std::string nonce;
	enum State { Sent, Pending, Failed } state = Sent;
	// Author shown for webhooks (they don't have a real user).
	std::string webhook_name, webhook_avatar;
	std::string call_info;          // text for call system messages
};

struct ChannelMessages {
	std::vector<MessagePtr> msgs;  // ascending by id
	bool loaded = false;           // initial page fetched
	bool loading = false;
	bool reached_start = false;    // no older messages
	int64_t last_access = 0;
};

struct GuildFolder {
	Snowflake id = 0;  // 0 = not a real folder (single guild)
	std::string name;
	uint32_t color = 0;
	std::vector<Snowflake> guild_ids;
};

struct Relationship {
	Snowflake id = 0;
	int type = 0;  // 1 friend, 2 blocked, 3 incoming, 4 outgoing
	std::string nickname;
};

class Store {
public:
	User me;
	std::string session_id;
	std::unordered_map<Snowflake, User> users;
	std::map<Snowflake, Guild> guilds;
	std::vector<Snowflake> guild_order;  // as received in READY
	std::vector<GuildFolder> folders;
	std::unordered_map<Snowflake, Channel> channels;
	std::unordered_map<Snowflake, ReadState> read_states;
	std::unordered_map<Snowflake, NotifyOverride> notify;  // guild or channel id
	std::unordered_map<Snowflake, Relationship> relationships;
	std::unordered_map<Snowflake, std::string> presences;  // user -> status
	// voice states keyed by user, per guild (0 = DM calls)
	std::unordered_map<Snowflake, std::unordered_map<Snowflake, VoiceState>> voice;
	std::unordered_map<Snowflake, ChannelMessages> messages;

	void clear();
	void load_ready(json &d);
	void load_ready_supplemental(json &d);

	User &upsert_user(const json &j);
	void upsert_member(Snowflake guild_id, const json &j, Snowflake user_id = 0);
	Channel &upsert_channel(const json &j, Snowflake guild_id = 0);
	void remove_channel(Snowflake id);
	void load_guild(const json &j);
	void update_guild(const json &j);
	void remove_guild(Snowflake id);
	void upsert_role(Snowflake guild_id, const json &j);
	void update_voice_state(const json &j);
	void parse_user_guild_settings(const json &arr);
	void parse_settings_proto(const std::string &b64);

	MessagePtr parse_message(const json &j);
	// Inserts or replaces; returns the stored pointer.
	MessagePtr add_message(MessagePtr m);
	MessagePtr find_message(Snowflake channel_id, Snowflake id);
	bool remove_message(Snowflake channel_id, Snowflake id);
	void trim_messages(Snowflake keep_channel);

	const User *user(Snowflake id) const;
	const Channel *channel(Snowflake id) const;
	Guild *guild(Snowflake id);
	const Member *member(Snowflake guild_id, Snowflake user_id) const;

	// Display name in a guild context: nick, else global name, else username.
	std::string display_name(Snowflake guild_id, Snowflake user_id) const;
	uint32_t name_color(Snowflake guild_id, Snowflake user_id) const;
	std::string channel_title(const Channel &c) const;

	uint64_t base_permissions(Snowflake guild_id) const;
	uint64_t permissions(const Channel &c) const;
	bool can_view(const Channel &c) const;

	bool is_muted(const Channel &c) const;
	bool is_guild_muted(Snowflake guild_id) const;
	bool is_unread(const Channel &c) const;
	int mention_count(Snowflake channel_id) const;
	bool guild_unread(Snowflake guild_id) const;
	int guild_mentions(Snowflake guild_id) const;
	bool mentions_me(const Message &m) const;

	// Children of a guild, ordered the way the official client shows them.
	struct ChannelNode {
		const Channel *ch;
		std::vector<const Channel *> children;
	};
	std::vector<ChannelNode> channel_tree(Snowflake guild_id) const;
	std::vector<const Channel *> private_channels_sorted() const;
	std::vector<VoiceState> voice_members(Snowflake channel_id) const;
	const VoiceState *my_voice_state() const;
};

extern Store g_store;

std::string avatar_url(const User &u, int size);
std::string guild_icon_url(const Guild &g, int size);
std::string emoji_url(Snowflake id, bool animated, int size);
