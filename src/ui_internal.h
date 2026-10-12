// Shared declarations between the UI files.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <gtk/gtk.h>

#include "api.h"
#include "media.h"
#include "store.h"

// G_CALLBACK for lambdas (their commas would split a one-argument macro).
#define CB(...) G_CALLBACK((__VA_ARGS__))

class ChatView;

struct MainUI {
	GtkWidget *window = nullptr;
	GtkWidget *hpaned = nullptr;       // sidebar | content
	GtkWidget *tree = nullptr;
	GtkTreeStore *tree_store = nullptr;
	GtkWidget *notebook = nullptr;
	GtkWidget *right_paned = nullptr;  // chat | member list
	GtkWidget *members = nullptr;      // member list box
	GtkWidget *members_scroll = nullptr;
	GtkWidget *status = nullptr;       // connection status label
	GtkWidget *voice_box = nullptr;    // voice panel
	GtkWidget *voice_label = nullptr;
	GtkWidget *voice_mute = nullptr, *voice_deaf = nullptr;
	GtkWidget *me_label = nullptr;
	GtkWidget *call_bar = nullptr;
	GtkWidget *call_label = nullptr;
	Snowflake call_channel = 0;
	std::vector<ChatView *> views;
	bool focused = true;
	int unread_badge = 0;
};
extern MainUI g_ui;

// ui_main.cpp
ChatView *ui_open_channel(Snowflake cid, bool new_tab);
ChatView *ui_current_view();
void ui_close_view(ChatView *v);
void ui_update_tab_label(ChatView *v);
void ui_update_title();
void ui_select_in_tree(Snowflake cid);
cairo_surface_t *ui_icon(const char *name, int size);
void ui_toast(const std::string &msg);
std::string color_hex(uint32_t rgb);
void ui_show_user_menu(Snowflake guild_id, Snowflake user_id, GdkEvent *ev);
void ui_member_list_show(Snowflake guild_id, Snowflake channel_id);
void ui_set_voice_speaking_rows();

// ui_chat.cpp
class ChatView {
public:
	explicit ChatView(Snowflake cid);
	~ChatView();
	GtkWidget *widget() const { return root_; }
	Snowflake channel_id() const { return cid_; }
	Snowflake guild_id() const { return gid_; }
	void set_channel(Snowflake cid);
	void reload();            // the store was reset (new gateway session)
	void on_message_created(const MessagePtr &m);
	void on_message_updated(const MessagePtr &m);
	void on_message_deleted(Snowflake id);
	void on_members_changed();
	void on_typing(Snowflake uid);
	void on_channel_updated();
	void on_shown();          // became the visible tab
	void rerender();          // settings changed
	void focus_composer();
	void attach_file(const std::string &path);
	void maybe_ack();
	// Test hooks (debug FIFO): put text in the composer and send it, etc.
	void debug_command(const std::string &cmd, const std::string &arg);

	struct Impl;
private:
	Snowflake cid_ = 0, gid_ = 0;
	GtkWidget *root_ = nullptr;
	Impl *d = nullptr;
};

// ui_login.cpp
void login_window_show(const std::string &error);
void login_window_hide();

// ui_extra.cpp
void image_viewer_show(const std::string &url, int w, int h, const std::string &open_url);
void gif_picker_show(GtkWidget *relative_to, std::function<void(const std::string &url)> on_pick);
void emoji_picker_show(GtkWidget *relative_to, Snowflake guild_id, std::function<void(const std::string &text)> on_pick);
void settings_dialog_show();
void quick_switcher_show();
std::string twemoji_url(const std::string &seq);
