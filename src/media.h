// Image and animation loading. Everything is decoded with the system ffmpeg
// libraries (JPEG, PNG, GIF, WebP, MP4/WebM "gifv"), scaled on worker threads
// to the size it is shown at, and cached in memory (plus on disk for small,
// often reused images like avatars and emoji).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <cairo.h>
#include <gtk/gtk.h>

#ifndef CB
#define CB(...) G_CALLBACK((__VA_ARGS__))
#endif

struct Media;
using MediaPtr = std::shared_ptr<Media>;

struct MediaOpts {
	int max_w = 0, max_h = 0;  // fit inside this box (device px); 0 = natural
	bool disk_cache = false;   // avatars, emoji, icons
	bool animate = false;      // keep the decoder for animation
	bool circle = false;       // round avatars
};

struct Media : std::enable_shared_from_this<Media> {
	enum State { Loading, Ready, Failed } state = Loading;
	int w = 0, h = 0;              // display size once known
	bool animated = false;
	std::string url;

	cairo_surface_t *frame() const;  // current frame (null while loading)
	// Advances the animation clock; returns true when the frame changed.
	bool tick(int64_t now_us);
	void add_waiter(std::function<void()> f);

	~Media();

	struct Impl;
	std::unique_ptr<Impl> d;
};

void media_init();
// Returns a shared, possibly still-loading image.
MediaPtr media_get(const std::string &url, const MediaOpts &opts);
// Decode a local file synchronously-ish (attachment previews); same caching rules.
MediaPtr media_get_file(const std::string &path, const MediaOpts &opts);
// Drops unused cached images beyond the memory budget.
void media_trim();

// A widget that shows the media at a fixed size, animating while visible.
// on_click receives the mouse button.
GtkWidget *media_widget(MediaPtr m, int w, int h, std::function<void(int button)> on_click = nullptr,
			bool spoiler = false);

// The main window gained/lost focus: animations only run while focused.
void media_set_window_active(bool active);

// Global switches
extern bool g_media_animate;          // play GIFs at all
extern double g_ui_scale;             // device px per logical px (Xft.dpi / 96)
inline int S(double px) { return (int)(px * g_ui_scale + 0.5); }

// Discord media proxy URL helper: asks for a resized static or animated copy.
std::string media_proxy_sized(const std::string &proxy_url, int w, int h, const char *format = nullptr);
