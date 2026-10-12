#include "audio.h"

#include <cstring>
#include <unistd.h>
#include <spa/param/audio/format-utils.h>
#include <pipewire/pipewire.h>

#include "util.h"

namespace {
pw_thread_loop *g_loop;
pw_stream *g_cap, *g_play;
std::function<void(const float *, int)> g_on_cap;
std::function<void(float *, int)> g_on_play;
bool g_inited;

void cap_process(void *)
{
	pw_buffer *b = pw_stream_dequeue_buffer(g_cap);
	if (!b)
		return;
	spa_buffer *buf = b->buffer;
	if (buf->datas[0].data && buf->datas[0].chunk) {
		const float *s = (const float *)((uint8_t *)buf->datas[0].data + buf->datas[0].chunk->offset);
		int n = (int)(buf->datas[0].chunk->size / sizeof(float));
		if (g_on_cap && n > 0)
			g_on_cap(s, n);
	}
	pw_stream_queue_buffer(g_cap, b);
}

void play_process(void *)
{
	pw_buffer *b = pw_stream_dequeue_buffer(g_play);
	if (!b)
		return;
	spa_buffer *buf = b->buffer;
	float *d = (float *)buf->datas[0].data;
	if (!d) {
		pw_stream_queue_buffer(g_play, b);
		return;
	}
	int stride = sizeof(float) * 2;
	int frames = buf->datas[0].maxsize / stride;
	if (b->requested && (int)b->requested < frames)
		frames = (int)b->requested;
	if (g_on_play)
		g_on_play(d, frames);
	else
		memset(d, 0, frames * stride);
	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = stride;
	buf->datas[0].chunk->size = frames * stride;
	pw_stream_queue_buffer(g_play, b);
}

pw_stream_events make_events(void (*process)(void *))
{
	pw_stream_events e = {};
	e.version = PW_VERSION_STREAM_EVENTS;
	e.process = process;
	return e;
}
const pw_stream_events cap_events = make_events(cap_process);
const pw_stream_events play_events = make_events(play_process);

pw_stream *make_stream(bool capture, const std::string &target)
{
	pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, capture ? "Capture" : "Playback",
		PW_KEY_MEDIA_ROLE, "Communication",
		PW_KEY_APP_NAME, "dcord",
		PW_KEY_NODE_NAME, capture ? "dcord-mic" : "dcord-voice",
		PW_KEY_NODE_LATENCY, "960/48000",
		nullptr);
	if (!target.empty())
		pw_properties_set(props, PW_KEY_TARGET_OBJECT, target.c_str());
	pw_stream *s = pw_stream_new_simple(pw_thread_loop_get_loop(g_loop), capture ? "dcord capture" : "dcord voice",
					    props, capture ? &cap_events : &play_events, nullptr);
	uint8_t pod_buf[1024];
	spa_pod_builder bld = SPA_POD_BUILDER_INIT(pod_buf, sizeof pod_buf);
	spa_audio_info_raw info = {};
	info.format = SPA_AUDIO_FORMAT_F32;
	info.rate = 48000;
	info.channels = capture ? 1 : 2;
	if (capture) {
		info.position[0] = SPA_AUDIO_CHANNEL_MONO;
	} else {
		info.position[0] = SPA_AUDIO_CHANNEL_FL;
		info.position[1] = SPA_AUDIO_CHANNEL_FR;
	}
	const spa_pod *params[1] = {spa_format_audio_raw_build(&bld, SPA_PARAM_EnumFormat, &info)};
	int rc = pw_stream_connect(s, capture ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT, PW_ID_ANY,
				   (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
						     PW_STREAM_FLAG_RT_PROCESS),
				   params, 1);
	if (rc < 0) {
		LOG("audio: stream connect failed: %d", rc);
		pw_stream_destroy(s);
		return nullptr;
	}
	return s;
}
}  // namespace

bool audio_start(std::function<void(const float *, int)> on_capture, std::function<void(float *, int)> on_playback,
		 const std::string &input_target, const std::string &output_target, bool capture)
{
	audio_stop();
	// The owner's system starts PipeWire on demand.
	if (access("/usr/local/bin/audio", X_OK) == 0) {
		run_wait({"/usr/local/bin/audio", "ensure"});
	}
	if (!g_inited) {
		pw_init(nullptr, nullptr);
		g_inited = true;
	}
	g_on_cap = std::move(on_capture);
	g_on_play = std::move(on_playback);
	g_loop = pw_thread_loop_new("dcord-audio", nullptr);
	if (!g_loop)
		return false;
	pw_thread_loop_lock(g_loop);
	if (pw_thread_loop_start(g_loop) < 0) {
		pw_thread_loop_unlock(g_loop);
		pw_thread_loop_destroy(g_loop);
		g_loop = nullptr;
		return false;
	}
	g_play = make_stream(false, output_target);
	g_cap = capture ? make_stream(true, input_target) : nullptr;
	pw_thread_loop_unlock(g_loop);
	LOG("audio: started (play %p, capture %p)", (void *)g_play, (void *)g_cap);
	return g_play != nullptr;
}

void audio_stop()
{
	if (!g_loop)
		return;
	pw_thread_loop_lock(g_loop);
	if (g_cap)
		pw_stream_destroy(g_cap);
	if (g_play)
		pw_stream_destroy(g_play);
	g_cap = g_play = nullptr;
	pw_thread_loop_unlock(g_loop);
	pw_thread_loop_stop(g_loop);
	pw_thread_loop_destroy(g_loop);
	g_loop = nullptr;
	g_on_cap = nullptr;
	g_on_play = nullptr;
}

void audio_set_capture_active(bool on)
{
	if (!g_loop || !g_cap)
		return;
	pw_thread_loop_lock(g_loop);
	pw_stream_set_active(g_cap, on);
	pw_thread_loop_unlock(g_loop);
}
