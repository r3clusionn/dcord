// Manual/automated probe without a GUI.
//   dcord-probe                     QR login handshake against Discord until the QR URL appears
//   dcord-probe voice GUILD CHANNEL SECONDS
//                                   log in with $DCORD_TOKEN, join voice, stay SECONDS, leave
#include <glib.h>

#include "app.h"
#include "http.h"
#include "props.h"
#include "remote_auth.h"
#include "util.h"
#include "voice.h"

extern std::function<void()> g_probe_on_ready;

static GMainLoop *loop;

int main(int argc, char **argv)
{
	loop = g_main_loop_new(nullptr, FALSE);
	http_init();
	int rc = 1;
	if (argc >= 5 && std::string(argv[1]) == "voice") {
		Snowflake gid = sf_parse(argv[2]), cid = sf_parse(argv[3]);
		int secs = atoi(argv[4]);
		voice_init();
		g_voice_changed = []() {
			VoiceStatus s = voice_status();
			LOG("probe: voice state %d e2ee %d dave v%d %s", (int)s.state, (int)s.e2ee_ready, s.dave_version, s.error.c_str());
		};
		g_probe_on_ready = [gid, cid]() { voice_join(gid, cid); };
		props_init([]() { app_start(token_load()); });
		g_timeout_add_seconds(secs, [](gpointer) -> gboolean {
			voice_leave();
			g_timeout_add(300, [](gpointer) -> gboolean { g_main_loop_quit(loop); return G_SOURCE_REMOVE; }, nullptr);
			return G_SOURCE_REMOVE;
		}, nullptr);
		g_main_loop_run(loop);
		VoiceStatus s = voice_status();
		(void)s;
		g_gateway.stop();
		http_shutdown();
		return 0;
	}
	RemoteAuth ra;
	props_init([&]() {
		LOG("build number ready; super props %zu bytes", props_super_b64().size());
		ra.on_qr = [&](const std::string &url) {
			LOG("QR URL: %s", url.c_str());
			rc = 0;
			g_main_loop_quit(loop);
		};
		ra.on_error = [&](const std::string &m) {
			LOG("error: %s", m.c_str());
			g_main_loop_quit(loop);
		};
		ra.start();
	});
	g_timeout_add_seconds(20, [](gpointer) -> gboolean { g_main_loop_quit(loop); return G_SOURCE_REMOVE; }, nullptr);
	g_main_loop_run(loop);
	ra.stop();
	http_shutdown();
	return rc;
}
