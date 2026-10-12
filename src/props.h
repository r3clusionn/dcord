// Client identity sent to Discord: super properties, user agent, headers.
// dcord presents itself as the Discord web client in Chrome on Linux.
#pragma once

#include <string>
#include <vector>

#include "util.h"

// Loads the cached build number and refreshes it from discord.com/app in the
// background. `done` runs on the main thread once a build number is known.
void props_init(std::function<void()> done);
json props_object();
const std::string &props_user_agent();
std::string props_super_b64();
// Headers for a discord.com API request. `auth` adds Authorization.
std::vector<std::string> api_headers(const std::string &token, bool json_body = true);
extern const char *kApiBase;  // https://discord.com/api/v9
