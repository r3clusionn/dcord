// Asynchronous HTTP on one background thread (libcurl multi, HTTP/2
// multiplexed). Callbacks run on the GTK main thread unless on_worker is set.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct HttpPart {
	std::string name;         // form field name, e.g. "files[0]" or "payload_json"
	std::string filename;     // empty for plain fields
	std::string content_type;
	std::string data;         // in-memory data, or
	std::string path;         // a file on disk
};

struct HttpResponse {
	long status = 0;          // 0 = transport error
	std::string body;
	std::string error;
	std::map<std::string, std::string> headers;  // lower-case names
	bool ok() const { return status >= 200 && status < 300; }
};

using HttpCallback = std::function<void(HttpResponse &)>;
using HttpProgress = std::function<void(double fraction)>;  // runs on the worker

struct HttpRequest {
	std::string method = "GET";
	std::string url;
	std::vector<std::string> headers;
	std::string body;
	std::vector<HttpPart> parts;  // multipart/form-data when non-empty
	bool media = false;           // low priority (images), separate concurrency limit
	bool on_worker = false;       // run the callback on the HTTP thread
	long timeout_s = 60;
	size_t max_body = 64u << 20;  // abort larger downloads
	HttpProgress progress;
	std::shared_ptr<bool> cancelled;  // set *cancelled = true to drop the callback
};

void http_init();
void http_shutdown();
void http_request(HttpRequest req, HttpCallback cb);
