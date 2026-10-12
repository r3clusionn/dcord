// Application controller: settings, the gateway connection, and the glue
// between gateway events, the store and the UI.
#pragma once

#include <string>

#include "gateway.h"
#include "store.h"

struct Settings {
	bool animate_gifs = true;
	bool show_avatars = true;
	bool compact = false;          // IRC-style one line per message
	bool show_member_list = true;
	bool ptt = false;              // push-to-talk instead of voice activity
	std::string ptt_key = "";      // X keysym name, e.g. "Caps_Lock" or "F13"
	double vad_threshold_db = -48;
	double input_gain = 1.0;
	double output_volume = 1.0;
	bool noise_gate = true;
	std::string input_device;      // PipeWire node name, empty = default
	std::string output_device;
	json user_volumes = json::object();  // user id -> 0..2
	json window = json::object();
	json tabs = json::array();
	json collapsed = json::array();
};

extern Settings g_settings;
extern Gateway g_gateway;

void settings_load();
void settings_save();

std::string token_load();
void token_save(const std::string &token);
void token_clear();

// Starts the gateway with the saved token (or shows the login window).
void app_start(const std::string &token);
void app_logout();

// Gateway helpers used by the UI.
void app_subscribe_guild(Snowflake guild_id);
void app_request_members(Snowflake guild_id, const std::vector<Snowflake> &users);
void app_voice_join(Snowflake guild_id, Snowflake channel_id);
void app_voice_leave();
void app_voice_set_self(bool mute, bool deaf);
void app_mark_read(Snowflake channel_id);
void app_request_member_list(Snowflake guild_id, Snowflake channel_id);
