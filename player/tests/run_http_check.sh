#!/bin/sh
set -eu

decoder=${1:-./mr_decode}
mode=${2:-http}
tmpdir=$(mktemp -d)
server_pid=

cleanup()
{
    if test -n "$server_pid"; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$tmpdir" tests/assets/hls tests/assets/no-zero-range.ts
}
trap cleanup EXIT INT TERM

# Derive an HLS VOD fixture by slicing the MPEG-TS test asset into TS-packet-
# aligned segments; concatenated, they are the original stream, so the HLS
# source must decode identically to it.
python3 - <<'PY'
import os
d=open('tests/assets/test_mpeg2.ts','rb').read(); PKT=188; n=len(d)//PKT
os.makedirs('tests/assets/hls',exist_ok=True)
open('tests/assets/no-zero-range.ts','wb').write(d)
b1=(n//3)*PKT; b2=(2*n//3)*PKT; parts=[d[:b1],d[b1:b2],d[b2:]]
m=["#EXTM3U","#EXT-X-VERSION:3","#EXT-X-TARGETDURATION:2","#EXT-X-MEDIA-SEQUENCE:0"]
for i,p in enumerate(parts):
    open(f'tests/assets/hls/seg{i}.ts','wb').write(p); m+=["#EXTINF:2.0,",f"seg{i}.ts"]
m.append("#EXT-X-ENDLIST")
open('tests/assets/hls/media.m3u8','w').write("\n".join(m)+"\n")
open('tests/assets/hls/master.m3u8','w').write(
 "#EXTM3U\n"
 "#EXT-X-STREAM-INF:BANDWIDTH=2400000,RESOLUTION=640x360,"
 "CODECS=\"av01.0.05M.08,mp4a.40.2\"\nmissing-av1.m3u8\n"
 "#EXT-X-STREAM-INF:BANDWIDTH=100000\nmissing.m3u8\n"
 "#EXT-X-STREAM-INF:BANDWIDTH=1200000,RESOLUTION=640x360,"
 "CODECS=\"avc1.4d401e,mp4a.40.2\"\nmedia.m3u8\n")
open('tests/assets/hls/youtube.html','w').write(
 '{"hlsManifestUrl":"https:\\/\\/manifest.googlevideo.com\\/api\\/'
 'manifest\\/hls_variant\\/file\\/index.m3u8?x=1\\u0026y=2"}')
# Also a 6-way split for the live playlist: the fixture server hands these out
# through a growing, sliding EXT-X-MEDIA-SEQUENCE window (see http_fixture_server
# /live/), so the client must re-fetch to follow segments lseg0..lseg5. In order
# they concatenate back to the whole stream, matching the same reference.
LN=6; step=n//LN
for i in range(LN):
    a=i*step*PKT; b=(len(d) if i==LN-1 else (i+1)*step*PKT)
    open(f'tests/assets/hls/lseg{i}.ts','wb').write(d[a:b])
PY

server_args="--root tests/assets --port-file $tmpdir/port --range-marker $tmpdir/range-used --header-log $tmpdir/headers"
scheme=http
if test "$mode" = https; then
    scheme=https
    openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
        -subj /CN=localhost \
        -keyout "$tmpdir/key.pem" -out "$tmpdir/cert.pem" \
        >/dev/null 2>&1
    server_args="$server_args --cert $tmpdir/cert.pem --key $tmpdir/key.pem"
fi

python3 tests/http_fixture_server.py $server_args \
    >"$tmpdir/server.log" 2>&1 &
server_pid=$!

tries=0
while test ! -s "$tmpdir/port"; do
    tries=$((tries + 1))
    if test "$tries" -ge 100; then
        cat "$tmpdir/server.log"
        exit 1
    fi
    sleep 0.05
done
port=$(cat "$tmpdir/port")
base="$scheme://127.0.0.1:$port"

# The YouTube resolver's bounded text downloader accepts both a regular body
# and chunked transfer encoding. HTTPS uses the separately linked SSL harness;
# this parser/downloader target is intentionally the plain host build.
if test "$mode" = http; then
    ./mr_source_buffer_check "$base/media/test_mpeg2.ts"
    ./mr_youtube_check "$base/media/hls/youtube.html"
    ./mr_youtube_check "$base/chunked/media/hls/youtube.html"
    long_token=$(awk 'BEGIN { for (i=0; i<1500; i++) printf "x" }')
    ./mr_youtube_check "$base/media/hls/youtube.html?token=$long_token"
    ./mr_youtube_check --post "$base/youtubei/v1/player"
fi

"$decoder" "$base/media/test_mpeg2.ts" \
    --check tests/assets/ref_mpeg2_ts
# Some CDNs reject an unnecessary open-ended Range on the initial request.
# A normal offset-zero GET must be used; later nonzero reconnects still Range.
"$decoder" "$base/media/no-zero-range.ts" \
    --check tests/assets/ref_mpeg2_ts
"$decoder" "$base/redirect/test_mpeg2.ts" \
    --check tests/assets/ref_mpeg2_ts
"$decoder" "$base/media/test_h264_high.mp4" \
    --check tests/assets/ref_h264_high
"$decoder" "$base/media/test_mp42.avi" \
    --check tests/assets/ref_mp42
"$decoder" "$base/chunked/media/test_mpeg2.ts" \
    --check tests/assets/ref_mpeg2_ts
"$decoder" "$base/chunked/redirect/test_mpeg2.ts" \
    --check tests/assets/ref_mpeg2_ts
"$decoder" "$base/chunked/media/test_h264_high.mp4" \
    --check tests/assets/ref_h264_high
"$decoder" "$base/chunked/media/test_mp42.avi" \
    --check tests/assets/ref_mp42

"$decoder" "$base/chunked-head/media/test_mpeg2.ts" \
    --check tests/assets/ref_mpeg2_ts
"$decoder" "$base/chunked-head/media/test_h264_high.mp4" \
    --check tests/assets/ref_h264_high

# Length-less forward-only stream: no Content-Length, Range ignored. Only the
# sequential MPEG-TS path can play this; the frames must still match exactly.
"$decoder" "$base/stream/media/test_mpeg2.ts" \
    --check tests/assets/ref_mpeg2_ts
"$decoder" "$base/stream/redirect/test_mpeg2.ts" \
    --check tests/assets/ref_mpeg2_ts

# Seekable server that drops each transfer after a small cap: the client must
# reconnect with a Range at a non-zero offset to finish (the byte-range resume
# path). The frames still match, and this is what touches the range marker
# asserted below - read-ahead now caches whole small files, so a plain
# sequential fetch never needs a mid-file Range on its own.
"$decoder" "$base/drop/media/test_h264_high.mp4" \
    --check tests/assets/ref_h264_high
"$decoder" "$base/drop/media/test_mpeg2.ts" \
    --check tests/assets/ref_mpeg2_ts

# HLS VOD: the media playlist's segments concatenate back to the TS asset, and a
# master playlist must resolve to the sole reachable variant. Both decode to the
# same reference frames.
"$decoder" "$base/media/hls/media.m3u8" \
    --check tests/assets/ref_mpeg2_ts
"$decoder" "$base/media/hls/master.m3u8" \
    --check tests/assets/ref_mpeg2_ts

# A CDN may use chunked transfer for both playlists and segments while refusing
# HEAD/range length probes. Buffering each bounded segment must still expose a
# seekable concatenated source to the MPEG-TS demuxer.
"$decoder" "$base/stream/media/hls/master.m3u8" \
    --hls-buffer-segments --check tests/assets/ref_mpeg2_ts

# Live HLS: a sliding-window playlist with no EXT-X-ENDLIST that the server
# grows one segment per poll (advancing EXT-X-MEDIA-SEQUENCE) until it caps and
# adds ENDLIST. The client must re-fetch to follow lseg0..lseg5 and dedupe by
# media sequence; concatenated they are the whole stream, so frames still match.
"$decoder" "$base/live/playlist.m3u8" \
    --check tests/assets/ref_mpeg2_ts

test -f "$tmpdir/range-used"

# Typed IPTV headers must survive redirects, range reconnects, HLS master and
# child playlists, and every HLS segment. The first request also proves the
# default UA and absence of an implicit Referer.
: >"$tmpdir/headers"
"$decoder" "$base/media/test_mpeg2.ts" --check tests/assets/ref_mpeg2_ts
grep -F '|MintVID/0.1 AmigaOS|' "$tmpdir/headers" >/dev/null
! grep -v -F '|MintVID/0.1 AmigaOS|' "$tmpdir/headers" >/dev/null

: >"$tmpdir/headers"
"$decoder" "$base/media/test_mpeg2.ts" --user-agent 'IPTV Agent With Spaces' \
    --check tests/assets/ref_mpeg2_ts
! grep -v -F '|IPTV Agent With Spaces|' "$tmpdir/headers" >/dev/null

: >"$tmpdir/headers"
"$decoder" "$base/media/test_mpeg2.ts" --referer 'https://example.com/' \
    --check tests/assets/ref_mpeg2_ts
! grep -v -F '|MintVID/0.1 AmigaOS|https://example.com/' \
    "$tmpdir/headers" >/dev/null

: >"$tmpdir/headers"
ua='Mozilla/5.0 MintVID header test'
ref='https://example.com/player?channel=one&mode=live'
"$decoder" "$base/redirect/test_mpeg2.ts" \
    --user-agent "$ua" --referer "$ref" \
    --check tests/assets/ref_mpeg2_ts
"$decoder" "$base/drop/media/test_mpeg2.ts" \
    --user-agent "$ua" --referer "$ref" \
    --check tests/assets/ref_mpeg2_ts
"$decoder" "$base/media/hls/master.m3u8" \
    --user-agent "$ua" --referer "$ref" \
    --check tests/assets/ref_mpeg2_ts
grep -F "|$ua|$ref" "$tmpdir/headers" >/dev/null
! grep -v -F "|$ua|$ref" "$tmpdir/headers" >/dev/null

# Newlines and fixed-buffer overflows are rejected before a request is sent.
if "$decoder" "$base/media/test_mpeg2.ts" --user-agent "bad$(printf '\r')value" \
    --check tests/assets/ref_mpeg2_ts >/dev/null 2>&1; then
    echo 'CR header injection was accepted' >&2; exit 1
fi
overlong=$(awk 'BEGIN { for (i=0; i<256; i++) printf "x" }')
if "$decoder" "$base/media/test_mpeg2.ts" --user-agent "$overlong" \
    --check tests/assets/ref_mpeg2_ts >/dev/null 2>&1; then
    echo 'overlong User-Agent was accepted' >&2; exit 1
fi
echo "$mode URL checks passed"
