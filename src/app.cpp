#include "app.h"

#include <algorithm>
#include <unistd.h>

#include <glib.h>

#include "api.h"
#include "ui.h"
#include "voice.h"

Settings g_settings;
Gateway g_gateway;

static std::string settings_path()
{
	return config_dir() + "/settings.json";
}

void settings_load()
{
	bool ok;
	std::string s = read_file(settings_path(), &ok);
	if (!ok)
		return;
	json j = json::parse(s, nullptr, false);
	if (!j.is_object())
		return;
	Settings &st = g_settings;
	st.animate_gifs = jbool(j, "animate_gifs", st.animate_gifs);
	st.show_avatars = jbool(j, "show_avatars", st.show_avatars);
	st.compact = jbool(j, "compact", st.compact);
	st.show_member_list = jbool(j, "show_member_list", st.show_member_list);
	st.ptt = jbool(j, "ptt", st.ptt);
	st.ptt_key = jstr(j, "ptt_key", st.ptt_key);
	if (j.contains("vad_threshold_db") && j["vad_threshold_db"].is_number())
		st.vad_threshold_db = j["vad_threshold_db"].get<double>();
	if (j.contains("input_gain") && j["input_gain"].is_number())
		st.input_gain = j["input_gain"].get<double>();
	if (j.contains("output_volume") && j["output_volume"].is_number())
		st.output_volume = j["output_volume"].get<double>();
	st.noise_gate = jbool(j, "noise_gate", st.noise_gate);
	st.input_device = jstr(j, "input_device");
	st.output_device = jstr(j, "output_device");
	if (j.contains("user_volumes") && j["user_volumes"].is_object())
		st.user_volumes = j["user_volumes"];
	if (j.contains("window") && j["window"].is_object())
		st.window = j["window"];
	if (j.contains("tabs") && j["tabs"].is_array())
		st.tabs = j["tabs"];
	if (j.contains("collapsed") && j["collapsed"].is_array())
		st.collapsed = j["collapsed"];
}

void settings_save()
{
	const Settings &st = g_settings;
	json j = {
		{"animate_gifs", st.animate_gifs}, {"show_avatars", st.show_avatars}, {"compact", st.compact},
		{"show_member_list", st.show_member_list}, {"ptt", st.ptt}, {"ptt_key", st.ptt_key},
		{"vad_threshold_db", st.vad_threshold_db}, {"input_gain", st.input_gain},
		{"output_volume", st.output_volume}, {"noise_gate", st.noise_gate},
		{"input_device", st.input_device}, {"output_device", st.output_device},
		{"user_volumes", st.user_volumes}, {"window", st.window}, {"tabs", st.tabs},
		{"collapsed", st.collapsed},
	};
	mkdirs(config_dir());
	write_file(settings_path(), j.dump(1, '\t') + "\n");
}

static std::string token_path()
{
	return config_dir() + "/token";
}

std::string token_load()
{
	if (const char *t = getenv("DCORD_TOKEN"); t && *t)
		return t;
	return trim(read_file(token_path()));
}

void token_save(const std::string &t)
{
	mkdirs(config_dir());
	write_file(token_path(), t + "\n", 0600);
}

void token_clear()
{
	unlink(token_path().c_str());
}

// --- gateway helpers ------------------------------------------------------------

static std::set<Snowflake> g_subscribed;

void app_subscribe_guild(Snowflake gid)
{
	if (!gid || g_subscribed.count(gid))
		return;
	g_subscribed.insert(gid);
	json sub = {{"typing", true}, {"threads", true}, {"activities", false}, {"member_updates", false},
		    {"thread_member_lists", json::array()}, {"members", json::array()}, {"channels", json::object()}};
	g_gateway.send(37, {{"subscriptions", {{sf_str(gid), sub}}}});
}

void app_request_member_list(Snowflake gid, Snowflake cid)
{
	if (!gid || !cid)
		return;
	g_subscribed.insert(gid);
	json sub = {{"typing", true}, {"threads", true}, {"activities", true}, {"member_updates", false},
		    {"thread_member_lists", json::array()}, {"members", json::array()},
		    {"channels", {{sf_str(cid), {{0, 99}}}}}};
	g_gateway.send(37, {{"subscriptions", {{sf_str(gid), sub}}}});
}

void app_request_members(Snowflake gid, const std::vector<Snowflake> &users)
{
	Guild *g = g_store.guild(gid);
	if (!g)
		return;
	json ids = json::array();
	for (Snowflake u : users) {
		if (g->members.count(u) || g->requested_members.count(u))
			continue;
		g->requested_members.insert(u);
		ids.push_back(sf_str(u));
		if (ids.size() == 100) {
			g_gateway.send(8, {{"guild_id", {sf_str(gid)}}, {"user_ids", ids}, {"presences", false}});
			ids = json::array();
		}
	}
	if (!ids.empty())
		g_gateway.send(8, {{"guild_id", {sf_str(gid)}}, {"user_ids", ids}, {"presences", false}});
}

void app_voice_join(Snowflake gid, Snowflake cid)
{
	voice_join(gid, cid);
}

void app_voice_leave()
{
	voice_leave();
}

void app_voice_set_self(bool mute, bool deaf)
{
	voice_set_mute(mute);
	voice_set_deaf(deaf);
}

static std::unordered_map<Snowflake, guint> g_ack_timers;

void app_mark_read(Snowflake cid)
{
	const Channel *c = g_store.channel(cid);
	if (!c || !c->last_message_id)
		return;
	ReadState &rs = g_store.read_states[cid];
	if (rs.last_message_id >= c->last_message_id && rs.mention_count == 0)
		return;
	rs.last_message_id = c->last_message_id;
	rs.mention_count = 0;
	ui_unread_changed(cid);
	// Debounce: the official client acks at most every few seconds per channel.
	if (g_ack_timers.count(cid))
		return;
	auto *id = new Snowflake(cid);
	g_ack_timers[cid] = g_timeout_add(1500, [](gpointer p) -> gboolean {
		Snowflake cid = *static_cast<Snowflake *>(p);
		delete static_cast<Snowflake *>(p);
		g_ack_timers.erase(cid);
		const Channel *c = g_store.channel(cid);
		if (!c)
			return G_SOURCE_REMOVE;
		int flags = (c->guild_id ? 1 : 0) | (c->is_thread() ? 2 : 0);
		int64_t days = (now_ms() - 1420070400000LL) / 86400000LL;
		api_ack(cid, c->last_message_id, flags, days);
		return G_SOURCE_REMOVE;
	}, id);
}

// --- dispatch ---------------------------------------------------------------------

static Reaction *find_reaction(Message &m, const json &emoji)
{
	Snowflake id = jsf(emoji, "id");
	std::string name = jstr(emoji, "name");
	for (auto &r : m.reactions)
		if (id ? r.emoji_id == id : (r.emoji_id == 0 && r.name == name))
			return &r;
	return nullptr;
}

static void on_message_create(json &d)
{
	MessagePtr m = g_store.parse_message(d);
	Channel *c = nullptr;
	if (auto it = g_store.channels.find(m->channel_id); it != g_store.channels.end())
		c = &it->second;
	if (!c) {
		// A DM we haven't seen yet (new conversation); the channel create event may follow.
		if (!m->guild_id) {
			json cj = {{"id", sf_str(m->channel_id)}, {"type", 1}, {"recipient_ids", {sf_str(m->author_id)}}};
			g_store.upsert_channel(cj);
			c = &g_store.channels[m->channel_id];
			ui_tree_rebuild();
		} else {
			return;
		}
	}
	if (m->id > c->last_message_id)
		c->last_message_id = m->id;
	auto cm = g_store.messages.find(m->channel_id);
	if (cm != g_store.messages.end() && cm->second.loaded)
		g_store.add_message(m);
	bool mine = m->author_id == g_store.me.id;
	ReadState &rs = g_store.read_states[m->channel_id];
	bool important = false;
	if (mine) {
		rs.last_message_id = m->id;
		rs.mention_count = 0;
	} else {
		bool rel_blocked = false;
		if (auto r = g_store.relationships.find(m->author_id); r != g_store.relationships.end())
			rel_blocked = r->second.type == 2;
		if (!rel_blocked) {
			if (c->is_private() && !g_store.is_muted(*c)) {
				rs.mention_count++;
				important = true;
			} else if (g_store.mentions_me(*m)) {
				rs.mention_count++;
				important = true;
			}
		}
	}
	ui_message_created(m);
	ui_unread_changed(m->channel_id);
	if (important)
		ui_attention(true);
}

static void on_message_update(json &d)
{
	Snowflake cid = jsf(d, "channel_id"), id = jsf(d, "id");
	MessagePtr old = g_store.find_message(cid, id);
	if (!old)
		return;
	if (d.contains("author") && d.contains("content")) {
		MessagePtr m = g_store.parse_message(d);
		if (!m->ref && old->ref)
			m->ref = old->ref;
		if (!d.contains("reactions"))
			m->reactions = old->reactions;
		g_store.add_message(m);
		ui_message_updated(m);
	} else {
		// Partial update (embeds unfurled, etc.): merge into a copy.
		json full = d;
		MessagePtr m = std::make_shared<Message>(*old);
		if (d.contains("embeds")) {
			MessagePtr tmp = g_store.parse_message(json{{"id", sf_str(id)}, {"channel_id", sf_str(cid)}, {"embeds", d["embeds"]}});
			m->embeds = tmp->embeds;
		}
		if (d.contains("pinned"))
			m->pinned = jbool(d, "pinned");
		if (d.contains("flags"))
			m->flags = (int)jint(d, "flags");
		g_store.add_message(m);
		ui_message_updated(m);
	}
}

static void handle_dispatch(const std::string &t, json &d)
{
	Store &s = g_store;
	if (t == "READY") {
		s.load_ready(d);
		g_subscribed.clear();
		LOG("ready: %zu guilds, %zu channels, %zu users", s.guilds.size(), s.channels.size(), s.users.size());
		ui_ready();
	} else if (t == "READY_SUPPLEMENTAL") {
		s.load_ready_supplemental(d);
		for (auto &[gid, g] : s.guilds)
			ui_voice_states_changed(gid);
	} else if (t == "RESUMED") {
	} else if (t == "MESSAGE_CREATE") {
		on_message_create(d);
	} else if (t == "MESSAGE_UPDATE") {
		on_message_update(d);
	} else if (t == "MESSAGE_DELETE") {
		Snowflake cid = jsf(d, "channel_id"), id = jsf(d, "id");
		if (s.remove_message(cid, id))
			ui_message_deleted(cid, id);
	} else if (t == "MESSAGE_DELETE_BULK") {
		Snowflake cid = jsf(d, "channel_id");
		for (auto &x : jget(d, "ids"))
			if (s.remove_message(cid, sf_parse(x)))
				ui_message_deleted(cid, sf_parse(x));
	} else if (t == "MESSAGE_REACTION_ADD" || t == "MESSAGE_REACTION_REMOVE") {
		MessagePtr m = s.find_message(jsf(d, "channel_id"), jsf(d, "message_id"));
		if (!m)
			return;
		bool add = t == "MESSAGE_REACTION_ADD";
		bool me = jsf(d, "user_id") == s.me.id;
		const json &e = jget(d, "emoji");
		Reaction *r = find_reaction(*m, e);
		if (add) {
			if (!r) {
				Reaction nr;
				nr.name = jstr(e, "name");
				nr.emoji_id = jsf(e, "id");
				nr.animated = jbool(e, "animated");
				m->reactions.push_back(nr);
				r = &m->reactions.back();
			}
			r->count++;
			if (me)
				r->me = true;
		} else if (r) {
			r->count--;
			if (me)
				r->me = false;
			if (r->count <= 0)
				m->reactions.erase(m->reactions.begin() + (r - m->reactions.data()));
		}
		ui_message_updated(m);
	} else if (t == "MESSAGE_REACTION_REMOVE_ALL" || t == "MESSAGE_REACTION_REMOVE_EMOJI") {
		MessagePtr m = s.find_message(jsf(d, "channel_id"), jsf(d, "message_id"));
		if (!m)
			return;
		if (t == "MESSAGE_REACTION_REMOVE_ALL")
			m->reactions.clear();
		else if (Reaction *r = find_reaction(*m, jget(d, "emoji")))
			m->reactions.erase(m->reactions.begin() + (r - m->reactions.data()));
		ui_message_updated(m);
	} else if (t == "MESSAGE_ACK") {
		Snowflake cid = jsf(d, "channel_id");
		ReadState &rs = s.read_states[cid];
		rs.last_message_id = jsf(d, "message_id");
		rs.mention_count = (int)jint(d, "mention_count", 0);
		ui_unread_changed(cid);
	} else if (t == "CHANNEL_CREATE" || t == "CHANNEL_UPDATE" || t == "THREAD_CREATE" || t == "THREAD_UPDATE") {
		Channel &c = s.upsert_channel(d);
		if (c.guild_id)
			ui_guild_rebuild(c.guild_id);
		else
			ui_tree_rebuild();
		ui_channel_updated(c.id);
	} else if (t == "CHANNEL_DELETE" || t == "THREAD_DELETE") {
		Snowflake id = jsf(d, "id");
		const Channel *c = s.channel(id);
		Snowflake gid = c ? c->guild_id : 0;
		s.remove_channel(id);
		if (gid)
			ui_guild_rebuild(gid);
		else
			ui_tree_rebuild();
		ui_channel_updated(id);
	} else if (t == "THREAD_LIST_SYNC") {
		Snowflake gid = jsf(d, "guild_id");
		for (auto &th : jget(d, "threads"))
			s.upsert_channel(th, gid);
		ui_guild_rebuild(gid);
	} else if (t == "CHANNEL_RECIPIENT_ADD" || t == "CHANNEL_RECIPIENT_REMOVE") {
		auto it = s.channels.find(jsf(d, "channel_id"));
		if (it == s.channels.end())
			return;
		Snowflake uid = s.upsert_user(jget(d, "user")).id;
		auto &r = it->second.recipients;
		std::erase(r, uid);
		if (t == "CHANNEL_RECIPIENT_ADD")
			r.push_back(uid);
		ui_tree_rebuild();
	} else if (t == "GUILD_CREATE") {
		Snowflake gid = jsf(d, "id");
		s.load_guild(d);
		if (std::find(s.guild_order.begin(), s.guild_order.end(), gid) == s.guild_order.end())
			s.guild_order.push_back(gid);
		ui_tree_rebuild();
	} else if (t == "GUILD_UPDATE") {
		s.update_guild(d);
		ui_tree_rebuild();
	} else if (t == "GUILD_DELETE") {
		Snowflake gid = jsf(d, "id");
		if (jbool(d, "unavailable")) {
			if (Guild *g = s.guild(gid))
				g->unavailable = true;
		} else {
			s.remove_guild(gid);
		}
		ui_tree_rebuild();
	} else if (t == "GUILD_ROLE_CREATE" || t == "GUILD_ROLE_UPDATE") {
		Snowflake gid = jsf(d, "guild_id");
		s.upsert_role(gid, jget(d, "role"));
		ui_guild_rebuild(gid);
		ui_members_changed(gid);
	} else if (t == "GUILD_ROLE_DELETE") {
		Snowflake gid = jsf(d, "guild_id");
		if (Guild *g = s.guild(gid))
			g->roles.erase(jsf(d, "role_id"));
		ui_guild_rebuild(gid);
	} else if (t == "GUILD_MEMBER_UPDATE") {
		Snowflake gid = jsf(d, "guild_id");
		s.upsert_member(gid, d);
		if (jsf(jget(d, "user"), "id") == s.me.id)
			ui_guild_rebuild(gid);
	} else if (t == "GUILD_MEMBERS_CHUNK") {
		Snowflake gid = jsf(d, "guild_id");
		for (auto &m : jget(d, "members"))
			s.upsert_member(gid, m);
		ui_members_changed(gid);
	} else if (t == "GUILD_MEMBER_LIST_UPDATE") {
		Snowflake gid = jsf(d, "guild_id");
		for (auto &op : jget(d, "ops")) {
			for (auto &it : jget(op, "items"))
				if (const json &m = jget(it, "member"); m.is_object())
					s.upsert_member(gid, m);
			if (const json &m = jget(jget(op, "item"), "member"); m.is_object())
				s.upsert_member(gid, m);
		}
		ui_member_list_update(gid, d);
	} else if (t == "GUILD_EMOJIS_UPDATE") {
		if (Guild *g = s.guild(jsf(d, "guild_id"))) {
			g->emojis.clear();
			for (auto &e : jget(d, "emojis")) {
				Emoji x;
				x.id = jsf(e, "id");
				x.name = jstr(e, "name");
				x.animated = jbool(e, "animated");
				g->emojis[x.id] = x;
			}
		}
	} else if (t == "TYPING_START") {
		Snowflake gid = jsf(d, "guild_id");
		Snowflake uid = jsf(d, "user_id");
		if (const json &m = jget(d, "member"); m.is_object() && gid)
			s.upsert_member(gid, m, uid);
		if (uid != s.me.id)
			ui_typing(jsf(d, "channel_id"), uid);
	} else if (t == "VOICE_STATE_UPDATE") {
		s.update_voice_state(d);
		if (jsf(d, "user_id") == s.me.id)
			voice_on_state_update(d);
		ui_voice_states_changed(jsf(d, "guild_id"));
	} else if (t == "VOICE_SERVER_UPDATE") {
		voice_on_server_update(d);
	} else if (t == "USER_GUILD_SETTINGS_UPDATE") {
		s.parse_user_guild_settings(json::array({d}));
		ui_tree_rebuild();
	} else if (t == "USER_SETTINGS_PROTO_UPDATE") {
		const json &st = jget(d, "settings");
		if (jint(st, "type") == 1) {
			s.parse_settings_proto(jstr(st, "proto"));
			ui_tree_rebuild();
		}
	} else if (t == "USER_UPDATE") {
		s.upsert_user(d);
	} else if (t == "PASSIVE_UPDATE_V2" || t == "PASSIVE_UPDATE_V1") {
		Snowflake gid = jsf(d, "guild_id");
		for (auto &c : jget(d, t == "PASSIVE_UPDATE_V2" ? "updated_channels" : "channels")) {
			auto it = s.channels.find(jsf(c, "id"));
			if (it == s.channels.end())
				continue;
			Snowflake lm = jsf(c, "last_message_id");
			if (lm > it->second.last_message_id) {
				it->second.last_message_id = lm;
				ui_unread_changed(it->first);
			}
		}
		for (auto &m : jget(d, "updated_members"))
			s.upsert_member(gid, m);
		bool vc = false;
		for (auto v : jget(d, "updated_voice_states")) {
			v["guild_id"] = sf_str(gid);
			s.update_voice_state(v);
			vc = true;
		}
		for (auto &u : jget(d, "removed_voice_states")) {
			s.voice[gid].erase(sf_parse(u));
			vc = true;
		}
		if (t == "PASSIVE_UPDATE_V1")
			for (auto v : jget(d, "voice_states")) {
				v["guild_id"] = sf_str(gid);
				s.update_voice_state(v);
				vc = true;
			}
		if (vc)
			ui_voice_states_changed(gid);
	} else if (t == "PRESENCE_UPDATE") {
		s.presences[jsf(jget(d, "user"), "id")] = jstr(d, "status");
	} else if (t == "RELATIONSHIP_ADD" || t == "RELATIONSHIP_UPDATE") {
		Relationship r;
		r.id = jsf(d, "id");
		r.type = (int)jint(d, "type");
		r.nickname = jstr(d, "nickname");
		if (d.contains("user"))
			s.upsert_user(d["user"]);
		s.relationships[r.id] = r;
	} else if (t == "RELATIONSHIP_REMOVE") {
		s.relationships.erase(jsf(d, "id"));
	} else if (t == "CALL_CREATE" || t == "CALL_UPDATE") {
		Snowflake cid = jsf(d, "channel_id");
		bool ringing = false;
		for (auto &u : jget(d, "ringing"))
			if (sf_parse(u) == s.me.id)
				ringing = true;
		for (auto v : jget(d, "voice_states"))
			s.update_voice_state(v);
		ui_incoming_call(cid, ringing);
		ui_voice_states_changed(0);
		if (ringing)
			ui_attention(true);
	} else if (t == "CALL_DELETE") {
		ui_incoming_call(jsf(d, "channel_id"), false);
	}
}

void app_start(const std::string &token)
{
	api_set_token(token);
	g_gateway.on_dispatch = [](const std::string &t, json &d) { handle_dispatch(t, d); };
	g_gateway.on_state = [](Gateway::State st) { ui_conn_state((int)st); };
	g_gateway.on_fatal = [](int code, const std::string &why) {
		LOG("gateway fatal %d: %s", code, why.c_str());
		if (code == 4004)
			token_clear();
		voice_leave();
		ui_show_login(why);
	};
	ui_show_main();
	g_gateway.start(token);
}

void app_logout()
{
	voice_leave();
	g_gateway.stop();
	token_clear();
	g_store.clear();
	ui_show_login();
}
