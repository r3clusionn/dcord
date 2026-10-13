// Debug command FIFO for automated UI tests: set DCORD_DEBUG_FIFO=/path and
// write lines like "open <channel>", "send <text>", "voice <guild> <channel>".
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "ui_internal.h"
#include "voice.h"

static std::string g_fifo_buf;

static void run_line(const std::string &line)
{
	size_t sp = line.find(' ');
	std::string cmd = line.substr(0, sp), arg = sp == std::string::npos ? "" : line.substr(sp + 1);
	LOG("debug: %s %s", cmd.c_str(), arg.c_str());
	if (cmd == "open") {
		ui_open_channel(sf_parse(arg.substr(0, arg.find(' '))), arg.find(" new") != std::string::npos);
	} else if (cmd == "voice") {
		size_t s2 = arg.find(' ');
		voice_join(sf_parse(arg.substr(0, s2)), sf_parse(arg.substr(s2 + 1)));
	} else if (cmd == "mute") {
		voice_set_mute(arg != "0");
	} else if (cmd == "deaf") {
		voice_set_deaf(arg != "0");
	} else if (cmd == "leave") {
		voice_leave();
	} else if (cmd == "settings") {
		settings_dialog_show();
	} else if (cmd == "view") {
		image_viewer_show(arg, 0, 0, arg);
	} else if (cmd == "switcher") {
		quick_switcher_show();
	} else if (cmd == "expand") {
		gtk_tree_view_expand_all(GTK_TREE_VIEW(g_ui.tree));
	} else if (cmd == "quit") {
		gtk_main_quit();
	} else if (ChatView *v = ui_current_view()) {
		v->debug_command(cmd, arg);
	}
}

void debug_fifo_init()
{
	const char *path = getenv("DCORD_DEBUG_FIFO");
	if (!path)
		return;
	unlink(path);
	mkfifo(path, 0600);
	int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	GIOChannel *ch = g_io_channel_unix_new(fd);
	g_io_add_watch(ch, G_IO_IN, [](GIOChannel *c, GIOCondition, gpointer) -> gboolean {
		char buf[4096];
		ssize_t n = read(g_io_channel_unix_get_fd(c), buf, sizeof buf);
		if (n > 0)
			g_fifo_buf.append(buf, n);
		size_t nl;
		while ((nl = g_fifo_buf.find('\n')) != std::string::npos) {
			std::string line = g_fifo_buf.substr(0, nl);
			g_fifo_buf.erase(0, nl + 1);
			if (!line.empty())
				run_line(line);
		}
		return TRUE;
	}, nullptr);
}
