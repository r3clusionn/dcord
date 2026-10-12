// UI entry points called by the app controller (main thread).
#pragma once

#include <string>

#include "store.h"

void ui_init();
void ui_show_login(const std::string &error = "");
void ui_show_main();
void ui_ready();                       // READY processed: rebuild everything
void ui_conn_state(int state);         // Gateway::State
void ui_tree_rebuild();                // guild list / folders changed
void ui_guild_rebuild(Snowflake guild_id);
void ui_channel_updated(Snowflake channel_id);
void ui_unread_changed(Snowflake channel_id);
void ui_message_created(const MessagePtr &m);
void ui_message_updated(const MessagePtr &m);
void ui_message_deleted(Snowflake channel_id, Snowflake message_id);
void ui_members_changed(Snowflake guild_id);  // names/colors may have changed
void ui_typing(Snowflake channel_id, Snowflake user_id);
void ui_voice_states_changed(Snowflake guild_id);
void ui_incoming_call(Snowflake channel_id, bool ringing);
void ui_member_list_update(Snowflake guild_id, const json &d);
void ui_attention(bool important);      // urgency hint / title badge
void ui_save_state();                   // window geometry, tabs, tree state
