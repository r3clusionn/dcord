// No-op UI for the headless probe tool.
#include <functional>

#include "ui.h"

void ui_init() {}
void ui_show_login(const std::string &e) { LOG("ui: login needed (%s)", e.c_str()); }
void ui_show_main() {}
std::function<void()> g_probe_on_ready;
void ui_ready()
{
	LOG("ui: ready");
	if (g_probe_on_ready)
		g_probe_on_ready();
}
void ui_conn_state(int s) { LOG("ui: conn state %d", s); }
void ui_tree_rebuild() {}
void ui_guild_rebuild(Snowflake) {}
void ui_channel_updated(Snowflake) {}
void ui_unread_changed(Snowflake) {}
void ui_message_created(const MessagePtr &m) { LOG("ui: message %llu in %llu", (unsigned long long)m->id, (unsigned long long)m->channel_id); }
void ui_message_updated(const MessagePtr &) {}
void ui_message_deleted(Snowflake, Snowflake) {}
void ui_members_changed(Snowflake) {}
void ui_typing(Snowflake, Snowflake) {}
void ui_voice_states_changed(Snowflake) {}
void ui_incoming_call(Snowflake, bool) {}
void ui_member_list_update(Snowflake, const json &) {}
void ui_attention(bool) {}
void ui_save_state() {}
