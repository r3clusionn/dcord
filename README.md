# dcord

A lightweight Discord client for Linux in the spirit of Ripcord: one dense window with every server and DM in a tree, channels in tabs, compact messages, images and GIFs inline, and voice channels you join with a double-click. Ripcord itself can't be fixed: it is closed source, its password login stopped working when Discord added captchas, and its voice can't connect since Discord made end-to-end encrypted voice (DAVE) mandatory on 2 March 2026. dcord is written from scratch in C++ on GTK3, libcurl, OpenSSL, Opus, PipeWire and ffmpeg (all already on the target laptop: no new system packages), with Discord's own libdave and Cisco's MLS library built in for the voice encryption.

**Status:** version 0.1, feature-complete and tested against a local mock of Discord (REST, gateway, voice gateway with DAVE, and a UDP voice server), plus the real Discord login and gateway handshakes. It has not yet been used with a real account. The screenshot shows the mock's made-up servers, users and generated test images.

![The dcord window at 1582x1764 in its dark theme. On the left, a tree: Direct Messages (a red badge with 1) containing Alice (online, 1 unread mention) and the group "Alice, Carol"; the server Gentoo Laptop Club with the categories Text Channels (general, linux, media) and Voice, where the voice channel Lounge lists Bob (admin) in green because he is speaking, Carol marked muted, and rec in green; then Gaming and the server dwm users. Below the tree, the voice panel says "Voice connected 0 ms, Lounge / Gentoo Laptop Club" with Mic, Deaf, Leave and settings buttons. In the middle, three tabs (#general, Alice with an unread badge, #linux) and #general with its topic "General chat. Be nice.": the end of a bot embed about a gentoo-sources update with a colored bar, a message from Alice "look at this" with a fractal image, a red "new messages" divider, Bob (admin) in his red role color with an animated GIF from a Tenor link, Carol in blue with an animated Game of Life GIF, Alice saying "voice in 5?" followed by game controller and fire emoji, and "Carol is typing…" above the message box with its +, emoji and GIF buttons. On the right, the member list grouped by role: Admin (Bob), Mod (Carol), Online (Alice, rec) and Offline (Gentoo Bot)](docs/images/dcord.png)

## Features

- **Ripcord-style layout:** servers (in your Discord folders), categories, channels, active threads you joined and the people in each voice channel, all in one tree with unread and mention badges. Channels open in tabs; the member list sits on the right.
- **Login that works:**
  - **QR code:** scan it with the Discord phone app (Settings → Scan QR Code). This avoids the captcha that broke Ripcord's login.
  - **Email and password** with two-factor codes (authenticator app, SMS or backup codes).
  - **Token** pasted directly.
- **Messages:** Discord markdown (bold, italic, underline, strikethrough, inline code and code blocks, quotes, headings, subtext, lists, spoilers, masked links), mentions, channel links, `<t:…>` timestamps, custom and Unicode emoji (drawn as Twemoji images, since the laptop has no color emoji font), replies with a preview of the original, embeds with their colored bar, reactions (click to add or remove), edits, deletes, pins, a "new messages" divider, date separators, typing indicators, and an optional compact one-line-per-message mode.
- **Images and GIFs:**
  - images, GIFs, animated emoji, stickers and Tenor/Giphy "gifv" videos show inline and play while the window is focused;
  - click an image to open it full size;
  - videos open in mpv;
  - send files with the + button, by pasting (Ctrl+V of an image or of copied files) or by dropping them on the chat, optionally marked as spoilers;
  - a GIF picker searches Tenor through Discord, and an emoji picker covers your servers' emoji.
- **Voice:**
  - double-click a voice channel to join it, or start a DM call with the Call button in the DM's header (it rings the other side);
  - end-to-end encrypted with DAVE (Discord's protocol, through its own libdave);
  - voice activity with an adjustable threshold, or push-to-talk on any key, which works while another window has focus;
  - mute and deafen;
  - per-user volume (right-click someone in the voice channel);
  - speaking indicators in the tree, and the ping shown in the voice panel.
- **Small things Ripcord users expect:** Ctrl+K quick switcher, typing anywhere goes to the message box, `@` suggests people to mention, middle-click a channel for a new tab, Up to edit your last message, the window and tabs restored on start, and the X urgency hint (so dwm marks the tag) on DMs and mentions.

## How to install

Requires gcc with C++20, meson, ninja, pkgconf, GTK 3, libcurl with WebSocket support (curl 8.11 or newer), OpenSSL 3, zlib, Opus, PipeWire, ffmpeg (libavformat, libavcodec, libswscale), libX11 and libXi. Linux with X11 only. libdave, mlspp, nlohmann/json and qrcodegen are included in `third_party/`.

```sh
git clone https://github.com/r3clusionn/dcord
cd dcord
meson setup build --prefix=$HOME/.local
ninja -C build install
```

This installs `dcord` (2.0 MB stripped) to `~/.local/bin`.

## How to use

Start `dcord` and log in; the token is kept in `~/.config/dcord/token` (mode 600), settings in `~/.config/dcord/settings.json`, and cached avatars and emoji in `~/.cache/dcord`.

| Action | How |
|---|---|
| Open a channel | Click it (replaces the current tab) |
| Open in a new tab | Middle-click, or Ctrl+click |
| Join voice | Double-click a voice channel |
| Leave voice | Leave in the voice panel |
| Quick switcher | Ctrl+K |
| Close tab / next tab | Ctrl+W / Ctrl+Tab (or Ctrl+Page Up/Down) |
| Member list on/off | Ctrl+M |
| Mute | Ctrl+Shift+M or the Mic button |
| Reply, edit, delete, react, copy | Right-click a message |
| Mention someone | Type `@` and part of the name, then Tab or Enter (Up/Down to choose) |
| Edit your last message | Up in the empty message box |
| Cancel reply or edit, close Settings | Esc |
| New line in a message | Shift+Enter |
| Reveal a spoiler | Click it |

Push-to-talk: in Settings, tick Push to talk, click the key button and press the key. It is read from X input directly, so it works when dcord is not focused and the key still reaches other programs.

## How it works

- **Network:** all REST calls and image downloads go through one libcurl multi handle on one thread, multiplexed over HTTP/2. The gateway is a libcurl WebSocket with zlib-stream compression, heartbeats, resume and reconnect with backoff. Responses are handed to the GTK thread, which owns all state.
- **How dcord looks to Discord:** it presents itself as the Discord web client in Chrome on Linux. That means the same super properties header, a current client build number read from discord.com at start, and the gateway capabilities the web client uses.
- **The model** is one in-memory store: servers, channels, roles, members seen so far, read states and voice states. Channel visibility and send rights are computed from roles and permission overwrites the way Discord does. History is fetched 50 messages at a time while you scroll up. Only the 30 most recently used channels keep their messages in memory.
- **The message view** is a single GtkTextView per tab, so text selection and copying work across messages. Images, emoji and avatars are small drawing areas anchored in the text. A message is added at the end when it arrives; edits, deletes and older history re-render the view, keeping the scroll position.
- **Images** are decoded with ffmpeg on worker threads, because gdk-pixbuf here only has PNG. Discord's media proxy is asked for the size actually shown, and the result is scaled once. Small animations keep all their frames; larger ones (gifv videos) are decoded frame by frame, one frame ahead. Animations only tick while visible and while the window is focused; unfocused, they cost nothing. Avatars, emoji and server icons are also cached on disk.
- **Voice** follows Discord's voice gateway v8:
  - IP discovery over UDP, then `aead_aes256_gcm_rtpsize` transport encryption (OpenSSL), with `aead_xchacha20_poly1305_rtpsize` as the fallback;
  - DAVE on top: libdave's MLS session, key packages, proposals, commits and welcomes, and protocol transitions, mirroring libdave's reference integration. With DAVE active, nothing is sent until the group has a key;
  - the microphone is encoded as mono Opus (complexity 8, the channel's bitrate up to 128 kbps), and voices are decoded into per-user jitter buffers and mixed into one PipeWire stream;
  - a NAT keepalive every 5 s while silent.
- **Audio on demand:** joining voice runs `audio ensure`, which starts PipeWire on this laptop. Child processes never inherit dcord's sockets.

## Measurements

On the target laptop, on battery, against the local mock server (real Discord numbers will differ with server size).

| | |
|---|---|
| Window open on a channel, not focused | 56 MB PSS (28 MB anonymous, the rest shared libraries), 0% CPU, 1 wakeup/s, 10 threads |
| Focused, with a GIF and a gifv video playing | about 4% of one core |
| In voice (sending, receiving and mixing one voice, Opus both ways) | about 5.5% of one core, 259 wakeups/s, +3 MB |
| Opus encoding alone (20 ms mono frames, complexity 8) | 0.2 ms per frame at full clock, 1% of a core |
| Binary | 2.0 MB stripped, libdave and MLS included |

## Verification

- `meson test -C build`:
  - **markdown:** 33 cases covering every inline and block style, escapes, URLs and their trailing punctuation, mentions, custom emoji, timestamps, and emoji sequences (ZWJ, flags, keycaps, skin tones, text-style symbols that must stay text).
  - **voice** (`tests/voice_test.sh`): the mock server, a UDP forwarder and two headless clients (`dcord-probe`) join one voice channel with DAVE. The MLS handshake runs: key packages, the external sender's proposal (made with libdave's own test helper), commit and welcome, transition ready, execute. Both clients report E2EE ready, and each receives the other's 440 Hz test tone at exactly −13.5 dB, the RMS of the 0.3-amplitude sine that was sent. The server decrypted all 701 transport packets and saw 0 bad ones. The test also checks that nothing is sent before the group has a key: an earlier version sent 3 seconds unencrypted, which this test caught.
- **Against the mock in the real window,** driven through a debug command FIFO (`DCORD_DEBUG_FIFO`) and screenshots of the window:
  - the tree, folders, permissions (a channel hidden by an overwrite stays hidden), unread and mention badges;
  - sending with custom emoji conversion, replies, edits, deletes, reactions, uploads with preview;
  - the typing indicator, a mention from someone else, the member list;
  - the emoji and GIF pickers, settings, compact mode, DM tabs;
  - a dropped gateway connection that comes back (resume, invalid session, new identify, tabs reloaded);
  - voice join, mute, unmute, deafen and leave, with the matching speaking messages on the server.
- **AddressSanitizer and UBSan build** (`-Db_sanitize=address,undefined`) through all of the above: no errors.
- **Against real Discord:** the QR login handshake (RSA key, nonce proof, fingerprint, QR URL) and the gateway handshake (zlib-stream, identify, then close code 4004 for a bad token, which brings back the login window).
- **Not verified:** a real account's READY payload, real servers' voice and DAVE, password login with a real account, captcha, and threads and forums beyond listing them. Discord's DAVE gateway may differ in details from the mock. The mock only covers the first group creation, not members joining an existing group.

## Limits

- Linux with X11 only; built for one machine (`-march=native` through the system's flags).
- No video, screen sharing or Go Live (watching or streaming), no forum browsing (forum channels link to the browser), no friend requests, server settings or profiles.
- Password login can't solve captchas; use the QR code if Discord asks for one.
- Discord's terms don't allow third-party clients. Accounts are rarely actioned for using one, but it is possible. dcord sends the same headers as the web client and doesn't automate anything.
- The emoji picker has your servers' emoji plus about 150 common Unicode ones, not the full Unicode set.

## License

MIT (see `LICENSE`). Third-party code in `third_party/` keeps its own licenses: libdave (MIT), mlspp (BSD-2-Clause), nlohmann/json (MIT), qrcodegen (MIT).
