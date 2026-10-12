#include "http.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <curl/curl.h>

#include "util.h"

namespace {

struct Job {
	HttpRequest req;
	HttpCallback cb;
	HttpResponse resp;
	CURL *easy = nullptr;
	curl_slist *hdrs = nullptr;
	curl_mime *mime = nullptr;
	bool too_big = false;
};

std::mutex g_mu;
std::deque<Job *> g_api_q, g_media_q;
CURLM *g_multi;
std::thread g_thread;
bool g_quit;
int g_active_api, g_active_media;
const int kMaxApi = 8, kMaxMedia = 8;

size_t on_body(char *p, size_t sz, size_t n, void *ud)
{
	Job *j = static_cast<Job *>(ud);
	size_t len = sz * n;
	if (j->resp.body.size() + len > j->req.max_body) {
		j->too_big = true;
		return 0;
	}
	j->resp.body.append(p, len);
	return len;
}

size_t on_header(char *p, size_t sz, size_t n, void *ud)
{
	Job *j = static_cast<Job *>(ud);
	size_t len = sz * n;
	std::string line(p, len);
	size_t c = line.find(':');
	if (c != std::string::npos) {
		std::string k = line.substr(0, c);
		for (auto &ch : k)
			ch = (char)tolower((unsigned char)ch);
		j->resp.headers[k] = trim(line.substr(c + 1));
	}
	return len;
}

int on_progress(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	Job *j = static_cast<Job *>(ud);
	if (j->req.cancelled && *j->req.cancelled)
		return 1;
	if (j->req.progress) {
		if (ultotal > 0 && ulnow < ultotal)
			j->req.progress((double)ulnow / (double)ultotal);
		else if (dltotal > 0)
			j->req.progress((double)dlnow / (double)dltotal);
	}
	return 0;
}

void start(Job *j)
{
	CURL *e = curl_easy_init();
	j->easy = e;
	curl_easy_setopt(e, CURLOPT_URL, j->req.url.c_str());
	curl_easy_setopt(e, CURLOPT_PRIVATE, j);
	curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_body);
	curl_easy_setopt(e, CURLOPT_WRITEDATA, j);
	curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, on_header);
	curl_easy_setopt(e, CURLOPT_HEADERDATA, j);
	curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(e, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(e, CURLOPT_TIMEOUT, j->req.timeout_s);
	curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(e, CURLOPT_PIPEWAIT, 1L);
	curl_easy_setopt(e, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
	curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
	if (j->req.progress || j->req.cancelled) {
		curl_easy_setopt(e, CURLOPT_XFERINFOFUNCTION, on_progress);
		curl_easy_setopt(e, CURLOPT_XFERINFODATA, j);
		curl_easy_setopt(e, CURLOPT_NOPROGRESS, 0L);
	}
	for (auto &h : j->req.headers)
		j->hdrs = curl_slist_append(j->hdrs, h.c_str());
	if (!j->req.parts.empty()) {
		j->mime = curl_mime_init(e);
		for (auto &p : j->req.parts) {
			curl_mimepart *mp = curl_mime_addpart(j->mime);
			curl_mime_name(mp, p.name.c_str());
			if (!p.path.empty())
				curl_mime_filedata(mp, p.path.c_str());
			else
				curl_mime_data(mp, p.data.data(), p.data.size());
			if (!p.filename.empty())
				curl_mime_filename(mp, p.filename.c_str());
			if (!p.content_type.empty())
				curl_mime_type(mp, p.content_type.c_str());
		}
		curl_easy_setopt(e, CURLOPT_MIMEPOST, j->mime);
		// The multipart is the body; a PATCH/PUT with parts still uses MIMEPOST.
		if (j->req.method != "POST")
			curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, j->req.method.c_str());
	} else if (j->req.method == "GET") {
		curl_easy_setopt(e, CURLOPT_HTTPGET, 1L);
	} else {
		curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, j->req.method.c_str());
		curl_easy_setopt(e, CURLOPT_POSTFIELDS, j->req.body.data());
		curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)j->req.body.size());
	}
	curl_easy_setopt(e, CURLOPT_HTTPHEADER, j->hdrs);
	curl_multi_add_handle(g_multi, e);
}

void finish(Job *j, CURLcode rc)
{
	curl_easy_getinfo(j->easy, CURLINFO_RESPONSE_CODE, &j->resp.status);
	if (rc != CURLE_OK) {
		j->resp.error = j->too_big ? "response too large" : curl_easy_strerror(rc);
		j->resp.status = 0;
	}
	curl_multi_remove_handle(g_multi, j->easy);
	curl_easy_cleanup(j->easy);
	curl_slist_free_all(j->hdrs);
	if (j->mime)
		curl_mime_free(j->mime);
	j->easy = nullptr;
	if (j->req.media)
		g_active_media--;
	else
		g_active_api--;
	if (j->req.cancelled && *j->req.cancelled) {
		delete j;
		return;
	}
	if (j->req.on_worker) {
		if (j->cb)
			j->cb(j->resp);
		delete j;
		return;
	}
	post_main([j]() {
		if (j->cb && !(j->req.cancelled && *j->req.cancelled))
			j->cb(j->resp);
		delete j;
	});
}

void run()
{
	while (true) {
		{
			std::lock_guard<std::mutex> lk(g_mu);
			if (g_quit)
				break;
			while (!g_api_q.empty() && g_active_api < kMaxApi) {
				Job *j = g_api_q.front();
				g_api_q.pop_front();
				g_active_api++;
				start(j);
			}
			while (!g_media_q.empty() && g_active_media < kMaxMedia) {
				Job *j = g_media_q.front();
				g_media_q.pop_front();
				if (j->req.cancelled && *j->req.cancelled) {
					delete j;
					continue;
				}
				g_active_media++;
				start(j);
			}
		}
		int running = 0;
		curl_multi_perform(g_multi, &running);
		CURLMsg *m;
		int left;
		while ((m = curl_multi_info_read(g_multi, &left))) {
			if (m->msg != CURLMSG_DONE)
				continue;
			Job *j;
			curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char **)&j);
			finish(j, m->data.result);
		}
		// curl shortens this to its own next timeout; new requests wake us up.
		curl_multi_poll(g_multi, nullptr, 0, 30000, nullptr);
	}
}

}  // namespace

void http_init()
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	g_multi = curl_multi_init();
	curl_multi_setopt(g_multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
	curl_multi_setopt(g_multi, CURLMOPT_MAX_HOST_CONNECTIONS, 6L);
	g_thread = std::thread(run);
}

void http_shutdown()
{
	{
		std::lock_guard<std::mutex> lk(g_mu);
		g_quit = true;
	}
	curl_multi_wakeup(g_multi);
	if (g_thread.joinable())
		g_thread.join();
}

void http_request(HttpRequest req, HttpCallback cb)
{
	Job *j = new Job;
	j->req = std::move(req);
	j->cb = std::move(cb);
	{
		std::lock_guard<std::mutex> lk(g_mu);
		(j->req.media ? g_media_q : g_api_q).push_back(j);
	}
	curl_multi_wakeup(g_multi);
}
