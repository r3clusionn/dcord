// A channel view: header, message log (one GtkTextView), composer.
#include <algorithm>
#include <cmath>
#include <ctime>
#include <set>

#include "app.h"
#include "markdown.h"
#include "ui.h"
#include "ui_internal.h"
#include "voice.h"

struct ChatView::Impl {
	ChatView *self;
	GtkWidget *header_title, *header_topic, *header_box;
	GtkWidget *call_btn;    // DMs only
	GtkWidget *scroll;
	GtkTextView *view;
	GtkTextBuffer *buf;
	GtkWidget *typing_label;
	GtkWidget *reply_bar, *reply_label;
	GtkWidget *attach_box;
	GtkWidget *composer_scroll;
	GtkTextView *composer;
	GtkWidget *send_info;   // upload progress / slowmode
	GtkWidget *noperm;      // "you can't send here"

	struct Mark {
		Snowflake id;
		GtkTextMark *mark;
	};
	std::vector<Mark> marks;            // message start marks, ascending
	std::vector<GtkTextTag *> dyn_tags; // per-render tags to drop on clear
	std::set<std::string> revealed;     // revealed spoilers "msgid:n"
	Snowflake new_divider = 0;          // first unread message at open
	bool divider_drawn = false;
	int last_width = 0;      // view width used for the last layout
	guint relayout_timer = 0;
	bool pending_layout = false;  // render once the view has a real width
	int max_media_w();

	Snowflake reply_to = 0;
	bool reply_mention = true;
	Snowflake editing = 0;
	std::vector<UploadFile> files;
	std::map<Snowflake, int64_t> typing;
	guint typing_timer = 0;
	int64_t last_typing_sent = 0;
	bool sticky_bottom = true;
	bool loading = false;
	bool uploading = false;
	guint ack_timer = 0;
	std::shared_ptr<bool> alive = std::make_shared<bool>(true);

	// @mention autocomplete
	GtkWidget *mention_pop = nullptr, *mention_list = nullptr;
	std::vector<Snowflake> mention_ids;                    // rows shown
	std::vector<std::pair<std::string, Snowflake>> mentions;  // "@name" inserted -> user
	void mention_update();
	bool mention_query(GtkTextIter *at);  // finds "@query" before the cursor
	void mention_pick(int row);
	std::string apply_mentions(std::string text);

	// rendering
	void build();
	void clear_buffer();
	void render_all(bool keep_position);
	void render_message(GtkTextIter *it, const Message &m, const Message *prev);
	void render_spans(GtkTextIter *it, const Message &m, const std::vector<MdSpan> &spans, std::vector<GtkTextTag *> base,
			  bool jumbo, std::function<void()> after_newline = nullptr);
	void render_embed(GtkTextIter *it, const Message &m, const Embed &e, GtkTextTag *body);
	void render_media(GtkTextIter *it, const std::string &url, int w, int h, bool animate, bool spoiler,
			  const std::string &open_url, bool is_video);
	void render_reactions(GtkTextIter *it, const Message &m, GtkTextTag *body);
	void insert(GtkTextIter *it, const std::string &text, std::vector<GtkTextTag *> tags);
	void insert_widget(GtkTextIter *it, GtkWidget *w);
	void newline_if_needed(GtkTextIter *it, GtkTextTag *body) { if (gtk_text_iter_get_line_offset(it) != 0) insert(it, "\n", {body}); }
	GtkTextTag *tag(const char *name);
	GtkTextTag *color_tag(uint32_t rgb);
	GtkTextTag *data_tag(const char *key, const std::string &value);

	// history
	void load_initial();
	void load_older();
	bool at_bottom();
	void scroll_to_bottom();

	// input
	void send();
	void start_edit(Snowflake id);
	void set_reply(Snowflake id);
	void cancel_reply_edit();
	void update_attach_box();
	bool paste_attachments();
	void update_typing_label();
	void update_header();
	void update_permissions();
	void sent_typing();

	Snowflake message_at(GtkTextIter *it);
	void message_menu(Snowflake id, GdkEvent *ev);
	bool on_click(GdkEventButton *ev);
};

static const char *kDim = "#949ba4";

GtkTextTag *ChatView::Impl::tag(const char *name)
{
	return gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf), name);
}

GtkTextTag *ChatView::Impl::color_tag(uint32_t rgb)
{
	std::string name = "fg" + color_hex(rgb);
	GtkTextTag *t = tag(name.c_str());
	if (!t)
		t = gtk_text_buffer_create_tag(buf, name.c_str(), "foreground", color_hex(rgb).c_str(), nullptr);
	return t;
}

GtkTextTag *ChatView::Impl::data_tag(const char *key, const std::string &value)
{
	GtkTextTag *t = gtk_text_buffer_create_tag(buf, nullptr, nullptr);
	g_object_set_data_full(G_OBJECT(t), key, g_strdup(value.c_str()), g_free);
	dyn_tags.push_back(t);
	return t;
}

void ChatView::Impl::insert(GtkTextIter *it, const std::string &text, std::vector<GtkTextTag *> tags)
{
	if (text.empty())
		return;
	GtkTextMark *m = gtk_text_buffer_create_mark(buf, nullptr, it, TRUE);
	gtk_text_buffer_insert(buf, it, text.data(), (int)text.size());
	GtkTextIter start;
	gtk_text_buffer_get_iter_at_mark(buf, &start, m);
	for (GtkTextTag *t : tags)
		if (t)
			gtk_text_buffer_apply_tag(buf, t, &start, it);
	gtk_text_buffer_delete_mark(buf, m);
}

void ChatView::Impl::insert_widget(GtkTextIter *it, GtkWidget *w)
{
	GtkTextChildAnchor *a = gtk_text_buffer_create_child_anchor(buf, it);
	gtk_text_view_add_child_at_anchor(view, w, a);
	gtk_widget_show(w);
}

static std::string time_str(int64_t ms, bool with_date)
{
	time_t t = ms / 1000;
	struct tm tm, now;
	localtime_r(&t, &tm);
	time_t n = time(nullptr);
	localtime_r(&n, &now);
	char b[64];
	if (with_date && (tm.tm_yday != now.tm_yday || tm.tm_year != now.tm_year))
		strftime(b, sizeof b, "%d/%m/%Y %H:%M", &tm);
	else
		strftime(b, sizeof b, "%H:%M", &tm);
	return b;
}

static std::string day_str(int64_t ms)
{
	time_t t = ms / 1000;
	struct tm tm;
	localtime_r(&t, &tm);
	char b[64];
	strftime(b, sizeof b, "%A, %e %B %Y", &tm);
	return trim(b);
}

static int day_of(int64_t ms)
{
	time_t t = ms / 1000;
	struct tm tm;
	localtime_r(&t, &tm);
	return tm.tm_year * 400 + tm.tm_yday;
}

std::string twemoji_url(const std::string &seq)
{
	return "https://cdn.jsdelivr.net/gh/jdecked/twemoji@16.0.1/assets/72x72/" + twemoji_name(seq) + ".png";
}

void ChatView::Impl::build()
{
	GtkWidget *root = self->root_;
	// Header
	header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(6));
	gtk_style_context_add_class(gtk_widget_get_style_context(header_box), "chat-header");
	header_title = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(header_title), 0);
	header_topic = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(header_topic), 0);
	gtk_label_set_ellipsize(GTK_LABEL(header_topic), PANGO_ELLIPSIZE_END);
	gtk_style_context_add_class(gtk_widget_get_style_context(header_topic), "dim-label");
	gtk_box_pack_start(GTK_BOX(header_box), header_title, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(header_box), header_topic, TRUE, TRUE, 0);
	call_btn = gtk_button_new_with_label("📞 Call");
	gtk_button_set_relief(GTK_BUTTON(call_btn), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(call_btn, "Start or join a voice call");
	gtk_widget_set_no_show_all(call_btn, TRUE);
	g_signal_connect(call_btn, "clicked", CB(+[](GtkButton *, gpointer p) {
		Snowflake cid = static_cast<Impl *>(p)->self->cid_;
		app_voice_join(0, cid);
		// Ring once our voice state has made the call exist.
		g_timeout_add(1200, [](gpointer c) -> gboolean {
			api_ring((Snowflake)(uintptr_t)c);
			return G_SOURCE_REMOVE;
		}, (gpointer)(uintptr_t)cid);
	}), this);
	gtk_box_pack_start(GTK_BOX(header_box), call_btn, FALSE, FALSE, 0);
	GtkWidget *pins = gtk_button_new_with_label("Pins");
	gtk_button_set_relief(GTK_BUTTON(pins), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(pins, "Show pinned messages");
	g_signal_connect(pins, "clicked", CB(+[](GtkButton *b, gpointer p) {
		Impl *d = static_cast<Impl *>(p);
		auto alive = d->alive;
		Snowflake cid = d->self->cid_;
		api_pins(cid, [alive, d, cid, b](ApiResult &r) {
			if (!*alive || !r.ok() || !r.body.is_array())
				return;
			GtkWidget *pop = gtk_popover_new(GTK_WIDGET(b));
			GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
			gtk_widget_set_size_request(sw, S(420), S(360));
			GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, S(6));
			g_object_set(box, "margin", S(6), nullptr);
			if (r.body.empty())
				gtk_box_pack_start(GTK_BOX(box), gtk_label_new("No pinned messages."), FALSE, FALSE, 0);
			for (auto &mj : r.body) {
				MessagePtr m = g_store.parse_message(mj);
				std::string t = "<b>" + markup_escape(g_store.display_name(m->guild_id, m->author_id)) +
						"</b> <span foreground='" + kDim + "' size='small'>" + time_str(m->ts, true) +
						"</span>\n" + markup_escape(m->content.empty() ? "[attachment]" : m->content);
				GtkWidget *l = gtk_label_new(nullptr);
				gtk_label_set_markup(GTK_LABEL(l), t.c_str());
				gtk_label_set_xalign(GTK_LABEL(l), 0);
				gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
				gtk_label_set_selectable(GTK_LABEL(l), TRUE);
				gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
				gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
			}
			gtk_container_add(GTK_CONTAINER(sw), box);
			gtk_container_add(GTK_CONTAINER(pop), sw);
			gtk_widget_show_all(sw);
			gtk_popover_popup(GTK_POPOVER(pop));
			(void)cid;
		});
	}), this);
	gtk_box_pack_end(GTK_BOX(header_box), pins, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(root), header_box, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

	// Message log
	scroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	view = GTK_TEXT_VIEW(gtk_text_view_new());
	buf = gtk_text_view_get_buffer(view);
	gtk_text_view_set_editable(view, FALSE);
	gtk_text_view_set_cursor_visible(view, FALSE);
	gtk_text_view_set_wrap_mode(view, GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_left_margin(view, S(8));
	gtk_text_view_set_right_margin(view, S(8));
	gtk_text_view_set_top_margin(view, S(4));
	gtk_text_view_set_bottom_margin(view, S(6));
	gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(view)), "chat-log");
	gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(view));
	gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

	int body_margin = g_settings.compact ? S(8) : S(8 + 30);
	gtk_text_buffer_create_tag(buf, "body", "left-margin", body_margin, nullptr);
	gtk_text_buffer_create_tag(buf, "header", "pixels-above-lines", S(9), nullptr);
	gtk_text_buffer_create_tag(buf, "name", "weight", PANGO_WEIGHT_BOLD, nullptr);
	gtk_text_buffer_create_tag(buf, "dim", "foreground", kDim, nullptr);
	gtk_text_buffer_create_tag(buf, "small", "scale", 0.8, nullptr);
	gtk_text_buffer_create_tag(buf, "bold", "weight", PANGO_WEIGHT_BOLD, nullptr);
	gtk_text_buffer_create_tag(buf, "italic", "style", PANGO_STYLE_ITALIC, nullptr);
	gtk_text_buffer_create_tag(buf, "underline", "underline", PANGO_UNDERLINE_SINGLE, nullptr);
	gtk_text_buffer_create_tag(buf, "strike", "strikethrough", TRUE, nullptr);
	gtk_text_buffer_create_tag(buf, "code", "family", "monospace", "background", "#2b2d31", nullptr);
	gtk_text_buffer_create_tag(buf, "codeblock", "family", "monospace", "paragraph-background", "#1e1f22",
				   "left-margin", body_margin + S(6), "right-margin", S(12), "pixels-above-lines", S(2),
				   "pixels-below-lines", S(2), nullptr);
	gtk_text_buffer_create_tag(buf, "quote", "left-margin", body_margin + S(12), "foreground", "#c4c9ce", nullptr);
	gtk_text_buffer_create_tag(buf, "h1", "scale", 1.5, "weight", PANGO_WEIGHT_BOLD, nullptr);
	gtk_text_buffer_create_tag(buf, "h2", "scale", 1.3, "weight", PANGO_WEIGHT_BOLD, nullptr);
	gtk_text_buffer_create_tag(buf, "h3", "scale", 1.15, "weight", PANGO_WEIGHT_BOLD, nullptr);
	gtk_text_buffer_create_tag(buf, "subtext", "scale", 0.8, "foreground", kDim, nullptr);
	gtk_text_buffer_create_tag(buf, "link", "foreground", "#00a8fc", "underline", PANGO_UNDERLINE_SINGLE, nullptr);
	gtk_text_buffer_create_tag(buf, "mention", "foreground", "#c9cdfb", "background", "#3c4270", nullptr);
	gtk_text_buffer_create_tag(buf, "spoiler", "background", "#111214", "foreground", "#111214", nullptr);
	gtk_text_buffer_create_tag(buf, "spoiler-open", "background", "#2b2d31", nullptr);
	gtk_text_buffer_create_tag(buf, "mentioned", "paragraph-background", "#3b3424", nullptr);
	gtk_text_buffer_create_tag(buf, "pending", "foreground", "#80848e", nullptr);
	gtk_text_buffer_create_tag(buf, "failed", "foreground", "#f23f43", nullptr);
	gtk_text_buffer_create_tag(buf, "system", "style", PANGO_STYLE_ITALIC, "foreground", kDim, nullptr);
	gtk_text_buffer_create_tag(buf, "divider", "justification", GTK_JUSTIFY_CENTER, "foreground", kDim,
				   "scale", 0.8, "pixels-above-lines", S(10), nullptr);
	gtk_text_buffer_create_tag(buf, "newdiv", "justification", GTK_JUSTIFY_CENTER, "foreground", "#f23f43",
				   "scale", 0.8, "pixels-above-lines", S(8), nullptr);
	gtk_text_buffer_create_tag(buf, "reaction", "background", "#2b2d31", nullptr);
	gtk_text_buffer_create_tag(buf, "reaction-me", "background", "#373b6b", "foreground", "#dee0fc", nullptr);
	gtk_text_buffer_create_tag(buf, "reply", "scale", 0.85, "foreground", "#b5bac1", "pixels-above-lines", S(9),
				   nullptr);
	gtk_text_buffer_create_tag(buf, "header-tight", "pixels-above-lines", 0, nullptr);
	gtk_text_buffer_create_tag(buf, "embed", "left-margin", body_margin + S(4), nullptr);
	gtk_text_buffer_create_tag(buf, "embed-title", "weight", PANGO_WEIGHT_BOLD, nullptr);
	gtk_text_buffer_create_tag(buf, "edited", "scale", 0.7, "foreground", kDim, nullptr);
	gtk_text_buffer_create_tag(buf, "bot", "scale", 0.7, "foreground", "#8c96ff", "weight", PANGO_WEIGHT_BOLD, nullptr);

	gtk_widget_add_events(GTK_WIDGET(view), GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
	g_signal_connect(view, "button-release-event", CB(+[](GtkWidget *, GdkEventButton *ev, gpointer p) -> gboolean {
		return static_cast<Impl *>(p)->on_click(ev);
	}), this);
	g_signal_connect(view, "button-press-event", CB(+[](GtkWidget *w, GdkEventButton *ev, gpointer p) -> gboolean {
		Impl *d = static_cast<Impl *>(p);
		if (ev->button != 3)
			return FALSE;
		int bx, by;
		gtk_text_view_window_to_buffer_coords(d->view, GTK_TEXT_WINDOW_WIDGET, (int)ev->x, (int)ev->y, &bx, &by);
		GtkTextIter it;
		gtk_text_view_get_iter_at_location(d->view, &it, bx, by);
		Snowflake id = d->message_at(&it);
		if (id)
			d->message_menu(id, (GdkEvent *)ev);
		(void)w;
		return TRUE;
	}), this);
	g_signal_connect(view, "motion-notify-event", CB(+[](GtkWidget *w, GdkEventMotion *ev, gpointer p) -> gboolean {
		Impl *d = static_cast<Impl *>(p);
		int bx, by;
		gtk_text_view_window_to_buffer_coords(d->view, GTK_TEXT_WINDOW_WIDGET, (int)ev->x, (int)ev->y, &bx, &by);
		GtkTextIter it;
		gtk_text_view_get_iter_at_location(d->view, &it, bx, by);
		bool hand = false;
		GSList *tags = gtk_text_iter_get_tags(&it);
		for (GSList *l = tags; l; l = l->next) {
			GObject *t = G_OBJECT(l->data);
			if (g_object_get_data(t, "url") || g_object_get_data(t, "chan") || g_object_get_data(t, "react") ||
			    g_object_get_data(t, "spoil") || g_object_get_data(t, "jump") || g_object_get_data(t, "user"))
				hand = true;
		}
		g_slist_free(tags);
		GdkWindow *win = gtk_text_view_get_window(d->view, GTK_TEXT_WINDOW_TEXT);
		static GdkCursor *hand_c = nullptr, *text_c = nullptr;
		if (!hand_c) {
			hand_c = gdk_cursor_new_from_name(gtk_widget_get_display(w), "pointer");
			text_c = gdk_cursor_new_from_name(gtk_widget_get_display(w), "text");
		}
		gdk_window_set_cursor(win, hand ? hand_c : text_c);
		return FALSE;
	}), this);

	GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
	// GtkTextView moves the adjustment itself while it lays out lines, so only
	// user input (wheel, scrollbar, keys) may unstick the view from the bottom.
	g_signal_connect(adj, "value-changed", CB(+[](GtkAdjustment *a, gpointer p) {
		Impl *d = static_cast<Impl *>(p);
		if (d->at_bottom())
			d->sticky_bottom = true;
		if (!d->sticky_bottom && gtk_adjustment_get_value(a) < S(400) &&
		    gtk_adjustment_get_upper(a) > gtk_adjustment_get_page_size(a))
			d->load_older();
		if (d->sticky_bottom)
			d->self->maybe_ack();
	}), this);
	g_signal_connect(adj, "changed", CB(+[](GtkAdjustment *, gpointer p) {
		Impl *d = static_cast<Impl *>(p);
		if (d->sticky_bottom)
			d->scroll_to_bottom();
	}), this);
	auto user_scrolled = +[](gpointer p) -> gboolean {
		Impl *d = static_cast<Impl *>(p);
		d->sticky_bottom = d->at_bottom();
		GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(d->scroll));
		if (gtk_adjustment_get_value(a) < S(400))
			d->load_older();
		return G_SOURCE_REMOVE;
	};
	g_object_set_data(G_OBJECT(scroll), "user-scrolled", (gpointer)user_scrolled);
	g_signal_connect_after(view, "scroll-event", CB(+[](GtkWidget *w, GdkEvent *, gpointer p) -> gboolean {
		auto f = (GSourceFunc)g_object_get_data(G_OBJECT(static_cast<Impl *>(p)->scroll), "user-scrolled");
		g_idle_add(f, p);
		(void)w;
		return FALSE;
	}), this);
	g_signal_connect_after(gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(scroll)), "change-value",
		CB(+[](GtkRange *, GtkScrollType, double, gpointer p) -> gboolean {
			auto f = (GSourceFunc)g_object_get_data(G_OBJECT(static_cast<Impl *>(p)->scroll), "user-scrolled");
			g_idle_add(f, p);
			return FALSE;
		}), this);
	g_signal_connect_after(view, "key-press-event", CB(+[](GtkWidget *, GdkEventKey *, gpointer p) -> gboolean {
		auto f = (GSourceFunc)g_object_get_data(G_OBJECT(static_cast<Impl *>(p)->scroll), "user-scrolled");
		g_idle_add(f, p);
		return FALSE;
	}), this);

	// Images are laid out for the current width; redo the layout when it changes a lot.
	g_signal_connect(view, "size-allocate", CB(+[](GtkWidget *, GdkRectangle *a, gpointer p) {
		Impl *d = static_cast<Impl *>(p);
		if (d->pending_layout && a->width > 1) {
			d->pending_layout = false;
			g_idle_add([](gpointer p) -> gboolean {
				static_cast<Impl *>(p)->render_all(false);
				return G_SOURCE_REMOVE;
			}, d);
			return;
		}
		int lw = d->last_width;
		bool was_clamped = lw > 1 && lw < S(400) + S(80);
		if (lw <= 1 || std::abs(a->width - lw) < S(40) || (a->width > lw && !was_clamped && lw > S(500)))
			return;
		if (d->relayout_timer)
			g_source_remove(d->relayout_timer);
		d->relayout_timer = g_timeout_add(150, [](gpointer p) -> gboolean {
			Impl *d = static_cast<Impl *>(p);
			d->relayout_timer = 0;
			d->render_all(true);
			return G_SOURCE_REMOVE;
		}, d);
	}), this);

	// Drag & drop files
	GtkTargetEntry te = {(gchar *)"text/uri-list", 0, 0};
	gtk_drag_dest_set(GTK_WIDGET(view), GTK_DEST_DEFAULT_ALL, &te, 1, GDK_ACTION_COPY);
	g_signal_connect(view, "drag-data-received", CB(+[](GtkWidget *, GdkDragContext *, int, int, GtkSelectionData *sd,
		guint, guint, gpointer p) {
		Impl *d = static_cast<Impl *>(p);
		gchar **uris = gtk_selection_data_get_uris(sd);
		for (int i = 0; uris && uris[i]; i++) {
			gchar *f = g_filename_from_uri(uris[i], nullptr, nullptr);
			if (f) {
				d->self->attach_file(f);
				g_free(f);
			}
		}
		g_strfreev(uris);
	}), this);

	// Typing indicator / status line
	typing_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(typing_label), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(typing_label), "typing");
	gtk_box_pack_start(GTK_BOX(root), typing_label, FALSE, FALSE, 0);

	// Reply / edit bar
	reply_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(6));
	gtk_style_context_add_class(gtk_widget_get_style_context(reply_bar), "reply-bar");
	reply_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(reply_label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(reply_label), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start(GTK_BOX(reply_bar), reply_label, TRUE, TRUE, 0);
	GtkWidget *ping = gtk_check_button_new_with_label("@mention");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ping), TRUE);
	g_object_set_data(G_OBJECT(reply_bar), "ping", ping);
	g_signal_connect(ping, "toggled", CB(+[](GtkToggleButton *b, gpointer p) {
		static_cast<Impl *>(p)->reply_mention = gtk_toggle_button_get_active(b);
	}), this);
	gtk_box_pack_start(GTK_BOX(reply_bar), ping, FALSE, FALSE, 0);
	GtkWidget *x = gtk_button_new_with_label("✕");
	gtk_button_set_relief(GTK_BUTTON(x), GTK_RELIEF_NONE);
	g_signal_connect(x, "clicked", CB(+[](GtkButton *, gpointer p) { static_cast<Impl *>(p)->cancel_reply_edit(); }), this);
	gtk_box_pack_start(GTK_BOX(reply_bar), x, FALSE, FALSE, 0);
	gtk_widget_set_no_show_all(reply_bar, TRUE);
	gtk_box_pack_start(GTK_BOX(root), reply_bar, FALSE, FALSE, 0);

	attach_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(4));
	gtk_widget_set_no_show_all(attach_box, TRUE);
	gtk_box_pack_start(GTK_BOX(root), attach_box, FALSE, FALSE, 0);

	// Composer row
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(2));
	gtk_style_context_add_class(gtk_widget_get_style_context(row), "composer-row");
	GtkWidget *att = gtk_button_new_with_label("+");
	gtk_widget_set_tooltip_text(att, "Attach files (or paste / drop them)");
	gtk_button_set_relief(GTK_BUTTON(att), GTK_RELIEF_NONE);
	g_signal_connect(att, "clicked", CB(+[](GtkButton *, gpointer p) {
		Impl *d = static_cast<Impl *>(p);
		GtkFileChooserNative *fc = gtk_file_chooser_native_new("Attach files", GTK_WINDOW(g_ui.window),
			GTK_FILE_CHOOSER_ACTION_OPEN, "Attach", "Cancel");
		gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(fc), TRUE);
		if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
			GSList *fs = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(fc));
			for (GSList *l = fs; l; l = l->next) {
				d->self->attach_file((const char *)l->data);
				g_free(l->data);
			}
			g_slist_free(fs);
		}
		g_object_unref(fc);
	}), this);
	gtk_box_pack_start(GTK_BOX(row), att, FALSE, FALSE, 0);

	composer_scroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(composer_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(composer_scroll), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(composer_scroll), S(160));
	composer = GTK_TEXT_VIEW(gtk_text_view_new());
	gtk_text_view_set_wrap_mode(composer, GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_accepts_tab(composer, FALSE);
	gtk_text_view_set_top_margin(composer, S(5));
	gtk_text_view_set_bottom_margin(composer, S(5));
	gtk_text_view_set_left_margin(composer, S(6));
	gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(composer)), "composer");
	gtk_container_add(GTK_CONTAINER(composer_scroll), GTK_WIDGET(composer));
	gtk_box_pack_start(GTK_BOX(row), composer_scroll, TRUE, TRUE, 0);
	g_signal_connect(composer, "key-press-event", CB(+[](GtkWidget *, GdkEventKey *ev, gpointer p) -> gboolean {
		Impl *d = static_cast<Impl *>(p);
		bool shift = ev->state & GDK_SHIFT_MASK, ctrl = ev->state & GDK_CONTROL_MASK;
		if (d->mention_pop && gtk_widget_get_visible(d->mention_pop) && !d->mention_ids.empty()) {
			GtkListBox *lb = GTK_LIST_BOX(d->mention_list);
			GtkListBoxRow *r = gtk_list_box_get_selected_row(lb);
			int i = r ? gtk_list_box_row_get_index(r) : 0, n = (int)d->mention_ids.size();
			switch (ev->keyval) {
			case GDK_KEY_Up:
			case GDK_KEY_Down:
				i = (i + (ev->keyval == GDK_KEY_Up ? n - 1 : 1)) % n;
				gtk_list_box_select_row(lb, gtk_list_box_get_row_at_index(lb, i));
				return TRUE;
			case GDK_KEY_Tab:
			case GDK_KEY_Return:
			case GDK_KEY_KP_Enter:
				d->mention_pick(i);
				return TRUE;
			case GDK_KEY_Escape:
				gtk_widget_hide(d->mention_pop);
				return TRUE;
			}
		}
		if ((ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) && !shift) {
			d->send();
			return TRUE;
		}
		if (ev->keyval == GDK_KEY_Escape) {
			d->cancel_reply_edit();
			return TRUE;
		}
		GtkTextBuffer *b = gtk_text_view_get_buffer(d->composer);
		if (ev->keyval == GDK_KEY_Up && gtk_text_buffer_get_char_count(b) == 0) {
			// Edit the last message I sent here.
			auto it = g_store.messages.find(d->self->cid_);
			if (it != g_store.messages.end())
				for (auto m = it->second.msgs.rbegin(); m != it->second.msgs.rend(); ++m)
					if ((*m)->author_id == g_store.me.id && (*m)->state == Message::Sent) {
						d->start_edit((*m)->id);
						return TRUE;
					}
		}
		if ((ctrl && (ev->keyval == GDK_KEY_v || ev->keyval == GDK_KEY_V)) || (shift && ev->keyval == GDK_KEY_Insert))
			if (d->paste_attachments())
				return TRUE;
		return FALSE;
	}), this);
	g_signal_connect(composer, "paste-clipboard", CB(+[](GtkTextView *tv, gpointer p) {
		if (static_cast<Impl *>(p)->paste_attachments())
			g_signal_stop_emission_by_name(tv, "paste-clipboard");
	}), this);
	g_signal_connect(gtk_text_view_get_buffer(composer), "changed", CB(+[](GtkTextBuffer *b, gpointer p) {
		static_cast<Impl *>(p)->mention_update();
		if (gtk_text_buffer_get_char_count(b) > 0)
			static_cast<Impl *>(p)->sent_typing();
	}), this);

	GtkWidget *emo = gtk_button_new_with_label("☺");
	gtk_widget_set_tooltip_text(emo, "Emoji");
	gtk_button_set_relief(GTK_BUTTON(emo), GTK_RELIEF_NONE);
	g_signal_connect(emo, "clicked", CB(+[](GtkButton *b, gpointer p) {
		Impl *d = static_cast<Impl *>(p);
		emoji_picker_show(GTK_WIDGET(b), d->self->gid_, [d](const std::string &text) {
			GtkTextBuffer *cb = gtk_text_view_get_buffer(d->composer);
			gtk_text_buffer_insert_at_cursor(cb, text.c_str(), -1);
			gtk_widget_grab_focus(GTK_WIDGET(d->composer));
		});
	}), this);
	gtk_box_pack_start(GTK_BOX(row), emo, FALSE, FALSE, 0);
	GtkWidget *gif = gtk_button_new_with_label("GIF");
	gtk_widget_set_tooltip_text(gif, "Search and send a GIF");
	gtk_button_set_relief(GTK_BUTTON(gif), GTK_RELIEF_NONE);
	g_signal_connect(gif, "clicked", CB(+[](GtkButton *b, gpointer p) {
		Impl *d = static_cast<Impl *>(p);
		auto alive = d->alive;
		gif_picker_show(GTK_WIDGET(b), [d, alive](const std::string &url) {
			if (!*alive)
				return;
			std::string nonce = sf_str(sf_from_unix_ms(now_ms()));
			api_send_message(d->self->cid_, url, nonce, d->reply_to, d->reply_mention, {}, nullptr);
			d->cancel_reply_edit();
			d->self->focus_composer();
		});
	}), this);
	gtk_box_pack_start(GTK_BOX(row), gif, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(root), row, FALSE, FALSE, 0);

	send_info = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(send_info), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(send_info), "dim-label");
	gtk_widget_set_no_show_all(send_info, TRUE);
	gtk_box_pack_start(GTK_BOX(root), send_info, FALSE, FALSE, 0);
	noperm = row;
}

void ChatView::Impl::clear_buffer()
{
	for (auto &mk : marks)
		gtk_text_buffer_delete_mark(buf, mk.mark);
	marks.clear();
	gtk_text_buffer_set_text(buf, "", 0);
	GtkTextTagTable *tt = gtk_text_buffer_get_tag_table(buf);
	for (GtkTextTag *t : dyn_tags)
		gtk_text_tag_table_remove(tt, t);
	dyn_tags.clear();
	divider_drawn = false;
}

bool ChatView::Impl::at_bottom()
{
	GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
	return gtk_adjustment_get_value(a) + gtk_adjustment_get_page_size(a) >= gtk_adjustment_get_upper(a) - S(30);
}

void ChatView::Impl::scroll_to_bottom()
{
	GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
	gtk_adjustment_set_value(a, gtk_adjustment_get_upper(a) - gtk_adjustment_get_page_size(a));
}

// One-line plain-text version of a message for reply previews.
static std::string plain_snippet(const Message &m)
{
	std::string out;
	for (const MdSpan &sp : md_parse(m.content)) {
		switch (sp.kind) {
		case MdSpan::Text: out += (sp.style & MD_SPOILER) ? "▇▇▇" : sp.text; break;
		case MdSpan::Newline: out += ' '; break;
		case MdSpan::CustomEmoji: out += ":" + sp.text + ":"; break;
		case MdSpan::UnicodeEmoji: out += sp.text; break;
		case MdSpan::UserMention: out += "@" + g_store.display_name(m.guild_id, sp.id); break;
		case MdSpan::RoleMention: out += "@role"; break;
		case MdSpan::ChannelMention: {
			const Channel *c = g_store.channel(sp.id);
			out += c ? "#" + c->name : "#channel";
			break;
		}
		case MdSpan::Everyone: out += sp.text; break;
		case MdSpan::Timestamp: out += format_timestamp(sp.ts, sp.ts_style); break;
		}
		if (out.size() > 200)
			break;
	}
	return out;
}

static bool is_system(int type)
{
	return !(type == 0 || type == 19 || type == 20 || type == 23);
}

static std::string system_text(const Message &m)
{
	std::string who = g_store.display_name(m.guild_id, m.author_id);
	auto first_mention = [&]() {
		return m.mentions.empty() ? std::string("someone") : g_store.display_name(m.guild_id, m.mentions[0]);
	};
	switch (m.type) {
	case 1: return who + " added " + first_mention() + " to the group.";
	case 2: return who + " removed " + first_mention() + " from the group.";
	case 3: return who + " " + (m.call_info.empty() ? "started a call" : m.call_info) + ".";
	case 4: return who + " changed the channel name: " + m.content;
	case 5: return who + " changed the channel icon.";
	case 6: return who + " pinned a message to this channel.";
	case 7: return who + " joined the server.";
	case 8: return who + " boosted the server!";
	case 9: case 10: case 11: return who + " boosted the server! The server reached a new level.";
	case 12: return who + " added a channel follow.";
	case 18: return who + " started a thread: " + m.content;
	case 21: return "Thread starter message";
	case 24: return "AutoMod blocked a message.";
	case 46: return "Poll results.";
	default: return m.content.empty() ? who + " did something (message type " + std::to_string(m.type) + ")." : m.content;
	}
}

void ChatView::Impl::render_media(GtkTextIter *it, const std::string &url, int w, int h, bool animate, bool spoiler,
				  const std::string &open_url, bool is_video)
{
	MediaOpts o;
	o.max_w = w;
	o.max_h = h;
	o.animate = animate;
	MediaPtr m = media_get(url, o);
	std::string ou = open_url;
	GtkWidget *wd = media_widget(m, w, h, [ou, m, is_video](int button) {
		if (button != 1)
			return;
		if (is_video) {
			gchar *mpv = g_find_program_in_path("mpv");
			if (mpv) {
				spawn_detached({mpv, "--force-window=yes", "--loop-file=no", ou});
				g_free(mpv);
			} else {
				open_external(ou);
			}
		} else {
			image_viewer_show(ou, m->w, m->h, ou);
		}
	}, spoiler);
	if (is_video) {
		// Draw a play triangle over the thumbnail.
		g_signal_connect_after(wd, "draw", CB(+[](GtkWidget *w, cairo_t *cr, gpointer) -> gboolean {
			int aw = gtk_widget_get_allocated_width(w), ah = gtk_widget_get_allocated_height(w);
			double r = S(18);
			cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
			cairo_arc(cr, aw / 2.0, ah / 2.0, r, 0, 2 * M_PI);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_move_to(cr, aw / 2.0 - r * 0.35, ah / 2.0 - r * 0.5);
			cairo_line_to(cr, aw / 2.0 + r * 0.55, ah / 2.0);
			cairo_line_to(cr, aw / 2.0 - r * 0.35, ah / 2.0 + r * 0.5);
			cairo_close_path(cr);
			cairo_fill(cr);
			return FALSE;
		}), nullptr);
	}
	insert_widget(it, wd);
}

int ChatView::Impl::max_media_w()
{
	int body = g_settings.compact ? S(8) : S(8 + 30);
	int avail = gtk_widget_get_allocated_width(GTK_WIDGET(view)) - body - S(28);
	if (avail < S(120))
		avail = S(400);  // not allocated yet
	return std::min(S(400), avail);
}

static void fit_box(int w, int h, int maxw, int maxh, int *ow, int *oh)
{
	if (w <= 0 || h <= 0) {
		*ow = maxw;
		*oh = maxh * 2 / 3;
		return;
	}
	double sc = std::min({1.0 * g_ui_scale, (double)maxw / w, (double)maxh / h});
	*ow = std::max(1, (int)(w * sc));
	*oh = std::max(1, (int)(h * sc));
}

void ChatView::Impl::render_spans(GtkTextIter *it, const Message &m, const std::vector<MdSpan> &spans,
				  std::vector<GtkTextTag *> base, bool jumbo, std::function<void()> after_newline)
{
	int esz = jumbo ? S(40) : S(18);
	for (const MdSpan &sp : spans) {
		std::vector<GtkTextTag *> tags = base;
		if (sp.style & MD_BOLD) tags.push_back(tag("bold"));
		if (sp.style & MD_ITALIC) tags.push_back(tag("italic"));
		if (sp.style & MD_UNDERLINE) tags.push_back(tag("underline"));
		if (sp.style & MD_STRIKE) tags.push_back(tag("strike"));
		if (sp.style & MD_CODE) tags.push_back(tag("code"));
		if (sp.style & MD_CODEBLOCK) tags.push_back(tag("codeblock"));
		if (sp.style & MD_QUOTE) tags.push_back(tag("quote"));
		if (sp.style & MD_H1) tags.push_back(tag("h1"));
		if (sp.style & MD_H2) tags.push_back(tag("h2"));
		if (sp.style & MD_H3) tags.push_back(tag("h3"));
		if (sp.style & MD_SUBTEXT) tags.push_back(tag("subtext"));
		if (sp.style & MD_LINK) {
			tags.push_back(tag("link"));
			tags.push_back(data_tag("url", sp.url));
		}
		bool hidden_spoiler = false;
		if (sp.style & MD_SPOILER) {
			std::string key = sf_str(m.id) + ":" + std::to_string(sp.spoiler);
			if (revealed.count(key)) {
				tags.push_back(tag("spoiler-open"));
			} else {
				tags.push_back(tag("spoiler"));
				tags.push_back(data_tag("spoil", key));
				hidden_spoiler = true;
			}
		}
		switch (sp.kind) {
		case MdSpan::Text:
			insert(it, sp.text, tags);
			break;
		case MdSpan::Newline:
			insert(it, "\n", base);
			if (after_newline)
				after_newline();
			break;
		case MdSpan::UnicodeEmoji:
			if (hidden_spoiler) {
				insert(it, "▇▇", tags);
			} else {
				MediaOpts o;
				o.max_w = o.max_h = esz;
				o.disk_cache = true;
				GtkWidget *w = media_widget(media_get(twemoji_url(sp.text), o), esz, esz);
				gtk_widget_set_tooltip_text(w, sp.text.c_str());
				insert_widget(it, w);
			}
			break;
		case MdSpan::CustomEmoji: {
			if (hidden_spoiler) {
				insert(it, "▇▇", tags);
				break;
			}
			MediaOpts o;
			o.max_w = o.max_h = esz;
			o.disk_cache = true;
			o.animate = sp.animated;
			GtkWidget *w = media_widget(media_get(emoji_url(sp.id, sp.animated, jumbo ? 96 : 48), o), esz, esz);
			std::string tip = ":" + sp.text + ":";
			gtk_widget_set_tooltip_text(w, tip.c_str());
			insert_widget(it, w);
			break;
		}
		case MdSpan::UserMention: {
			tags.push_back(tag("mention"));
			tags.push_back(data_tag("user", sf_str(sp.id)));
			insert(it, "@" + g_store.display_name(m.guild_id, sp.id), tags);
			break;
		}
		case MdSpan::RoleMention: {
			std::string name = "@unknown-role";
			if (Guild *g = g_store.guild(m.guild_id))
				if (auto r = g->roles.find(sp.id); r != g->roles.end()) {
					name = "@" + r->second.name;
					if (r->second.color)
						tags.push_back(color_tag(r->second.color));
				}
			tags.push_back(tag("mention"));
			insert(it, name, tags);
			break;
		}
		case MdSpan::ChannelMention: {
			const Channel *c = g_store.channel(sp.id);
			tags.push_back(tag("mention"));
			tags.push_back(data_tag("chan", sf_str(sp.id)));
			insert(it, c ? "#" + c->name : "#unknown", tags);
			break;
		}
		case MdSpan::Everyone:
			tags.push_back(tag("mention"));
			insert(it, sp.text, tags);
			break;
		case MdSpan::Timestamp:
			tags.push_back(tag("code"));
			insert(it, format_timestamp(sp.ts, sp.ts_style), tags);
			break;
		}
	}
}

static bool emoji_only(const std::vector<MdSpan> &spans)
{
	int n = 0;
	for (auto &s : spans) {
		if (s.kind == MdSpan::UnicodeEmoji || s.kind == MdSpan::CustomEmoji)
			n++;
		else if (!(s.kind == MdSpan::Text && trim(s.text).empty()))
			return false;
	}
	return n > 0 && n <= 27;
}

void ChatView::Impl::render_embed(GtkTextIter *it, const Message &m, const Embed &e, GtkTextTag *body)
{
	int maxw = max_media_w() - S(16), maxh = S(300);
	// gifv (Tenor, Giphy, Discord video gifs): just the animation.
	if (e.type == "gifv" && !e.video.empty()) {
		int w, h;
		fit_box(e.video.width ? e.video.width : e.thumbnail.width, e.video.height ? e.video.height : e.thumbnail.height,
			maxw, maxh, &w, &h);
		std::string src = e.video.proxy_url.empty() ? e.video.url : e.video.proxy_url;
		newline_if_needed(it, body);
		render_media(it, src, w, h, true, false, e.url.empty() ? e.video.url : e.url, false);
		return;
	}
	if (e.type == "image" && (!e.thumbnail.empty() || !e.image.empty())) {
		const EmbedMedia &im = e.image.empty() ? e.thumbnail : e.image;
		int w, h;
		fit_box(im.width, im.height, maxw, maxh, &w, &h);
		bool gif = im.url.find(".gif") != std::string::npos;
		std::string src = im.proxy_url.empty() ? im.url : (gif ? im.proxy_url : media_proxy_sized(im.proxy_url, w, h));
		newline_if_needed(it, body);
		render_media(it, src, w, h, gif, false, im.url, false);
		return;
	}
	if (e.type == "video" && !e.thumbnail.empty() && e.title.empty() && e.description.empty()) {
		int w, h;
		fit_box(e.thumbnail.width, e.thumbnail.height, maxw, maxh, &w, &h);
		newline_if_needed(it, body);
		render_media(it, media_proxy_sized(e.thumbnail.proxy_url, w, h), w, h, false, false,
			     e.video.url.empty() ? e.url : e.video.url, true);
		return;
	}
	GtkTextTag *et = tag("embed");
	GtkTextTag *bar = e.has_color ? color_tag(e.color) : color_tag(0x4e5058);
	auto line = [&](std::function<void()> content) {
		newline_if_needed(it, body);
		insert(it, "▍ ", {et, bar});
		content();
	};
	if (!e.provider.empty())
		line([&] { insert(it, e.provider, {et, tag("dim"), tag("small")}); });
	if (!e.author.empty())
		line([&] {
			std::vector<GtkTextTag *> t = {et, tag("bold"), tag("small")};
			if (!e.author_url.empty()) {
				t.push_back(tag("link"));
				t.push_back(data_tag("url", e.author_url));
			}
			insert(it, e.author, t);
		});
	if (!e.title.empty())
		line([&] {
			std::vector<GtkTextTag *> t = {et, tag("embed-title")};
			if (!e.url.empty()) {
				t.push_back(tag("link"));
				t.push_back(data_tag("url", e.url));
			}
			insert(it, e.title, t);
		});
	if (!e.description.empty()) {
		auto spans = md_parse(e.description);
		line([&] { render_spans(it, m, spans, {et}, false, [&] { insert(it, "▍ ", {et, bar}); }); });
	}
	for (auto &f : e.fields) {
		line([&] { insert(it, f.name, {et, tag("bold")}); });
		auto spans = md_parse(f.value);
		line([&] { render_spans(it, m, spans, {et}, false, [&] { insert(it, "▍ ", {et, bar}); }); });
	}
	const EmbedMedia &img = !e.image.empty() ? e.image : e.thumbnail;
	if (!img.empty()) {
		int w, h;
		bool thumb_only = e.image.empty();
		fit_box(img.width, img.height, thumb_only ? S(160) : maxw, thumb_only ? S(160) : maxh, &w, &h);
		bool gif = img.url.find(".gif") != std::string::npos;
		std::string src = img.proxy_url.empty() ? img.url : (gif ? img.proxy_url : media_proxy_sized(img.proxy_url, w, h));
		bool video = e.type == "video" && !e.video.url.empty();
		newline_if_needed(it, body);
		insert(it, "▍ ", {et, bar});
		render_media(it, src, w, h, gif, false, video ? e.video.url : img.url, video);
	}
	if (!e.footer.empty())
		line([&] { insert(it, e.footer, {et, tag("dim"), tag("small")}); });
}

void ChatView::Impl::render_reactions(GtkTextIter *it, const Message &m, GtkTextTag *body)
{
	if (m.reactions.empty())
		return;
	insert(it, "\n", {body});
	for (const Reaction &r : m.reactions) {
		GtkTextTag *bg = tag(r.me ? "reaction-me" : "reaction");
		GtkTextTag *act = data_tag("react", sf_str(m.id) + "\x1f" + r.key() + "\x1f" + (r.me ? "1" : "0"));
		insert(it, " ", {body, bg, act});
		int esz = S(16);
		MediaOpts o;
		o.max_w = o.max_h = esz;
		o.disk_cache = true;
		GtkWidget *w;
		if (r.emoji_id) {
			o.animate = r.animated;
			w = media_widget(media_get(emoji_url(r.emoji_id, r.animated, 48), o), esz, esz);
		} else {
			w = media_widget(media_get(twemoji_url(r.name), o), esz, esz);
		}
		gtk_widget_set_tooltip_text(w, r.emoji_id ? (":" + r.name + ":").c_str() : r.name.c_str());
		insert_widget(it, w);
		insert(it, " " + std::to_string(r.count) + " ", {body, bg, act});
		insert(it, " ", {body});
	}
}

void ChatView::Impl::render_message(GtkTextIter *it, const Message &m, const Message *prev)
{
	bool first_in_buffer = gtk_text_iter_is_start(it) && gtk_text_iter_is_end(it);
	auto nl = [&]() {
		if (!first_in_buffer)
			insert(it, "\n", {});
		first_in_buffer = false;
	};
	// Day separator
	if (!prev || day_of(prev->ts) != day_of(m.ts)) {
		nl();
		insert(it, "──────  " + day_str(m.ts) + "  ──────", {tag("divider")});
	}
	if (new_divider && m.id == new_divider && !divider_drawn) {
		nl();
		insert(it, "──────  new messages  ──────", {tag("newdiv")});
		divider_drawn = true;
	}
	GtkTextMark *mark = gtk_text_buffer_create_mark(buf, nullptr, it, TRUE);
	marks.push_back({m.id, mark});

	GtkTextTag *body = tag("body");
	std::vector<GtkTextTag *> state_tags;
	if (m.state == Message::Pending)
		state_tags.push_back(tag("pending"));
	else if (m.state == Message::Failed)
		state_tags.push_back(tag("failed"));
	bool mentioned = m.author_id != g_store.me.id && g_store.mentions_me(m);

	std::string author = m.webhook_id && !m.webhook_name.empty() ? m.webhook_name : g_store.display_name(m.guild_id, m.author_id);
	uint32_t color = m.webhook_id ? 0 : g_store.name_color(m.guild_id, m.author_id);
	const User *au = g_store.user(m.author_id);

	if (is_system(m.type)) {
		nl();
		insert(it, "→ ", {tag("system")});
		insert(it, system_text(m), {tag("system")});
		insert(it, "  " + time_str(m.ts, true), {tag("dim"), tag("small")});
		return;
	}

	bool grouped = prev && !is_system(prev->type) && prev->author_id == m.author_id && !m.ref && !m.ref_message_id &&
		       m.ts - prev->ts < 7 * 60 * 1000 && day_of(prev->ts) == day_of(m.ts) &&
		       prev->webhook_name == m.webhook_name && !(new_divider && m.id == new_divider);
	auto name_tags = [&]() {
		std::vector<GtkTextTag *> t = {tag("name"), data_tag("user", sf_str(m.author_id))};
		if (color)
			t.push_back(color_tag(color));
		return t;
	};

	// Reply preview line
	if (m.ref_message_id && !m.forwarded) {
		nl();
		std::vector<GtkTextTag *> rt = {body, tag("reply"), data_tag("jump", sf_str(m.ref_message_id))};
		if (m.ref) {
			std::string snippet = plain_snippet(*m.ref);
			if (g_utf8_strlen(snippet.c_str(), -1) > 120) {
				gchar *cut = g_utf8_substring(snippet.c_str(), 0, 117);
				snippet = std::string(cut) + "…";
				g_free(cut);
			}
			if (snippet.empty())
				snippet = m.ref->attachments.empty() ? "(click to see message)" : "[attachment]";
			insert(it, "╭ @" + g_store.display_name(m.ref->guild_id, m.ref->author_id) + "  ", rt);
			insert(it, snippet, rt);
		} else {
			insert(it, "╭ original message was deleted", rt);
		}
	}

	if (g_settings.compact) {
		nl();
		insert(it, time_str(m.ts, false) + " ", {tag("dim"), tag("small")});
		insert(it, author, name_tags());
		insert(it, ": ", {});
	} else if (!grouped) {
		nl();
		GtkTextMark *hs = gtk_text_buffer_create_mark(buf, nullptr, it, TRUE);
		if (g_settings.show_avatars) {
			MediaOpts o;
			o.max_w = o.max_h = S(22);
			o.disk_cache = true;
			o.circle = true;
			std::string url;
			if (m.webhook_id && !m.webhook_avatar.empty())
				url = "https://cdn.discordapp.com/avatars/" + sf_str(m.author_id) + "/" + m.webhook_avatar + ".png?size=64";
			else if (au)
				url = avatar_url(*au, 64);
			GtkWidget *w = media_widget(media_get(url, o), S(22), S(22), [uid = m.author_id, gid = m.guild_id](int) {
				ui_show_user_menu(gid, uid, gtk_get_current_event());
			});
			insert_widget(it, w);
			insert(it, "  ", {});
		}
		insert(it, author, name_tags());
		if ((au && au->bot) || m.webhook_id)
			insert(it, "  BOT", {tag("bot")});
		insert(it, "  " + time_str(m.ts, true), {tag("dim"), tag("small")});
		GtkTextIter hsi;
		gtk_text_buffer_get_iter_at_mark(buf, &hsi, hs);
		gtk_text_buffer_apply_tag(buf, tag(m.ref_message_id && !m.forwarded ? "header-tight" : "header"), &hsi, it);
		gtk_text_buffer_delete_mark(buf, hs);
		insert(it, "\n", {});
	} else {
		insert(it, "\n", {});
	}

	GtkTextIter body_start_it = *it;
	GtkTextMark *bs = gtk_text_buffer_create_mark(buf, nullptr, &body_start_it, TRUE);

	if (m.forwarded)
		insert(it, "↪ Forwarded\n", {body, tag("dim"), tag("small"), tag("italic")});

	// Content: hide a lone gifv link (Discord shows only the animation).
	std::string content = m.content;
	if (m.embeds.size() == 1 && (m.embeds[0].type == "gifv" || m.embeds[0].type == "image") &&
	    trim(content) == m.embeds[0].url)
		content.clear();
	std::vector<GtkTextTag *> base = {body};
	for (auto *t : state_tags)
		base.push_back(t);
	if (!content.empty()) {
		auto spans = md_parse(content);
		render_spans(it, m, spans, base, emoji_only(spans));
	}
	if (m.edited_ts)
		insert(it, " (edited)", {body, tag("edited")});
	if (m.state == Message::Failed)
		insert(it, "  (failed to send — right-click to retry)", {body, tag("failed"), tag("small")});

	// Attachments
	for (const Attachment &a : m.attachments) {
		if (gtk_text_iter_get_line_offset(it) != 0 || !content.empty())
			insert(it, "\n", {body});
		if (a.is_image()) {
			int w, h;
			fit_box(a.width, a.height, max_media_w(), S(300), &w, &h);
			bool gif = a.content_type == "image/gif" || (a.filename.size() > 4 &&
				   strcasecmp(a.filename.c_str() + a.filename.size() - 4, ".gif") == 0);
			std::string src = gif ? a.proxy_url : media_proxy_sized(a.proxy_url, w, h);
			render_media(it, src, w, h, gif, a.spoiler, a.url, false);
		} else if (a.is_video()) {
			int w, h;
			fit_box(a.width, a.height, max_media_w(), S(300), &w, &h);
			render_media(it, media_proxy_sized(a.proxy_url, w, h, "jpeg"), w, h, false, a.spoiler, a.url, true);
		} else {
			insert(it, "📎 ", {body});
			insert(it, a.filename, {body, tag("link"), data_tag("url", a.url)});
			insert(it, "  " + human_size(a.size), {body, tag("dim"), tag("small")});
		}
	}
	for (const StickerItem &st : m.stickers) {
		if (gtk_text_iter_get_line_offset(it) != 0)
			insert(it, "\n", {body});
		if (st.format == 3) {
			insert(it, "[sticker: " + st.name + "]", {body, tag("dim")});
			continue;
		}
		std::string url = "https://media.discordapp.net/stickers/" + sf_str(st.id) + (st.format == 4 ? ".gif" : ".png") + "?size=160";
		MediaOpts o;
		o.max_w = o.max_h = S(120);
		o.animate = st.format == 4;
		GtkWidget *w = media_widget(media_get(url, o), S(120), S(120));
		gtk_widget_set_tooltip_text(w, st.name.c_str());
		insert_widget(it, w);
	}
	for (const Embed &e : m.embeds)
		render_embed(it, m, e, body);
	render_reactions(it, m, body);

	GtkTextIter bsi;
	gtk_text_buffer_get_iter_at_mark(buf, &bsi, bs);
	gtk_text_buffer_apply_tag(buf, body, &bsi, it);
	if (mentioned) {
		GtkTextIter ms;
		gtk_text_buffer_get_iter_at_mark(buf, &ms, mark);
		gtk_text_buffer_apply_tag(buf, tag("mentioned"), &ms, it);
	}
	gtk_text_buffer_delete_mark(buf, bs);
}

void ChatView::Impl::render_all(bool keep_position)
{
	// Remember what is at the top of the view to restore it afterwards.
	Snowflake anchor = 0;
	int anchor_off = 0;
	bool bottom = sticky_bottom || at_bottom();
	if (keep_position && !bottom) {
		GdkRectangle vis;
		gtk_text_view_get_visible_rect(view, &vis);
		for (auto &mk : marks) {
			GtkTextIter i;
			gtk_text_buffer_get_iter_at_mark(buf, &i, mk.mark);
			int y, h;
			gtk_text_view_get_line_yrange(view, &i, &y, &h);
			if (y + h >= vis.y) {
				anchor = mk.id;
				anchor_off = y - vis.y;
				break;
			}
		}
	}
	if (gtk_widget_get_allocated_width(GTK_WIDGET(view)) <= 1) {
		pending_layout = true;
		return;
	}
	pending_layout = false;
	clear_buffer();
	last_width = gtk_widget_get_allocated_width(GTK_WIDGET(view));
	auto it = g_store.messages.find(self->cid_);
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(buf, &end);
	if (it == g_store.messages.end() || it->second.msgs.empty()) {
		if (it != g_store.messages.end() && it->second.loaded)
			insert(&end, "No messages here yet.", {tag("dim")});
		else
			insert(&end, "Loading…", {tag("dim")});
		return;
	}
	auto &msgs = it->second.msgs;
	if (it->second.reached_start) {
		const Channel *c = g_store.channel(self->cid_);
		std::string start = !c ? "This is the start of the channel." :
				    c->is_private() ? "This is the beginning of your conversation with " + g_store.channel_title(*c) + "." :
				    "This is the start of #" + c->name + ".";
		insert(&end, start, {tag("dim")});
	}
	const Message *prev = nullptr;
	for (auto &m : msgs) {
		gtk_text_buffer_get_end_iter(buf, &end);
		render_message(&end, *m, prev);
		prev = m.get();
	}
	if (bottom) {
		sticky_bottom = true;
		scroll_to_bottom();
	} else if (anchor) {
		for (auto &mk : marks)
			if (mk.id == anchor) {
				// Restore after layout has caught up.
				struct Ctx { Impl *d; std::shared_ptr<bool> alive; GtkTextMark *mark; int off; };
				auto *ctx = new Ctx{this, alive, mk.mark, anchor_off};
				g_idle_add_full(G_PRIORITY_LOW, [](gpointer p) -> gboolean {
					Ctx *c = static_cast<Ctx *>(p);
					if (!*c->alive || gtk_text_mark_get_deleted(c->mark))
						return G_SOURCE_REMOVE;
					GtkTextIter i;
					gtk_text_buffer_get_iter_at_mark(c->d->buf, &i, c->mark);
					int y, h;
					gtk_text_view_get_line_yrange(c->d->view, &i, &y, &h);
					GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(c->d->scroll));
					gtk_adjustment_set_value(a, y - c->off);
					return G_SOURCE_REMOVE;
				}, ctx, [](gpointer p) { delete static_cast<Ctx *>(p); });
				break;
			}
	}
}

// --- history -----------------------------------------------------------------------

void ChatView::Impl::load_initial()
{
	Snowflake cid = self->cid_;
	ChannelMessages &cm = g_store.messages[cid];
	cm.last_access = now_ms();
	const Channel *c = g_store.channel(cid);
	auto rs = g_store.read_states.find(cid);
	new_divider = 0;
	if (c && rs != g_store.read_states.end() && c->last_message_id > rs->second.last_message_id)
		new_divider = rs->second.last_message_id;  // resolved to the next message after load
	if (cm.loaded) {
		render_all(false);
		return;
	}
	if (cm.loading)
		return;
	if (c && (c->type == CH_FORUM || c->type == CH_MEDIA)) {
		clear_buffer();
		GtkTextIter end;
		gtk_text_buffer_get_end_iter(buf, &end);
		insert(&end, "#" + c->name + " is a forum channel. dcord shows forum posts you have joined as threads under it; "
			     "to browse all posts, ", {tag("dim")});
		std::string url = "https://discord.com/channels/" + sf_str(c->guild_id) + "/" + sf_str(c->id);
		insert(&end, "open it in the browser", {tag("link"), data_tag("url", url)});
		insert(&end, ".", {tag("dim")});
		return;
	}
	cm.loading = true;
	render_all(false);
	auto a = alive;
	api_fetch_messages(cid, 0, 50, [this, a, cid](ApiResult &r) {
		ChannelMessages &cm = g_store.messages[cid];
		cm.loading = false;
		if (!r.ok()) {
			if (*a && self->cid_ == cid) {
				clear_buffer();
				GtkTextIter end;
				gtk_text_buffer_get_end_iter(buf, &end);
				insert(&end, "Could not load messages: " + r.error, {tag("failed")});
			}
			return;
		}
		cm.loaded = true;
		std::vector<Snowflake> authors;
		for (auto &mj : r.body) {
			MessagePtr m = g_store.parse_message(mj);
			g_store.add_message(m);
			authors.push_back(m->author_id);
		}
		if (r.body.size() < 50)
			cm.reached_start = true;
		if (!*a || self->cid_ != cid)
			return;
		if (self->gid_)
			app_request_members(self->gid_, authors);
		// The divider goes before the first message newer than the read marker.
		if (new_divider) {
			Snowflake marker = new_divider;
			new_divider = 0;
			for (auto &m : cm.msgs)
				if (m->id > marker && m->author_id != g_store.me.id) {
					new_divider = m->id;
					break;
				}
		}
		sticky_bottom = true;
		render_all(false);
		self->maybe_ack();
	});
}

void ChatView::Impl::load_older()
{
	Snowflake cid = self->cid_;
	auto it = g_store.messages.find(cid);
	if (it == g_store.messages.end())
		return;
	ChannelMessages &cm = it->second;
	if (!cm.loaded || cm.loading || cm.reached_start || cm.msgs.empty())
		return;
	cm.loading = true;
	Snowflake before = cm.msgs.front()->id;
	auto a = alive;
	api_fetch_messages(cid, before, 50, [this, a, cid](ApiResult &r) {
		ChannelMessages &cm = g_store.messages[cid];
		cm.loading = false;
		if (!r.ok())
			return;
		std::vector<Snowflake> authors;
		for (auto &mj : r.body) {
			MessagePtr m = g_store.parse_message(mj);
			g_store.add_message(m);
			authors.push_back(m->author_id);
		}
		if (r.body.size() < 50)
			cm.reached_start = true;
		if (!*a || self->cid_ != cid)
			return;
		if (self->gid_)
			app_request_members(self->gid_, authors);
		sticky_bottom = false;
		render_all(true);
	});
}

// --- input --------------------------------------------------------------------------

void ChatView::Impl::sent_typing()
{
	if (editing)
		return;
	int64_t now = now_ms();
	if (now - last_typing_sent < 8000)
		return;
	last_typing_sent = now;
	api_typing(self->cid_);
}

// --- @mention autocomplete ---------------------------------------------------------------

static std::string lower(std::string s)
{
	for (char &c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

bool ChatView::Impl::mention_query(GtkTextIter *at)
{
	GtkTextBuffer *b = gtk_text_view_get_buffer(composer);
	GtkTextIter cur;
	gtk_text_buffer_get_iter_at_mark(b, &cur, gtk_text_buffer_get_insert(b));
	GtkTextIter it = cur;
	for (int n = 0; n < 40 && gtk_text_iter_backward_char(&it); n++) {
		gunichar c = gtk_text_iter_get_char(&it);
		if (g_unichar_isspace(c))
			return false;
		if (c == '@') {
			GtkTextIter prev = it;
			if (gtk_text_iter_backward_char(&prev) && !g_unichar_isspace(gtk_text_iter_get_char(&prev)))
				return false;  // an email address, not a mention
			*at = it;
			return true;
		}
	}
	return false;
}

void ChatView::Impl::mention_update()
{
	GtkTextIter at;
	if (!mention_query(&at)) {
		if (mention_pop)
			gtk_widget_hide(mention_pop);
		return;
	}
	GtkTextBuffer *b = gtk_text_view_get_buffer(composer);
	GtkTextIter cur, q = at;
	gtk_text_buffer_get_iter_at_mark(b, &cur, gtk_text_buffer_get_insert(b));
	gtk_text_iter_forward_char(&q);
	gchar *qs = gtk_text_buffer_get_text(b, &q, &cur, FALSE);
	std::string query = lower(qs);
	g_free(qs);

	// Everyone we know in this channel: guild members seen, DM recipients, recent authors.
	std::vector<Snowflake> ids;
	std::set<Snowflake> seen;
	auto add = [&](Snowflake id) {
		if (id && id != g_store.me.id && seen.insert(id).second)
			ids.push_back(id);
	};
	if (auto it = g_store.messages.find(self->cid_); it != g_store.messages.end())
		for (auto m = it->second.msgs.rbegin(); m != it->second.msgs.rend(); ++m)
			add((*m)->author_id);
	if (const Channel *ch = g_store.channel(self->cid_))
		for (Snowflake r : ch->recipients)
			add(r);
	if (Guild *g = g_store.guild(self->gid_))
		for (auto &[uid, mem] : g->members)
			add(uid);

	std::vector<std::pair<int, Snowflake>> hits;  // score (lower is better), user
	for (size_t i = 0; i < ids.size(); i++) {
		const User *u = g_store.user(ids[i]);
		if (!u)
			continue;
		int best = 99;
		for (const std::string &name : {u->username, u->global_name, g_store.display_name(self->gid_, ids[i])}) {
			std::string l = lower(name);
			size_t pos = l.find(query);
			if (!name.empty() && pos != std::string::npos)
				best = std::min(best, pos == 0 ? 0 : 1);
		}
		if (best < 99)
			hits.push_back({best * 100000 + (int)i, ids[i]});  // recent talkers first
	}
	std::sort(hits.begin(), hits.end());
	if (hits.size() > 10)
		hits.resize(10);
	if (hits.empty()) {
		if (mention_pop)
			gtk_widget_hide(mention_pop);
		return;
	}

	if (!mention_pop) {
		mention_pop = gtk_popover_new(GTK_WIDGET(composer));
		gtk_popover_set_modal(GTK_POPOVER(mention_pop), FALSE);  // keep typing in the composer
		gtk_popover_set_position(GTK_POPOVER(mention_pop), GTK_POS_TOP);
		mention_list = gtk_list_box_new();
		gtk_list_box_set_selection_mode(GTK_LIST_BOX(mention_list), GTK_SELECTION_BROWSE);
		gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(mention_list), TRUE);
		gtk_widget_set_can_focus(mention_list, FALSE);
		g_signal_connect(mention_list, "row-activated", CB(+[](GtkListBox *, GtkListBoxRow *r, gpointer p) {
			static_cast<Impl *>(p)->mention_pick(gtk_list_box_row_get_index(r));
		}), this);
		gtk_container_add(GTK_CONTAINER(mention_pop), mention_list);
	}
	gtk_container_foreach(GTK_CONTAINER(mention_list), [](GtkWidget *w, gpointer) { gtk_widget_destroy(w); }, nullptr);
	mention_ids.clear();
	for (auto &[score, id] : hits) {
		const User *u = g_store.user(id);
		std::string m = "<b>" + markup_escape(g_store.display_name(self->gid_, id)) + "</b>  <span alpha=\"60%\">" +
				markup_escape(u->username) + "</span>";
		GtkWidget *l = gtk_label_new(nullptr);
		gtk_label_set_markup(GTK_LABEL(l), m.c_str());
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		g_object_set(l, "margin", S(4), nullptr);
		gtk_list_box_insert(GTK_LIST_BOX(mention_list), l, -1);
		mention_ids.push_back(id);
	}
	gtk_list_box_select_row(GTK_LIST_BOX(mention_list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(mention_list), 0));
	// Point at the "@".
	GdkRectangle r;
	gtk_text_view_get_iter_location(composer, &at, &r);
	gtk_text_view_buffer_to_window_coords(composer, GTK_TEXT_WINDOW_WIDGET, r.x, r.y, &r.x, &r.y);
	gtk_popover_set_pointing_to(GTK_POPOVER(mention_pop), &r);
	gtk_widget_show_all(mention_pop);
}

void ChatView::Impl::mention_pick(int row)
{
	if (row < 0 || row >= (int)mention_ids.size())
		return;
	Snowflake id = mention_ids[row];
	const User *u = g_store.user(id);
	GtkTextIter at;
	if (!u || !mention_query(&at))
		return;
	GtkTextBuffer *b = gtk_text_view_get_buffer(composer);
	GtkTextIter cur;
	gtk_text_buffer_get_iter_at_mark(b, &cur, gtk_text_buffer_get_insert(b));
	std::string name = "@" + u->username;
	gtk_text_buffer_delete(b, &at, &cur);
	gtk_text_buffer_insert(b, &at, (name + " ").c_str(), -1);
	mentions.push_back({name, id});
	gtk_widget_hide(mention_pop);
	gtk_widget_grab_focus(GTK_WIDGET(composer));
}

// Turns the "@username" picked from the popup into real <@id> mentions.
std::string ChatView::Impl::apply_mentions(std::string text)
{
	for (auto &[name, id] : mentions) {
		std::string tag = "<@" + sf_str(id) + ">";
		size_t p = 0;
		while ((p = text.find(name, p)) != std::string::npos) {
			size_t e = p + name.size();
			if (e < text.size() && (isalnum((unsigned char)text[e]) || text[e] == '_' || text[e] == '.')) {
				p = e;  // a longer name that starts the same
				continue;
			}
			text.replace(p, name.size(), tag);
			p += tag.size();
		}
	}
	mentions.clear();
	return text;
}

void ChatView::Impl::send()
{
	GtkTextBuffer *b = gtk_text_view_get_buffer(composer);
	GtkTextIter s, e;
	gtk_text_buffer_get_bounds(b, &s, &e);
	gchar *txt = gtk_text_buffer_get_text(b, &s, &e, FALSE);
	std::string text = apply_mentions(trim(txt));
	g_free(txt);
	Snowflake cid = self->cid_;
	if (editing) {
		Snowflake id = editing;
		if (!text.empty())
			api_edit_message(cid, id, text, nullptr);
		gtk_text_buffer_set_text(b, "", 0);
		cancel_reply_edit();
		return;
	}
	if (text.empty() && files.empty())
		return;
	if (uploading)
		return;
	// Convert ":name:" to custom emoji of this guild when we know them.
	if (Guild *g = g_store.guild(self->gid_)) {
		std::string out;
		size_t i = 0;
		while (i < text.size()) {
			if (text[i] == ':') {
				size_t j = text.find(':', i + 1);
				if (j != std::string::npos && j > i + 1 && text.substr(i + 1, j - i - 1).find(' ') == std::string::npos) {
					std::string name = text.substr(i + 1, j - i - 1);
					const Emoji *found = nullptr;
					for (auto &[eid, em] : g->emojis)
						if (em.name == name) {
							found = &em;
							break;
						}
					if (found) {
						out += std::string(found->animated ? "<a:" : "<:") + name + ":" + sf_str(found->id) + ">";
						i = j + 1;
						continue;
					}
				}
			}
			out += text[i++];
		}
		text = out;
	}
	std::string nonce = sf_str(sf_from_unix_ms(now_ms()) + (g_random_int() & 0x3fffff));
	// Local echo
	auto m = std::make_shared<Message>();
	m->id = sf_parse(nonce);
	m->channel_id = cid;
	m->guild_id = self->gid_;
	m->author_id = g_store.me.id;
	m->content = text;
	m->ts = now_ms();
	m->nonce = nonce;
	m->state = Message::Pending;
	if (reply_to) {
		m->ref_message_id = reply_to;
		m->ref = g_store.find_message(cid, reply_to);
	}
	for (auto &f : files) {
		Attachment a;
		a.filename = f.filename;
		a.size = f.path.empty() ? f.data.size() : 0;
		m->attachments.push_back(a);
	}
	auto &cm = g_store.messages[cid];
	if (cm.loaded) {
		g_store.add_message(m);
		self->on_message_created(m);
	}
	std::vector<UploadFile> up = std::move(files);
	files.clear();
	update_attach_box();
	Snowflake rt = reply_to;
	bool rm = reply_mention;
	cancel_reply_edit();
	gtk_text_buffer_set_text(b, "", 0);
	last_typing_sent = 0;
	auto a = alive;
	if (!up.empty()) {
		uploading = true;
		gtk_label_set_text(GTK_LABEL(send_info), "Uploading…");
		gtk_widget_show(send_info);
	}
	struct Prog { std::shared_ptr<bool> a; Impl *d; };
	auto prog = std::make_shared<Prog>(Prog{a, this});
	api_send_message(cid, text, nonce, rt, rm, up, [this, a, m, cid, text, nonce, rt, rm](ApiResult &r) {
		if (*a) {
			uploading = false;
			gtk_widget_hide(send_info);
		}
		if (r.ok()) {
			// The gateway MESSAGE_CREATE replaces the echo; do it now too in case it's late.
			MessagePtr real = g_store.parse_message(r.body);
			real->nonce = nonce;
			if (g_store.messages[cid].loaded)
				g_store.add_message(real);
			if (*a && self->cid_ == cid)
				render_all(true);
			return;
		}
		m->state = Message::Failed;
		if (*a && self->cid_ == cid) {
			render_all(true);
			ui_toast("Message not sent: " + r.error);
		}
	}, up.empty() ? HttpProgress() : HttpProgress([prog](double f) {
		int pct = (int)(f * 100);
		post_main([prog, pct]() {
			if (*prog->a) {
				std::string t = "Uploading… " + std::to_string(pct) + "%";
				gtk_label_set_text(GTK_LABEL(prog->d->send_info), t.c_str());
			}
		});
	}));
}

void ChatView::Impl::start_edit(Snowflake id)
{
	MessagePtr m = g_store.find_message(self->cid_, id);
	if (!m)
		return;
	editing = id;
	reply_to = 0;
	GtkTextBuffer *b = gtk_text_view_get_buffer(composer);
	gtk_text_buffer_set_text(b, m->content.c_str(), -1);
	gtk_label_set_markup(GTK_LABEL(reply_label), "<b>Editing message</b> — Enter to save, Esc to cancel");
	gtk_widget_hide((GtkWidget *)g_object_get_data(G_OBJECT(reply_bar), "ping"));
	gtk_widget_show_all(reply_bar);
	gtk_widget_hide((GtkWidget *)g_object_get_data(G_OBJECT(reply_bar), "ping"));
	gtk_widget_grab_focus(GTK_WIDGET(composer));
}

void ChatView::Impl::set_reply(Snowflake id)
{
	MessagePtr m = g_store.find_message(self->cid_, id);
	if (!m)
		return;
	editing = 0;
	reply_to = id;
	std::string t = "Replying to <b>" + markup_escape(g_store.display_name(m->guild_id, m->author_id)) + "</b>";
	gtk_label_set_markup(GTK_LABEL(reply_label), t.c_str());
	gtk_widget_show_all(reply_bar);
	gtk_widget_grab_focus(GTK_WIDGET(composer));
}

void ChatView::Impl::cancel_reply_edit()
{
	if (editing)
		gtk_text_buffer_set_text(gtk_text_view_get_buffer(composer), "", 0);
	editing = 0;
	reply_to = 0;
	gtk_widget_hide(reply_bar);
}

bool ChatView::Impl::paste_attachments()
{
			GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	if (gtk_clipboard_wait_is_image_available(cb)) {
		GdkPixbuf *pb = gtk_clipboard_wait_for_image(cb);
		if (pb) {
			gchar *data = nullptr;
			gsize len = 0;
			if (gdk_pixbuf_save_to_buffer(pb, &data, &len, "png", nullptr, nullptr)) {
				UploadFile f;
				f.data.assign(data, len);
				f.filename = "image.png";
				f.content_type = "image/png";
				files.push_back(std::move(f));
				update_attach_box();
				g_free(data);
			}
			g_object_unref(pb);
			return true;
		}
	}
	if (gtk_clipboard_wait_is_uris_available(cb)) {
		gchar **uris = gtk_clipboard_wait_for_uris(cb);
		bool any = false;
		for (int i = 0; uris && uris[i]; i++)
			if (gchar *f = g_filename_from_uri(uris[i], nullptr, nullptr)) {
				self->attach_file(f);
				g_free(f);
				any = true;
			}
		g_strfreev(uris);
		if (any)
			return true;
	}
	return false;
}

void ChatView::Impl::update_attach_box()
{
	gtk_container_foreach(GTK_CONTAINER(attach_box), [](GtkWidget *w, gpointer) { gtk_widget_destroy(w); }, nullptr);
	if (files.empty()) {
		gtk_widget_hide(attach_box);
		return;
	}
	for (size_t i = 0; i < files.size(); i++) {
		GtkWidget *chip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, S(2));
		gtk_style_context_add_class(gtk_widget_get_style_context(chip), "attach-chip");
		const UploadFile &f = files[i];
		if (f.content_type.rfind("image/", 0) == 0) {
			MediaOpts o;
			o.max_w = o.max_h = S(48);
			MediaPtr m;
			if (!f.path.empty())
				m = media_get_file(f.path, o);
			if (m) {
				gtk_box_pack_start(GTK_BOX(chip), media_widget(m, S(48), S(48)), FALSE, FALSE, 0);
			} else if (!f.data.empty()) {
				GdkPixbufLoader *ld = gdk_pixbuf_loader_new();
				if (gdk_pixbuf_loader_write(ld, (const guchar *)f.data.data(), f.data.size(), nullptr) &&
				    gdk_pixbuf_loader_close(ld, nullptr)) {
					if (GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf(ld)) {
						int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb), mx = S(120);
						double k = std::min(1.0, (double)mx / std::max(w, h));
						GdkPixbuf *sc = gdk_pixbuf_scale_simple(pb, std::max(1, (int)(w * k)), std::max(1, (int)(h * k)),
											GDK_INTERP_BILINEAR);
						GtkWidget *img = gtk_image_new_from_pixbuf(sc);
						g_object_unref(sc);
						gtk_box_pack_start(GTK_BOX(chip), img, FALSE, FALSE, 0);
					}
				}
				g_object_unref(ld);
			}
		}
		std::string label = f.filename + (f.spoiler ? " (spoiler)" : "");
		GtkWidget *l = gtk_label_new(label.c_str());
		gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
		gtk_label_set_max_width_chars(GTK_LABEL(l), 24);
		gtk_box_pack_start(GTK_BOX(chip), l, FALSE, FALSE, 0);
		GtkWidget *sp = gtk_button_new_with_label("spoiler");
		gtk_button_set_relief(GTK_BUTTON(sp), GTK_RELIEF_NONE);
		g_object_set_data(G_OBJECT(sp), "idx", GINT_TO_POINTER((int)i));
		g_signal_connect(sp, "clicked", CB(+[](GtkButton *b, gpointer p) {
			Impl *d = static_cast<Impl *>(p);
			size_t i = (size_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "idx"));
			if (i < d->files.size())
				d->files[i].spoiler = !d->files[i].spoiler;
			d->update_attach_box();
		}), this);
		gtk_box_pack_start(GTK_BOX(chip), sp, FALSE, FALSE, 0);
		GtkWidget *x = gtk_button_new_with_label("✕");
		gtk_button_set_relief(GTK_BUTTON(x), GTK_RELIEF_NONE);
		g_object_set_data(G_OBJECT(x), "idx", GINT_TO_POINTER((int)i));
		g_signal_connect(x, "clicked", CB(+[](GtkButton *b, gpointer p) {
			Impl *d = static_cast<Impl *>(p);
			size_t i = (size_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "idx"));
			if (i < d->files.size())
				d->files.erase(d->files.begin() + i);
			d->update_attach_box();
		}), this);
		gtk_box_pack_start(GTK_BOX(chip), x, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(attach_box), chip, FALSE, FALSE, 0);
	}
	gtk_widget_show_all(attach_box);
	gtk_widget_show(attach_box);
}

void ChatView::Impl::update_typing_label()
{
	int64_t now = now_ms();
	std::vector<std::string> names;
	for (auto it = typing.begin(); it != typing.end();) {
		if (now - it->second > 10000) {
			it = typing.erase(it);
			continue;
		}
		names.push_back(g_store.display_name(self->gid_, it->first));
		++it;
	}
	std::string t;
	if (names.size() == 1)
		t = "<b>" + markup_escape(names[0]) + "</b> is typing…";
	else if (names.size() == 2)
		t = "<b>" + markup_escape(names[0]) + "</b> and <b>" + markup_escape(names[1]) + "</b> are typing…";
	else if (names.size() > 2)
		t = "Several people are typing…";
	gtk_label_set_markup(GTK_LABEL(typing_label), t.c_str());
	if (typing.empty() && typing_timer) {
		g_source_remove(typing_timer);
		typing_timer = 0;
	}
}

void ChatView::Impl::update_header()
{
	const Channel *c = g_store.channel(self->cid_);
	if (!c)
		return;
	std::string title;
	if (c->is_private())
		title = (c->type == CH_DM ? "@" : "") + g_store.channel_title(*c);
	else
		title = c->is_voice() ? c->name + " (voice)" : "#" + c->name;
	std::string mk = "<b>" + markup_escape(title) + "</b>";
	gtk_label_set_markup(GTK_LABEL(header_title), mk.c_str());
	std::string topic = c->topic;
	for (auto &ch : topic)
		if (ch == '\n')
			ch = ' ';
	gtk_label_set_text(GTK_LABEL(header_topic), topic.c_str());
	gtk_widget_set_tooltip_text(header_topic, c->topic.c_str());
	gtk_widget_set_visible(call_btn, c->is_private());
}

void ChatView::Impl::update_permissions()
{
	const Channel *c = g_store.channel(self->cid_);
	bool can = true;
	if (c && !c->is_private()) {
		uint64_t p = g_store.permissions(*c);
		can = c->is_thread() ? (p & perm::SEND_IN_THREADS) : (p & perm::SEND_MESSAGES);
	}
	gtk_widget_set_sensitive(noperm, can);
	if (!can)
		gtk_text_buffer_set_text(gtk_text_view_get_buffer(composer), "", 0);
}

Snowflake ChatView::Impl::message_at(GtkTextIter *it)
{
	int off = gtk_text_iter_get_offset(it);
	Snowflake found = 0;
	// Marks are in buffer order: binary search on their offsets.
	size_t lo = 0, hi = marks.size();
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		GtkTextIter mi;
		gtk_text_buffer_get_iter_at_mark(buf, &mi, marks[mid].mark);
		if (gtk_text_iter_get_offset(&mi) <= off) {
			found = marks[mid].id;
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return found;
}

static void copy_text(const std::string &s)
{
	gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), s.c_str(), -1);
}

void ChatView::Impl::message_menu(Snowflake id, GdkEvent *ev)
{
	MessagePtr m = g_store.find_message(self->cid_, id);
	if (!m)
		return;
	GtkWidget *menu = gtk_menu_new();
	auto add = [&](const char *label, std::function<void()> fn) {
		GtkWidget *it = gtk_menu_item_new_with_label(label);
		auto *f = new std::function<void()>(std::move(fn));
		g_signal_connect_data(it, "activate", CB(+[](GtkMenuItem *, gpointer p) {
			(*static_cast<std::function<void()> *>(p))();
		}), f, [](gpointer p, GClosure *) { delete static_cast<std::function<void()> *>(p); }, (GConnectFlags)0);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
	};
	auto a = alive;
	Snowflake cid = self->cid_;
	bool mine = m->author_id == g_store.me.id;
	const Channel *c = g_store.channel(cid);
	uint64_t perms = c ? g_store.permissions(*c) : 0;
	if (m->state == Message::Failed) {
		add("Retry", [this, a, m, cid]() {
			if (!*a)
				return;
			g_store.remove_message(cid, m->id);
			GtkTextBuffer *b = gtk_text_view_get_buffer(composer);
			gtk_text_buffer_set_text(b, m->content.c_str(), -1);
			render_all(true);
			send();
		});
		add("Discard", [this, a, m, cid]() {
			g_store.remove_message(cid, m->id);
			if (*a)
				render_all(true);
		});
		gtk_widget_show_all(menu);
		gtk_menu_popup_at_pointer(GTK_MENU(menu), ev);
		return;
	}
	add("Reply", [this, a, id]() { if (*a) set_reply(id); });
	if (mine)
		add("Edit", [this, a, id]() { if (*a) start_edit(id); });
	add("Add reaction…", [this, a, m, cid]() {
		if (!*a)
			return;
		emoji_picker_show(GTK_WIDGET(view), self->gid_, [m, cid](const std::string &text) {
			// Picker returns "<:name:id>" for custom emoji or the unicode emoji itself.
			std::string e = text;
			if (e.size() > 3 && e[0] == '<') {
				std::string inner = e.substr(1, e.size() - 2);
				if (inner.rfind("a:", 0) == 0)
					inner = inner.substr(2);
				else if (inner[0] == ':')
					inner = inner.substr(1);
				e = inner;
			}
			api_react(cid, m->id, e, true, nullptr);
		});
	});
	add("Copy text", [m]() { copy_text(m->content); });
	add("Copy message link", [m]() {
		copy_text("https://discord.com/channels/" + (m->guild_id ? sf_str(m->guild_id) : std::string("@me")) + "/" +
			  sf_str(m->channel_id) + "/" + sf_str(m->id));
	});
	add("Copy message ID", [m]() { copy_text(sf_str(m->id)); });
	for (auto &at : m->attachments)
		add(("Open " + at.filename).c_str(), [u = at.url]() { open_external(u); });
	if (!m->guild_id || mine || (perms & perm::MANAGE_MESSAGES))
		if (mine || (perms & perm::MANAGE_MESSAGES))
			add("Delete", [cid, id]() {
				GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_ui.window), GTK_DIALOG_MODAL,
					GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, "Delete this message?");
				if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK)
					api_delete_message(cid, id, nullptr);
				gtk_widget_destroy(dlg);
			});
	gtk_widget_show_all(menu);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), ev);
}

bool ChatView::Impl::on_click(GdkEventButton *ev)
{
	if (ev->button != 1 && ev->button != 2)
		return FALSE;
	GtkTextIter s, e;
	if (gtk_text_buffer_get_selection_bounds(buf, &s, &e) && gtk_text_iter_get_offset(&s) != gtk_text_iter_get_offset(&e))
		return FALSE;
	int bx, by;
	gtk_text_view_window_to_buffer_coords(view, GTK_TEXT_WINDOW_WIDGET, (int)ev->x, (int)ev->y, &bx, &by);
	GtkTextIter it;
	gtk_text_view_get_iter_at_location(view, &it, bx, by);
	GSList *tags = gtk_text_iter_get_tags(&it);
	bool handled = false;
	for (GSList *l = tags; l && !handled; l = l->next) {
		GObject *t = G_OBJECT(l->data);
		if (const char *url = (const char *)g_object_get_data(t, "url")) {
			std::string u = url;
			// discord.com/channels links open in the client.
			const char *pre = "https://discord.com/channels/";
			if (u.rfind(pre, 0) == 0) {
				std::string rest = u.substr(strlen(pre));
				size_t a = rest.find('/');
				if (a != std::string::npos) {
					std::string cs = rest.substr(a + 1);
					size_t b = cs.find('/');
					Snowflake cid = sf_parse(cs.substr(0, b));
					if (g_store.channel(cid)) {
						ui_open_channel(cid, ev->button == 2);
						handled = true;
						break;
					}
				}
			}
			open_external(u);
			handled = true;
		} else if (const char *ch = (const char *)g_object_get_data(t, "chan")) {
			Snowflake cid = sf_parse(std::string_view(ch));
			if (g_store.channel(cid))
				ui_open_channel(cid, ev->button == 2);
			handled = true;
		} else if (const char *sp = (const char *)g_object_get_data(t, "spoil")) {
			revealed.insert(sp);
			render_all(true);
			handled = true;
		} else if (const char *rx = (const char *)g_object_get_data(t, "react")) {
			std::string s(rx);
			size_t a = s.find('\x1f'), b = s.rfind('\x1f');
			Snowflake mid = sf_parse(std::string_view(s).substr(0, a));
			std::string key = s.substr(a + 1, b - a - 1);
			bool me = s.substr(b + 1) == "1";
			api_react(self->cid_, mid, key, !me, nullptr);
			handled = true;
		} else if (const char *j = (const char *)g_object_get_data(t, "jump")) {
			Snowflake mid = sf_parse(std::string_view(j));
			for (auto &mk : marks)
				if (mk.id == mid) {
					gtk_text_view_scroll_to_mark(view, mk.mark, 0, TRUE, 0, 0.3);
					break;
				}
			handled = true;
		} else if (const char *u = (const char *)g_object_get_data(t, "user")) {
			ui_show_user_menu(self->gid_, sf_parse(std::string_view(u)), (GdkEvent *)ev);
			handled = true;
		}
	}
	g_slist_free(tags);
	return handled;
}

// --- public -------------------------------------------------------------------------

ChatView::ChatView(Snowflake cid)
{
	root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	g_object_ref_sink(root_);
	d = new Impl;
	d->self = this;
	d->build();
	gtk_widget_show_all(root_);
	set_channel(cid);
}

ChatView::~ChatView()
{
	*d->alive = false;
	while (g_source_remove_by_user_data(d)) {
	}
	if (d->typing_timer)
		g_source_remove(d->typing_timer);
	if (d->ack_timer)
		g_source_remove(d->ack_timer);
	d->clear_buffer();
	gtk_widget_destroy(root_);
	g_object_unref(root_);
	delete d;
}

void ChatView::set_channel(Snowflake cid)
{
	if (cid_ == cid && cid)
		return;
	cid_ = cid;
	const Channel *c = g_store.channel(cid);
	gid_ = c ? c->guild_id : 0;
	d->revealed.clear();
	d->typing.clear();
	d->files.clear();
	d->update_attach_box();
	d->cancel_reply_edit();
	d->update_typing_label();
	d->update_header();
	d->update_permissions();
	d->sticky_bottom = true;
	if (gid_)
		app_subscribe_guild(gid_);
	d->load_initial();
	ui_update_tab_label(this);
}

void ChatView::reload()
{
	const Channel *c = g_store.channel(cid_);
	gid_ = c ? c->guild_id : 0;
	d->update_header();
	d->update_permissions();
	if (gid_)
		app_subscribe_guild(gid_);
	d->sticky_bottom = true;
	d->load_initial();
}

void ChatView::on_message_created(const MessagePtr &m)
{
	if (m->channel_id != cid_)
		return;
	d->typing.erase(m->author_id);
	d->update_typing_label();
	auto it = g_store.messages.find(cid_);
	if (it == g_store.messages.end() || !it->second.loaded)
		return;
	auto &msgs = it->second.msgs;
	bool incremental = msgs.size() >= 2 && msgs.back() == m && !d->marks.empty() &&
			   d->marks.back().id == msgs[msgs.size() - 2]->id;
	if (incremental) {
		bool bottom = d->sticky_bottom || d->at_bottom();
		GtkTextIter end;
		gtk_text_buffer_get_end_iter(d->buf, &end);
		d->render_message(&end, *m, msgs[msgs.size() - 2].get());
		if (bottom) {
			d->sticky_bottom = true;
			d->scroll_to_bottom();
		}
	} else {
		d->render_all(true);
	}
	if (gid_ && m->author_id && !g_store.member(gid_, m->author_id))
		app_request_members(gid_, {m->author_id});
	maybe_ack();
}

void ChatView::on_message_updated(const MessagePtr &m)
{
	if (m->channel_id != cid_)
		return;
	d->render_all(true);
}

void ChatView::on_message_deleted(Snowflake)
{
	d->render_all(true);
}

void ChatView::on_members_changed()
{
	d->render_all(true);
}

void ChatView::rerender()
{
	int body_margin = g_settings.compact ? S(8) : S(8 + 30);
	g_object_set(d->tag("body"), "left-margin", body_margin, nullptr);
	g_object_set(d->tag("codeblock"), "left-margin", body_margin + S(6), nullptr);
	g_object_set(d->tag("quote"), "left-margin", body_margin + S(12), nullptr);
	g_object_set(d->tag("embed"), "left-margin", body_margin + S(4), nullptr);
	d->render_all(true);
}

void ChatView::on_typing(Snowflake uid)
{
	d->typing[uid] = now_ms();
	d->update_typing_label();
	if (!d->typing_timer)
		d->typing_timer = g_timeout_add_seconds(1, [](gpointer p) -> gboolean {
			Impl *d = static_cast<Impl *>(p);
			d->update_typing_label();
			return d->typing_timer ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
		}, d);
}

void ChatView::on_channel_updated()
{
	const Channel *c = g_store.channel(cid_);
	if (!c) {
		GtkTextIter end;
		d->clear_buffer();
		gtk_text_buffer_get_end_iter(d->buf, &end);
		d->insert(&end, "This channel was deleted or you lost access to it.", {d->tag("failed")});
		return;
	}
	d->update_header();
	d->update_permissions();
	ui_update_tab_label(this);
}

void ChatView::on_shown()
{
	auto it = g_store.messages.find(cid_);
	if (it != g_store.messages.end())
		it->second.last_access = now_ms();
	maybe_ack();
	focus_composer();
}

void ChatView::focus_composer()
{
	gtk_widget_grab_focus(GTK_WIDGET(d->composer));
	// Again once the click that got us here (channel tree, tab, menu) has finished taking focus.
	GWeakRef *ref = g_new0(GWeakRef, 1);
	g_weak_ref_init(ref, d->composer);
	g_idle_add([](gpointer p) -> gboolean {
		GWeakRef *ref = (GWeakRef *)p;
		if (GtkWidget *w = (GtkWidget *)g_weak_ref_get(ref)) {
			if (gtk_widget_get_mapped(w))
				gtk_widget_grab_focus(w);
			g_object_unref(w);
		}
		g_weak_ref_clear(ref);
		g_free(ref);
		return G_SOURCE_REMOVE;
	}, ref);
}

static std::string guess_type(const std::string &path)
{
	gboolean uncertain;
	gchar *ct = g_content_type_guess(path.c_str(), nullptr, 0, &uncertain);
	gchar *mime = ct ? g_content_type_get_mime_type(ct) : nullptr;
	std::string r = mime ? mime : "application/octet-stream";
	g_free(ct);
	g_free(mime);
	return r;
}

void ChatView::attach_file(const std::string &path)
{
	UploadFile f;
	f.path = path;
	gchar *base = g_path_get_basename(path.c_str());
	f.filename = base;
	g_free(base);
	f.content_type = guess_type(path);
	d->files.push_back(std::move(f));
	d->update_attach_box();
	focus_composer();
}

void ChatView::maybe_ack()
{
	if (ui_current_view() != this || !g_ui.focused || !d->sticky_bottom)
		return;
	app_mark_read(cid_);
}

void ChatView::debug_command(const std::string &cmd, const std::string &arg)
{
	if (cmd == "send") {
		gtk_text_buffer_set_text(gtk_text_view_get_buffer(d->composer), arg.c_str(), -1);
		d->send();
	} else if (cmd == "type") {
		gtk_text_buffer_insert_at_cursor(gtk_text_view_get_buffer(d->composer), arg.c_str(), -1);
	} else if (cmd == "mention") {
		d->mention_pick(atoi(arg.c_str()));
	} else if (cmd == "enter") {
		d->send();
	} else if (cmd == "attach") {
		attach_file(arg);
	} else if (cmd == "reply") {
		auto it = g_store.messages.find(cid_);
		if (it != g_store.messages.end() && !it->second.msgs.empty())
			d->set_reply(it->second.msgs.back()->id);
	} else if (cmd == "edit") {
		auto it = g_store.messages.find(cid_);
		if (it != g_store.messages.end())
			for (auto m = it->second.msgs.rbegin(); m != it->second.msgs.rend(); ++m)
				if ((*m)->author_id == g_store.me.id) {
					d->start_edit((*m)->id);
					break;
				}
	} else if (cmd == "react") {
		auto it = g_store.messages.find(cid_);
		if (it != g_store.messages.end() && !it->second.msgs.empty())
			api_react(cid_, it->second.msgs.back()->id, arg, true, nullptr);
	} else if (cmd == "delete") {
		auto it = g_store.messages.find(cid_);
		if (it != g_store.messages.end())
			for (auto m = it->second.msgs.rbegin(); m != it->second.msgs.rend(); ++m)
				if ((*m)->author_id == g_store.me.id) {
					api_delete_message(cid_, (*m)->id, nullptr);
					break;
				}
	} else if (cmd == "gif") {
		gif_picker_show(GTK_WIDGET(d->composer), [](const std::string &) {});
	} else if (cmd == "emoji") {
		emoji_picker_show(GTK_WIDGET(d->composer), gid_, [](const std::string &) {});
	} else if (cmd == "top") {
		GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(d->scroll));
		d->sticky_bottom = false;
		gtk_adjustment_set_value(a, 0);
	}
}
