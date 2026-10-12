// dcord: a lightweight Discord client in the spirit of Ripcord.
#include <cstdio>
#include <unistd.h>

#include <gtk/gtk.h>

#include "app.h"
#include "http.h"
#include "media.h"
#include "props.h"
#include "ui.h"
#include "voice.h"

void debug_fifo_init();

int main(int argc, char **argv)
{
	// Our own file chooser and no accessibility bus; keep GTK lean.
	g_setenv("NO_AT_BRIDGE", "1", FALSE);
	gtk_init(&argc, &argv);
	log_init();
	settings_load();
	http_init();
	media_init();
	voice_init();
	ui_init();
	debug_fifo_init();
	props_init([]() {
		std::string token = token_load();
		if (token.empty())
			ui_show_login();
		else
			app_start(token);
	});
	gtk_main();
	ui_save_state();
	voice_leave();
	g_gateway.stop();
	settings_save();
	http_shutdown();
	// The image decoder threads are detached; skip static destructors (their
	// condition variable would be destroyed under them) and leave right away.
	fflush(nullptr);
	_exit(0);
}
