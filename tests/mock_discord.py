#!/usr/bin/env python3
"""A small fake Discord (REST + zlib-stream gateway + media) for testing dcord
without an account. Python standard library only.

    tests/mock_discord.py [--port 8765] [--media DIR]

Then run dcord with:
    DCORD_API_BASE=http://127.0.0.1:8765/api/v9 DCORD_GATEWAY=ws://127.0.0.1:8765/gateway \
    DCORD_APP_URL=http://127.0.0.1:8765/app DCORD_TOKEN=mock build/dcord

POST /mock/say {"channel_id": ..., "content": ..., "author": "alice"} injects a
message from another user (MESSAGE_CREATE over the gateway).
"""
import argparse
import base64
import email.parser
import email.policy
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

EPOCH = 1420070400000


def snow(ms=None, inc=[0]):
    inc[0] += 1
    ms = int(time.time() * 1000) if ms is None else ms
    return str(((ms - EPOCH) << 22) | (inc[0] & 0xFFF))


def iso(ms):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(ms / 1000)) + ".%03d000+00:00" % (ms % 1000)


ARGS = None
BASE = ""
CLIENTS = []
CLIENTS_LOCK = threading.Lock()

ME = {"id": "100000000000000001", "username": "rec", "global_name": "rec", "avatar": "me", "discriminator": "0"}
USERS = {
    "alice": {"id": "100000000000000002", "username": "alice", "global_name": "Alice", "avatar": "alice", "discriminator": "0"},
    "bob": {"id": "100000000000000003", "username": "bob_builds", "global_name": "Bob", "avatar": "bob", "discriminator": "0"},
    "carol": {"id": "100000000000000004", "username": "carol", "global_name": "Carol", "avatar": "carol", "discriminator": "0"},
    "bot": {"id": "100000000000000005", "username": "Gentoo Bot", "global_name": None, "avatar": "bot", "discriminator": "0", "bot": True},
}
G1, G2 = "200000000000000001", "200000000000000002"
CH = {
    "cat_text": "300000000000000001", "general": "300000000000000002", "linux": "300000000000000003",
    "media": "300000000000000004", "cat_voice": "300000000000000005", "lounge": "300000000000000006",
    "gaming": "300000000000000007", "secret": "300000000000000008", "g2_chat": "300000000000000009",
    "dm_alice": "300000000000000010", "dm_group": "300000000000000011", "g2_voice": "300000000000000012",
}
ROLE_ADMIN, ROLE_MOD = "400000000000000001", "400000000000000002"
EMOJI_ID = "500000000000000001"
MESSAGES = {}  # channel -> list of message dicts (ascending)
LOCK = threading.Lock()


def media(name):
    return BASE + "/media/" + name


def make_media(d):
    os.makedirs(d, exist_ok=True)

    def ff(out, *args):
        p = os.path.join(d, out)
        if not os.path.exists(p):
            subprocess.run(["ffmpeg", "-loglevel", "error", "-y", *args, p], check=True)

    colors = {"me": "0x5865f2", "alice": "0xeb459e", "bob": "0x57f287", "carol": "0xfee75c", "bot": "0x99aab5",
              "g1": "0x7c3aed", "g2": "0x0ea5e9", "emoji": "0xf97316"}
    for n, c in colors.items():
        ff(n + ".png", "-f", "lavfi", "-i", f"color=c={c}:s=128x128,drawbox=x=32:y=32:w=64:h=64:color=white@0.6:t=fill",
           "-frames:v", "1")
    ff("photo.jpg", "-f", "lavfi", "-i", "mandelbrot=s=1280x800:start_scale=1.2", "-frames:v", "1", "-q:v", "3")
    ff("anim.gif", "-f", "lavfi", "-i", "life=s=160x90:mold=10:r=10:ratio=0.1:death_color=#1e1f22:life_color=#57f287,trim=duration=4,scale=320x180:flags=neighbor",
       "-vf", "split[a][b];[a]palettegen[p];[b][p]paletteuse")
    ff("clip.mp4", "-f", "lavfi", "-i", "mandelbrot=s=400x224:r=15:maxiter=256:start_scale=0.8,trim=duration=3", "-c:v", "mpeg4", "-q:v", "4", "-pix_fmt", "yuv420p")
    ff("thumb.png", "-f", "lavfi", "-i", "mandelbrot=s=400x224:start_scale=0.8", "-frames:v", "1")


def user_avatar(u):
    return u


def msg(channel, author, content, ms, **extra):
    m = {"id": snow(ms), "channel_id": channel, "author": author, "content": content, "timestamp": iso(ms),
         "edited_timestamp": None, "type": 0, "flags": 0, "mentions": [], "mention_roles": [],
         "mention_everyone": False, "attachments": [], "embeds": [], "pinned": False}
    m.update(extra)
    return m


def seed():
    now = int(time.time() * 1000)
    a, b, c, bot = USERS["alice"], USERS["bob"], USERS["carol"], USERS["bot"]
    t = now - 3 * 3600 * 1000
    gen = []
    gen.append(msg(CH["general"], a, "morning all", t))
    gen.append(msg(CH["general"], a, "anyone tried the new **kernel 7.2** yet?", t + 20000))
    gen.append(msg(CH["general"], b, "yeah, running `7.2.9` here. *zero* issues so far", t + 90000))
    gen.append(msg(CH["general"], c, "how do you keep the package count that low? \U0001F440", t + 400000))
    gen.append(msg(CH["general"], ME, "depclean after every batch, and `package.provided` for the empty acct-group ones", t + 460000))
    gen.append(msg(CH["general"], b, "```bash\nemerge --ask --depclean\nemerge @preserved-rebuild\n```", t + 520000))
    gen.append(msg(CH["general"], a, "nice <:gentoo:%s> also ||the spoiler is that it took a week||" % EMOJI_ID, t + 600000,
                   reactions=[{"emoji": {"id": None, "name": "\U0001F602"}, "count": 3, "me": True},
                              {"emoji": {"id": EMOJI_ID, "name": "gentoo"}, "count": 1, "me": False}]))
    reply_to = gen[-3]
    gen.append(msg(CH["general"], c, "<@%s> that's the trick, thanks!" % ME["id"], t + 700000,
                   mentions=[ME], message_reference={"message_id": reply_to["id"], "channel_id": CH["general"]},
                   referenced_message=reply_to, type=19))
    gen.append(msg(CH["general"], bot, "", t + 760000, embeds=[{
        "type": "rich", "title": "Package update: sys-kernel/gentoo-sources-7.2.10", "url": "https://packages.gentoo.org/",
        "description": "Stable on **amd64**. 3 security fixes.", "color": 0x54487a,
        "fields": [{"name": "Changes", "value": "- amdgpu fixes\n- xfs: faster fsync", "inline": False}],
        "footer": {"text": "packages.gentoo.org"}}]))
    gen.append(msg(CH["general"], a, "look at this", now - 600000, attachments=[{
        "id": snow(), "filename": "photo.jpg", "size": 182311, "url": media("photo.jpg"), "proxy_url": media("photo.jpg"),
        "width": 1280, "height": 800, "content_type": "image/jpeg"}]))
    gen.append(msg(CH["general"], b, "https://tenor.com/view/mock-gif", now - 500000, embeds=[{
        "type": "gifv", "url": "https://tenor.com/view/mock-gif", "provider": {"name": "Tenor"},
        "thumbnail": {"url": media("thumb.png"), "proxy_url": media("thumb.png"), "width": 400, "height": 224},
        "video": {"url": media("clip.mp4"), "proxy_url": media("clip.mp4"), "width": 400, "height": 224}}]))
    gen.append(msg(CH["general"], c, "", now - 400000, attachments=[{
        "id": snow(), "filename": "anim.gif", "size": 91234, "url": media("anim.gif"), "proxy_url": media("anim.gif"),
        "width": 320, "height": 180, "content_type": "image/gif"}]))
    gen.append(msg(CH["general"], a, "voice in 5? \U0001F3AE\U0001F525", now - 120000))
    MESSAGES[CH["general"]] = gen
    MESSAGES[CH["linux"]] = [msg(CH["linux"], b, "# Tip of the day\nUse `-march=native` and LTO.", now - 86400000)]
    MESSAGES[CH["media"]] = []
    MESSAGES[CH["dm_alice"]] = [msg(CH["dm_alice"], a, "hey, you around?", now - 300000),
                                msg(CH["dm_alice"], ME, "yep", now - 280000),
                                msg(CH["dm_alice"], a, "sending you the config later \U0001F44D", now - 60000)]
    MESSAGES[CH["dm_group"]] = [msg(CH["dm_group"], c, "group chat test", now - 7200000)]
    MESSAGES[CH["g2_chat"]] = [msg(CH["g2_chat"], bot, "Welcome to the server!", now - 999999)]


def channel(id_, gid, name, type_, pos, parent=None, **kw):
    c = {"id": id_, "guild_id": gid, "name": name, "type": type_, "position": pos, "parent_id": parent,
         "permission_overwrites": [], "last_message_id": None}
    c.update(kw)
    return c


def last_id(cid):
    l = MESSAGES.get(cid) or []
    return l[-1]["id"] if l else None


def ready_payload():
    g1_channels = [
        channel(CH["cat_text"], G1, "Text Channels", 4, 0),
        channel(CH["general"], G1, "general", 0, 0, CH["cat_text"], topic="General chat. Be nice.", last_message_id=last_id(CH["general"])),
        channel(CH["linux"], G1, "linux", 0, 1, CH["cat_text"], last_message_id=last_id(CH["linux"])),
        channel(CH["media"], G1, "media", 0, 2, CH["cat_text"]),
        channel(CH["secret"], G1, "mods-only", 0, 3, CH["cat_text"],
                permission_overwrites=[{"id": G1, "type": 0, "allow": "0", "deny": "1024"}]),
        channel(CH["cat_voice"], G1, "Voice", 4, 1),
        channel(CH["lounge"], G1, "Lounge", 2, 0, CH["cat_voice"], bitrate=64000),
        channel(CH["gaming"], G1, "Gaming", 2, 1, CH["cat_voice"], bitrate=96000),
    ]
    g2_channels = [channel(CH["g2_chat"], G2, "welcome", 0, 0, last_message_id=last_id(CH["g2_chat"])),
                   channel(CH["g2_voice"], G2, "talk", 2, 1)]
    roles = [{"id": G1, "name": "@everyone", "permissions": str(1024 | 2048 | 65536 | 1048576 | 2097152 | 32768 | 64 | 33554432),
              "position": 0, "color": 0},
             {"id": ROLE_ADMIN, "name": "Admin", "permissions": "8", "position": 2, "color": 0xE74C3C, "hoist": True},
             {"id": ROLE_MOD, "name": "Mod", "permissions": "0", "position": 1, "color": 0x3498DB, "hoist": True}]
    guilds = [
        {"id": G1, "properties": {"id": G1, "name": "Gentoo Laptop Club", "icon": "g1", "owner_id": USERS["bob"]["id"]},
         "channels": g1_channels, "roles": roles, "threads": [], "member_count": 5, "data_mode": "full",
         "emojis": [{"id": EMOJI_ID, "name": "gentoo", "animated": False}]},
        {"id": G2, "properties": {"id": G2, "name": "dwm users", "icon": "g2", "owner_id": ME["id"]},
         "channels": g2_channels, "roles": [{"id": G2, "name": "@everyone", "permissions": "104324673", "position": 0, "color": 0}],
         "threads": [], "member_count": 2, "data_mode": "full", "emojis": []},
    ]
    merged = [[{"user_id": ME["id"], "roles": [], "nick": None}], [{"user_id": ME["id"], "roles": []}]]
    privs = [{"id": CH["dm_alice"], "type": 1, "recipient_ids": [USERS["alice"]["id"]], "last_message_id": last_id(CH["dm_alice"])},
             {"id": CH["dm_group"], "type": 3, "name": None, "recipient_ids": [USERS["alice"]["id"], USERS["carol"]["id"]],
              "last_message_id": last_id(CH["dm_group"])}]
    gen = MESSAGES[CH["general"]]
    read_state = [{"id": CH["general"], "last_message_id": gen[-4]["id"], "mention_count": 1},
                  {"id": CH["linux"], "last_message_id": last_id(CH["linux"]), "mention_count": 0},
                  {"id": CH["dm_alice"], "last_message_id": MESSAGES[CH["dm_alice"]][1]["id"], "mention_count": 1},
                  {"id": CH["dm_group"], "last_message_id": last_id(CH["dm_group"]), "mention_count": 0},
                  {"id": CH["g2_chat"], "last_message_id": last_id(CH["g2_chat"]), "mention_count": 0}]
    return {
        "v": 9, "user": ME, "users": list(USERS.values()), "session_id": "mocksession", "resume_gateway_url": "",
        "guilds": guilds, "merged_members": merged, "private_channels": privs,
        "read_state": {"entries": read_state, "partial": False, "version": 1},
        "user_guild_settings": {"entries": [], "partial": False, "version": 1},
        "relationships": [{"id": USERS["alice"]["id"], "type": 1}],
        "merged_presences": {"friends": [{"user_id": USERS["alice"]["id"], "status": "online"}], "guilds": []},
    }


def supplemental():
    vs = [{"user_id": USERS["bob"]["id"], "channel_id": CH["lounge"], "session_id": "x", "self_mute": False, "self_deaf": False},
          {"user_id": USERS["carol"]["id"], "channel_id": CH["lounge"], "session_id": "y", "self_mute": True, "self_deaf": False}]
    return {"guilds": [{"id": G1, "voice_states": vs}, {"id": G2, "voice_states": []}],
            "merged_members": [[{"user_id": USERS["bob"]["id"], "roles": [ROLE_ADMIN], "nick": "Bob (admin)"},
                                {"user_id": USERS["carol"]["id"], "roles": [ROLE_MOD]}], []],
            "merged_presences": {"friends": [], "guilds": []}}


# --- gateway ---------------------------------------------------------------------

VOICE_KEY = os.urandom(32)
VOICE_LOCK = threading.Lock()
VOICE = {"clients": {}, "kp": {}, "ready": set(), "group": False, "dave": None, "seq": 0}
SSRCS = {}


def dave_cmd(line):
    d = VOICE["dave"]
    d.stdin.write(line + "\n")
    d.stdin.flush()
    out = d.stdout.readline().strip()
    if out.startswith("ERR"):
        sys.stderr.write("mock dave: " + out + "\n")
    return out


class VoiceClient:
    """Voice gateway v8. With --dave, runs Discord's DAVE MLS flow between the
    connected clients (external sender = tests/dave_sender)."""
    def __init__(self, sock):
        self.g = GatewayClient(sock)
        self.uid = None
        self.lock = threading.Lock()

    def send(self, op, d):
        with self.lock:
            VOICE["seq"] += 1
            self.g.send_frame(json.dumps({"op": op, "d": d, "seq": VOICE["seq"]}).encode(), 1)

    def send_bin(self, op, payload):
        with self.lock:
            VOICE["seq"] += 1
            self.g.send_frame(struct.pack("!HB", VOICE["seq"] & 0xFFFF, op) + payload, 2)

    def others(self):
        return [c for u, c in VOICE["clients"].items() if u != self.uid]

    def run(self):
        self.send(8, {"heartbeat_interval": 5000})
        try:
            while True:
                op, data = self.g.recv_frame()
                if op == 8:
                    break
                if op == 2:
                    self.on_binary(data[0], data[1:])
                    continue
                if op != 1:
                    continue
                m = json.loads(data)
                self.on_op(m.get("op"), m.get("d"))
        except (ConnectionError, OSError):
            pass
        with VOICE_LOCK:
            if VOICE["clients"].get(self.uid) is self:
                del VOICE["clients"][self.uid]
                VOICE["kp"].pop(self.uid, None)
                VOICE["group"] = False
                VOICE["ready"].clear()
                for c in self.others():
                    c.send(13, {"user_id": self.uid})

    def on_op(self, o, d):
        if o == 0:
            self.uid = d["user_id"]
            sys.stderr.write("mock voice: identify %s max_dave=%s\n" % (self.uid, d.get("max_dave_protocol_version")))
            SSRCS.setdefault(self.uid, 1111 + 1111 * len(SSRCS))
            self.send(2, {"ssrc": SSRCS[self.uid], "ip": "127.0.0.1", "port": ARGS.port + 1,
                          "modes": ["aead_aes256_gcm_rtpsize", "aead_xchacha20_poly1305_rtpsize"]})
        elif o == 1:
            dv = 1 if ARGS.dave else 0
            self.send(4, {"mode": d["data"]["mode"], "secret_key": list(VOICE_KEY), "dave_protocol_version": dv})
            with VOICE_LOCK:
                VOICE["clients"][self.uid] = self
                others = self.others()
                if others:
                    self.send(11, {"user_ids": [c.uid for c in others]})
                for c in others:
                    c.send(11, {"user_ids": [self.uid]})
                    c.send(5, {"user_id": self.uid, "ssrc": SSRCS[self.uid], "speaking": 1})
                    self.send(5, {"user_id": c.uid, "ssrc": SSRCS[c.uid], "speaking": 1})
                if not others:
                    self.send(5, {"user_id": USERS["bob"]["id"], "ssrc": 2222, "speaking": 1})
                if dv:
                    self.send_bin(25, bytes.fromhex(dave_cmd("sender")))
        elif o == 3:
            self.send(6, d)
        elif o == 5:
            sys.stderr.write("mock voice: %s speaking %s\n" % (self.uid, d.get("speaking")))
        elif o == 23:
            with VOICE_LOCK:
                VOICE["ready"].add(self.uid)
                sys.stderr.write("mock dave: %s ready for transition %s\n" % (self.uid, d.get("transition_id")))
                if len(VOICE["ready"]) == len(VOICE["clients"]):
                    for c in VOICE["clients"].values():
                        c.send(22, {"transition_id": d.get("transition_id")})
                    sys.stderr.write("mock dave: execute transition %s\n" % d.get("transition_id"))
        elif o == 31:
            sys.stderr.write("mock dave: %s INVALID COMMIT/WELCOME %s\n" % (self.uid, d))

    def on_binary(self, op, payload):
        if op == 26:  # key package
            with VOICE_LOCK:
                VOICE["kp"][self.uid] = payload
                sys.stderr.write("mock dave: key package from %s (%d bytes)\n" % (self.uid, len(payload)))
                if len(VOICE["kp"]) >= 2 and not VOICE["group"]:
                    VOICE["group"] = True
                    uids = sorted(VOICE["kp"])
                    creator, joiner = VOICE["clients"][uids[0]], uids[1]
                    prop = dave_cmd("propose 0 " + VOICE["kp"][joiner].hex())
                    creator.send_bin(27, bytes.fromhex(prop))
                    sys.stderr.write("mock dave: asked %s to add %s\n" % (creator.uid, joiner))
        elif op == 28:  # commit (+ welcome)
            out = dave_cmd("split " + payload.hex()).split()
            commit, welcome = bytes.fromhex(out[0]), bytes.fromhex(out[1])
            sys.stderr.write("mock dave: commit %d bytes, welcome %d bytes from %s\n" % (len(commit), len(welcome), self.uid))
            with VOICE_LOCK:
                self.send_bin(29, struct.pack("!H", 1) + commit)
                for c in self.others():
                    c.send_bin(30, struct.pack("!H", 1) + welcome)


class GatewayClient:
    def __init__(self, sock):
        self.sock = sock
        self.z = zlib.compressobj()
        self.seq = 0
        self.lock = threading.Lock()

    def send_frame(self, data, opcode=2):
        n = len(data)
        if n < 126:
            head = struct.pack("!BB", 0x80 | opcode, n)
        elif n < 65536:
            head = struct.pack("!BBH", 0x80 | opcode, 126, n)
        else:
            head = struct.pack("!BBQ", 0x80 | opcode, 127, n)
        self.sock.sendall(head + data)

    def send(self, op, d=None, t=None):
        with self.lock:
            m = {"op": op, "d": d}
            if op == 0:
                self.seq += 1
                m["s"] = self.seq
                m["t"] = t
            raw = json.dumps(m).encode()
            comp = self.z.compress(raw) + self.z.flush(zlib.Z_SYNC_FLUSH)
            self.send_frame(comp)

    def dispatch(self, t, d):
        self.send(0, d, t)

    def recv_frame(self):
        def recv_exact(n):
            b = b""
            while len(b) < n:
                c = self.sock.recv(n - len(b))
                if not c:
                    raise ConnectionError
                b += c
            return b
        h = recv_exact(2)
        op = h[0] & 0x0F
        ln = h[1] & 0x7F
        if ln == 126:
            ln = struct.unpack("!H", recv_exact(2))[0]
        elif ln == 127:
            ln = struct.unpack("!Q", recv_exact(8))[0]
        mask = recv_exact(4) if h[1] & 0x80 else b"\0\0\0\0"
        data = bytearray(recv_exact(ln))
        for i in range(len(data)):
            data[i] ^= mask[i % 4]
        return op, bytes(data)

    def run(self):
        self.send(10, {"heartbeat_interval": 41250})
        try:
            while True:
                op, data = self.recv_frame()
                if op == 8:
                    break
                if op == 9:
                    self.send_frame(data, 10)
                    continue
                if op != 1:
                    continue
                m = json.loads(data)
                self.on_op(m.get("op"), m.get("d"))
        except (ConnectionError, OSError):
            pass
        with CLIENTS_LOCK:
            if self in CLIENTS:
                CLIENTS.remove(self)

    def on_op(self, op, d):
        if op == 1:
            self.send(11)
        elif op == 2:
            if d.get("token") not in ("mock", "mock2"):
                self.send_frame(struct.pack("!H", 4004) + b"Authentication failed.", 8)
                return
            self.me = USERS["alice"] if d.get("token") == "mock2" else ME
            with LOCK:
                r = ready_payload()
                if self.me is not ME:
                    r["user"] = self.me
                    r["users"] = [u for u in USERS.values() if u is not self.me] + [ME]
                    r["session_id"] = "mocksession2"
                    r["merged_members"] = [[{"user_id": self.me["id"], "roles": []}], [{"user_id": self.me["id"], "roles": []}]]
                self.dispatch("READY", r)
            self.dispatch("READY_SUPPLEMENTAL", supplemental())
            with CLIENTS_LOCK:
                CLIENTS.append(self)
        elif op == 6:
            # No resume support: tell the client to start a new session.
            self.send(9, False)
        elif op == 8:
            members = []
            for uid in d.get("user_ids") or []:
                for u in USERS.values():
                    if u["id"] == uid:
                        roles = [ROLE_ADMIN] if u is USERS["bob"] else [ROLE_MOD] if u is USERS["carol"] else []
                        members.append({"user": u, "roles": roles, "nick": None})
            gid = d["guild_id"][0] if isinstance(d.get("guild_id"), list) else d.get("guild_id")
            self.dispatch("GUILD_MEMBERS_CHUNK", {"guild_id": gid, "members": members, "chunk_index": 0, "chunk_count": 1})
        elif op == 37:
            for gid, sub in (d.get("subscriptions") or {}).items():
                if gid != G1 or not sub.get("channels"):
                    continue
                items = [{"group": {"id": ROLE_ADMIN, "count": 1}},
                         {"member": {"user": USERS["bob"], "roles": [ROLE_ADMIN], "presence": {"status": "online"}}},
                         {"group": {"id": ROLE_MOD, "count": 1}},
                         {"member": {"user": USERS["carol"], "roles": [ROLE_MOD], "presence": {"status": "idle"}}},
                         {"group": {"id": "online", "count": 2}},
                         {"member": {"user": USERS["alice"], "roles": [], "presence": {"status": "online"}}},
                         {"member": {"user": ME, "roles": [], "presence": {"status": "online"}}},
                         {"group": {"id": "offline", "count": 1}},
                         {"member": {"user": USERS["bot"], "roles": [], "presence": {"status": "offline"}}}]
                self.dispatch("GUILD_MEMBER_LIST_UPDATE", {"guild_id": gid, "id": "everyone", "member_count": 5,
                                                           "online_count": 4, "groups": [],
                                                           "ops": [{"op": "SYNC", "range": [0, 99], "items": items}]})
        elif op == 4:
            me = getattr(self, "me", ME)
            st = {"user_id": me["id"], "guild_id": d.get("guild_id"), "channel_id": d.get("channel_id"),
                  "session_id": "mocksession" if me is ME else "mocksession2", "self_mute": d.get("self_mute"), "self_deaf": d.get("self_deaf")}
            self.dispatch("VOICE_STATE_UPDATE", st)
            if d.get("channel_id") and not getattr(self, "voice_sent", False):
                self.voice_sent = True
                self.dispatch("VOICE_SERVER_UPDATE", {"token": "vtok", "guild_id": d.get("guild_id"),
                                                      "endpoint": "ws://127.0.0.1:%d/voice" % ARGS.port})
            if not d.get("channel_id"):
                self.voice_sent = False


def broadcast(t, d):
    with CLIENTS_LOCK:
        cl = list(CLIENTS)
    for c in cl:
        try:
            c.dispatch(t, d)
        except OSError:
            pass


# --- HTTP ------------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        sys.stderr.write("mock: " + (fmt % a) + "\n")

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def reply(self, code, obj=None, ctype="application/json", raw=None):
        data = raw if raw is not None else (json.dumps(obj).encode() if obj is not None else b"")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        p = self.path.split("?")[0]
        if p in ("/voice/", "/gateway/"):
            p = p[:-1]
        if p in ("/gateway", "/voice") and self.headers.get("Upgrade", "").lower() == "websocket":
            key = self.headers["Sec-WebSocket-Key"]
            acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            self.send_response(101)
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", acc)
            self.end_headers()
            self.wfile.flush()
            (VoiceClient if p == "/voice" else GatewayClient)(self.connection).run()
            self.close_connection = True
            return
        if p == "/app":
            return self.reply(200, raw=b'<script>window.GLOBAL_ENV = {"BUILD_NUMBER":"634304"}</script>', ctype="text/html")
        if p.startswith("/media/") or p.startswith("/avatars/") or p.startswith("/icons/") or p.startswith("/emojis/"):
            name = p.rsplit("/", 1)[-1].split(".")[0] if not p.startswith("/media/") else p[7:]
            if p.startswith("/emojis/"):
                name = "emoji"
            fn = os.path.join(ARGS.media, name if "." in name else name + ".png")
            if not os.path.exists(fn):
                return self.reply(404, {"message": "no such media"})
            ctype = {"jpg": "image/jpeg", "gif": "image/gif", "mp4": "video/mp4"}.get(fn.rsplit(".", 1)[-1], "image/png")
            return self.reply(200, raw=open(fn, "rb").read(), ctype=ctype)
        parts = p.split("/")
        if p.startswith("/api/v9/channels/") and p.endswith("/messages"):
            cid = parts[4]
            q = dict(x.split("=") for x in self.path.split("?")[1].split("&")) if "?" in self.path else {}
            with LOCK:
                l = list(MESSAGES.get(cid, []))
            if "before" in q:
                l = [m for m in l if int(m["id"]) < int(q["before"])]
            l = l[-int(q.get("limit", 50)):]
            return self.reply(200, list(reversed(l)))
        if p.endswith("/pins"):
            return self.reply(200, [])
        if p.startswith("/api/v9/gifs/"):
            g = {"id": "1", "title": "mock gif", "url": "https://tenor.com/view/mock-gif", "src": media("clip.mp4"),
                 "gif_src": media("anim.gif"), "width": 400, "height": 224, "preview": media("thumb.png")}
            return self.reply(200, [g] * 6)
        self.reply(404, {"message": "404: Not Found", "code": 0})

    def do_POST(self):
        p = self.path.split("?")[0]
        raw = self.body()
        parts = p.split("/")
        if p == "/mock/say":
            d = json.loads(raw)
            m = msg(d["channel_id"], USERS[d.get("author", "alice")], d["content"], int(time.time() * 1000))
            if ME["id"] in d["content"]:
                m["mentions"] = [ME]
            with LOCK:
                MESSAGES.setdefault(d["channel_id"], []).append(m)
            m2 = dict(m)
            if d["channel_id"] in (CH["general"], CH["linux"], CH["media"]):
                m2["guild_id"] = G1
            broadcast("MESSAGE_CREATE", m2)
            return self.reply(200, m)
        if p == "/mock/drop":
            with CLIENTS_LOCK:
                cl = list(CLIENTS)
            for c in cl:
                try:
                    c.sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            return self.reply(200, {"dropped": len(cl)})
        if p == "/mock/typing":
            d = json.loads(raw)
            broadcast("TYPING_START", {"channel_id": d["channel_id"], "user_id": USERS[d.get("author", "alice")]["id"],
                                       "timestamp": int(time.time())})
            return self.reply(200, {})
        if p.startswith("/api/v9/channels/") and p.endswith("/messages"):
            cid = parts[4]
            ctype = self.headers.get("Content-Type", "")
            attachments = []
            if ctype.startswith("multipart/form-data"):
                msgp = email.parser.BytesParser(policy=email.policy.HTTP).parsebytes(
                    b"Content-Type: " + ctype.encode() + b"\r\n\r\n" + raw)
                payload = {}
                for part in msgp.iter_parts():
                    name = part.get_param("name", header="content-disposition")
                    data = part.get_payload(decode=True)
                    if name == "payload_json":
                        payload = json.loads(data)
                    else:
                        fn = part.get_filename() or "file"
                        out = "up_" + snow() + "_" + fn
                        open(os.path.join(ARGS.media, out), "wb").write(data)
                        ct = part.get_content_type()
                        att = {"id": snow(), "filename": fn, "size": len(data), "url": media(out), "proxy_url": media(out),
                               "content_type": ct}
                        if ct.startswith("image/"):
                            try:
                                o = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                                    "stream=width,height", "-of", "csv=p=0", os.path.join(ARGS.media, out)],
                                                   capture_output=True, text=True).stdout.strip().split(",")
                                att["width"], att["height"] = int(o[0]), int(o[1])
                            except Exception:
                                pass
                        attachments.append(att)
            else:
                payload = json.loads(raw or b"{}")
            m = msg(cid, ME, payload.get("content", ""), int(time.time() * 1000), nonce=payload.get("nonce"),
                    attachments=attachments)
            ref = payload.get("message_reference")
            if ref:
                with LOCK:
                    orig = next((x for x in MESSAGES.get(cid, []) if x["id"] == ref["message_id"]), None)
                m["type"] = 19
                m["message_reference"] = ref
                m["referenced_message"] = orig
            with LOCK:
                MESSAGES.setdefault(cid, []).append(m)
            m2 = dict(m)
            if cid in (CH["general"], CH["linux"], CH["media"]):
                m2["guild_id"] = G1
            broadcast("MESSAGE_CREATE", m2)
            return self.reply(200, m)
        if p.endswith("/ack"):
            broadcast("MESSAGE_ACK", {"channel_id": parts[4], "message_id": parts[6], "version": 1})
            return self.reply(200, {"token": None})
        if p.endswith("/typing"):
            return self.reply(204)
        if p == "/api/v9/auth/login":
            d = json.loads(raw)
            if d.get("password") == "mockpass":
                return self.reply(200, {"mfa": True, "totp": True, "ticket": "t1", "user_id": ME["id"]})
            return self.reply(400, {"message": "Invalid Form Body", "code": 50035,
                                    "errors": {"login": {"_errors": [{"code": "INVALID_LOGIN", "message": "Login or password is invalid."}]}}})
        if p == "/api/v9/auth/mfa/totp":
            d = json.loads(raw)
            if d.get("code") == "123456":
                return self.reply(200, {"token": "mock"})
            return self.reply(400, {"message": "Invalid two-factor code", "code": 60008})
        self.reply(404, {"message": "404: Not Found", "code": 0})

    def do_PATCH(self):
        parts = self.path.split("?")[0].split("/")
        d = json.loads(self.body() or b"{}")
        if len(parts) >= 7 and parts[5] == "messages":
            cid, mid = parts[4], parts[6]
            with LOCK:
                for m in MESSAGES.get(cid, []):
                    if m["id"] == mid:
                        m["content"] = d.get("content", m["content"])
                        m["edited_timestamp"] = iso(int(time.time() * 1000))
                        broadcast("MESSAGE_UPDATE", m)
                        return self.reply(200, m)
        self.reply(404, {"message": "Unknown Message", "code": 10008})

    def do_DELETE(self):
        parts = self.path.split("?")[0].split("/")
        if len(parts) >= 7 and parts[5] == "messages" and len(parts) == 7:
            cid, mid = parts[4], parts[6]
            with LOCK:
                MESSAGES[cid] = [m for m in MESSAGES.get(cid, []) if m["id"] != mid]
            broadcast("MESSAGE_DELETE", {"id": mid, "channel_id": cid})
            return self.reply(204)
        if "reactions" in parts:
            return self.react(parts, False)
        self.reply(404, {"message": "404", "code": 0})

    def do_PUT(self):
        parts = self.path.split("?")[0].split("/")
        if "reactions" in parts:
            return self.react(parts, True)
        self.reply(404, {"message": "404", "code": 0})

    def react(self, parts, add):
        from urllib.parse import unquote
        cid, mid, emoji = parts[4], parts[6], unquote(parts[8])
        name, _, eid = emoji.partition(":")
        e = {"name": name, "id": eid or None}
        broadcast("MESSAGE_REACTION_ADD" if add else "MESSAGE_REACTION_REMOVE",
                  {"user_id": ME["id"], "channel_id": cid, "message_id": mid, "emoji": e})
        return self.reply(204)


def main():
    global ARGS, BASE
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--media", default="/tmp/dcord-mock-media")
    ap.add_argument("--echo", help="path to the voice_echo binary (enables voice)")
    ap.add_argument("--dave", help="path to the dave_sender binary (enables DAVE E2EE)")
    ARGS = ap.parse_args()
    BASE = "http://127.0.0.1:%d" % ARGS.port
    make_media(ARGS.media)
    for u in list(USERS.values()) + [ME]:
        u["avatar"] = u["avatar"]
    seed()
    if ARGS.echo:
        subprocess.Popen([ARGS.echo, str(ARGS.port + 1), VOICE_KEY.hex()] + (["--sfu"] if ARGS.dave else []))
    if ARGS.dave:
        VOICE["dave"] = subprocess.Popen([ARGS.dave, CH["lounge"]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    srv = ThreadingHTTPServer(("127.0.0.1", ARGS.port), Handler)
    srv.daemon_threads = True
    print("mock discord on", BASE, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
