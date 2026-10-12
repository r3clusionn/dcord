// QR-code login through Discord's remote auth gateway: the phone app scans
// the code and approves, and we receive the account token. Callbacks run on
// the main thread.
#pragma once

#include <functional>
#include <memory>
#include <string>

class RemoteAuth {
public:
	std::function<void(const std::string &qr_url)> on_qr;
	std::function<void(const std::string &username)> on_scanned;
	std::function<void(const std::string &token)> on_token;
	std::function<void(const std::string &message)> on_error;

	RemoteAuth();
	~RemoteAuth();
	void start();
	void stop();

private:
	struct Impl;
	std::unique_ptr<Impl> d;
};
