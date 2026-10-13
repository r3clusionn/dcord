// Main window: server/channel tree, tabs, voice panel, member list.
#include <algorithm>
#include <cmath>
#include <unordered_set>

#include <cairo-gobject.h>

#include "app.h"
#include "ui.h"
#include "ui_internal.h"
#include "voice.h"

MainUI g_ui;

namespace {

enum Col { COL_ICON, COL_MARKUP, COL_KIND, COL_ID, COL_BADGE, COL_GUILD, N_COLS };
enum Kind { K_DMS, K_DM, K_FOLDER, K_GUILD, K_CATEGORY, K_TEXT, K_VOICE, K_VUSER, K_THREAD };

std::unordered_map<Snowflake, GtkTreeRowReference *> g_rows;  // guild/channel id -> row
GtkTreeRowReference *g_dms_row = nullptr;
std::unordered_set<Snowflake> g_expanded;   // expanded guilds/folders
std::unordered_set<Snowflake> g_collapsed;  // collapsed categories
bool g_building = false;
guint g_toast_timer = 0;
std::map<Snowflake, std::vector<json>> g_member_items;
static std::map<Snowflake, std::map<std::string, int64_t>> g_group_counts;
guint g_member_timer = 0;
Snowflake g_member_guild = 0, g_member_channel = 0;

const char *kDimC = "#949ba4";
const char *kUnreadC = "#f2f3f5";
const char *kMutedC = "#5c5e66";

std::string esc(const std::string &s)
{
	return markup_escape(s);
}

std::string badge(int n)
{
	if (n <= 0)
		return "";
	return "<span background='#f23f43' foreground='#ffffff' weight='bold' size='small'> " + std::to_string(n) + " </span>";
}

GtkTreeIter *row_iter(GtkTreeRowReference *ref, GtkTreeIter *out)
{
	if (!ref || !gtk_tree_row_reference_valid(ref))
		return nullptr;
	GtkTreePath *p = gtk_tree_row_reference_get_path(ref);
	bool ok = gtk_tree_model_get_iter(GTK_TREE_MODEL(g_ui.tree_store), out, p);
	gtk_tree_path_free(p);
	return ok ? out : nullptr;
}

void remember_row(Snowflake id, GtkTreeIter *it)
{
	GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(g_ui.tree_store), it);
	if (auto old = g_rows.find(id); old != g_rows.end())
		gtk_tree_row_reference_free(old->second);
	g_rows[id] = gtk_tree_row_reference_new(GTK_TREE_MODEL(g_ui.tree_store), p);
	gtk_tree_path_free(p);
}

// Small cached images in tree rows: update the row when they finish loading.
void set_row_image(GtkTreeIter *it, const std::string &url, bool circle, int size)
{
	if (url.empty())
		return;
	MediaOpts o;
	o.max_w = o.max_h = size;
	o.disk_cache = true;
	o.circle = circle;
	MediaPtr m = media_get(url, o);
	if (m->state == Media::Ready) {
		gtk_tree_store_set(g_ui.tree_store, it, COL_ICON, m->frame(), -1);
		return;
	}
	GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(g_ui.tree_store), it);
	GtkTreeRowReference *ref = gtk_tree_row_reference_new(GTK_TREE_MODEL(g_ui.tree_store), p);
	gtk_tree_path_free(p);
	m->add_waiter([m, ref]() {
		GtkTreeIter i;
		if (row_iter(ref, &i) && m->frame())
			gtk_tree_store_set(g_ui.tree_store, &i, COL_ICON, m->frame(), -1);
		gtk_tree_row_reference_free(ref);
	});
}

std::string channel_markup(const Channel &c)
{
	bool unread = g_store.is_unread(c);
	bool muted = g_store.is_muted(c);
	const char *col = muted ? kMutedC : unread ? kUnreadC : kDimC;
	std::string name = c.is_private() ? g_store.channel_title(c) : c.name;
	std::string m = "<span foreground='" + std::string(col) + "'" + (unread && !muted ? " weight='bold'" : "") + ">" + esc(name) + "</span>";
	if (c.type == CH_GROUP_DM)
		m += " <span size='small' foreground='" + std::string(kDimC) + "'>" + std::to_string(c.recipients.size() + 1) + "</span>";
	return m;
}

std::string guild_markup(const Guild &g)
{
	bool unread = g_store.guild_unread(g.id);
	bool muted = g_store.is_guild_muted(g.id);
	const char *col = muted ? kMutedC : unread ? kUnreadC : "#dbdee1";
	std::string name = g.unavailable ? g.name + " (unavailable)" : g.name;
	return "<span foreground='" + std::string(col) + "'" + (unread ? " weight='bold'" : "") + ">" + esc(name) + "</span>";
}

std::string user_status_dot(Snowflake uid)
{
	auto it = g_store.presences.find(uid);
	std::string st = it == g_store.presences.end() ? "offline" : it->second;
	const char *c = st == "online" ? "#23a55a" : st == "idle" ? "#f0b232" : st == "dnd" ? "#f23f43" : "#80848e";
	return "<span foreground='" + std::string(c) + "'>●</span> ";
}

void add_voice_users(GtkTreeIter *parent, const Channel &c)
{
	auto speaking = voice_speaking();
	for (const VoiceState &v : g_store.voice_members(c.id)) {
		GtkTreeIter it;
		gtk_tree_store_append(g_ui.tree_store, &it, parent);
		std::string name = g_store.display_name(c.guild_id, v.user_id);
		bool sp = speaking.count(v.user_id) > 0;
		std::string m = "<span foreground='" + std::string(sp ? "#23a55a" : "#b5bac1") + "'" + (sp ? " weight='bold'" : "") +
				">" + esc(name) + "</span>";
		std::string flags;
		if (v.self_deaf || v.deaf)
			flags += " deaf";
		else if (v.self_mute || v.mute || v.suppress)
			flags += " muted";
		if (v.self_stream)
			flags += " LIVE";
		if (v.self_video)
			flags += " video";
		if (!flags.empty())
			m += " <span size='small' foreground='" + std::string(kDimC) + "'>" + esc(flags) + "</span>";
		gtk_tree_store_set(g_ui.tree_store, &it, COL_MARKUP, m.c_str(), COL_KIND, K_VUSER, COL_ID, (guint64)v.user_id,
				   COL_GUILD, (guint64)c.id, -1);
		if (const User *u = g_store.user(v.user_id))
			set_row_image(&it, avatar_url(*u, 32), true, S(16));
	}
}

void add_channel_row(GtkTreeIter *parent, const Channel &c, Snowflake gid)
{
	GtkTreeIter it;
	gtk_tree_store_append(g_ui.tree_store, &it, parent);
	int kind = c.is_voice() ? K_VOICE : c.is_thread() ? K_THREAD : K_TEXT;
	const char *icon = c.is_voice() ? "speaker" : c.type == CH_NEWS ? "news" : c.is_thread() ? "thread" :
			   (c.type == CH_FORUM || c.type == CH_MEDIA) ? "forum" : "hash";
	gtk_tree_store_set(g_ui.tree_store, &it, COL_ICON, ui_icon(icon, S(16)), COL_MARKUP, channel_markup(c).c_str(),
			   COL_KIND, kind, COL_ID, (guint64)c.id, COL_BADGE, badge(g_store.mention_count(c.id)).c_str(),
			   COL_GUILD, (guint64)gid, -1);
	remember_row(c.id, &it);
	if (c.is_voice())
		add_voice_users(&it, c);
	// Joined, active threads under their parent.
	if (!c.is_thread() && gid)
		if (Guild *g = g_store.guild(gid))
			for (Snowflake tid : g->threads) {
				const Channel *t = g_store.channel(tid);
				if (t && t->parent_id == c.id && t->joined && !t->archived)
					add_channel_row(&it, *t, gid);
			}
}

void fill_guild(GtkTreeIter *git, Snowflake gid)
{
	GtkTreeIter child;
	while (gtk_tree_model_iter_children(GTK_TREE_MODEL(g_ui.tree_store), &child, git))
		gtk_tree_store_remove(g_ui.tree_store, &child);
	for (auto &node : g_store.channel_tree(gid)) {
		if (node.ch->type == CH_CATEGORY) {
			GtkTreeIter cit;
			gtk_tree_store_append(g_ui.tree_store, &cit, git);
			std::string m = "<span size='small' weight='bold' foreground='" + std::string(kDimC) + "'>" +
					esc(node.ch->name) + "</span>";
			for (auto &ch : m)
				(void)ch;
			gtk_tree_store_set(g_ui.tree_store, &cit, COL_MARKUP, m.c_str(), COL_KIND, K_CATEGORY,
					   COL_ID, (guint64)node.ch->id, COL_GUILD, (guint64)gid, -1);
			remember_row(node.ch->id, &cit);
			for (const Channel *c : node.children)
				add_channel_row(&cit, *c, gid);
		} else {
			add_channel_row(git, *node.ch, gid);
		}
	}
}

void add_guild_row(GtkTreeIter *parent, Snowflake gid)
{
	Guild *g = g_store.guild(gid);
	if (!g)
		return;
	GtkTreeIter it;
	gtk_tree_store_append(g_ui.tree_store, &it, parent);
	gtk_tree_store_set(g_ui.tree_store, &it, COL_ICON, ui_icon("guild", S(20)), COL_MARKUP, guild_markup(*g).c_str(),
			   COL_KIND, K_GUILD, COL_ID, (guint64)gid, COL_BADGE, badge(g_store.guild_mentions(gid)).c_str(),
			   COL_GUILD, (guint64)gid, -1);
	remember_row(gid, &it);
	set_row_image(&it, guild_icon_url(*g, 64), true, S(20));
	fill_guild(&it, gid);
}

void fill_dms(GtkTreeIter *root)
{
	GtkTreeIter child;
	while (gtk_tree_model_iter_children(GTK_TREE_MODEL(g_ui.tree_store), &child, root))
		gtk_tree_store_remove(g_ui.tree_store, &child);
	int total = 0;
	for (const Channel *c : g_store.private_channels_sorted()) {
		GtkTreeIter it;
		gtk_tree_store_append(g_ui.tree_store, &it, root);
		int mc = g_store.mention_count(c->id);
		total += mc;
		std::string mk = (c->type == CH_DM && !c->recipients.empty() ? user_status_dot(c->recipients[0]) : "") + channel_markup(*c);
		gtk_tree_store_set(g_ui.tree_store, &it, COL_ICON, ui_icon("at", S(20)), COL_MARKUP, mk.c_str(),
				   COL_KIND, K_DM, COL_ID, (guint64)c->id, COL_BADGE, badge(mc).c_str(), -1);
		remember_row(c->id, &it);
		if (c->type == CH_DM && !c->recipients.empty()) {
			if (const User *u = g_store.user(c->recipients[0]))
				set_row_image(&it, avatar_url(*u, 64), true, S(20));
		} else if (!c->icon.empty()) {
			set_row_image(&it, "https://cdn.discordapp.com/channel-icons/" + sf_str(c->id) + "/" + c->icon + ".png?size=64", true, S(20));
		}
		// Ongoing DM call participants
		auto vit = g_store.voice.find(0);
		if (vit != g_store.voice.end()) {
			bool any = false;
			for (auto &[u, v] : vit->second)
				any |= v.channel_id == c->id;
			if (any)
				add_voice_users(&it, *c);
		}
	}
	std::string m = "<b>Direct Messages</b>";
	gtk_tree_store_set(g_ui.tree_store, root, COL_MARKUP, m.c_str(), COL_BADGE, badge(total).c_str(), -1);
}

void restore_expansion()
{
	GtkTreeView *tv = GTK_TREE_VIEW(g_ui.tree);
	GtkTreeIter it;
	if (row_iter(g_dms_row, &it) && g_expanded.count(1)) {
		GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(g_ui.tree_store), &it);
		gtk_tree_view_expand_row(tv, p, FALSE);
		gtk_tree_path_free(p);
	}
	// Expand guilds (and folders) the user had open; categories open unless collapsed.
	std::function<void(GtkTreeIter *)> walk = [&](GtkTreeIter *parent) {
		GtkTreeIter c;
		if (!gtk_tree_model_iter_children(GTK_TREE_MODEL(g_ui.tree_store), &c, parent))
			return;
		do {
			int kind;
			guint64 id;
			gtk_tree_model_get(GTK_TREE_MODEL(g_ui.tree_store), &c, COL_KIND, &kind, COL_ID, &id, -1);
			bool expand = (kind == K_GUILD || kind == K_FOLDER) ? g_expanded.count(id) > 0 :
				      kind == K_CATEGORY ? !g_collapsed.count(id) : kind == K_TEXT || kind == K_VOICE;
			if (expand) {
				GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(g_ui.tree_store), &c);
				gtk_tree_view_expand_row(tv, p, FALSE);
				gtk_tree_path_free(p);
				walk(&c);
			}
		} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(g_ui.tree_store), &c));
	};
	walk(nullptr);
}

void save_expansion()
{
	json e = json::array(), c = json::array();
	for (Snowflake id : g_expanded)
		e.push_back(sf_str(id));
	for (Snowflake id : g_collapsed)
		c.push_back(sf_str(id));
	g_settings.window["expanded"] = e;
	g_settings.collapsed = c;
}

void rebuild_tree()
{
	g_building = true;
	for (auto &[id, r] : g_rows)
		gtk_tree_row_reference_free(r);
	g_rows.clear();
	if (g_dms_row)
		gtk_tree_row_reference_free(g_dms_row);
	g_dms_row = nullptr;
	gtk_tree_store_clear(g_ui.tree_store);

	GtkTreeIter dms;
	gtk_tree_store_append(g_ui.tree_store, &dms, nullptr);
	gtk_tree_store_set(g_ui.tree_store, &dms, COL_ICON, ui_icon("at", S(20)), COL_KIND, K_DMS, COL_ID, (guint64)1, -1);
	GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(g_ui.tree_store), &dms);
	g_dms_row = gtk_tree_row_reference_new(GTK_TREE_MODEL(g_ui.tree_store), p);
	gtk_tree_path_free(p);
	fill_dms(&dms);

	std::unordered_set<Snowflake> placed;
	auto &folders = g_store.folders;
	for (auto &f : folders) {
		if (f.id && f.guild_ids.size() > 0) {
			GtkTreeIter fit;
			gtk_tree_store_append(g_ui.tree_store, &fit, nullptr);
			std::string name = f.name.empty() ? "Folder" : f.name;
			std::string col = f.color ? color_hex(f.color) : "#dbdee1";
			std::string m = "<span foreground='" + col + "' weight='bold'>" + esc(name) + "</span>";
			gtk_tree_store_set(g_ui.tree_store, &fit, COL_ICON, ui_icon("folder", S(20)), COL_MARKUP, m.c_str(),
					   COL_KIND, K_FOLDER, COL_ID, (guint64)f.id, -1);
			for (Snowflake gid : f.guild_ids)
				if (g_store.guild(gid) && placed.insert(gid).second)
					add_guild_row(&fit, gid);
		} else {
			for (Snowflake gid : f.guild_ids)
				if (g_store.guild(gid) && placed.insert(gid).second)
					add_guild_row(nullptr, gid);
		}
	}
	for (Snowflake gid : g_store.guild_order)
		if (placed.insert(gid).second)
			add_guild_row(nullptr, gid);
	restore_expansion();
	g_building = false;
}

void update_channel_row(Snowflake cid)
{
	const Channel *c = g_store.channel(cid);
	GtkTreeIter it;
	if (!c || !row_iter(g_rows.count(cid) ? g_rows[cid] : nullptr, &it))
		return;
	std::string mk = (c->type == CH_DM && !c->recipients.empty() ? user_status_dot(c->recipients[0]) : "") + channel_markup(*c);
	gtk_tree_store_set(g_ui.tree_store, &it, COL_MARKUP, mk.c_str(), COL_BADGE, badge(g_store.mention_count(cid)).c_str(), -1);
	if (c->guild_id) {
		if (Guild *g = g_store.guild(c->guild_id); g && row_iter(g_rows.count(g->id) ? g_rows[g->id] : nullptr, &it))
			gtk_tree_store_set(g_ui.tree_store, &it, COL_MARKUP, guild_markup(*g).c_str(),
					   COL_BADGE, badge(g_store.guild_mentions(g->id)).c_str(), -1);
	} else if (row_iter(g_dms_row, &it)) {
		int total = 0;
		for (const Channel *pc : g_store.private_channels_sorted())
			total += g_store.mention_count(pc->id);
		gtk_tree_store_set(g_ui.tree_store, &it, COL_BADGE, badge(total).c_str(), -1);
	}
}

// --- tree interaction ---------------------------------------------------------------

bool row_at(GdkEventButton *ev, GtkTreeIter *it)
{
	GtkTreePath *p = nullptr;
	if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(g_ui.tree), (int)ev->x, (int)ev->y, &p, nullptr, nullptr, nullptr))
		return false;
	bool ok = gtk_tree_model_get_iter(GTK_TREE_MODEL(g_ui.tree_store), it, p);
	gtk_tree_path_free(p);
	return ok;
}

void menu_add(GtkWidget *menu, const char *label, std::function<void()> fn)
{
	GtkWidget *it = gtk_menu_item_new_with_label(label);
	auto *f = new std::function<void()>(std::move(fn));
	g_signal_connect_data(it, "activate", CB(+[](GtkMenuItem *, gpointer p) {
		(*static_cast<std::function<void()> *>(p))();
	}), f, [](gpointer p, GClosure *) { delete static_cast<std::function<void()> *>(p); }, (GConnectFlags)0);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
}

void mark_guild_read(Snowflake gid)
{
	Guild *g = g_store.guild(gid);
	if (!g)
		return;
	json entries = json::array();
	for (Snowflake cid : g->channels) {
		const Channel *c = g_store.channel(cid);
		if (c && g_store.is_unread(*c)) {
			entries.push_back({{"channel_id", sf_str(cid)}, {"message_id", sf_str(c->last_message_id)}, {"read_state_type", 0}});
			ReadState &rs = g_store.read_states[cid];
			rs.last_message_id = c->last_message_id;
			rs.mention_count = 0;
		}
	}
	if (!entries.empty())
		api_call("POST", "/read-states/ack-bulk", {{"read_states", entries}}, nullptr);
	ui_guild_rebuild(gid);
}

gboolean tree_press(GtkWidget *, GdkEventButton *ev, gpointer)
{
	GtkTreeIter it;
	if (ev->type != GDK_BUTTON_PRESS && ev->type != GDK_2BUTTON_PRESS)
		return FALSE;
	if (!row_at(ev, &it))
		return FALSE;
	int kind;
	guint64 id, gid;
	gtk_tree_model_get(GTK_TREE_MODEL(g_ui.tree_store), &it, COL_KIND, &kind, COL_ID, &id, COL_GUILD, &gid, -1);
	bool new_tab = ev->button == 2 || (ev->button == 1 && (ev->state & GDK_CONTROL_MASK));
	if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1 && kind == K_VOICE) {
		app_voice_join(gid, id);
		return TRUE;
	}
	if (ev->button == 3) {
		GtkWidget *menu = gtk_menu_new();
		if (kind == K_TEXT || kind == K_DM || kind == K_THREAD || kind == K_VOICE) {
			menu_add(menu, "Open in new tab", [id]() { ui_open_channel(id, true); });
			menu_add(menu, "Mark as read", [id]() { app_mark_read(id); });
		}
		if (kind == K_VOICE)
			menu_add(menu, "Join voice", [gid, id]() { app_voice_join(gid, id); });
		if (kind == K_DM)
			menu_add(menu, "Start / join call", [id]() {
				app_voice_join(0, id);
				api_ring(id);
			});
		if (kind == K_GUILD)
			menu_add(menu, "Mark server as read", [id]() { mark_guild_read(id); });
		if (kind == K_VUSER) {
			Snowflake uid = id;
			double v = voice_user_volume(uid);
			std::string lbl = "Volume: " + std::to_string((int)lround(v * 100)) + "%";
			menu_add(menu, lbl.c_str(), []() {});
			for (int pct : {25, 50, 100, 150, 200})
				menu_add(menu, ("  set " + std::to_string(pct) + "%").c_str(), [uid, pct]() {
					voice_set_user_volume(uid, pct / 100.0);
				});
		}
		menu_add(menu, "Copy ID", [id]() {
			gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), sf_str(id).c_str(), -1);
		});
		gtk_widget_show_all(menu);
		gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
		return TRUE;
	}
	if (ev->type == GDK_BUTTON_PRESS && (ev->button == 1 || ev->button == 2)) {
		if (kind == K_TEXT || kind == K_DM || kind == K_THREAD || kind == K_VOICE) {
			ui_open_channel(id, new_tab);
			return FALSE;
		}
		if (kind == K_GUILD || kind == K_FOLDER || kind == K_CATEGORY || kind == K_DMS) {
			GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(g_ui.tree_store), &it);
			// Toggle on single click, like Ripcord.
			if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(g_ui.tree), p))
				gtk_tree_view_collapse_row(GTK_TREE_VIEW(g_ui.tree), p);
			else
				gtk_tree_view_expand_row(GTK_TREE_VIEW(g_ui.tree), p, FALSE);
			gtk_tree_path_free(p);
			return TRUE;
		}
		if (kind == K_VUSER) {
			ui_show_user_menu(0, id, (GdkEvent *)ev);
			return TRUE;
		}
	}
	return FALSE;
}

void on_row_expanded(GtkTreeView *, GtkTreeIter *it, GtkTreePath *, gpointer)
{
	if (g_building)
		return;
	int kind;
	guint64 id;
	gtk_tree_model_get(GTK_TREE_MODEL(g_ui.tree_store), it, COL_KIND, &kind, COL_ID, &id, -1);
	if (kind == K_GUILD || kind == K_FOLDER || kind == K_DMS)
		g_expanded.insert(id);
	if (kind == K_CATEGORY)
		g_collapsed.erase(id);
	if (kind == K_GUILD || kind == K_FOLDER || kind == K_CATEGORY) {
		// Children that are expanded by default.
		GtkTreeIter c;
		if (gtk_tree_model_iter_children(GTK_TREE_MODEL(g_ui.tree_store), &c, it))
			do {
				int k;
				guint64 cid;
				gtk_tree_model_get(GTK_TREE_MODEL(g_ui.tree_store), &c, COL_KIND, &k, COL_ID, &cid, -1);
				if ((k == K_CATEGORY && !g_collapsed.count(cid)) || k == K_VOICE || k == K_TEXT ||
				    ((k == K_GUILD) && g_expanded.count(cid))) {
					GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(g_ui.tree_store), &c);
					gtk_tree_view_expand_row(GTK_TREE_VIEW(g_ui.tree), p, FALSE);
					gtk_tree_path_free(p);
				}
			} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(g_ui.tree_store), &c));
	}
	save_expansion();
}

void on_row_collapsed(GtkTreeView *, GtkTreeIter *it, GtkTreePath *, gpointer)
{
	if (g_building)
		return;
	int kind;
	guint64 id;
	gtk_tree_model_get(GTK_TREE_MODEL(g_ui.tree_store), it, COL_KIND, &kind, COL_ID, &id, -1);
	if (kind == K_GUILD || kind == K_FOLDER || kind == K_DMS)
		g_expanded.erase(id);
	if (kind == K_CATEGORY)
		g_collapsed.insert(id);
	save_expansion();
}

// --- voice panel --------------------------------------------------------------------

void update_voice_panel()
{
	VoiceStatus st = voice_status();
	std::string m;
	switch (st.state) {
	case VoiceStatus::Idle:
		m = "";
		break;
	case VoiceStatus::Joining:
	case VoiceStatus::Connecting:
		m = "<span foreground='#f0b232'>Voice connecting…</span>";
		break;
	case VoiceStatus::Connected: {
		const Channel *c = g_store.channel(st.channel_id);
		Guild *g = g_store.guild(st.guild_id);
		std::string where = c ? (c->is_private() ? g_store.channel_title(*c) : c->name) : "?";
		if (g)
			where += " / " + g->name;
		m = "<span foreground='#23a55a' weight='bold'>Voice connected</span>";
		if (st.ping_ms >= 0)
			m += " <span size='small' foreground='" + std::string(kDimC) + "'>" + std::to_string(st.ping_ms) + " ms</span>";
		m += st.e2ee_ready ? " <span size='small' foreground='#23a55a'>E2EE</span>" : "";
		m += "\n<span size='small'>" + esc(where) + "</span>";
		break;
	}
	case VoiceStatus::Failed:
		m = "<span foreground='#f23f43'>Voice failed: " + esc(st.error) + "</span>";
		break;
	}
	gtk_label_set_markup(GTK_LABEL(g_ui.voice_label), m.c_str());
	bool active = st.state != VoiceStatus::Idle && st.state != VoiceStatus::Failed;
	GtkWidget *leave = (GtkWidget *)g_object_get_data(G_OBJECT(g_ui.voice_box), "leave");
	gtk_widget_set_visible(leave, active);
	gtk_widget_set_visible(g_ui.voice_label, !m.empty());
	g_signal_handlers_block_matched(g_ui.voice_mute, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, (gpointer)1);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_ui.voice_mute), voice_self_mute());
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_ui.voice_deaf), voice_self_deaf());
	g_signal_handlers_unblock_matched(g_ui.voice_mute, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, (gpointer)1);
}

void update_me_label()
{
	std::string m = "<b>" + esc(g_store.me.display()) + "</b>";
	if (!g_store.me.username.empty() && g_store.me.username != g_store.me.display())
		m += " <span size='small' foreground='" + std::string(kDimC) + "'>" + esc(g_store.me.username) + "</span>";
	gtk_label_set_markup(GTK_LABEL(g_ui.me_label), m.c_str());
}

GtkWidget *build_voice_panel()
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(2));
	gtk_style_context_add_class(gtk_widget_get_style_context(box), "voice-panel");
	g_ui.status = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(g_ui.status), 0);
	gtk_label_set_line_wrap(GTK_LABEL(g_ui.status), TRUE);
	gtk_widget_set_no_show_all(g_ui.status, TRUE);
	gtk_box_pack_start(GTK_BOX(box), g_ui.status, FALSE, FALSE, 0);
	g_ui.voice_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(g_ui.voice_label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(g_ui.voice_label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_no_show_all(g_ui.voice_label, TRUE);
	gtk_box_pack_start(GTK_BOX(box), g_ui.voice_label, FALSE, FALSE, 0);

	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(2));
	g_ui.me_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(g_ui.me_label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(g_ui.me_label), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start(GTK_BOX(row), g_ui.me_label, TRUE, TRUE, 0);
	g_ui.voice_mute = gtk_toggle_button_new_with_label("Mic");
	gtk_widget_set_tooltip_text(g_ui.voice_mute, "Mute microphone");
	g_signal_connect(g_ui.voice_mute, "toggled", CB(+[](GtkToggleButton *b, gpointer) {
		voice_set_mute(gtk_toggle_button_get_active(b));
	}), (gpointer)1);
	g_ui.voice_deaf = gtk_toggle_button_new_with_label("Deaf");
	gtk_widget_set_tooltip_text(g_ui.voice_deaf, "Deafen (mute sound and mic)");
	g_signal_connect(g_ui.voice_deaf, "toggled", CB(+[](GtkToggleButton *b, gpointer) {
		voice_set_deaf(gtk_toggle_button_get_active(b));
	}), (gpointer)1);
	GtkWidget *leave = gtk_button_new_with_label("Leave");
	gtk_widget_set_tooltip_text(leave, "Disconnect from voice");
	gtk_style_context_add_class(gtk_widget_get_style_context(leave), "destructive-action");
	g_signal_connect(leave, "clicked", CB(+[](GtkButton *, gpointer) { app_voice_leave(); }), nullptr);
	gtk_widget_set_no_show_all(leave, TRUE);
	g_object_set_data(G_OBJECT(box), "leave", leave);
	GtkWidget *set = gtk_button_new_with_label("⚙");
	gtk_widget_set_tooltip_text(set, "Settings");
	g_signal_connect(set, "clicked", CB(+[](GtkButton *, gpointer) { settings_dialog_show(); }), nullptr);
	for (GtkWidget *w : {g_ui.voice_mute, g_ui.voice_deaf, leave, set}) {
		gtk_button_set_relief(GTK_BUTTON(w), GTK_RELIEF_NONE);
		gtk_box_pack_start(GTK_BOX(row), w, FALSE, FALSE, 0);
	}
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
	g_ui.voice_box = box;
	return box;
}

// --- member list ---------------------------------------------------------------------

void render_member_list()
{
	g_member_timer = 0;
	GtkWidget *box = g_ui.members;
	gtk_container_foreach(GTK_CONTAINER(box), [](GtkWidget *w, gpointer) { gtk_widget_destroy(w); }, nullptr);
	auto add_label = [&](const std::string &markup, Snowflake uid, Snowflake gid) {
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(6));
		if (uid) {
			if (const User *u = g_store.user(uid)) {
				MediaOpts o;
				o.max_w = o.max_h = S(20);
				o.disk_cache = true;
				o.circle = true;
				gtk_box_pack_start(GTK_BOX(row), media_widget(media_get(avatar_url(*u, 64), o), S(20), S(20)), FALSE, FALSE, 0);
			}
		}
		GtkWidget *l = gtk_label_new(nullptr);
		gtk_label_set_markup(GTK_LABEL(l), markup.c_str());
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
		gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
		if (uid) {
			GtkWidget *eb = gtk_event_box_new();
			gtk_container_add(GTK_CONTAINER(eb), row);
			g_object_set_data(G_OBJECT(eb), "uid", (gpointer)new Snowflake(uid));
			g_signal_connect(eb, "button-press-event", CB(+[](GtkWidget *w, GdkEventButton *ev, gpointer gid) -> gboolean {
				Snowflake uid = *(Snowflake *)g_object_get_data(G_OBJECT(w), "uid");
				ui_show_user_menu((Snowflake)(uintptr_t)gid, uid, (GdkEvent *)ev);
				return TRUE;
			}), (gpointer)(uintptr_t)gid);
			g_signal_connect(eb, "destroy", CB(+[](GtkWidget *w, gpointer) {
				delete (Snowflake *)g_object_get_data(G_OBJECT(w), "uid");
			}), nullptr);
			gtk_box_pack_start(GTK_BOX(box), eb, FALSE, FALSE, 0);
		} else {
			gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
		}
	};
	ChatView *v = ui_current_view();
	const Channel *c = v ? g_store.channel(v->channel_id()) : nullptr;
	if (c && c->is_private()) {
		std::vector<Snowflake> people = c->recipients;
		people.push_back(g_store.me.id);
		add_label("<span size='small' weight='bold' foreground='#949ba4'>MEMBERS — " + std::to_string(people.size()) + "</span>", 0, 0);
		for (Snowflake u : people)
			add_label(user_status_dot(u) + esc(g_store.display_name(0, u)), u, 0);
	} else if (c) {
		auto it = g_member_items.find(c->guild_id);
		if (it != g_member_items.end()) {
			for (auto &item : it->second) {
				if (const json &grp = jget(item, "group"); grp.is_object()) {
					std::string id = jstr(grp, "id");
					std::string name = id == "online" ? "Online" : id == "offline" ? "Offline" : id;
					if (Guild *g = g_store.guild(c->guild_id))
						if (auto r = g->roles.find(sf_parse(id)); r != g->roles.end())
							name = r->second.name;
					int64_t cnt = jint(grp, "count");
					auto gc = g_group_counts.find(c->guild_id);
					if (gc != g_group_counts.end())
						if (auto n = gc->second.find(id); n != gc->second.end())
							cnt = n->second;
					add_label("<span size='small' weight='bold' foreground='#949ba4'>" + esc(name) + " — " +
						  std::to_string(cnt) + "</span>", 0, 0);
				} else if (const json &m = jget(item, "member"); m.is_object()) {
					Snowflake uid = jsf(jget(m, "user"), "id");
					std::string status = jstr(jget(m, "presence"), "status", "offline");
					g_store.presences[uid] = status;
					uint32_t col = g_store.name_color(c->guild_id, uid);
					std::string name = esc(g_store.display_name(c->guild_id, uid));
					if (col)
						name = "<span foreground='" + color_hex(col) + "'>" + name + "</span>";
					add_label(user_status_dot(uid) + name, uid, c->guild_id);
				}
			}
		} else {
			add_label("<span foreground='#949ba4'>Loading members…</span>", 0, 0);
		}
	}
	gtk_widget_show_all(box);
}

void schedule_member_render()
{
	if (!g_member_timer)
		g_member_timer = g_timeout_add(250, [](gpointer) -> gboolean {
			render_member_list();
			return G_SOURCE_REMOVE;
		}, nullptr);
}

// --- notebook -----------------------------------------------------------------------

GtkWidget *make_tab_label(ChatView *v)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(4));
	GtkWidget *l = gtk_label_new("");
	gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 18);
	g_object_set_data(G_OBJECT(box), "label", l);
	gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
	GtkWidget *x = gtk_button_new_with_label("✕");
	gtk_button_set_relief(GTK_BUTTON(x), GTK_RELIEF_NONE);
	gtk_style_context_add_class(gtk_widget_get_style_context(x), "tab-close");
	g_signal_connect(x, "clicked", CB(+[](GtkButton *, gpointer p) { ui_close_view(static_cast<ChatView *>(p)); }), v);
	gtk_box_pack_start(GTK_BOX(box), x, FALSE, FALSE, 0);
	GtkWidget *eb = gtk_event_box_new();
	gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), FALSE);
	gtk_container_add(GTK_CONTAINER(eb), box);
	g_object_set_data(G_OBJECT(eb), "label", l);
	g_signal_connect(eb, "button-press-event", CB(+[](GtkWidget *, GdkEventButton *ev, gpointer p) -> gboolean {
		if (ev->button == 2) {
			ui_close_view(static_cast<ChatView *>(p));
			return TRUE;
		}
		return FALSE;
	}), v);
	gtk_widget_show_all(eb);
	return eb;
}

void save_tabs()
{
	json t = json::array();
	for (ChatView *v : g_ui.views)
		t.push_back(sf_str(v->channel_id()));
	g_settings.tabs = t;
	int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_ui.notebook));
	g_settings.window["current_tab"] = cur;
}

void on_switch_page(GtkNotebook *, GtkWidget *page, guint, gpointer)
{
	for (ChatView *v : g_ui.views)
		if (v->widget() == page) {
			// on_shown runs after the page switch completes
			g_idle_add([](gpointer p) -> gboolean {
				ChatView *v = static_cast<ChatView *>(p);
				for (ChatView *x : g_ui.views)
					if (x == v) {
						v->on_shown();
						ui_select_in_tree(v->channel_id());
						ui_member_list_show(v->guild_id(), v->channel_id());
						ui_update_title();
					}
				return G_SOURCE_REMOVE;
			}, v);
		}
}

void apply_css()
{
	char css[8192];
	snprintf(css, sizeof css,
		"window, .sidebar, treeview.view { background-color: #2b2d31; color: #dbdee1; }"
		"treeview.view:selected { background-color: #404249; color: #ffffff; }"
		"treeview.view { padding: %dpx 0; }"
		"textview.chat-log, textview.chat-log text { background-color: #313338; color: #dbdee1; }"
		".chat-header { background-color: #313338; padding: %dpx %dpx; }"
		".composer-row { background-color: #313338; padding: %dpx %dpx %dpx %dpx; }"
		"textview.composer, textview.composer text { background-color: #383a40; color: #dbdee1; }"
		"textview.composer { border-radius: %dpx; }"
		".typing { background-color: #313338; color: #b5bac1; font-size: smaller; padding: 0 %dpx; min-height: %dpx; }"
		".reply-bar { background-color: #2b2d31; padding: %dpx %dpx; }"
		".attach-chip { background-color: #2b2d31; border-radius: %dpx; padding: %dpx; }"
		".voice-panel { background-color: #232428; padding: %dpx; }"
		".members { background-color: #2b2d31; padding: %dpx; }"
		"notebook header { background-color: #1e1f22; }"
		"notebook tab { padding: %dpx %dpx; min-height: 0; }"
		"button.tab-close { padding: 0 %dpx; min-height: 0; min-width: 0; }"
		"paned > separator { background-color: #1e1f22; }"
		".toast { background-color: #111214; color: #f2f3f5; border-radius: %dpx; padding: %dpx %dpx; }",
		S(2), S(4), S(10), S(4), S(8), S(8), S(8), S(6), S(12), S(16), S(4), S(10), S(4), S(4), S(6), S(6), S(3), S(6),
		S(2), S(6), S(8), S(12));
	GtkCssProvider *p = gtk_css_provider_new();
	gtk_css_provider_load_from_data(p, css, -1, nullptr);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
						  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(p);
}

void cell_icon_data(GtkTreeViewColumn *, GtkCellRenderer *cell, GtkTreeModel *model, GtkTreeIter *it, gpointer)
{
	cairo_surface_t *s = nullptr;
	gtk_tree_model_get(model, it, COL_ICON, &s, -1);
	g_object_set(cell, "surface", s, "visible", s != nullptr, nullptr);
	if (s)
		cairo_surface_destroy(s);
}

GtkWidget *build_tree()
{
	g_ui.tree_store = gtk_tree_store_new(N_COLS, CAIRO_GOBJECT_TYPE_SURFACE, G_TYPE_STRING, G_TYPE_INT, G_TYPE_UINT64,
					     G_TYPE_STRING, G_TYPE_UINT64);
	GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_ui.tree_store));
	g_ui.tree = tv;
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tv), FALSE);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(tv), FALSE);
	gtk_tree_view_set_level_indentation(GTK_TREE_VIEW(tv), S(4));
	GtkTreeViewColumn *col = gtk_tree_view_column_new();
	GtkCellRenderer *ri = gtk_cell_renderer_pixbuf_new();
	g_object_set(ri, "xpad", S(2), nullptr);
	gtk_tree_view_column_pack_start(col, ri, FALSE);
	gtk_tree_view_column_set_cell_data_func(col, ri, cell_icon_data, nullptr, nullptr);
	GtkCellRenderer *rt = gtk_cell_renderer_text_new();
	g_object_set(rt, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", S(1), nullptr);
	gtk_tree_view_column_pack_start(col, rt, TRUE);
	gtk_tree_view_column_add_attribute(col, rt, "markup", COL_MARKUP);
	GtkCellRenderer *rb = gtk_cell_renderer_text_new();
	g_object_set(rb, "xalign", 1.0, "xpad", S(4), nullptr);
	gtk_tree_view_column_pack_end(col, rb, FALSE);
	gtk_tree_view_column_add_attribute(col, rb, "markup", COL_BADGE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
	g_signal_connect(tv, "button-press-event", G_CALLBACK(tree_press), nullptr);
	g_signal_connect(tv, "row-expanded", G_CALLBACK(on_row_expanded), nullptr);
	g_signal_connect(tv, "row-collapsed", G_CALLBACK(on_row_collapsed), nullptr);
	GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(sw), tv);
	return sw;
}

void save_window()
{
	int w, h;
	gtk_window_get_size(GTK_WINDOW(g_ui.window), &w, &h);
	g_settings.window["w"] = w;
	g_settings.window["h"] = h;
	g_settings.window["sidebar"] = gtk_paned_get_position(GTK_PANED(g_ui.hpaned));
	g_settings.window["members"] = gtk_paned_get_position(GTK_PANED(g_ui.right_paned));
	save_tabs();
	save_expansion();
	settings_save();
}

gboolean on_key(GtkWidget *, GdkEventKey *ev, gpointer)
{
	bool ctrl = ev->state & GDK_CONTROL_MASK;
	if (ctrl && ev->keyval == GDK_KEY_k) {
		quick_switcher_show();
		return TRUE;
	}
	if (ctrl && ev->keyval == GDK_KEY_w) {
		if (ChatView *v = ui_current_view())
			ui_close_view(v);
		return TRUE;
	}
	if (ctrl && (ev->keyval == GDK_KEY_Tab || ev->keyval == GDK_KEY_ISO_Left_Tab || ev->keyval == GDK_KEY_Page_Down ||
		     ev->keyval == GDK_KEY_Page_Up)) {
		int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_ui.notebook));
		if (n > 0) {
			int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_ui.notebook));
			bool back = (ev->state & GDK_SHIFT_MASK) || ev->keyval == GDK_KEY_Page_Up || ev->keyval == GDK_KEY_ISO_Left_Tab;
			gtk_notebook_set_current_page(GTK_NOTEBOOK(g_ui.notebook), (cur + (back ? n - 1 : 1)) % n);
		}
		return TRUE;
	}
	if (ctrl && ev->keyval == GDK_KEY_m) {
		g_settings.show_member_list = !g_settings.show_member_list;
		gtk_widget_set_visible(g_ui.members_scroll, g_settings.show_member_list);
		return TRUE;
	}
	if (ctrl && (ev->state & GDK_SHIFT_MASK) && (ev->keyval == GDK_KEY_M || ev->keyval == GDK_KEY_m)) {
		voice_set_mute(!voice_self_mute());
		return TRUE;
	}
	// Typing anywhere outside a text field goes to the message box, like Discord.
	GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(g_ui.window));
	bool text_focus = f && (GTK_IS_EDITABLE(f) || (GTK_IS_TEXT_VIEW(f) && gtk_text_view_get_editable(GTK_TEXT_VIEW(f))));
	bool plain = !(ev->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK));
	if (!text_focus && plain && (gdk_keyval_to_unicode(ev->keyval) >= 0x20 || ev->keyval == GDK_KEY_BackSpace))
		if (ChatView *v = ui_current_view()) {
			v->focus_composer();
			return gtk_window_propagate_key_event(GTK_WINDOW(g_ui.window), ev);
		}
	return FALSE;
}

}  // namespace

// --- icons ---------------------------------------------------------------------------

cairo_surface_t *ui_icon(const char *name, int size)
{
	static std::map<std::string, cairo_surface_t *> cache;
	std::string key = std::string(name) + std::to_string(size);
	if (auto it = cache.find(key); it != cache.end())
		return it->second;
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	double z = size;
	cairo_set_source_rgb(cr, 0.58, 0.61, 0.64);
	cairo_set_line_width(cr, std::max(1.0, z / 10));
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	std::string n = name;
	if (n == "hash" || n == "news") {
		for (double x : {0.38, 0.62}) {
			cairo_move_to(cr, z * (x + 0.06), z * 0.18);
			cairo_line_to(cr, z * (x - 0.06), z * 0.82);
		}
		for (double y : {0.38, 0.62}) {
			cairo_move_to(cr, z * 0.18, z * y);
			cairo_line_to(cr, z * 0.82, z * y);
		}
		cairo_stroke(cr);
		if (n == "news") {
			cairo_arc(cr, z * 0.84, z * 0.18, z * 0.12, 0, 2 * M_PI);
			cairo_fill(cr);
		}
	} else if (n == "speaker") {
		cairo_move_to(cr, z * 0.15, z * 0.38);
		cairo_line_to(cr, z * 0.32, z * 0.38);
		cairo_line_to(cr, z * 0.52, z * 0.2);
		cairo_line_to(cr, z * 0.52, z * 0.8);
		cairo_line_to(cr, z * 0.32, z * 0.62);
		cairo_line_to(cr, z * 0.15, z * 0.62);
		cairo_close_path(cr);
		cairo_fill(cr);
		cairo_arc(cr, z * 0.52, z * 0.5, z * 0.18, -0.9, 0.9);
		cairo_stroke(cr);
		cairo_arc(cr, z * 0.52, z * 0.5, z * 0.32, -0.9, 0.9);
		cairo_stroke(cr);
	} else if (n == "thread" || n == "forum") {
		cairo_move_to(cr, z * 0.25, z * 0.2);
		cairo_line_to(cr, z * 0.25, z * 0.6);
		cairo_curve_to(cr, z * 0.25, z * 0.75, z * 0.35, z * 0.8, z * 0.5, z * 0.8);
		cairo_line_to(cr, z * 0.8, z * 0.8);
		cairo_stroke(cr);
		if (n == "forum") {
			cairo_rectangle(cr, z * 0.45, z * 0.2, z * 0.35, z * 0.35);
			cairo_stroke(cr);
		}
	} else if (n == "folder") {
		cairo_set_source_rgb(cr, 0.35, 0.4, 0.95);
		cairo_move_to(cr, z * 0.1, z * 0.25);
		cairo_line_to(cr, z * 0.4, z * 0.25);
		cairo_line_to(cr, z * 0.5, z * 0.35);
		cairo_line_to(cr, z * 0.9, z * 0.35);
		cairo_line_to(cr, z * 0.9, z * 0.8);
		cairo_line_to(cr, z * 0.1, z * 0.8);
		cairo_close_path(cr);
		cairo_fill(cr);
	} else if (n == "at" || n == "guild") {
		cairo_set_source_rgb(cr, n == "at" ? 0.35 : 0.25, n == "at" ? 0.4 : 0.26, n == "at" ? 0.95 : 0.29);
		cairo_arc(cr, z / 2, z / 2, z / 2, 0, 2 * M_PI);
		cairo_fill(cr);
		if (n == "at") {
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_set_line_width(cr, z / 12);
			cairo_arc(cr, z / 2, z / 2, z * 0.16, 0, 2 * M_PI);
			cairo_stroke(cr);
			cairo_arc(cr, z / 2, z / 2, z * 0.3, 0.3, 2 * M_PI - 0.6);
			cairo_stroke(cr);
		}
	}
	cairo_destroy(cr);
	cache[key] = s;
	return s;
}

std::string color_hex(uint32_t rgb)
{
	char b[8];
	snprintf(b, sizeof b, "#%06x", rgb & 0xffffff);
	return b;
}

void ui_toast(const std::string &msg)
{
	gtk_label_set_text(GTK_LABEL(g_ui.status), msg.c_str());
	gtk_widget_show(g_ui.status);
	if (g_toast_timer)
		g_source_remove(g_toast_timer);
	g_toast_timer = g_timeout_add_seconds(6, [](gpointer) -> gboolean {
		g_toast_timer = 0;
		if (g_gateway.state() == Gateway::Connected)
			gtk_widget_hide(g_ui.status);
		return G_SOURCE_REMOVE;
	}, nullptr);
}

// --- views ---------------------------------------------------------------------------

ChatView *ui_current_view()
{
	if (!g_ui.notebook)
		return nullptr;
	int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_ui.notebook));
	if (cur < 0)
		return nullptr;
	GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_ui.notebook), cur);
	for (ChatView *v : g_ui.views)
		if (v->widget() == page)
			return v;
	return nullptr;
}

ChatView *ui_open_channel(Snowflake cid, bool new_tab)
{
	if (!g_store.channel(cid))
		return nullptr;
	// Already open somewhere? Switch to it.
	for (ChatView *v : g_ui.views)
		if (v->channel_id() == cid) {
			int n = gtk_notebook_page_num(GTK_NOTEBOOK(g_ui.notebook), v->widget());
			gtk_notebook_set_current_page(GTK_NOTEBOOK(g_ui.notebook), n);
			v->focus_composer();
			return v;
		}
	ChatView *cur = ui_current_view();
	if (cur && !new_tab) {
		cur->set_channel(cid);
		cur->on_shown();
		ui_member_list_show(cur->guild_id(), cid);
		ui_update_title();
		save_tabs();
		cur->focus_composer();
		return cur;
	}
	ChatView *v = new ChatView(cid);
	g_ui.views.push_back(v);
	int n = gtk_notebook_append_page(GTK_NOTEBOOK(g_ui.notebook), v->widget(), make_tab_label(v));
	gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(g_ui.notebook), v->widget(), TRUE);
	ui_update_tab_label(v);
	gtk_notebook_set_current_page(GTK_NOTEBOOK(g_ui.notebook), n);
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(g_ui.notebook), g_ui.views.size() > 1);
	save_tabs();
	v->focus_composer();
	return v;
}

void ui_close_view(ChatView *v)
{
	auto it = std::find(g_ui.views.begin(), g_ui.views.end(), v);
	if (it == g_ui.views.end())
		return;
	g_ui.views.erase(it);
	int n = gtk_notebook_page_num(GTK_NOTEBOOK(g_ui.notebook), v->widget());
	if (n >= 0)
		gtk_notebook_remove_page(GTK_NOTEBOOK(g_ui.notebook), n);
	delete v;
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(g_ui.notebook), g_ui.views.size() > 1);
	save_tabs();
	g_store.trim_messages(0);
	media_trim();
}

void ui_update_tab_label(ChatView *v)
{
	if (!g_ui.notebook)
		return;
	if (gtk_notebook_page_num(GTK_NOTEBOOK(g_ui.notebook), v->widget()) < 0)
		return;
	GtkWidget *tl = gtk_notebook_get_tab_label(GTK_NOTEBOOK(g_ui.notebook), v->widget());
	if (!tl)
		return;
	GtkWidget *l = (GtkWidget *)g_object_get_data(G_OBJECT(tl), "label");
	const Channel *c = g_store.channel(v->channel_id());
	if (!c || !l)
		return;
	std::string t = c->is_private() ? g_store.channel_title(*c) : "#" + c->name;
	bool unread = g_store.is_unread(*c) && v != ui_current_view();
	std::string mk = unread ? "<b>" + esc(t) + "</b>" : esc(t);
	int mc = g_store.mention_count(c->id);
	if (mc && v != ui_current_view())
		mk += " " + badge(mc);
	gtk_label_set_markup(GTK_LABEL(l), mk.c_str());
	gtk_label_set_width_chars(GTK_LABEL(l), std::min(18, (int)g_utf8_strlen(t.c_str(), -1) + 2));
}

void ui_update_title()
{
	int mentions = 0;
	for (auto &[cid, rs] : g_store.read_states)
		if (rs.mention_count > 0) {
			const Channel *c = g_store.channel(cid);
			if (c && (c->is_private() || !g_store.is_muted(*c)))
				mentions += rs.mention_count;
		}
	std::string t = "dcord";
	if (ChatView *v = ui_current_view())
		if (const Channel *c = g_store.channel(v->channel_id())) {
			t = (c->is_private() ? g_store.channel_title(*c) : "#" + c->name);
			if (Guild *g = g_store.guild(c->guild_id))
				t += " - " + g->name;
			t += " - dcord";
		}
	if (mentions)
		t = "(" + std::to_string(mentions) + ") " + t;
	gtk_window_set_title(GTK_WINDOW(g_ui.window), t.c_str());
}

void ui_select_in_tree(Snowflake cid)
{
	GtkTreeIter it;
	auto r = g_rows.find(cid);
	if (r == g_rows.end() || !row_iter(r->second, &it))
		return;
	GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(g_ui.tree_store), &it);
	gtk_tree_view_expand_to_path(GTK_TREE_VIEW(g_ui.tree), p);
	gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(g_ui.tree)), p);
	gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(g_ui.tree), p, nullptr, FALSE, 0, 0);
	gtk_tree_path_free(p);
}

void ui_show_user_menu(Snowflake gid, Snowflake uid, GdkEvent *ev)
{
	GtkWidget *menu = gtk_menu_new();
	std::string name = g_store.display_name(gid, uid);
	const User *u = g_store.user(uid);
	std::string head = name + (u && !u->username.empty() ? "  (" + u->username + ")" : "");
	GtkWidget *h = gtk_menu_item_new_with_label(head.c_str());
	gtk_widget_set_sensitive(h, FALSE);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), h);
	if (uid != g_store.me.id)
		menu_add(menu, "Message", [uid]() {
			for (auto &[cid, c] : g_store.channels)
				if (c.type == CH_DM && c.recipients.size() == 1 && c.recipients[0] == uid) {
					ui_open_channel(cid, false);
					return;
				}
			api_open_dm(uid, [](ApiResult &r) {
				if (!r.ok())
					return;
				Channel &c = g_store.upsert_channel(r.body);
				ui_tree_rebuild();
				ui_open_channel(c.id, false);
			});
		});
	menu_add(menu, "Mention", [uid]() {
		if (ChatView *v = ui_current_view()) {
			(void)v;
			GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(g_ui.window));
			if (GTK_IS_TEXT_VIEW(f) && gtk_text_view_get_editable(GTK_TEXT_VIEW(f)))
				gtk_text_buffer_insert_at_cursor(gtk_text_view_get_buffer(GTK_TEXT_VIEW(f)), ("<@" + sf_str(uid) + "> ").c_str(), -1);
		}
	});
	if (voice_status().state == VoiceStatus::Connected && uid != g_store.me.id) {
		double v = voice_user_volume(uid);
		GtkWidget *vl = gtk_menu_item_new_with_label(("Voice volume " + std::to_string((int)lround(v * 100)) + "%").c_str());
		gtk_widget_set_sensitive(vl, FALSE);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), vl);
		for (int pct : {0, 50, 100, 150, 200})
			menu_add(menu, ("  " + std::to_string(pct) + "%").c_str(), [uid, pct]() { voice_set_user_volume(uid, pct / 100.0); });
	}
	menu_add(menu, "Copy user ID", [uid]() {
		gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), sf_str(uid).c_str(), -1);
	});
	gtk_widget_show_all(menu);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), ev);
}

void ui_member_list_show(Snowflake gid, Snowflake cid)
{
	if (!g_ui.members)
		return;
	if (gid && (gid != g_member_guild || cid != g_member_channel)) {
		g_member_guild = gid;
		g_member_channel = cid;
		g_member_items.erase(gid);
		g_group_counts.erase(gid);
		app_request_member_list(gid, cid);
	}
	schedule_member_render();
}

void ui_set_voice_speaking_rows()
{
	// Re-render voice user rows of the channel we're in.
	VoiceStatus st = voice_status();
	if (st.channel_id) {
		GtkTreeIter it;
		auto r = g_rows.find(st.channel_id);
		if (r != g_rows.end() && row_iter(r->second, &it)) {
			const Channel *c = g_store.channel(st.channel_id);
			if (!c)
				return;
			GtkTreeIter child;
			std::vector<GtkTreeIter> remove;
			if (gtk_tree_model_iter_children(GTK_TREE_MODEL(g_ui.tree_store), &child, &it))
				do {
					int k;
					gtk_tree_model_get(GTK_TREE_MODEL(g_ui.tree_store), &child, COL_KIND, &k, -1);
					if (k == K_VUSER)
						remove.push_back(child);
				} while (gtk_tree_model_iter_next(GTK_TREE_MODEL(g_ui.tree_store), &child));
			bool expanded = true;
			for (auto &x : remove)
				gtk_tree_store_remove(g_ui.tree_store, &x);
			add_voice_users(&it, *c);
			if (expanded) {
				GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(g_ui.tree_store), &it);
				gtk_tree_view_expand_row(GTK_TREE_VIEW(g_ui.tree), p, FALSE);
				gtk_tree_path_free(p);
			}
		}
	}
}

// --- ui.h -------------------------------------------------------------------------------

void ui_init()
{
	GtkSettings *gs = gtk_settings_get_default();
	g_object_set(gs, "gtk-application-prefer-dark-theme", TRUE, nullptr);
	int dpi = 0;
	g_object_get(gs, "gtk-xft-dpi", &dpi, nullptr);
	g_ui_scale = dpi > 0 ? std::max(1.0, dpi / 1024.0 / 96.0) : 1.0;
	if (const char *s = getenv("DCORD_SCALE"))
		g_ui_scale = atof(s);
	g_media_animate = g_settings.animate_gifs;
	apply_css();
	for (auto &x : jget(g_settings.window, "expanded"))
		g_expanded.insert(sf_parse(x));
	for (auto &x : g_settings.collapsed)
		g_collapsed.insert(sf_parse(x));
	g_voice_changed = []() {
		update_voice_panel();
		ui_set_voice_speaking_rows();
	};
	g_voice_speaking = []() { ui_set_voice_speaking_rows(); };
}

void ui_show_main()
{
	login_window_hide();
	if (g_ui.window) {
		gtk_widget_show(g_ui.window);
		return;
	}
	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_ui.window = win;
	gtk_window_set_title(GTK_WINDOW(win), "dcord");
	gtk_window_set_icon_name(GTK_WINDOW(win), "internet-chat");
	int w = (int)jint(g_settings.window, "w", S(1100)), h = (int)jint(g_settings.window, "h", S(720));
	gtk_window_set_default_size(GTK_WINDOW(win), w, h);
	g_signal_connect(win, "delete-event", CB(+[](GtkWidget *, GdkEvent *, gpointer) -> gboolean {
		gtk_main_quit();
		return FALSE;
	}), nullptr);
	g_signal_connect(win, "key-press-event", G_CALLBACK(on_key), nullptr);
	g_signal_connect(win, "notify::is-active", CB(+[](GObject *o, GParamSpec *, gpointer) {
		g_ui.focused = gtk_window_is_active(GTK_WINDOW(o));
		media_set_window_active(g_ui.focused);
		if (g_ui.focused) {
			gtk_window_set_urgency_hint(GTK_WINDOW(o), FALSE);
			if (ChatView *v = ui_current_view())
				v->maybe_ack();
		}
	}), nullptr);

	g_ui.hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(side), "sidebar");
	gtk_box_pack_start(GTK_BOX(side), build_tree(), TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(side), build_voice_panel(), FALSE, FALSE, 0);
	gtk_paned_pack1(GTK_PANED(g_ui.hpaned), side, FALSE, FALSE);

	g_ui.notebook = gtk_notebook_new();
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(g_ui.notebook), TRUE);
	gtk_notebook_set_show_border(GTK_NOTEBOOK(g_ui.notebook), FALSE);
	g_signal_connect(g_ui.notebook, "switch-page", G_CALLBACK(on_switch_page), nullptr);
	g_signal_connect(g_ui.notebook, "page-reordered", CB(+[](GtkNotebook *nb, GtkWidget *, guint, gpointer) {
		std::vector<ChatView *> order;
		for (int i = 0; i < gtk_notebook_get_n_pages(nb); i++)
			for (ChatView *v : g_ui.views)
				if (v->widget() == gtk_notebook_get_nth_page(nb, i))
					order.push_back(v);
		g_ui.views = order;
		save_tabs();
	}), nullptr);

	g_ui.call_bar = gtk_info_bar_new_with_buttons("Join", 1, "Ignore", 2, nullptr);
	gtk_info_bar_set_message_type(GTK_INFO_BAR(g_ui.call_bar), GTK_MESSAGE_INFO);
	g_ui.call_label = gtk_label_new("");
	gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(g_ui.call_bar))), g_ui.call_label);
	g_signal_connect(g_ui.call_bar, "response", CB(+[](GtkInfoBar *b, int resp, gpointer) {
		if (resp == 1 && g_ui.call_channel) {
			app_voice_join(0, g_ui.call_channel);
			ui_open_channel(g_ui.call_channel, false);
		}
		gtk_widget_hide(GTK_WIDGET(b));
	}), nullptr);
	gtk_widget_set_no_show_all(g_ui.call_bar, TRUE);

	GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start(GTK_BOX(center), g_ui.call_bar, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(center), g_ui.notebook, TRUE, TRUE, 0);

	g_ui.right_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_pack1(GTK_PANED(g_ui.right_paned), center, TRUE, FALSE);
	g_ui.members = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(3));
	gtk_style_context_add_class(gtk_widget_get_style_context(g_ui.members), "members");
	g_ui.members_scroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g_ui.members_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(g_ui.members_scroll), g_ui.members);
	gtk_paned_pack2(GTK_PANED(g_ui.right_paned), g_ui.members_scroll, FALSE, FALSE);
	gtk_paned_pack2(GTK_PANED(g_ui.hpaned), g_ui.right_paned, TRUE, FALSE);
	gtk_container_add(GTK_CONTAINER(win), g_ui.hpaned);

	gtk_paned_set_position(GTK_PANED(g_ui.hpaned), (int)jint(g_settings.window, "sidebar", S(260)));
	gtk_widget_set_size_request(g_ui.members_scroll, S(190), -1);
	if (int mp = (int)jint(g_settings.window, "members", 0); mp > 0)
		gtk_paned_set_position(GTK_PANED(g_ui.right_paned), mp);
	gtk_widget_show_all(win);
	gtk_widget_set_visible(g_ui.members_scroll, g_settings.show_member_list);
	gtk_label_set_text(GTK_LABEL(g_ui.status), "Connecting…");
	gtk_widget_show(g_ui.status);
	update_voice_panel();
}

void ui_ready()
{
	update_me_label();
	rebuild_tree();
	// Restore the tabs from last time (or keep the ones already open across reconnects).
	if (g_ui.views.empty()) {
		for (auto &t : g_settings.tabs) {
			Snowflake cid = sf_parse(t);
			if (g_store.channel(cid))
				ui_open_channel(cid, true);
		}
		int cur = (int)jint(g_settings.window, "current_tab", 0);
		if (cur >= 0 && cur < gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_ui.notebook)))
			gtk_notebook_set_current_page(GTK_NOTEBOOK(g_ui.notebook), cur);
	} else {
		// Reconnected: messages may have been missed; reload open views.
		for (ChatView *v : std::vector<ChatView *>(g_ui.views)) {
			if (g_store.channel(v->channel_id()))
				v->reload();
			else
				ui_close_view(v);
		}
	}
	ui_update_title();
}

void ui_conn_state(int st)
{
	if (!g_ui.status)
		return;
	if (st == Gateway::Connected) {
		gtk_widget_hide(g_ui.status);
	} else {
		gtk_label_set_markup(GTK_LABEL(g_ui.status), "<span foreground='#f0b232'>Connecting to Discord…</span>");
		gtk_widget_show(g_ui.status);
	}
}

void ui_tree_rebuild()
{
	if (g_ui.tree)
		rebuild_tree();
}

void ui_guild_rebuild(Snowflake gid)
{
	GtkTreeIter it;
	auto r = g_rows.find(gid);
	if (r == g_rows.end() || !row_iter(r->second, &it)) {
		ui_tree_rebuild();
		return;
	}
	g_building = true;
	fill_guild(&it, gid);
	if (Guild *g = g_store.guild(gid))
		gtk_tree_store_set(g_ui.tree_store, &it, COL_MARKUP, guild_markup(*g).c_str(),
				   COL_BADGE, badge(g_store.guild_mentions(gid)).c_str(), -1);
	// Re-expand this guild's categories.
	GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(g_ui.tree_store), &it);
	bool expand = gtk_tree_view_row_expanded(GTK_TREE_VIEW(g_ui.tree), p) || g_expanded.count(gid);
	g_building = false;
	if (expand) {
		gtk_tree_view_expand_row(GTK_TREE_VIEW(g_ui.tree), p, FALSE);
		on_row_expanded(GTK_TREE_VIEW(g_ui.tree), &it, p, nullptr);
	}
	gtk_tree_path_free(p);
	if (ChatView *v = ui_current_view())
		ui_select_in_tree(v->channel_id());
}

void ui_channel_updated(Snowflake cid)
{
	for (ChatView *v : g_ui.views)
		if (v->channel_id() == cid)
			v->on_channel_updated();
}

void ui_unread_changed(Snowflake cid)
{
	const Channel *c = g_store.channel(cid);
	if (c && c->is_private()) {
		// DM order depends on the last message.
		GtkTreeIter it;
		if (row_iter(g_dms_row, &it)) {
			GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(g_ui.tree_store), &it);
			bool exp = gtk_tree_view_row_expanded(GTK_TREE_VIEW(g_ui.tree), p);
			fill_dms(&it);
			if (exp)
				gtk_tree_view_expand_row(GTK_TREE_VIEW(g_ui.tree), p, FALSE);
			gtk_tree_path_free(p);
		}
	} else {
		update_channel_row(cid);
	}
	for (ChatView *v : g_ui.views)
		if (v->channel_id() == cid)
			ui_update_tab_label(v);
	ui_update_title();
}

void ui_message_created(const MessagePtr &m)
{
	for (ChatView *v : g_ui.views)
		v->on_message_created(m);
}

void ui_message_updated(const MessagePtr &m)
{
	for (ChatView *v : g_ui.views)
		v->on_message_updated(m);
}

void ui_message_deleted(Snowflake cid, Snowflake mid)
{
	for (ChatView *v : g_ui.views)
		if (v->channel_id() == cid)
			v->on_message_deleted(mid);
}

void ui_members_changed(Snowflake gid)
{
	static std::set<Snowflake> pending;
	static guint timer = 0;
	pending.insert(gid);
	if (timer)
		return;
	// Member chunks arrive in bursts; re-render once.
	timer = g_timeout_add(200, [](gpointer) -> gboolean {
		timer = 0;
		for (ChatView *v : g_ui.views)
			if (pending.count(v->guild_id()))
				v->on_members_changed();
		pending.clear();
		return G_SOURCE_REMOVE;
	}, nullptr);
}

void ui_typing(Snowflake cid, Snowflake uid)
{
	for (ChatView *v : g_ui.views)
		if (v->channel_id() == cid)
			v->on_typing(uid);
}

void ui_voice_states_changed(Snowflake gid)
{
	if (!g_ui.tree)
		return;
	static std::set<Snowflake> pending;
	static guint timer = 0;
	pending.insert(gid);
	if (timer)
		return;
	timer = g_timeout_add(150, [](gpointer) -> gboolean {
		timer = 0;
		for (Snowflake g : pending) {
			if (g == 0) {
				GtkTreeIter it;
				if (row_iter(g_dms_row, &it)) {
					GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(g_ui.tree_store), &it);
					bool exp = gtk_tree_view_row_expanded(GTK_TREE_VIEW(g_ui.tree), p);
					fill_dms(&it);
					if (exp)
						gtk_tree_view_expand_row(GTK_TREE_VIEW(g_ui.tree), p, FALSE);
					gtk_tree_path_free(p);
				}
			} else {
				ui_guild_rebuild(g);
			}
		}
		pending.clear();
		return G_SOURCE_REMOVE;
	}, nullptr);
}

void ui_incoming_call(Snowflake cid, bool ringing)
{
	if (!g_ui.call_bar)
		return;
	if (!ringing) {
		if (g_ui.call_channel == cid)
			gtk_widget_hide(g_ui.call_bar);
		return;
	}
	const Channel *c = g_store.channel(cid);
	g_ui.call_channel = cid;
	std::string t = "Incoming call: " + (c ? g_store.channel_title(*c) : std::string("DM"));
	gtk_label_set_text(GTK_LABEL(g_ui.call_label), t.c_str());
	gtk_widget_show_all(g_ui.call_bar);
}

void ui_member_list_update(Snowflake gid, const json &d)
{
	auto &items = g_member_items[gid];
	if (const json &groups = jget(d, "groups"); groups.is_array())
		for (auto &gr : groups)
			g_group_counts[gid][jstr(gr, "id")] = jint(gr, "count");
	for (auto &op : jget(d, "ops")) {
		std::string o = jstr(op, "op");
		if (o == "SYNC") {
			const json &range = jget(op, "range");
			size_t a = range.is_array() && range.size() == 2 ? range[0].get<size_t>() : 0;
			const json &its = jget(op, "items");
			if (items.size() < a + its.size())
				items.resize(a + its.size());
			for (size_t i = 0; i < its.size(); i++)
				items[a + i] = its[i];
		} else if (o == "INSERT") {
			size_t i = (size_t)jint(op, "index");
			if (i <= items.size())
				items.insert(items.begin() + i, jget(op, "item"));
		} else if (o == "UPDATE") {
			size_t i = (size_t)jint(op, "index");
			if (i < items.size())
				items[i] = jget(op, "item");
		} else if (o == "DELETE") {
			size_t i = (size_t)jint(op, "index");
			if (i < items.size())
				items.erase(items.begin() + i);
		}
	}
	// Discard nulls left by sparse syncs.
	std::erase_if(items, [](const json &j) { return j.is_null(); });
	if (gid == g_member_guild)
		schedule_member_render();
}

void ui_attention(bool important)
{
	if (!g_ui.window)
		return;
	if (important && !g_ui.focused)
		gtk_window_set_urgency_hint(GTK_WINDOW(g_ui.window), TRUE);
	ui_update_title();
}

void ui_save_state()
{
	if (g_ui.window && gtk_widget_get_visible(g_ui.window))
		save_window();
}

void ui_show_login(const std::string &error)
{
	if (g_ui.window)
		gtk_widget_hide(g_ui.window);
	for (ChatView *v : std::vector<ChatView *>(g_ui.views))
		ui_close_view(v);
	login_window_show(error);
}
