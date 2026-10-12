#include "store.h"

#include <algorithm>

Store g_store;

static bool ends_with_ci(const std::string &s, const char *suf)
{
	size_t n = strlen(suf);
	if (s.size() < n)
		return false;
	for (size_t i = 0; i < n; i++)
		if (tolower((unsigned char)s[s.size() - n + i]) != suf[i])
			return false;
	return true;
}

bool Attachment::is_image() const
{
	if (content_type.rfind("image/", 0) == 0)
		return true;
	for (const char *e : {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".avif"})
		if (ends_with_ci(filename, e))
			return true;
	return false;
}

bool Attachment::is_video() const
{
	if (content_type.rfind("video/", 0) == 0)
		return true;
	for (const char *e : {".mp4", ".webm", ".mov", ".mkv"})
		if (ends_with_ci(filename, e))
			return true;
	return false;
}

bool Attachment::is_audio() const
{
	return content_type.rfind("audio/", 0) == 0;
}

static uint64_t perm_parse(const json &v)
{
	if (v.is_string())
		return strtoull(v.get_ref<const std::string &>().c_str(), nullptr, 10);
	if (v.is_number())
		return v.get<uint64_t>();
	return 0;
}

void Store::clear()
{
	*this = Store();
}

User &Store::upsert_user(const json &j)
{
	Snowflake id = jsf(j, "id");
	User &u = users[id];
	u.id = id;
	if (j.contains("username"))
		u.username = jstr(j, "username");
	if (j.contains("global_name"))
		u.global_name = jstr(j, "global_name");
	if (j.contains("avatar"))
		u.avatar = jstr(j, "avatar");
	if (j.contains("discriminator"))
		u.discriminator = (int)jint(j, "discriminator");
	if (j.contains("bot"))
		u.bot = jbool(j, "bot");
	if (id == me.id && id)
		me = u;
	return u;
}

void Store::upsert_member(Snowflake guild_id, const json &j, Snowflake user_id)
{
	Guild *g = guild(guild_id);
	if (!g)
		return;
	if (!user_id) {
		if (j.contains("user") && j["user"].is_object())
			user_id = upsert_user(j["user"]).id;
		else
			user_id = jsf(j, "user_id");
	}
	if (!user_id)
		return;
	Member &m = g->members[user_id];
	m.loaded = true;
	if (j.contains("nick"))
		m.nick = jstr(j, "nick");
	if (j.contains("avatar"))
		m.avatar = jstr(j, "avatar");
	if (j.contains("roles") && j["roles"].is_array()) {
		m.roles.clear();
		for (auto &r : j["roles"])
			m.roles.push_back(sf_parse(r));
	}
}

Channel &Store::upsert_channel(const json &j, Snowflake guild_id)
{
	Snowflake id = jsf(j, "id");
	Channel &c = channels[id];
	c.id = id;
	if (Snowflake g = jsf(j, "guild_id"))
		guild_id = g;
	if (guild_id)
		c.guild_id = guild_id;
	if (j.contains("type"))
		c.type = (int)jint(j, "type");
	if (j.contains("name"))
		c.name = jstr(j, "name");
	if (j.contains("topic"))
		c.topic = jstr(j, "topic");
	if (j.contains("icon"))
		c.icon = jstr(j, "icon");
	if (j.contains("position"))
		c.position = (int)jint(j, "position");
	if (j.contains("parent_id"))
		c.parent_id = jsf(j, "parent_id");
	if (j.contains("owner_id"))
		c.owner_id = jsf(j, "owner_id");
	if (j.contains("last_message_id"))
		c.last_message_id = jsf(j, "last_message_id");
	if (j.contains("last_pin_timestamp"))
		c.last_pin_ms = iso8601_ms(jstr(j, "last_pin_timestamp"));
	if (j.contains("nsfw"))
		c.nsfw = jbool(j, "nsfw");
	if (j.contains("bitrate"))
		c.bitrate = (int)jint(j, "bitrate", 64000);
	if (j.contains("user_limit"))
		c.user_limit = (int)jint(j, "user_limit");
	if (j.contains("rate_limit_per_user"))
		c.rate_limit = (int)jint(j, "rate_limit_per_user");
	if (const json &tm = jget(j, "thread_metadata"); tm.is_object())
		c.archived = jbool(tm, "archived");
	if (j.contains("member") && j["member"].is_object())
		c.joined = true;
	if (const json &ow = jget(j, "permission_overwrites"); ow.is_array()) {
		c.overwrites.clear();
		for (auto &o : ow) {
			Overwrite w;
			w.id = jsf(o, "id");
			const json &t = jget(o, "type");
			w.type = t.is_string() ? (t.get<std::string>() == "member" ? 1 : 0) : (int)jint(o, "type");
			w.allow = perm_parse(jget(o, "allow"));
			w.deny = perm_parse(jget(o, "deny"));
			c.overwrites.push_back(w);
		}
	}
	if (const json &r = jget(j, "recipient_ids"); r.is_array()) {
		c.recipients.clear();
		for (auto &x : r)
			c.recipients.push_back(sf_parse(x));
	} else if (const json &r2 = jget(j, "recipients"); r2.is_array()) {
		c.recipients.clear();
		for (auto &x : r2)
			c.recipients.push_back(upsert_user(x).id);
	}
	if (guild_id) {
		if (Guild *g = guild(guild_id)) {
			auto &list = c.is_thread() ? g->threads : g->channels;
			if (std::find(list.begin(), list.end(), id) == list.end())
				list.push_back(id);
		}
	}
	return c;
}

void Store::remove_channel(Snowflake id)
{
	auto it = channels.find(id);
	if (it == channels.end())
		return;
	if (Guild *g = guild(it->second.guild_id)) {
		std::erase(g->channels, id);
		std::erase(g->threads, id);
	}
	channels.erase(it);
	messages.erase(id);
}

void Store::upsert_role(Snowflake guild_id, const json &j)
{
	Guild *g = guild(guild_id);
	if (!g)
		return;
	Role r;
	r.id = jsf(j, "id");
	r.name = jstr(j, "name");
	r.color = (uint32_t)jint(j, "color");
	if (const json &cs = jget(j, "colors"); cs.is_object() && !r.color)
		r.color = (uint32_t)jint(cs, "primary_color");
	r.position = (int)jint(j, "position");
	r.permissions = perm_parse(jget(j, "permissions"));
	r.hoist = jbool(j, "hoist");
	g->roles[r.id] = r;
}

static void parse_guild_props(Guild &g, const json &p)
{
	if (p.contains("name"))
		g.name = jstr(p, "name");
	if (p.contains("icon"))
		g.icon = jstr(p, "icon");
	if (p.contains("owner_id"))
		g.owner_id = jsf(p, "owner_id");
}

void Store::load_guild(const json &j)
{
	Snowflake id = jsf(j, "id");
	Guild &g = guilds[id];
	g.id = id;
	g.unavailable = jbool(j, "unavailable");
	if (g.unavailable)
		return;
	if (const json &p = jget(j, "properties"); p.is_object())
		parse_guild_props(g, p);
	else
		parse_guild_props(g, j);
	if (j.contains("member_count"))
		g.member_count = (int)jint(j, "member_count");
	bool partial = jstr(j, "data_mode") == "partial";
	if (const json &roles = jget(j, "roles"); roles.is_array()) {
		if (!partial)
			g.roles.clear();
		for (auto &r : roles)
			upsert_role(id, r);
	}
	if (const json &em = jget(j, "emojis"); em.is_array()) {
		if (!partial)
			g.emojis.clear();
		for (auto &e : em) {
			Emoji x;
			x.id = jsf(e, "id");
			x.name = jstr(e, "name");
			x.animated = jbool(e, "animated");
			x.available = jbool(e, "available", true);
			g.emojis[x.id] = x;
		}
	}
	if (const json &ch = jget(j, "channels"); ch.is_array()) {
		if (!partial) {
			for (Snowflake c : g.channels)
				channels.erase(c);
			g.channels.clear();
		}
		for (auto &c : ch)
			upsert_channel(c, id);
	}
	if (const json &th = jget(j, "threads"); th.is_array())
		for (auto &c : th)
			upsert_channel(c, id);
	if (const json &pu = jget(j, "partial_updates"); pu.is_object()) {
		for (auto &c : jget(pu, "channels"))
			upsert_channel(c, id);
		for (auto &r : jget(pu, "roles"))
			upsert_role(id, r);
		for (auto &c : jget(pu, "deleted_channel_ids"))
			remove_channel(sf_parse(c));
	}
	if (const json &vs = jget(j, "voice_states"); vs.is_array())
		for (auto v : vs) {
			v["guild_id"] = sf_str(id);
			update_voice_state(v);
		}
	if (const json &mem = jget(j, "members"); mem.is_array())
		for (auto &m : mem)
			upsert_member(id, m);
}

void Store::update_guild(const json &j)
{
	Guild *g = guild(jsf(j, "guild_id") ? jsf(j, "guild_id") : jsf(j, "id"));
	if (!g)
		return;
	parse_guild_props(*g, j);
	if (const json &roles = jget(j, "roles"); roles.is_array())
		for (auto &r : roles)
			upsert_role(g->id, r);
}

void Store::remove_guild(Snowflake id)
{
	auto it = guilds.find(id);
	if (it == guilds.end())
		return;
	for (Snowflake c : it->second.channels) {
		channels.erase(c);
		messages.erase(c);
	}
	for (Snowflake c : it->second.threads)
		channels.erase(c);
	voice.erase(id);
	guilds.erase(it);
	std::erase(guild_order, id);
}

void Store::update_voice_state(const json &j)
{
	VoiceState v;
	v.user_id = jsf(j, "user_id");
	v.channel_id = jsf(j, "channel_id");
	v.guild_id = jsf(j, "guild_id");
	v.session_id = jstr(j, "session_id");
	v.mute = jbool(j, "mute");
	v.deaf = jbool(j, "deaf");
	v.self_mute = jbool(j, "self_mute");
	v.self_deaf = jbool(j, "self_deaf");
	v.self_video = jbool(j, "self_video");
	v.self_stream = jbool(j, "self_stream");
	v.suppress = jbool(j, "suppress");
	if (const json &m = jget(j, "member"); m.is_object() && v.guild_id)
		upsert_member(v.guild_id, m, v.user_id);
	if (!v.channel_id && v.guild_id == 0) {
		// DM call states have no guild; find the call the user was in.
		voice[0].erase(v.user_id);
		return;
	}
	if (!v.channel_id)
		voice[v.guild_id].erase(v.user_id);
	else
		voice[v.guild_id][v.user_id] = v;
}

void Store::parse_user_guild_settings(const json &in)
{
	const json &arr = in.is_object() ? jget(in, "entries") : in;
	if (!arr.is_array())
		return;
	int64_t now = now_ms();
	auto is_muted = [now](const json &o) {
		if (!jbool(o, "muted"))
			return false;
		const json &mc = jget(o, "mute_config");
		if (mc.is_object()) {
			std::string end = jstr(mc, "end_time");
			if (!end.empty() && iso8601_ms(end) < now)
				return false;
		}
		return true;
	};
	for (auto &s : arr) {
		Snowflake gid = jsf(s, "guild_id");  // null guild = DM settings
		NotifyOverride &n = notify[gid];
		n.muted = gid ? is_muted(s) : false;
		n.level = (int)jint(s, "message_notifications", -1);
		for (auto &o : jget(s, "channel_overrides")) {
			NotifyOverride &c = notify[jsf(o, "channel_id")];
			c.muted = is_muted(o);
			c.level = (int)jint(o, "message_notifications", -1);
		}
	}
}

// --- minimal protobuf reader, enough for guild folders --------------------

namespace {
struct Pb {
	const uint8_t *p, *end;
	bool ok() const { return p < end; }
	uint64_t varint()
	{
		uint64_t r = 0;
		int s = 0;
		while (p < end) {
			uint8_t b = *p++;
			r |= (uint64_t)(b & 0x7f) << s;
			if (!(b & 0x80))
				break;
			s += 7;
		}
		return r;
	}
	uint64_t fixed64()
	{
		uint64_t r = 0;
		for (int i = 0; i < 8 && p < end; i++)
			r |= (uint64_t)*p++ << (8 * i);
		return r;
	}
	Pb sub()
	{
		uint64_t n = varint();
		const uint8_t *s = p;
		p = std::min(end, p + n);
		return Pb{s, p};
	}
	void skip(int wt)
	{
		if (wt == 0)
			varint();
		else if (wt == 1)
			p = std::min(end, p + 8);
		else if (wt == 2)
			sub();
		else if (wt == 5)
			p = std::min(end, p + 4);
		else
			p = end;
	}
};

// google.protobuf.*Value wrappers hold their payload in field 1.
uint64_t pb_wrapped_int(Pb m)
{
	uint64_t v = 0;
	while (m.ok()) {
		uint64_t k = m.varint();
		if ((k >> 3) == 1 && (k & 7) == 0)
			v = m.varint();
		else
			m.skip(k & 7);
	}
	return v;
}

std::string pb_wrapped_str(Pb m)
{
	std::string v;
	while (m.ok()) {
		uint64_t k = m.varint();
		if ((k >> 3) == 1 && (k & 7) == 2) {
			Pb s = m.sub();
			v.assign((const char *)s.p, s.end - s.p);
		} else {
			m.skip(k & 7);
		}
	}
	return v;
}
}  // namespace

void Store::parse_settings_proto(const std::string &b64)
{
	std::string raw = b64decode(b64);
	Pb top{(const uint8_t *)raw.data(), (const uint8_t *)raw.data() + raw.size()};
	std::vector<GuildFolder> out;
	while (top.ok()) {
		uint64_t k = top.varint();
		if ((k >> 3) == 14 && (k & 7) == 2) {  // guild_folders
			Pb gf = top.sub();
			while (gf.ok()) {
				uint64_t k2 = gf.varint();
				if ((k2 >> 3) == 1 && (k2 & 7) == 2) {  // folders
					Pb f = gf.sub();
					GuildFolder folder;
					while (f.ok()) {
						uint64_t k3 = f.varint();
						int field = (int)(k3 >> 3), wt = (int)(k3 & 7);
						if (field == 1 && wt == 2) {  // packed fixed64
							Pb ids = f.sub();
							while (ids.ok())
								folder.guild_ids.push_back(ids.fixed64());
						} else if (field == 1 && wt == 1) {
							folder.guild_ids.push_back(f.fixed64());
						} else if (field == 2 && wt == 2) {
							folder.id = pb_wrapped_int(f.sub());
						} else if (field == 3 && wt == 2) {
							folder.name = pb_wrapped_str(f.sub());
						} else if (field == 4 && wt == 2) {
							folder.color = (uint32_t)pb_wrapped_int(f.sub());
						} else {
							f.skip(wt);
						}
					}
					out.push_back(std::move(folder));
				} else {
					gf.skip(k2 & 7);
				}
			}
		} else {
			top.skip(k & 7);
		}
	}
	if (!out.empty())
		folders = std::move(out);
}

// --- READY ----------------------------------------------------------------

void Store::load_ready(json &d)
{
	clear();
	session_id = jstr(d, "session_id");
	me = User();
	upsert_user(jget(d, "user"));
	me = users[jsf(jget(d, "user"), "id")];
	for (auto &u : jget(d, "users"))
		upsert_user(u);

	const json &gl = jget(d, "guilds");
	const json &mm = jget(d, "merged_members");
	for (size_t i = 0; i < gl.size(); i++) {
		load_guild(gl[i]);
		Snowflake gid = jsf(gl[i], "id");
		guild_order.push_back(gid);
		if (mm.is_array() && i < mm.size())
			for (auto &m : mm[i])
				upsert_member(gid, m);
	}
	for (auto &c : jget(d, "private_channels"))
		upsert_channel(c);
	const json &rs = jget(d, "read_state");
	const json &rsa = rs.is_object() ? jget(rs, "entries") : rs;
	for (auto &r : rsa) {
		if (jint(r, "read_state_type", 0) != 0)
			continue;
		ReadState s;
		s.last_message_id = jsf(r, "last_message_id");
		s.mention_count = (int)jint(r, "mention_count");
		s.flags = (int)jint(r, "flags");
		s.last_viewed = jint(r, "last_viewed");
		read_states[jsf(r, "id")] = s;
	}
	parse_user_guild_settings(jget(d, "user_guild_settings"));
	for (auto &r : jget(d, "relationships")) {
		Relationship rel;
		rel.id = jsf(r, "id");
		rel.type = (int)jint(r, "type");
		rel.nickname = jstr(r, "nickname");
		if (r.contains("user"))
			upsert_user(r["user"]);
		relationships[rel.id] = rel;
	}
	if (std::string p = jstr(d, "user_settings_proto"); !p.empty())
		parse_settings_proto(p);
	if (const json &mp = jget(d, "merged_presences"); mp.is_object())
		for (auto &p : jget(mp, "friends"))
			presences[jsf(p, "user_id")] = jstr(p, "status");
	for (auto &p : jget(d, "presences"))
		presences[jsf(jget(p, "user"), "id")] = jstr(p, "status");
}

void Store::load_ready_supplemental(json &d)
{
	const json &gl = jget(d, "guilds");
	const json &mm = jget(d, "merged_members");
	for (size_t i = 0; i < gl.size(); i++) {
		Snowflake gid = jsf(gl[i], "id");
		for (auto v : jget(gl[i], "voice_states")) {
			v["guild_id"] = sf_str(gid);
			update_voice_state(v);
		}
		if (mm.is_array() && i < mm.size())
			for (auto &m : mm[i])
				upsert_member(gid, m);
	}
	if (const json &mp = jget(d, "merged_presences"); mp.is_object())
		for (auto &p : jget(mp, "friends"))
			presences[jsf(p, "user_id")] = jstr(p, "status");
}

// --- messages ---------------------------------------------------------------

static EmbedMedia parse_media(const json &j)
{
	EmbedMedia m;
	if (!j.is_object())
		return m;
	m.url = jstr(j, "url");
	m.proxy_url = jstr(j, "proxy_url");
	m.width = (int)jint(j, "width");
	m.height = (int)jint(j, "height");
	return m;
}

static void parse_content(Store &s, Message &m, const json &j)
{
	m.content = jstr(j, "content");
	m.attachments.clear();
	for (auto &a : jget(j, "attachments")) {
		Attachment x;
		x.id = jsf(a, "id");
		x.filename = jstr(a, "filename");
		x.url = jstr(a, "url");
		x.proxy_url = jstr(a, "proxy_url");
		x.content_type = jstr(a, "content_type");
		x.description = jstr(a, "description");
		x.size = (uint64_t)jint(a, "size");
		x.width = (int)jint(a, "width");
		x.height = (int)jint(a, "height");
		const json &dur = jget(a, "duration_secs");
		x.duration = dur.is_number() ? dur.get<double>() : 0;
		x.spoiler = x.filename.rfind("SPOILER_", 0) == 0 || (jint(a, "flags") & (1 << 3));
		m.attachments.push_back(std::move(x));
	}
	m.embeds.clear();
	for (auto &e : jget(j, "embeds")) {
		Embed x;
		x.type = jstr(e, "type");
		x.title = jstr(e, "title");
		x.description = jstr(e, "description");
		x.url = jstr(e, "url");
		if (e.contains("color") && e["color"].is_number()) {
			x.color = (uint32_t)jint(e, "color");
			x.has_color = true;
		}
		x.author = jstr(jget(e, "author"), "name");
		x.author_url = jstr(jget(e, "author"), "url");
		x.footer = jstr(jget(e, "footer"), "text");
		x.provider = jstr(jget(e, "provider"), "name");
		x.image = parse_media(jget(e, "image"));
		x.thumbnail = parse_media(jget(e, "thumbnail"));
		x.video = parse_media(jget(e, "video"));
		for (auto &f : jget(e, "fields"))
			x.fields.push_back({jstr(f, "name"), jstr(f, "value"), jbool(f, "inline")});
		m.embeds.push_back(std::move(x));
	}
	m.stickers.clear();
	for (auto &st : jget(j, "sticker_items"))
		m.stickers.push_back({jsf(st, "id"), jstr(st, "name"), (int)jint(st, "format_type", 1)});
	(void)s;
}

MessagePtr Store::parse_message(const json &j)
{
	auto m = std::make_shared<Message>();
	m->id = jsf(j, "id");
	m->channel_id = jsf(j, "channel_id");
	m->guild_id = jsf(j, "guild_id");
	if (!m->guild_id)
		if (const Channel *c = channel(m->channel_id))
			m->guild_id = c->guild_id;
	m->type = (int)jint(j, "type");
	m->flags = (int)jint(j, "flags");
	m->webhook_id = jsf(j, "webhook_id");
	const json &author = jget(j, "author");
	if (author.is_object()) {
		if (m->webhook_id) {
			m->author_id = jsf(author, "id");
			m->webhook_name = jstr(author, "username");
			m->webhook_avatar = jstr(author, "avatar");
		} else {
			m->author_id = upsert_user(author).id;
		}
	}
	if (const json &mem = jget(j, "member"); mem.is_object() && m->guild_id && !m->webhook_id)
		upsert_member(m->guild_id, mem, m->author_id);
	parse_content(*this, *m, j);
	m->ts = iso8601_ms(jstr(j, "timestamp"));
	if (!m->ts)
		m->ts = sf_unix_ms(m->id);
	m->edited_ts = iso8601_ms(jstr(j, "edited_timestamp"));
	m->pinned = jbool(j, "pinned");
	m->mention_everyone = jbool(j, "mention_everyone");
	m->nonce = jget(j, "nonce").is_string() ? jstr(j, "nonce") : "";
	for (auto &u : jget(j, "mentions")) {
		m->mentions.push_back(upsert_user(u).id);
		if (const json &mem = jget(u, "member"); mem.is_object() && m->guild_id)
			upsert_member(m->guild_id, mem, m->mentions.back());
	}
	for (auto &r : jget(j, "mention_roles"))
		m->mention_roles.push_back(sf_parse(r));
	for (auto &r : jget(j, "reactions")) {
		Reaction x;
		const json &e = jget(r, "emoji");
		x.name = jstr(e, "name");
		x.emoji_id = jsf(e, "id");
		x.animated = jbool(e, "animated");
		x.count = (int)jint(r, "count");
		x.me = jbool(r, "me");
		m->reactions.push_back(std::move(x));
	}
	const json &ref = jget(j, "message_reference");
	if (ref.is_object()) {
		m->ref_message_id = jsf(ref, "message_id");
		m->ref_channel_id = jsf(ref, "channel_id");
		if (jint(ref, "type") == 1)
			m->forwarded = true;
	}
	if (const json &snaps = jget(j, "message_snapshots"); snaps.is_array() && !snaps.empty()) {
		m->forwarded = true;
		parse_content(*this, *m, jget(snaps[0], "message"));
	}
	if (const json &rm = jget(j, "referenced_message"); rm.is_object() && !m->forwarded)
		m->ref = parse_message(rm);
	if (const json &call = jget(j, "call"); call.is_object()) {
		int64_t end = iso8601_ms(jstr(call, "ended_timestamp"));
		if (end) {
			int secs = (int)((end - m->ts) / 1000);
			char buf[64];
			snprintf(buf, sizeof buf, "call lasted %d:%02d", secs / 60, secs % 60);
			m->call_info = buf;
		} else {
			m->call_info = "started a call";
		}
	}
	return m;
}

MessagePtr Store::add_message(MessagePtr m)
{
	ChannelMessages &cm = messages[m->channel_id];
	auto &v = cm.msgs;
	// Replace a pending local echo with the same nonce.
	if (!m->nonce.empty()) {
		for (auto &x : v)
			if (x->state != Message::Sent && x->nonce == m->nonce) {
				x = m;
				std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a->id < b->id; });
				return m;
			}
	}
	auto it = std::lower_bound(v.begin(), v.end(), m->id, [](const MessagePtr &a, Snowflake id) { return a->id < id; });
	if (it != v.end() && (*it)->id == m->id) {
		*it = m;
		return m;
	}
	v.insert(it, m);
	if (auto c = channels.find(m->channel_id); c != channels.end() && m->state == Message::Sent &&
	    m->id > c->second.last_message_id)
		c->second.last_message_id = m->id;
	return m;
}

MessagePtr Store::find_message(Snowflake channel_id, Snowflake id)
{
	auto it = messages.find(channel_id);
	if (it == messages.end())
		return nullptr;
	auto &v = it->second.msgs;
	auto p = std::lower_bound(v.begin(), v.end(), id, [](const MessagePtr &a, Snowflake x) { return a->id < x; });
	return (p != v.end() && (*p)->id == id) ? *p : nullptr;
}

bool Store::remove_message(Snowflake channel_id, Snowflake id)
{
	auto it = messages.find(channel_id);
	if (it == messages.end())
		return false;
	auto &v = it->second.msgs;
	auto p = std::lower_bound(v.begin(), v.end(), id, [](const MessagePtr &a, Snowflake x) { return a->id < x; });
	if (p == v.end() || (*p)->id != id)
		return false;
	v.erase(p);
	return true;
}

void Store::trim_messages(Snowflake keep_channel)
{
	// Keep at most 30 channels' worth of history; others are refetched on demand.
	if (messages.size() <= 30)
		return;
	std::vector<std::pair<int64_t, Snowflake>> lru;
	for (auto &[id, cm] : messages)
		if (id != keep_channel)
			lru.push_back({cm.last_access, id});
	std::sort(lru.begin(), lru.end());
	for (size_t i = 0; i + 30 < messages.size() && i < lru.size(); i++)
		messages.erase(lru[i].second);
}

// --- lookups ----------------------------------------------------------------

const User *Store::user(Snowflake id) const
{
	auto it = users.find(id);
	return it == users.end() ? nullptr : &it->second;
}

const Channel *Store::channel(Snowflake id) const
{
	auto it = channels.find(id);
	return it == channels.end() ? nullptr : &it->second;
}

Guild *Store::guild(Snowflake id)
{
	auto it = guilds.find(id);
	return it == guilds.end() ? nullptr : &it->second;
}

const Member *Store::member(Snowflake gid, Snowflake uid) const
{
	auto g = guilds.find(gid);
	if (g == guilds.end())
		return nullptr;
	auto m = g->second.members.find(uid);
	return m == g->second.members.end() ? nullptr : &m->second;
}

std::string Store::display_name(Snowflake gid, Snowflake uid) const
{
	if (gid)
		if (const Member *m = member(gid, uid); m && !m->nick.empty())
			return m->nick;
	if (auto r = relationships.find(uid); r != relationships.end() && !r->second.nickname.empty())
		return r->second.nickname;
	if (const User *u = user(uid))
		return u->display();
	return "unknown-user";
}

uint32_t Store::name_color(Snowflake gid, Snowflake uid) const
{
	auto g = guilds.find(gid);
	if (g == guilds.end())
		return 0;
	const Member *m = member(gid, uid);
	if (!m)
		return 0;
	const Role *best = nullptr;
	for (Snowflake rid : m->roles) {
		auto r = g->second.roles.find(rid);
		if (r == g->second.roles.end() || !r->second.color)
			continue;
		if (!best || r->second.position > best->position)
			best = &r->second;
	}
	return best ? best->color : 0;
}

std::string Store::channel_title(const Channel &c) const
{
	if (c.type == CH_DM) {
		if (!c.recipients.empty())
			return display_name(0, c.recipients[0]);
		return "DM";
	}
	if (c.type == CH_GROUP_DM) {
		if (!c.name.empty())
			return c.name;
		std::string s;
		for (Snowflake r : c.recipients) {
			if (!s.empty())
				s += ", ";
			s += display_name(0, r);
		}
		return s.empty() ? "Group DM" : s;
	}
	return c.name;
}

// --- permissions --------------------------------------------------------------

uint64_t Store::base_permissions(Snowflake gid) const
{
	auto gi = guilds.find(gid);
	if (gi == guilds.end())
		return 0;
	const Guild &g = gi->second;
	if (g.owner_id == me.id)
		return perm::ALL;
	uint64_t p = 0;
	if (auto e = g.roles.find(gid); e != g.roles.end())
		p = e->second.permissions;
	if (const Member *m = member(gid, me.id))
		for (Snowflake r : m->roles)
			if (auto ri = g.roles.find(r); ri != g.roles.end())
				p |= ri->second.permissions;
	if (p & perm::ADMINISTRATOR)
		return perm::ALL;
	return p;
}

uint64_t Store::permissions(const Channel &c0) const
{
	if (c0.is_private() || !c0.guild_id)
		return perm::ALL;
	const Channel *c = &c0;
	if (c0.is_thread())
		if (const Channel *p = channel(c0.parent_id))
			c = p;
	uint64_t p = base_permissions(c->guild_id);
	if (p == perm::ALL)
		return p;
	const Member *m = member(c->guild_id, me.id);
	for (auto &o : c->overwrites)
		if (o.type == 0 && o.id == c->guild_id) {
			p &= ~o.deny;
			p |= o.allow;
		}
	uint64_t allow = 0, deny = 0;
	if (m)
		for (auto &o : c->overwrites)
			if (o.type == 0 && std::find(m->roles.begin(), m->roles.end(), o.id) != m->roles.end()) {
				allow |= o.allow;
				deny |= o.deny;
			}
	p &= ~deny;
	p |= allow;
	for (auto &o : c->overwrites)
		if (o.type == 1 && o.id == me.id) {
			p &= ~o.deny;
			p |= o.allow;
		}
	if (!(p & perm::VIEW_CHANNEL))
		return 0;
	if (c->is_voice() && !(p & perm::CONNECT))
		p &= ~(perm::SEND_MESSAGES);
	return p;
}

bool Store::can_view(const Channel &c) const
{
	return (permissions(c) & perm::VIEW_CHANNEL) != 0;
}

// --- unread / mute --------------------------------------------------------------

bool Store::is_guild_muted(Snowflake gid) const
{
	auto it = notify.find(gid);
	return it != notify.end() && it->second.muted;
}

bool Store::is_muted(const Channel &c) const
{
	auto it = notify.find(c.id);
	if (it != notify.end() && it->second.muted)
		return true;
	if (c.parent_id) {
		auto p = notify.find(c.parent_id);
		if (p != notify.end() && p->second.muted)
			return true;
	}
	return false;
}

bool Store::is_unread(const Channel &c) const
{
	if (!c.last_message_id || c.type == CH_CATEGORY)
		return false;
	auto it = read_states.find(c.id);
	if (it == read_states.end())
		return false;
	return c.last_message_id > it->second.last_message_id;
}

int Store::mention_count(Snowflake cid) const
{
	auto it = read_states.find(cid);
	return it == read_states.end() ? 0 : it->second.mention_count;
}

bool Store::guild_unread(Snowflake gid) const
{
	auto gi = guilds.find(gid);
	if (gi == guilds.end() || is_guild_muted(gid))
		return false;
	for (Snowflake cid : gi->second.channels) {
		const Channel *c = channel(cid);
		if (c && !c->is_voice() && is_unread(*c) && !is_muted(*c) && can_view(*c))
			return true;
	}
	return false;
}

int Store::guild_mentions(Snowflake gid) const
{
	auto gi = guilds.find(gid);
	if (gi == guilds.end())
		return 0;
	int n = 0;
	for (Snowflake cid : gi->second.channels)
		n += mention_count(cid);
	for (Snowflake cid : gi->second.threads)
		n += mention_count(cid);
	return n;
}

bool Store::mentions_me(const Message &m) const
{
	if (std::find(m.mentions.begin(), m.mentions.end(), me.id) != m.mentions.end())
		return true;
	if (m.mention_everyone)
		return true;
	if (m.guild_id && !m.mention_roles.empty())
		if (const Member *mem = member(m.guild_id, me.id))
			for (Snowflake r : m.mention_roles)
				if (std::find(mem->roles.begin(), mem->roles.end(), r) != mem->roles.end())
					return true;
	return false;
}

// --- ordering -----------------------------------------------------------------

static bool chan_less(const Channel *a, const Channel *b)
{
	bool av = a->is_voice(), bv = b->is_voice();
	if (av != bv)
		return !av;
	if (a->position != b->position)
		return a->position < b->position;
	return a->id < b->id;
}

std::vector<Store::ChannelNode> Store::channel_tree(Snowflake gid) const
{
	std::vector<ChannelNode> out;
	auto gi = guilds.find(gid);
	if (gi == guilds.end())
		return out;
	std::vector<const Channel *> top, cats;
	std::unordered_map<Snowflake, std::vector<const Channel *>> kids;
	for (Snowflake cid : gi->second.channels) {
		const Channel *c = channel(cid);
		if (!c)
			continue;
		if (c->type == CH_CATEGORY) {
			cats.push_back(c);
			continue;
		}
		if (!c->is_text() && c->type != CH_FORUM && c->type != CH_MEDIA)
			continue;
		if (!can_view(*c))
			continue;
		if (c->parent_id && channel(c->parent_id))
			kids[c->parent_id].push_back(c);
		else
			top.push_back(c);
	}
	std::sort(top.begin(), top.end(), chan_less);
	std::sort(cats.begin(), cats.end(), [](auto *a, auto *b) {
		return a->position != b->position ? a->position < b->position : a->id < b->id;
	});
	for (auto *c : top)
		out.push_back({c, {}});
	for (auto *cat : cats) {
		auto &k = kids[cat->id];
		if (k.empty())
			continue;
		std::sort(k.begin(), k.end(), chan_less);
		out.push_back({cat, k});
	}
	return out;
}

std::vector<const Channel *> Store::private_channels_sorted() const
{
	std::vector<const Channel *> v;
	for (auto &[id, c] : channels)
		if (c.is_private())
			v.push_back(&c);
	std::sort(v.begin(), v.end(), [](auto *a, auto *b) {
		Snowflake la = a->last_message_id ? a->last_message_id : a->id;
		Snowflake lb = b->last_message_id ? b->last_message_id : b->id;
		return la > lb;
	});
	return v;
}

std::vector<VoiceState> Store::voice_members(Snowflake cid) const
{
	std::vector<VoiceState> v;
	const Channel *c = channel(cid);
	Snowflake gid = c ? c->guild_id : 0;
	auto it = voice.find(gid);
	if (it == voice.end())
		return v;
	for (auto &[uid, s] : it->second)
		if (s.channel_id == cid)
			v.push_back(s);
	std::sort(v.begin(), v.end(), [this, gid](auto &a, auto &b) {
		return display_name(gid, a.user_id) < display_name(gid, b.user_id);
	});
	return v;
}

const VoiceState *Store::my_voice_state() const
{
	for (auto &[gid, m] : voice) {
		auto it = m.find(me.id);
		if (it != m.end() && it->second.session_id == session_id)
			return &it->second;
	}
	return nullptr;
}

// --- CDN ------------------------------------------------------------------------

static std::string cdn()
{
	static std::string base = getenv("DCORD_CDN") ? getenv("DCORD_CDN") : "https://cdn.discordapp.com";
	return base;
}

std::string avatar_url(const User &u, int size)
{
	if (u.avatar.empty()) {
		int idx = u.discriminator ? u.discriminator % 5 : (int)((u.id >> 22) % 6);
		return cdn() + "/embed/avatars/" + std::to_string(idx) + ".png";
	}
	return cdn() + "/avatars/" + sf_str(u.id) + "/" + u.avatar + ".png?size=" + std::to_string(size);
}

std::string guild_icon_url(const Guild &g, int size)
{
	if (g.icon.empty())
		return "";
	return cdn() + "/icons/" + sf_str(g.id) + "/" + g.icon + ".png?size=" + std::to_string(size);
}

std::string emoji_url(Snowflake id, bool animated, int size)
{
	return cdn() + "/emojis/" + sf_str(id) + (animated ? ".gif" : ".png") + "?size=" + std::to_string(size);
}
