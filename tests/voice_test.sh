#!/bin/sh
# End-to-end voice test without Discord: the mock server, the UDP SFU and two
# headless clients (dcord-probe) in one voice channel with DAVE E2EE. Each
# client sends a 440 Hz tone (no microphone, no audio output) and must receive
# the other's tone at -13.5 dB (the RMS of a 0.3-amplitude sine).
#   tests/voice_test.sh [BUILD_DIR]
set -eu
B=$(cd "${1:-build}" && pwd)
T=$(mktemp -d)
cd "$(dirname "$0")/.."
python3 tests/mock_discord.py --port 8790 --media "$T/media" --echo "$B/voice_echo" --dave "$B/dave_sender" > "$T/mock.log" 2>&1 &
MOCK=$!
trap 'set +e; kill $MOCK 2>/dev/null; pkill -x voice_echo 2>/dev/null; pkill -x dave_sender 2>/dev/null; rm -rf "$T"; true' EXIT
# wait for the mock (it renders its test images with ffmpeg on first start)
i=0
until curl -s -o /dev/null http://127.0.0.1:8790/app; do
	i=$((i + 1)); [ $i -lt 120 ] || { echo "mock did not start"; cat "$T/mock.log"; exit 1; }
	sleep 0.5
done
run() {
	env DCORD_API_BASE=http://127.0.0.1:8790/api/v9 DCORD_GATEWAY=ws://127.0.0.1:8790/gateway \
	    DCORD_APP_URL=http://127.0.0.1:8790/app DCORD_CDN=http://127.0.0.1:8790 \
	    DCORD_TEST_TONE=1 DCORD_NO_AUDIO=1 XDG_CONFIG_HOME="$T/$1c" XDG_CACHE_HOME="$T/$1k" DCORD_TOKEN="$2" \
	    "$B/dcord-probe" voice 200000000000000001 300000000000000006 "$3" > "$T/$1.log" 2>&1
}
run a mock 10 &
A=$!
sleep 2
run b mock2 7
wait $A
ok=0
for c in a b; do
	if grep -q "e2ee 1 dave v1" "$T/$c.log" && grep -q "level -13.5 dB" "$T/$c.log"; then
		echo "client $c: E2EE ready, received the other tone at -13.5 dB"
	else
		echo "client $c: FAILED"; tail -20 "$T/$c.log"; ok=1
	fi
done
if grep -q "INVALID\|DECRYPT FAILED" "$T/mock.log" || ! grep -q "sfu: .* 0 bad" "$T/mock.log"; then
	echo "server saw bad packets:"; grep -E "sfu|dave|INVALID|FAILED" "$T/mock.log" | tail; ok=1
else
	grep "sfu:" "$T/mock.log" | tail -1
fi
exit $ok
