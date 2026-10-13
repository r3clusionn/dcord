#include "media.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <list>
#include <set>
#include <mutex>
#include <thread>
#include <unordered_map>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "http.h"
#include "props.h"
#include "util.h"

bool g_media_animate = true;
double g_ui_scale = 1.0;

namespace {

// --- worker pool -------------------------------------------------------------

std::mutex g_pool_mu;
std::condition_variable g_pool_cv;
std::deque<std::function<void()>> g_pool_q;
std::vector<std::thread> g_pool;

void pool_run(std::function<void()> f)
{
	{
		std::lock_guard<std::mutex> lk(g_pool_mu);
		g_pool_q.push_back(std::move(f));
	}
	g_pool_cv.notify_one();
}

void pool_main()
{
	while (true) {
		std::function<void()> f;
		{
			std::unique_lock<std::mutex> lk(g_pool_mu);
			g_pool_cv.wait(lk, [] { return !g_pool_q.empty(); });
			f = std::move(g_pool_q.front());
			g_pool_q.pop_front();
		}
		f();
	}
}

// --- ffmpeg decoding ------------------------------------------------------------

struct Decoder {
	std::string data;
	size_t pos = 0;
	AVIOContext *avio = nullptr;
	AVFormatContext *fmt = nullptr;
	AVCodecContext *cc = nullptr;
	SwsContext *sws = nullptr;
	AVFrame *frame = nullptr;
	AVPacket *pkt = nullptr;
	int stream = -1;
	int src_w = 0, src_h = 0, src_fmt = -1;
	int out_w = 0, out_h = 0;
	int max_w = 0, max_h = 0;
	bool has_alpha = false;
	bool eof_sent = false;
	int64_t last_pts = AV_NOPTS_VALUE;
	int frames = 0;
	uint8_t *tmp = nullptr;  // padded swscale output
	size_t tmp_size = 0;

	~Decoder()
	{
		close();
		av_free(tmp);
	}

	void close()
	{
		if (sws)
			sws_freeContext(sws);
		sws = nullptr;
		if (frame)
			av_frame_free(&frame);
		if (pkt)
			av_packet_free(&pkt);
		if (cc)
			avcodec_free_context(&cc);
		if (fmt)
			avformat_close_input(&fmt);
		if (avio) {
			av_freep(&avio->buffer);
			avio_context_free(&avio);
		}
		stream = -1;
		eof_sent = false;
		frames = 0;
		last_pts = AV_NOPTS_VALUE;
	}

	static int read_cb(void *op, uint8_t *buf, int n)
	{
		Decoder *d = static_cast<Decoder *>(op);
		size_t left = d->data.size() - d->pos;
		if (!left)
			return AVERROR_EOF;
		size_t k = std::min(left, (size_t)n);
		memcpy(buf, d->data.data() + d->pos, k);
		d->pos += k;
		return (int)k;
	}

	static int64_t seek_cb(void *op, int64_t off, int whence)
	{
		Decoder *d = static_cast<Decoder *>(op);
		if (whence == AVSEEK_SIZE)
			return (int64_t)d->data.size();
		int64_t base = whence == SEEK_CUR ? (int64_t)d->pos : whence == SEEK_END ? (int64_t)d->data.size() : 0;
		int64_t np = base + off;
		if (np < 0 || np > (int64_t)d->data.size())
			return -1;
		d->pos = (size_t)np;
		return np;
	}

	bool open()
	{
		close();
		pos = 0;
		const int bs = 32768;
		uint8_t *buf = (uint8_t *)av_malloc(bs);
		avio = avio_alloc_context(buf, bs, 0, this, read_cb, nullptr, seek_cb);
		fmt = avformat_alloc_context();
		fmt->pb = avio;
		fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
		if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0) {
			fmt = nullptr;
			return false;
		}
		if (avformat_find_stream_info(fmt, nullptr) < 0)
			return false;
		const AVCodec *codec = nullptr;
		stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
		if (stream < 0 || !codec)
			return false;
		cc = avcodec_alloc_context3(codec);
		avcodec_parameters_to_context(cc, fmt->streams[stream]->codecpar);
		cc->thread_count = 1;
		if (avcodec_open2(cc, codec, nullptr) < 0)
			return false;
		frame = av_frame_alloc();
		pkt = av_packet_alloc();
		return true;
	}

	bool rewind()
	{
		if (fmt && av_seek_frame(fmt, stream, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
			avcodec_flush_buffers(cc);
			eof_sent = false;
			last_pts = AV_NOPTS_VALUE;
			return true;
		}
		return open();
	}

	// Returns the next decoded frame as a cairo surface, or null at the end.
	cairo_surface_t *next(int *delay_ms)
	{
		while (true) {
			int r = avcodec_receive_frame(cc, frame);
			if (r == 0)
				return convert(delay_ms);
			if (r == AVERROR_EOF)
				return nullptr;
			if (r != AVERROR(EAGAIN))
				return nullptr;
			if (eof_sent)
				return nullptr;
			r = av_read_frame(fmt, pkt);
			if (r < 0) {
				avcodec_send_packet(cc, nullptr);
				eof_sent = true;
				continue;
			}
			if (pkt->stream_index == stream)
				avcodec_send_packet(cc, pkt);
			av_packet_unref(pkt);
		}
	}

	cairo_surface_t *convert(int *delay_ms)
	{
		AVStream *st = fmt->streams[stream];
		int64_t dur = frame->duration;
		double tb = av_q2d(st->time_base);
		int d = dur > 0 ? (int)(dur * tb * 1000) : 0;
		if (d <= 0 && frame->pts != AV_NOPTS_VALUE && last_pts != AV_NOPTS_VALUE)
			d = (int)((frame->pts - last_pts) * tb * 1000);
		if (d <= 0) {
			AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
			d = fr.num ? (int)(1000.0 * fr.den / fr.num) : 100;
		}
		// Browsers clamp tiny GIF delays to 100 ms; do the same.
		if (d < 20)
			d = 100;
		last_pts = frame->pts;
		*delay_ms = d;
		frames++;
		if (frame->width != src_w || frame->height != src_h || frame->format != src_fmt || !sws) {
			src_w = frame->width;
			src_h = frame->height;
			src_fmt = frame->format;
			const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((AVPixelFormat)src_fmt);
			has_alpha = desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA);
			if (src_fmt == AV_PIX_FMT_PAL8)
				has_alpha = true;
			double sc = 1.0;
			if (max_w > 0 && src_w > max_w)
				sc = std::min(sc, (double)max_w / src_w);
			if (max_h > 0 && src_h > max_h)
				sc = std::min(sc, (double)max_h / src_h);
			out_w = std::max(1, (int)lround(src_w * sc));
			out_h = std::max(1, (int)lround(src_h * sc));
			if (sws)
				sws_freeContext(sws);
			sws = sws_getContext(src_w, src_h, (AVPixelFormat)src_fmt, out_w, out_h, AV_PIX_FMT_BGRA,
					     sc < 1.0 ? SWS_AREA : SWS_BILINEAR, nullptr, nullptr, nullptr);
		}
		// swscale's SIMD paths may write past the end of a row, so scale into a padded buffer and
		// copy; writing straight into cairo's exactly-sized surface corrupted the heap (MP4 GIFs).
		int tstride = FFALIGN(out_w * 4, 64);
		size_t need = (size_t)tstride * (out_h + 2) + 64;
		if (tmp_size < need) {
			av_free(tmp);
			tmp = (uint8_t *)av_malloc(need);
			tmp_size = need;
		}
		uint8_t *dst[4] = {tmp, nullptr, nullptr, nullptr};
		int dst_stride[4] = {tstride, 0, 0, 0};
		sws_scale(sws, frame->data, frame->linesize, 0, src_h, dst, dst_stride);
		cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, out_w, out_h);
		uint8_t *px = cairo_image_surface_get_data(s);
		int stride = cairo_image_surface_get_stride(s);
		for (int y = 0; y < out_h; y++)
			memcpy(px + (size_t)y * stride, tmp + (size_t)y * tstride, (size_t)out_w * 4);
		if (has_alpha) {
			// Cairo wants premultiplied alpha.
			for (int y = 0; y < out_h; y++) {
				uint8_t *p = px + y * stride;
				for (int x = 0; x < out_w; x++, p += 4) {
					unsigned a = p[3];
					if (a != 255) {
						p[0] = (uint8_t)(p[0] * a / 255);
						p[1] = (uint8_t)(p[1] * a / 255);
						p[2] = (uint8_t)(p[2] * a / 255);
					}
				}
			}
		} else {
			for (int y = 0; y < out_h; y++) {
				uint8_t *p = px + y * stride;
				for (int x = 0; x < out_w; x++, p += 4)
					p[3] = 255;
			}
		}
		cairo_surface_mark_dirty(s);
		av_frame_unref(frame);
		return s;
	}
};

cairo_surface_t *make_circle(cairo_surface_t *src)
{
	int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
	cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(dst);
	cairo_arc(cr, w / 2.0, h / 2.0, std::min(w, h) / 2.0, 0, 2 * M_PI);
	cairo_clip(cr);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(src);
	return dst;
}

uint64_t fnv1a(const std::string &s)
{
	uint64_t h = 1469598103934665603ULL;
	for (unsigned char c : s) {
		h ^= c;
		h *= 1099511628211ULL;
	}
	return h;
}

std::string disk_path(const std::string &url)
{
	char b[32];
	snprintf(b, sizeof b, "%016llx", (unsigned long long)fnv1a(url));
	return cache_dir() + "/media/" + b;
}

const size_t kFrameCacheBudget = 6u << 20;  // decode all frames if they fit
const size_t kMemBudget = 96u << 20;        // total for cached surfaces

}  // namespace

struct Media::Impl {
	MediaOpts opts;
	std::mutex mu;
	std::vector<std::pair<cairo_surface_t *, int>> frames;  // cached frames + delays
	size_t idx = 0;
	int64_t due_us = 0;
	// Streaming mode (animations too large to keep decoded).
	std::unique_ptr<Decoder> dec;
	bool streaming = false;
	bool busy = false;
	cairo_surface_t *pending = nullptr;
	int pending_delay = 100;
	cairo_surface_t *cur = nullptr;  // streaming current frame
	std::vector<std::function<void()>> waiters;
	size_t bytes = 0;
	int64_t last_use = 0;

	~Impl()
	{
		for (auto &f : frames)
			cairo_surface_destroy(f.first);
		if (pending)
			cairo_surface_destroy(pending);
		if (cur)
			cairo_surface_destroy(cur);
	}
};

Media::~Media() = default;

cairo_surface_t *Media::frame() const
{
	if (state != Ready)
		return nullptr;
	if (d->streaming && d->cur)
		return d->cur;
	if (d->frames.empty())
		return nullptr;
	return d->frames[std::min(d->idx, d->frames.size() - 1)].first;
}

void Media::add_waiter(std::function<void()> f)
{
	if (state != Loading) {
		f();
		return;
	}
	d->waiters.push_back(std::move(f));
}

namespace {
std::unordered_map<std::string, MediaPtr> g_cache;

void finish(MediaPtr m, bool ok)
{
	post_main([m, ok]() {
		m->state = ok ? Media::Ready : Media::Failed;
		auto w = std::move(m->d->waiters);
		for (auto &f : w)
			f();
	});
}

// Decodes on a pool thread.
void decode(MediaPtr m, std::string bytes)
{
	pool_run([m, bytes = std::move(bytes)]() mutable {
		auto dec = std::make_unique<Decoder>();
		dec->data = std::move(bytes);
		dec->max_w = m->d->opts.max_w;
		dec->max_h = m->d->opts.max_h;
		if (!dec->open()) {
			finish(m, false);
			return;
		}
		std::vector<std::pair<cairo_surface_t *, int>> frames;
		size_t total = 0;
		bool want_anim = m->d->opts.animate;
		bool complete = false;
		while (true) {
			int delay = 100;
			cairo_surface_t *s = dec->next(&delay);
			if (!s) {
				complete = true;
				break;
			}
			if (m->d->opts.circle)
				s = make_circle(s);
			total += (size_t)cairo_image_surface_get_stride(s) * cairo_image_surface_get_height(s);
			frames.push_back({s, delay});
			if (!want_anim)
				break;
			if (total > kFrameCacheBudget && frames.size() > 1)
				break;
		}
		if (frames.empty()) {
			finish(m, false);
			return;
		}
		int w = cairo_image_surface_get_width(frames[0].first);
		int h = cairo_image_surface_get_height(frames[0].first);
		bool streaming = want_anim && !complete;
		if (streaming) {
			// Too big to keep: keep only the first frame and stream the rest.
			for (size_t i = 1; i < frames.size(); i++)
				cairo_surface_destroy(frames[i].first);
			frames.resize(1);
			dec->rewind();
			int dl;
			if (cairo_surface_t *f0 = dec->next(&dl))
				cairo_surface_destroy(f0);  // skip frame 0, it's already shown
		} else {
			dec.reset();
		}
		post_main([m, frames, w, h, streaming, total, dec = dec.release()]() mutable {
			m->w = w;
			m->h = h;
			m->d->frames = std::move(frames);
			m->animated = m->d->frames.size() > 1 || streaming;
			m->d->streaming = streaming;
			m->d->dec.reset(dec);
			m->d->bytes = streaming ? (size_t)w * h * 4 * 3 : total;
			if (streaming) {
				m->d->cur = cairo_surface_reference(m->d->frames[0].first);
				m->d->pending_delay = m->d->frames[0].second;
			}
			m->state = Media::Ready;
			auto ws = std::move(m->d->waiters);
			for (auto &f : ws)
				f();
		});
	});
}

void fetch(MediaPtr m)
{
	const std::string url = m->url;
	bool disk = m->d->opts.disk_cache;
	auto net = [m, url, disk]() {
		HttpRequest r;
		r.url = url;
		r.media = true;
		r.on_worker = true;
		r.headers = {"User-Agent: " + props_user_agent()};
		r.max_body = 50u << 20;
		http_request(std::move(r), [m, url, disk](HttpResponse &resp) {
			if (!resp.ok() || resp.body.empty()) {
				LOG("media: %s -> %ld %s", url.c_str(), resp.status, resp.error.c_str());
				finish(m, false);
				return;
			}
			if (disk) {
				std::string p = disk_path(url);
				write_file(p, resp.body);
			}
			decode(m, std::move(resp.body));
		});
	};
	if (disk) {
		pool_run([m, url, net]() {
			bool ok = false;
			std::string data = read_file(disk_path(url), &ok);
			if (ok && !data.empty())
				decode(m, std::move(data));
			else
				net();
		});
	} else {
		net();
	}
}

std::string cache_key(const std::string &url, const MediaOpts &o)
{
	return url + "|" + std::to_string(o.max_w) + "x" + std::to_string(o.max_h) + (o.animate ? "a" : "") +
	       (o.circle ? "c" : "");
}
}  // namespace

bool Media::tick(int64_t now_us)
{
	if (state != Ready || !animated || !g_media_animate)
		return false;
	d->last_use = now_us;
	if (!d->due_us) {
		int first = d->streaming ? d->pending_delay : d->frames[d->idx].second;
		d->due_us = now_us + first * 1000LL;
		return false;
	}
	if (now_us < d->due_us)
		return false;
	if (!d->streaming) {
		d->idx = (d->idx + 1) % d->frames.size();
		d->due_us += d->frames[d->idx].second * 1000LL;
		if (d->due_us < now_us)
			d->due_us = now_us + d->frames[d->idx].second * 1000LL;
		return true;
	}
	bool changed = false;
	{
		std::lock_guard<std::mutex> lk(d->mu);
		if (d->pending) {
			if (d->cur)
				cairo_surface_destroy(d->cur);
			d->cur = d->pending;
			d->pending = nullptr;
			d->due_us = now_us + d->pending_delay * 1000LL;
			changed = true;
		}
		if (d->busy || d->pending)
			return changed;
		d->busy = true;
	}
	// Decode the following frame in the background.
	std::weak_ptr<Media> keep = weak_from_this();
	pool_run([keep]() {
		MediaPtr m = keep.lock();
		if (!m)
			return;
		Impl *d = m->d.get();
		int delay = 100;
		cairo_surface_t *s = d->dec ? d->dec->next(&delay) : nullptr;
		if (!s && d->dec) {
			d->dec->rewind();
			s = d->dec->next(&delay);
		}
		std::lock_guard<std::mutex> lk(d->mu);
		d->pending = s;
		d->pending_delay = delay;
		d->busy = false;
	});
	return changed;
}

void media_init()
{
	mkdirs(cache_dir() + "/media");
	av_log_set_level(AV_LOG_ERROR);
	unsigned n = std::max(2u, std::min(4u, std::thread::hardware_concurrency() / 4));
	for (unsigned i = 0; i < n; i++) {
		g_pool.emplace_back(pool_main);
		g_pool.back().detach();
	}
}

static MediaPtr make_media(const std::string &url, const MediaOpts &opts, bool *fresh)
{
	std::string key = cache_key(url, opts);
	auto it = g_cache.find(key);
	if (it != g_cache.end()) {
		*fresh = false;
		it->second->d->last_use = g_get_monotonic_time();
		return it->second;
	}
	auto m = std::make_shared<Media>();
	m->d = std::make_unique<Media::Impl>();
	m->d->opts = opts;
	m->d->last_use = g_get_monotonic_time();
	m->url = url;
	g_cache[key] = m;
	*fresh = true;
	return m;
}

MediaPtr media_get(const std::string &url, const MediaOpts &opts)
{
	bool fresh;
	MediaPtr m = make_media(url, opts, &fresh);
	if (fresh) {
		if (url.empty()) {
			m->state = Media::Failed;
			return m;
		}
		fetch(m);
	}
	return m;
}

MediaPtr media_get_file(const std::string &path, const MediaOpts &opts)
{
	bool fresh;
	MediaPtr m = make_media("file://" + path, opts, &fresh);
	if (fresh)
		pool_run([m, path]() {
			bool ok;
			std::string data = read_file(path, &ok);
			if (!ok)
				finish(m, false);
			else
				decode(m, std::move(data));
		});
	return m;
}

void media_trim()
{
	size_t total = 0;
	std::vector<std::pair<int64_t, std::string>> idle;
	for (auto &[k, m] : g_cache) {
		total += m->d->bytes;
		if (m.use_count() == 1 && m->state != Media::Loading)
			idle.push_back({m->d->last_use, k});
	}
	std::sort(idle.begin(), idle.end());
	for (auto &[t, k] : idle) {
		if (total <= kMemBudget)
			break;
		auto it = g_cache.find(k);
		total -= it->second->d->bytes;
		g_cache.erase(it);
	}
	// Streaming decoders of animations nobody shows can go right away.
	for (auto &[k, m] : g_cache)
		if (m.use_count() == 1 && m->d->streaming && m->d->dec && !m->d->busy) {
			std::lock_guard<std::mutex> lk(m->d->mu);
			m->d->dec->close();
			m->d->dec.reset();
		}
}

std::string media_proxy_sized(const std::string &proxy_url, int w, int h, const char *format)
{
	if (proxy_url.empty())
		return proxy_url;
	std::string u = proxy_url;
	u += u.find('?') == std::string::npos ? '?' : '&';
	u += "width=" + std::to_string(w) + "&height=" + std::to_string(h);
	if (format)
		u += std::string("&format=") + format;
	return u;
}

// --- widget -----------------------------------------------------------------------

namespace {
bool g_window_active = true;
std::set<GtkWidget *> g_anim_widgets;  // mapped widgets showing animations

struct MediaWidget {
	MediaPtr m;
	std::function<void(int)> on_click;
	guint tick_id = 0;
	bool spoiler = false;
	int w, h;
};

gboolean mw_draw(GtkWidget *wd, cairo_t *cr, gpointer p)
{
	MediaWidget *mw = static_cast<MediaWidget *>(p);
	int aw = gtk_widget_get_allocated_width(wd), ah = gtk_widget_get_allocated_height(wd);
	cairo_surface_t *s = mw->m->frame();
	if (!s) {
		cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, mw->m->state == Media::Failed ? 0.25 : 0.12);
		cairo_rectangle(cr, 0, 0, aw, ah);
		cairo_fill(cr);
		return FALSE;
	}
	int sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
	double sc = std::min((double)aw / sw, (double)ah / sh);
	cairo_save(cr);
	cairo_translate(cr, (aw - sw * sc) / 2, (ah - sh * sc) / 2);
	cairo_scale(cr, sc, sc);
	cairo_set_source_surface(cr, s, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), sc < 1 ? CAIRO_FILTER_GOOD : CAIRO_FILTER_BILINEAR);
	cairo_paint(cr);
	cairo_restore(cr);
	if (mw->spoiler) {
		cairo_set_source_rgba(cr, 0.12, 0.12, 0.14, 0.97);
		cairo_rectangle(cr, 0, 0, aw, ah);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
		cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, S(11));
		cairo_text_extents_t te;
		cairo_text_extents(cr, "SPOILER", &te);
		cairo_move_to(cr, (aw - te.width) / 2, (ah + te.height) / 2);
		cairo_show_text(cr, "SPOILER");
	}
	return FALSE;
}

gboolean mw_tick(GtkWidget *wd, GdkFrameClock *clock, gpointer p)
{
	MediaWidget *mw = static_cast<MediaWidget *>(p);
	if (mw->spoiler)
		return G_SOURCE_CONTINUE;
	if (mw->m->tick(gdk_frame_clock_get_frame_time(clock)))
		gtk_widget_queue_draw(wd);
	return G_SOURCE_CONTINUE;
}

void mw_update_tick(GtkWidget *wd, MediaWidget *mw)
{
	bool anim = gtk_widget_get_mapped(wd) && mw->m->state == Media::Ready && mw->m->animated;
	if (anim)
		g_anim_widgets.insert(wd);
	else
		g_anim_widgets.erase(wd);
	bool want = anim && g_media_animate && g_window_active;
	if (want && !mw->tick_id)
		mw->tick_id = gtk_widget_add_tick_callback(wd, mw_tick, mw, nullptr);
	else if (!want && mw->tick_id) {
		gtk_widget_remove_tick_callback(wd, mw->tick_id);
		mw->tick_id = 0;
	}
}

gboolean mw_press(GtkWidget *wd, GdkEventButton *ev, gpointer p)
{
	MediaWidget *mw = static_cast<MediaWidget *>(p);
	if (ev->type != GDK_BUTTON_PRESS)
		return FALSE;
	if (mw->spoiler && ev->button == 1) {
		mw->spoiler = false;
		mw_update_tick(wd, mw);
		gtk_widget_queue_draw(wd);
		return TRUE;
	}
	if (mw->on_click) {
		// Copy: the handler may destroy this widget (and mw with it).
		auto f = mw->on_click;
		f((int)ev->button);
		return TRUE;
	}
	return FALSE;
}
}  // namespace

GtkWidget *media_widget(MediaPtr m, int w, int h, std::function<void(int)> on_click, bool spoiler)
{
	GtkWidget *da = gtk_drawing_area_new();
	gtk_widget_set_size_request(da, w, h);
	auto *mw = new MediaWidget{m, std::move(on_click), 0, spoiler, w, h};
	g_object_set_data_full(G_OBJECT(da), "mw", mw, [](gpointer p) { delete static_cast<MediaWidget *>(p); });
	g_signal_connect(da, "draw", G_CALLBACK(mw_draw), mw);
	gtk_widget_add_events(da, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(da, "button-press-event", G_CALLBACK(mw_press), mw);
	g_signal_connect(da, "map", CB(+[](GtkWidget *wd, gpointer p) { mw_update_tick(wd, (MediaWidget *)p); }), mw);
	g_signal_connect(da, "unmap", CB(+[](GtkWidget *wd, gpointer p) { mw_update_tick(wd, (MediaWidget *)p); }), mw);
	g_signal_connect(da, "destroy", CB(+[](GtkWidget *wd, gpointer) { g_anim_widgets.erase(wd); }), nullptr);
	if (on_click || spoiler) {
		g_signal_connect(da, "realize", CB(+[](GtkWidget *wd, gpointer) {
			GdkCursor *c = gdk_cursor_new_from_name(gtk_widget_get_display(wd), "pointer");
			gdk_window_set_cursor(gtk_widget_get_window(wd), c);
			if (c)
				g_object_unref(c);
		}), nullptr);
	}
	// Redraw once loaded; the weak ref keeps us safe if the widget is gone by then.
	GWeakRef *ref = g_new0(GWeakRef, 1);
	g_weak_ref_init(ref, da);
	m->add_waiter([ref]() {
		GtkWidget *wd = (GtkWidget *)g_weak_ref_get(ref);
		if (wd) {
			MediaWidget *mw = (MediaWidget *)g_object_get_data(G_OBJECT(wd), "mw");
			gtk_widget_queue_draw(wd);
			if (mw)
				mw_update_tick(wd, mw);
			g_object_unref(wd);
		}
		g_weak_ref_clear(ref);
		g_free(ref);
	});
	return da;
}

void media_set_window_active(bool active)
{
	if (g_window_active == active)
		return;
	g_window_active = active;
	for (GtkWidget *w : std::vector<GtkWidget *>(g_anim_widgets.begin(), g_anim_widgets.end()))
		if (auto *mw = (MediaWidget *)g_object_get_data(G_OBJECT(w), "mw"))
			mw_update_tick(w, mw);
}
