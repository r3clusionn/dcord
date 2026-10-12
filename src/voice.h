// Voice: voice gateway (v8) + UDP RTP with aead_aes256_gcm_rtpsize transport
// encryption, DAVE end-to-end encryption (libdave), Opus, PipeWire audio.
#pragma once

#include <functional>
#include <set>
#include <string>

#include "util.h"

struct VoiceStatus {
	enum State { Idle, Joining, Connecting, Connected, Failed } state = Idle;
	std::string error;
	Snowflake guild_id = 0, channel_id = 0;
	int ping_ms = -1;
	int dave_version = 0;   // 0 = transport encryption only
	bool e2ee_ready = false;
};

void voice_init();
// Join: sends the gateway voice state update; the rest follows from events.
void voice_join(Snowflake guild_id, Snowflake channel_id);
void voice_leave();
void voice_set_mute(bool m);
void voice_set_deaf(bool d);
bool voice_self_mute();
bool voice_self_deaf();
void voice_ptt(bool pressed);

// Gateway events about our own voice session.
void voice_on_state_update(const json &d);
void voice_on_server_update(const json &d);

VoiceStatus voice_status();
std::set<Snowflake> voice_speaking();
void voice_set_user_volume(Snowflake user, double vol);  // 0..2
double voice_user_volume(Snowflake user);
float voice_input_level_db();   // for the settings meter
void voice_reload_audio();      // device/volume settings changed

extern std::function<void()> g_voice_changed;   // status changed (main thread)
extern std::function<void()> g_voice_speaking;  // speaking set changed (main thread)
