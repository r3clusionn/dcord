// Image viewer, GIF picker, emoji picker, settings dialog, quick switcher.
#include <algorithm>
#include <cmath>

#include "app.h"
#include "ui.h"
#include "ui_internal.h"
#include "voice.h"

// --- image viewer ----------------------------------------------------------------------

void image_viewer_show(const std::string &url, int w, int h, const std::string &open_url)
{
	GdkRectangle geo = {0, 0, 1920, 1080};
	GdkMonitor *mon = gdk_display_get_primary_monitor(gdk_display_get_default());
	if (!mon)
		mon = gdk_display_get_monitor(gdk_display_get_default(), 0);
	if (mon)
		gdk_monitor_get_workarea(mon, &geo);
	int maxw = geo.width * 9 / 10, maxh = geo.height * 85 / 100;
	bool gif = url.find(".gif") != std::string::npos || url.find(".mp4") != std::string::npos;
	MediaOpts o;
	o.max_w = maxw;
	o.max_h = maxh;
	o.animate = gif;
	// Ask the media proxy for a screen-sized copy when it is one of Discord's.
	std::string src = url;
	if (!gif && url.find("cdn.discordapp.com/attachments/") != std::string::npos) {
		src = url;
		size_t p = src.find("cdn.discordapp.com");
		src.replace(p, 18, "media.discordapp.net");
		src = media_proxy_sized(src, maxw, maxh);
	}
	MediaPtr m = media_get(src, o);
	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(g_ui.window));
	gtk_window_set_title(GTK_WINDOW(win), "Image");
	gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DIALOG);
	int dw = w > 0 ? std::min(maxw, w) : maxw / 2, dh = h > 0 ? std::min(maxh, h) : maxh / 2;
	if (w > 0 && h > 0) {
		double sc = std::min({1.0, (double)maxw / w, (double)maxh / h});
		dw = (int)(w * sc);
		dh = (int)(h * sc);
	}
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *mw = media_widget(m, dw, dh, [win](int) { gtk_widget_destroy(win); });
	gtk_box_pack_start(GTK_BOX(box), mw, TRUE, TRUE, 0);
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(4));
	GtkWidget *open = gtk_button_new_with_label("Open in browser");
	GtkWidget *copy = gtk_button_new_with_label("Copy link");
	auto *u = new std::string(open_url);
	g_signal_connect_data(open, "clicked", CB(+[](GtkButton *, gpointer p) { open_external(*(std::string *)p); }), u,
			      [](gpointer p, GClosure *) { delete (std::string *)p; }, (GConnectFlags)0);
	g_signal_connect(copy, "clicked", CB(+[](GtkButton *b, gpointer) {
		const char *s = (const char *)g_object_get_data(G_OBJECT(b), "url");
		gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), s, -1);
	}), nullptr);
	g_object_set_data_full(G_OBJECT(copy), "url", g_strdup(open_url.c_str()), g_free);
	gtk_box_pack_end(GTK_BOX(bar), open, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(bar), copy, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(win), box);
	g_signal_connect(win, "key-press-event", CB(+[](GtkWidget *w, GdkEventKey *ev, gpointer) -> gboolean {
		if (ev->keyval == GDK_KEY_Escape) {
			gtk_widget_destroy(w);
			return TRUE;
		}
		return FALSE;
	}), nullptr);
	gtk_widget_show_all(win);
}

// --- GIF picker ------------------------------------------------------------------------

namespace {
struct GifPicker {
	GtkWidget *pop, *entry, *flow;
	std::function<void(const std::string &)> on_pick;
	guint search_timer = 0;
	std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

void gif_fill(GifPicker *gp, const json &list)
{
	gtk_container_foreach(GTK_CONTAINER(gp->flow), [](GtkWidget *w, gpointer) { gtk_widget_destroy(w); }, nullptr);
	const json &arr = list.is_array() ? list : jget(list, "gifs");
	int tile = S(150);
	for (auto &g : arr) {
		int w = (int)jint(g, "width", 200), h = (int)jint(g, "height", 150);
		int th = std::max(S(40), std::min(S(200), h * tile / std::max(1, w)));
		std::string still = jstr(g, "preview");
		std::string anim = jstr(g, "src");
		std::string url = jstr(g, "url");
		MediaOpts o;
		o.max_w = tile;
		o.max_h = th;
		MediaPtr m = media_get(still, o);
		GtkWidget *mw = media_widget(m, tile, th, [gp, url](int b) {
			if (b != 1)
				return;
			auto cb = gp->on_pick;
			gtk_popover_popdown(GTK_POPOVER(gp->pop));
			cb(url);
		});
		gtk_widget_set_tooltip_text(mw, jstr(g, "title").c_str());
		// Animate on hover: swap in a second widget showing the video.
		GtkWidget *stack = gtk_stack_new();
		gtk_stack_add_named(GTK_STACK(stack), mw, "still");
		GtkWidget *eb = gtk_event_box_new();
		gtk_container_add(GTK_CONTAINER(eb), stack);
		gtk_widget_add_events(eb, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
		struct Hover { GifPicker *gp; std::string anim, url; int w, h; GtkWidget *stack; };
		auto *hv = new Hover{gp, anim, url, tile, th, stack};
		g_signal_connect_data(eb, "enter-notify-event", CB(+[](GtkWidget *, GdkEvent *, gpointer p) -> gboolean {
			Hover *h = (Hover *)p;
			if (!gtk_stack_get_child_by_name(GTK_STACK(h->stack), "anim")) {
				MediaOpts o;
				o.max_w = h->w;
				o.max_h = h->h;
				o.animate = true;
				GifPicker *gp = h->gp;
				std::string url = h->url;
				GtkWidget *a = media_widget(media_get(h->anim, o), h->w, h->h, [gp, url](int b) {
					if (b != 1)
						return;
					auto cb = gp->on_pick;
					gtk_popover_popdown(GTK_POPOVER(gp->pop));
					cb(url);
				});
				gtk_widget_show(a);
				gtk_stack_add_named(GTK_STACK(h->stack), a, "anim");
			}
			gtk_stack_set_visible_child_name(GTK_STACK(h->stack), "anim");
			return FALSE;
		}), hv, [](gpointer p, GClosure *) { delete (Hover *)p; }, (GConnectFlags)0);
		g_signal_connect(eb, "leave-notify-event", CB(+[](GtkWidget *, GdkEvent *, gpointer s) -> gboolean {
			gtk_stack_set_visible_child_name(GTK_STACK(s), "still");
			if (GtkWidget *a = gtk_stack_get_child_by_name(GTK_STACK(s), "anim"))
				gtk_widget_destroy(a);
			return FALSE;
		}), stack);
		gtk_widget_set_valign(eb, GTK_ALIGN_START);
		gtk_flow_box_insert(GTK_FLOW_BOX(gp->flow), eb, -1);
	}
	gtk_widget_show_all(gp->flow);
}

void gif_search(GifPicker *gp)
{
	std::string q = trim(gtk_entry_get_text(GTK_ENTRY(gp->entry)));
	auto alive = gp->alive;
	api_gifs(q, [gp, alive](ApiResult &r) {
		if (*alive && r.ok())
			gif_fill(gp, r.body);
	});
}
}  // namespace

void gif_picker_show(GtkWidget *rel, std::function<void(const std::string &)> on_pick)
{
	auto *gp = new GifPicker;
	gp->on_pick = std::move(on_pick);
	gp->pop = gtk_popover_new(rel);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(4));
	g_object_set(box, "margin", S(6), nullptr);
	gp->entry = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(gp->entry), "Search GIFs");
	gtk_box_pack_start(GTK_BOX(box), gp->entry, FALSE, FALSE, 0);
	GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_widget_set_size_request(sw, S(500), S(420));
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gp->flow = gtk_flow_box_new();
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(gp->flow), GTK_SELECTION_NONE);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(gp->flow), 3);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(gp->flow), FALSE);
	gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(gp->flow), S(4));
	gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(gp->flow), S(4));
	gtk_container_add(GTK_CONTAINER(sw), gp->flow);
	gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(gp->pop), box);
	g_signal_connect(gp->entry, "search-changed", CB(+[](GtkSearchEntry *, gpointer p) { gif_search((GifPicker *)p); }), gp);
	g_signal_connect(gp->pop, "closed", CB(+[](GtkPopover *pop, gpointer p) {
		GifPicker *gp = (GifPicker *)p;
		*gp->alive = false;
		g_idle_add([](gpointer w) -> gboolean {
			gtk_widget_destroy(GTK_WIDGET(w));
			return G_SOURCE_REMOVE;
		}, pop);
		g_idle_add([](gpointer p) -> gboolean {
			delete (GifPicker *)p;
			media_trim();
			return G_SOURCE_REMOVE;
		}, gp);
	}), gp);
	gtk_widget_show_all(box);
	gtk_popover_popup(GTK_POPOVER(gp->pop));
	gif_search(gp);
}

// --- emoji picker ------------------------------------------------------------------------

namespace {
struct EmojiDef {
	const char *name, *text;
};
const EmojiDef kEmoji[] = {
	{"thumbsup +1", "👍"}, {"thumbsdown -1", "👎"}, {"joy laugh", "😂"}, {"rofl laugh", "🤣"}, {"heart love", "❤️"},
	{"smile", "😄"}, {"grin", "😁"}, {"sweat_smile", "😅"}, {"wink", "😉"}, {"blush", "😊"}, {"slight_smile", "🙂"},
	{"upside_down", "🙃"}, {"heart_eyes", "😍"}, {"kissing_heart", "😘"}, {"yum", "😋"}, {"sunglasses cool", "😎"},
	{"thinking", "🤔"}, {"neutral", "😐"}, {"expressionless", "😑"}, {"unamused", "😒"}, {"rolling_eyes", "🙄"},
	{"grimacing", "😬"}, {"relieved", "😌"}, {"pensive", "😔"}, {"sleepy", "😪"}, {"sleeping", "😴"},
	{"mask", "😷"}, {"nerd", "🤓"}, {"smirk", "😏"}, {"confused", "😕"}, {"worried", "😟"}, {"frowning", "🙁"},
	{"open_mouth", "😮"}, {"astonished", "😲"}, {"flushed", "😳"}, {"pleading", "🥺"}, {"cry", "😢"},
	{"sob", "😭"}, {"scream", "😱"}, {"angry", "😠"}, {"rage", "😡"}, {"skull dead", "💀"}, {"clown", "🤡"},
	{"poop", "💩"}, {"ghost", "👻"}, {"alien", "👽"}, {"robot", "🤖"}, {"eyes", "👀"}, {"brain", "🧠"},
	{"wave", "👋"}, {"ok_hand", "👌"}, {"pinch", "🤏"}, {"v peace", "✌️"}, {"crossed_fingers", "🤞"},
	{"metal", "🤘"}, {"point_up", "☝️"}, {"point_right", "👉"}, {"clap", "👏"}, {"raised_hands", "🙌"},
	{"pray", "🙏"}, {"handshake", "🤝"}, {"muscle", "💪"}, {"fire", "🔥"}, {"100", "💯"}, {"sparkles", "✨"},
	{"star", "⭐"}, {"tada party", "🎉"}, {"check white_check_mark", "✅"}, {"x cross", "❌"}, {"warning", "⚠️"},
	{"question", "❓"}, {"exclamation", "❗"}, {"zzz", "💤"}, {"broken_heart", "💔"}, {"orange_heart", "🧡"},
	{"yellow_heart", "💛"}, {"green_heart", "💚"}, {"blue_heart", "💙"}, {"purple_heart", "💜"},
	{"black_heart", "🖤"}, {"white_heart", "🤍"}, {"star_struck", "🤩"}, {"partying", "🥳"}, {"hot", "🥵"},
	{"cold", "🥶"}, {"zany", "🤪"}, {"shush", "🤫"}, {"monocle", "🧐"}, {"cowboy", "🤠"}, {"salute", "🫡"},
	{"melting", "🫠"}, {"skull_crossbones", "☠️"}, {"moyai", "🗿"}, {"cat", "🐱"}, {"dog", "🐶"}, {"frog", "🐸"},
	{"monkey see_no_evil", "🙈"}, {"pizza", "🍕"}, {"beer", "🍺"}, {"coffee", "☕"}, {"cake", "🎂"},
	{"gift", "🎁"}, {"trophy", "🏆"}, {"medal", "🥇"}, {"soccer", "⚽"}, {"video_game", "🎮"}, {"music", "🎵"},
	{"headphones", "🎧"}, {"computer", "💻"}, {"phone", "📱"}, {"money", "💰"}, {"rocket", "🚀"},
	{"rainbow", "🌈"}, {"sun", "☀️"}, {"moon", "🌙"}, {"snowflake", "❄️"}, {"zap", "⚡"}, {"boom", "💥"},
	{"sweat_drops", "💦"}, {"eggplant", "🍆"}, {"peach", "🍑"}, {"goat", "🐐"}, {"cap billed_cap", "🧢"},
	{"crown", "👑"}, {"gem", "💎"}, {"lock", "🔒"}, {"bell", "🔔"}, {"pushpin", "📌"}, {"link", "🔗"},
	{"arrow_up", "⬆️"}, {"arrow_down", "⬇️"}, {"arrows_counterclockwise", "🔄"}, {"no_entry", "⛔"},
	{"speaking_head", "🗣️"}, {"facepalm", "🤦"}, {"shrug", "🤷"}, {"man_technologist", "👨‍💻"}, {"ok", "🆗"},
	{"new", "🆕"}, {"sos", "🆘"}, {"flag_us", "🇺🇸"}, {"flag_gb", "🇬🇧"}, {"one", "1️⃣"}, {"two", "2️⃣"},
	{"three", "3️⃣"},
};

struct EmojiPicker {
	GtkWidget *pop, *entry, *flow;
	Snowflake gid;
	std::function<void(const std::string &)> on_pick;
};

void emoji_fill(EmojiPicker *ep)
{
	gtk_container_foreach(GTK_CONTAINER(ep->flow), [](GtkWidget *w, gpointer) { gtk_widget_destroy(w); }, nullptr);
	std::string q = trim(gtk_entry_get_text(GTK_ENTRY(ep->entry)));
	for (auto &c : q)
		c = (char)tolower((unsigned char)c);
	int sz = S(28);
	int count = 0;
	auto add = [&](MediaPtr m, const std::string &text, const std::string &tip) {
		if (count++ > 400)
			return;
		GtkWidget *w = media_widget(m, sz, sz, [ep, text](int b) {
			if (b != 1)
				return;
			auto cb = ep->on_pick;
			gtk_popover_popdown(GTK_POPOVER(ep->pop));
			cb(text);
		});
		gtk_widget_set_tooltip_text(w, tip.c_str());
		gtk_flow_box_insert(GTK_FLOW_BOX(ep->flow), w, -1);
	};
	MediaOpts o;
	o.max_w = o.max_h = sz;
	o.disk_cache = true;
	// This server's emoji first, then other servers'.
	std::vector<Snowflake> order;
	if (ep->gid)
		order.push_back(ep->gid);
	for (Snowflake g : g_store.guild_order)
		if (g != ep->gid)
			order.push_back(g);
	for (Snowflake gid : order) {
		Guild *g = g_store.guild(gid);
		if (!g)
			continue;
		for (auto &[id, e] : g->emojis) {
			std::string ln = e.name;
			for (auto &c : ln)
				c = (char)tolower((unsigned char)c);
			if (!q.empty() && ln.find(q) == std::string::npos)
				continue;
			if (q.empty() && gid != ep->gid)
				continue;  // keep the default view small
			std::string text = std::string(e.animated ? "<a:" : "<:") + e.name + ":" + sf_str(id) + ">";
			o.animate = false;
			add(media_get(emoji_url(id, e.animated, 48), o), text, ":" + e.name + ": (" + g->name + ")");
		}
	}
	for (auto &e : kEmoji) {
		if (!q.empty() && std::string(e.name).find(q) == std::string::npos)
			continue;
		add(media_get(twemoji_url(e.text), o), e.text, std::string(":") + e.name + ":");
	}
	gtk_widget_show_all(ep->flow);
}
}  // namespace

void emoji_picker_show(GtkWidget *rel, Snowflake gid, std::function<void(const std::string &)> on_pick)
{
	auto *ep = new EmojiPicker;
	ep->gid = gid;
	ep->on_pick = std::move(on_pick);
	ep->pop = gtk_popover_new(rel);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(4));
	g_object_set(box, "margin", S(6), nullptr);
	ep->entry = gtk_search_entry_new();
	gtk_box_pack_start(GTK_BOX(box), ep->entry, FALSE, FALSE, 0);
	GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_widget_set_size_request(sw, S(380), S(320));
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	ep->flow = gtk_flow_box_new();
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(ep->flow), GTK_SELECTION_NONE);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(ep->flow), 10);
	gtk_container_add(GTK_CONTAINER(sw), ep->flow);
	gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(ep->pop), box);
	g_signal_connect(ep->entry, "search-changed", CB(+[](GtkSearchEntry *, gpointer p) { emoji_fill((EmojiPicker *)p); }), ep);
	g_signal_connect(ep->pop, "closed", CB(+[](GtkPopover *pop, gpointer p) {
		g_idle_add([](gpointer w) -> gboolean {
			gtk_widget_destroy(GTK_WIDGET(w));
			return G_SOURCE_REMOVE;
		}, pop);
		g_idle_add([](gpointer p) -> gboolean {
			delete (EmojiPicker *)p;
			return G_SOURCE_REMOVE;
		}, p);
	}), ep);
	emoji_fill(ep);
	gtk_widget_show_all(box);
	gtk_popover_popup(GTK_POPOVER(ep->pop));
	gtk_widget_grab_focus(ep->entry);
}

// --- settings --------------------------------------------------------------------------------

namespace {
GtkWidget *g_settings_win;

GtkWidget *check(const char *label, bool *value, std::function<void()> changed = nullptr)
{
	GtkWidget *c = gtk_check_button_new_with_label(label);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), *value);
	struct Ctx { bool *v; std::function<void()> f; };
	auto *ctx = new Ctx{value, std::move(changed)};
	g_signal_connect_data(c, "toggled", CB(+[](GtkToggleButton *b, gpointer p) {
		Ctx *c = (Ctx *)p;
		*c->v = gtk_toggle_button_get_active(b);
		if (c->f)
			c->f();
		settings_save();
	}), ctx, [](gpointer p, GClosure *) { delete (Ctx *)p; }, (GConnectFlags)0);
	return c;
}

GtkWidget *slider(const char *label, double lo, double hi, double step, double *value, std::function<void()> changed)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(6));
	GtkWidget *l = gtk_label_new(label);
	gtk_widget_set_size_request(l, S(150), -1);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_box_pack_start(GTK_BOX(row), l, FALSE, FALSE, 0);
	GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
	gtk_range_set_value(GTK_RANGE(s), *value);
	gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_RIGHT);
	struct Ctx { double *v; std::function<void()> f; };
	auto *ctx = new Ctx{value, std::move(changed)};
	g_signal_connect_data(s, "value-changed", CB(+[](GtkRange *r, gpointer p) {
		Ctx *c = (Ctx *)p;
		*c->v = gtk_range_get_value(r);
		if (c->f)
			c->f();
	}), ctx, [](gpointer p, GClosure *) { delete (Ctx *)p; }, (GConnectFlags)0);
	gtk_box_pack_start(GTK_BOX(row), s, TRUE, TRUE, 0);
	return row;
}

void rerender_all()
{
	for (ChatView *v : g_ui.views)
		v->rerender();
}
}  // namespace

void settings_dialog_show()
{
	if (g_settings_win) {
		gtk_window_present(GTK_WINDOW(g_settings_win));
		return;
	}
	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_settings_win = win;
	gtk_window_set_title(GTK_WINDOW(win), "dcord settings");
	gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(g_ui.window));
	gtk_window_set_default_size(GTK_WINDOW(win), S(480), -1);
	g_signal_connect(win, "destroy", CB(+[](GtkWidget *, gpointer) {
		g_settings_win = nullptr;
		settings_save();
	}), nullptr);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(6));
	g_object_set(box, "margin", S(12), nullptr);
	auto heading = [&](const char *t) {
		GtkWidget *l = gtk_label_new(nullptr);
		gtk_label_set_markup(GTK_LABEL(l), (std::string("<b>") + t + "</b>").c_str());
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, S(4));
	};
	// Title row with a close button (tiling WMs draw no title bar).
	GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *title = gtk_label_new(nullptr);
	gtk_label_set_markup(GTK_LABEL(title), "<big><b>Settings</b></big>");
	gtk_box_pack_start(GTK_BOX(top), title, FALSE, FALSE, 0);
	GtkWidget *x = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
	gtk_button_set_relief(GTK_BUTTON(x), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(x, "Close (Esc)");
	g_signal_connect_swapped(x, "clicked", G_CALLBACK(gtk_widget_destroy), win);
	gtk_box_pack_end(GTK_BOX(top), x, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);
	g_signal_connect_after(win, "key-press-event", CB(+[](GtkWidget *w, GdkEventKey *ev, gpointer) -> gboolean {
		if (ev->keyval == GDK_KEY_Escape) {
			gtk_widget_destroy(w);
			return TRUE;
		}
		return FALSE;
	}), nullptr);
	// (after: the push-to-talk key button sees Esc first)
	heading("Chat");
	gtk_box_pack_start(GTK_BOX(box), check("Play GIFs and animated emoji", &g_settings.animate_gifs, []() {
		g_media_animate = g_settings.animate_gifs;
		media_set_window_active(!g_ui.focused);  // re-evaluate every animation
		media_set_window_active(g_ui.focused);
	}), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), check("Show avatars", &g_settings.show_avatars, rerender_all), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), check("Compact mode (one line per message)", &g_settings.compact, rerender_all), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), check("Show member list (Ctrl+M)", &g_settings.show_member_list, []() {
		gtk_widget_set_visible(g_ui.members_scroll, g_settings.show_member_list);
	}), FALSE, FALSE, 0);

	heading("Voice");
	gtk_box_pack_start(GTK_BOX(box), check("Push to talk (otherwise voice activity)", &g_settings.ptt, voice_reload_audio), FALSE, FALSE, 0);
	GtkWidget *prow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(6));
	GtkWidget *pl = gtk_label_new("Push-to-talk key");
	gtk_widget_set_size_request(pl, S(150), -1);
	gtk_label_set_xalign(GTK_LABEL(pl), 0);
	GtkWidget *pk = gtk_button_new_with_label(g_settings.ptt_key.empty() ? "(click, then press a key)" : g_settings.ptt_key.c_str());
	g_signal_connect(pk, "clicked", CB(+[](GtkButton *b, gpointer) {
		gtk_button_set_label(b, "press a key…");
		g_object_set_data(G_OBJECT(b), "grab", GINT_TO_POINTER(1));
	}), nullptr);
	g_signal_connect(pk, "key-press-event", CB(+[](GtkWidget *w, GdkEventKey *ev, gpointer) -> gboolean {
		if (!g_object_get_data(G_OBJECT(w), "grab"))
			return FALSE;
		g_object_set_data(G_OBJECT(w), "grab", nullptr);
		const char *n = gdk_keyval_name(ev->keyval);
		g_settings.ptt_key = n ? n : "";
		gtk_button_set_label(GTK_BUTTON(w), g_settings.ptt_key.c_str());
		settings_save();
		voice_reload_audio();
		return TRUE;
	}), nullptr);
	gtk_box_pack_start(GTK_BOX(prow), pl, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(prow), pk, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), prow, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), slider("Voice activity (dB)", -80, -10, 1, &g_settings.vad_threshold_db, voice_reload_audio), FALSE, FALSE, 0);
	GtkWidget *meter = gtk_level_bar_new_for_interval(0, 80);  // dB + 80
	gtk_level_bar_set_mode(GTK_LEVEL_BAR(meter), GTK_LEVEL_BAR_MODE_CONTINUOUS);
	gtk_box_pack_start(GTK_BOX(box), meter, FALSE, FALSE, 0);
	guint tid = g_timeout_add(100, [](gpointer m) -> gboolean {
		gtk_level_bar_set_value(GTK_LEVEL_BAR(m), std::clamp(voice_input_level_db() + 80.0f, 0.0f, 80.0f));
		return G_SOURCE_CONTINUE;
	}, meter);
	g_signal_connect(meter, "destroy", CB(+[](GtkWidget *, gpointer t) { g_source_remove(GPOINTER_TO_UINT(t)); }), GUINT_TO_POINTER(tid));
	GtkWidget *mnote = gtk_label_new("The meter moves while you are in a voice channel.");
	gtk_style_context_add_class(gtk_widget_get_style_context(mnote), "dim-label");
	gtk_label_set_xalign(GTK_LABEL(mnote), 0);
	gtk_box_pack_start(GTK_BOX(box), mnote, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), slider("Mic gain", 0, 3, 0.05, &g_settings.input_gain, voice_reload_audio), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), slider("Output volume", 0, 2, 0.05, &g_settings.output_volume, voice_reload_audio), FALSE, FALSE, 0);
	auto dev_row = [&](const char *label, std::string *val) {
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(6));
		GtkWidget *l = gtk_label_new(label);
		gtk_widget_set_size_request(l, S(150), -1);
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		GtkWidget *e = gtk_entry_new();
		gtk_entry_set_text(GTK_ENTRY(e), val->c_str());
		gtk_entry_set_placeholder_text(GTK_ENTRY(e), "default (PipeWire node name)");
		g_signal_connect(e, "changed", CB(+[](GtkEntry *e, gpointer p) {
			*(std::string *)p = gtk_entry_get_text(e);
			settings_save();
		}), val);
		g_signal_connect(e, "activate", CB(+[](GtkEntry *, gpointer) { voice_reload_audio(); }), nullptr);
		gtk_box_pack_start(GTK_BOX(row), l, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(row), e, TRUE, TRUE, 0);
		gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
	};
	dev_row("Input device", &g_settings.input_device);
	dev_row("Output device", &g_settings.output_device);

	heading("Account");
	GtkWidget *who = gtk_label_new(("Logged in as " + g_store.me.username).c_str());
	gtk_label_set_xalign(GTK_LABEL(who), 0);
	gtk_box_pack_start(GTK_BOX(box), who, FALSE, FALSE, 0);
	GtkWidget *logout = gtk_button_new_with_label("Log out");
	gtk_style_context_add_class(gtk_widget_get_style_context(logout), "destructive-action");
	g_signal_connect(logout, "clicked", CB(+[](GtkButton *, gpointer w) {
		gtk_widget_destroy(GTK_WIDGET(w));
		app_logout();
	}), win);
	gtk_box_pack_start(GTK_BOX(box), logout, FALSE, FALSE, 0);
	GtkWidget *keys = gtk_label_new(nullptr);
	gtk_label_set_markup(GTK_LABEL(keys),
		"<small>Ctrl+K switch channel · Ctrl+W close tab · Ctrl+Tab next tab · Ctrl+M member list\n"
		"Ctrl+Shift+M mute · middle-click a channel for a new tab · double-click a voice channel to join\n"
		"Up in an empty box edits your last message · Esc cancels reply/edit · paste or drop images to attach</small>");
	gtk_label_set_xalign(GTK_LABEL(keys), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(keys), "dim-label");
	gtk_box_pack_start(GTK_BOX(box), keys, FALSE, FALSE, S(6));
	gtk_container_add(GTK_CONTAINER(win), box);
	gtk_widget_show_all(win);
}

// --- quick switcher ------------------------------------------------------------------------

namespace {
struct Switcher {
	GtkWidget *win, *entry, *list;
	std::vector<std::pair<Snowflake, std::string>> all;  // channel id, label
};

int fuzzy_score(const std::string &hay, const std::string &needle)
{
	if (needle.empty())
		return 1;
	size_t p = hay.find(needle);
	if (p != std::string::npos)
		return 1000 - (int)p;
	// subsequence match
	size_t j = 0;
	for (char c : hay)
		if (j < needle.size() && c == needle[j])
			j++;
	return j == needle.size() ? 100 : 0;
}

void switcher_fill(Switcher *s)
{
	gtk_container_foreach(GTK_CONTAINER(s->list), [](GtkWidget *w, gpointer) { gtk_widget_destroy(w); }, nullptr);
	std::string q = gtk_entry_get_text(GTK_ENTRY(s->entry));
	for (auto &c : q)
		c = (char)tolower((unsigned char)c);
	std::vector<std::pair<int, size_t>> hits;
	for (size_t i = 0; i < s->all.size(); i++) {
		std::string l = s->all[i].second;
		for (auto &c : l)
			c = (char)tolower((unsigned char)c);
		int sc = fuzzy_score(l, q);
		if (sc)
			hits.push_back({-sc, i});
	}
	std::stable_sort(hits.begin(), hits.end());
	for (size_t k = 0; k < hits.size() && k < 30; k++) {
		auto &e = s->all[hits[k].second];
		GtkWidget *l = gtk_label_new(e.second.c_str());
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		g_object_set_data(G_OBJECT(l), "cid", new Snowflake(e.first));
		g_signal_connect(l, "destroy", CB(+[](GtkWidget *w, gpointer) { delete (Snowflake *)g_object_get_data(G_OBJECT(w), "cid"); }), nullptr);
		gtk_list_box_insert(GTK_LIST_BOX(s->list), l, -1);
	}
	gtk_widget_show_all(s->list);
	if (GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(s->list), 0))
		gtk_list_box_select_row(GTK_LIST_BOX(s->list), r);
}

void switcher_open(Switcher *s, GtkListBoxRow *row)
{
	if (!row)
		return;
	GtkWidget *l = gtk_bin_get_child(GTK_BIN(row));
	Snowflake cid = *(Snowflake *)g_object_get_data(G_OBJECT(l), "cid");
	gtk_widget_destroy(s->win);
	ui_open_channel(cid, false);
}
}  // namespace

void quick_switcher_show()
{
	auto *s = new Switcher;
	for (const Channel *c : g_store.private_channels_sorted())
		s->all.push_back({c->id, "@" + g_store.channel_title(*c)});
	for (Snowflake gid : g_store.guild_order) {
		Guild *g = g_store.guild(gid);
		if (!g)
			continue;
		for (auto &node : g_store.channel_tree(gid)) {
			auto add = [&](const Channel *c) {
				if (c->type != CH_CATEGORY)
					s->all.push_back({c->id, (c->is_voice() ? "voice: " : "#") + c->name + "   — " + g->name});
			};
			add(node.ch);
			for (auto *c : node.children)
				add(c);
		}
	}
	s->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_transient_for(GTK_WINDOW(s->win), GTK_WINDOW(g_ui.window));
	gtk_window_set_modal(GTK_WINDOW(s->win), TRUE);
	gtk_window_set_decorated(GTK_WINDOW(s->win), FALSE);
	gtk_window_set_position(GTK_WINDOW(s->win), GTK_WIN_POS_CENTER_ON_PARENT);
	gtk_window_set_default_size(GTK_WINDOW(s->win), S(460), S(360));
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(4));
	g_object_set(box, "margin", S(8), nullptr);
	s->entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(s->entry), "Where would you like to go?");
	gtk_box_pack_start(GTK_BOX(box), s->entry, FALSE, FALSE, 0);
	GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
	s->list = gtk_list_box_new();
	gtk_container_add(GTK_CONTAINER(sw), s->list);
	gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(s->win), box);
	g_signal_connect(s->entry, "changed", CB(+[](GtkEditable *, gpointer p) { switcher_fill((Switcher *)p); }), s);
	g_signal_connect(s->entry, "activate", CB(+[](GtkEntry *, gpointer p) {
		Switcher *s = (Switcher *)p;
		switcher_open(s, gtk_list_box_get_selected_row(GTK_LIST_BOX(s->list)));
	}), s);
	g_signal_connect(s->list, "row-activated", CB(+[](GtkListBox *, GtkListBoxRow *r, gpointer p) {
		switcher_open((Switcher *)p, r);
	}), s);
	g_signal_connect(s->win, "key-press-event", CB(+[](GtkWidget *w, GdkEventKey *ev, gpointer p) -> gboolean {
		Switcher *s = (Switcher *)p;
		if (ev->keyval == GDK_KEY_Escape) {
			gtk_widget_destroy(w);
			return TRUE;
		}
		if (ev->keyval == GDK_KEY_Down || ev->keyval == GDK_KEY_Up) {
			GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(s->list));
			int i = r ? gtk_list_box_row_get_index(r) : -1;
			i += ev->keyval == GDK_KEY_Down ? 1 : -1;
			if (GtkListBoxRow *n = gtk_list_box_get_row_at_index(GTK_LIST_BOX(s->list), std::max(0, i)))
				gtk_list_box_select_row(GTK_LIST_BOX(s->list), n);
			return TRUE;
		}
		return FALSE;
	}), s);
	g_signal_connect(s->win, "destroy", CB(+[](GtkWidget *, gpointer p) { delete (Switcher *)p; }), s);
	switcher_fill(s);
	gtk_widget_show_all(s->win);
	gtk_widget_grab_focus(s->entry);
}
