#!/usr/bin/env python3
"""Fake Xtream Codes server for testing sit.

    python3 test/fakextream.py [port] [video.mp4]

Log in as user "demo", password "s3cr/t!" (the special characters check
url encoding). The JSON is deliberately sloppy the way real panels are:
ids as strings or numbers, null and missing categories, category_ids
arrays, escaped slashes and unicode. Live streams redirect to public
test streams; movies and episodes all serve video.mp4 (with ranges, so
seeking works), e.g. one made by
    ffmpeg -f lavfi -i testsrc2=size=640x360:duration=180 \
        -f lavfi -i sine=duration=180 -movflags +faststart video.mp4
Posters are generated BMPs. Every channel has a guide from yesterday to
the day after tomorrow (base64 titles, plain text on every fifth
channel); channels with archive serve their past programmes through
/timeshift/ urls.
"""
import base64
import datetime
import json
import os
import struct
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

USER, PASS = "demo", "s3cr/t!"
SCALE = int(os.environ.get("FAKE_SCALE", "1"))   # bigger lists for memory tests
# real panels redirect raw TS; these are HLS behind a .ts url, so ffmpeg
# resolves relative segment urls against ours. Only use playlists whose
# segment urls survive that.
LIVE = [
    "https://demo.unified-streaming.com/k8s/live/stable/live.isml/.m3u8",
]

CATS = [
    {"category_id": "1", "category_name": "News", "parent_id": 0},
    {"category_id": 2, "category_name": "Sports", "parent_id": 0},
    {"category_id": "3", "category_name": "Movies é Séries", "parent_id": 0},
    {"category_id": "4", "category_name": "Kids \U0001F3A8", "parent_id": 0},
    {"category_id": "5", "category_name": "Empty group", "parent_id": 0},
    {"category_id": "6", "category_name": "International / World", "parent_id": 0},
    {"category_id": "8", "category_name": "عربي / Arabic", "parent_id": 0},
    {"category_id": "7", "category_name": "Россия / Ελλάδα", "parent_id": 0},
]

def streams():
    out = []
    names = ["Daily News", "World Report", "Sports One", "Goal TV", "Cinema",
             "Séries Plus", "Cartoon Box", "Kids Planet", "Al Jazeera-ish", "NHK-ish"]
    cats = ["1", 1, "2", 2, "3", "3", "4", "4", "6", "6"]
    for i in range(1200 * SCALE):
        sid = 1000 + i
        s = {
            "num": i + 1,
            "name": "%s %d" % (names[i % 10], i // 10 + 1),
            "stream_type": "live",
            "stream_id": sid if i % 2 else str(sid),
            "stream_icon": "",
            "epg_channel_id": None if i % 3 else "ch%d.example" % i,
            "added": "1700000000",
            "category_id": cats[i % 10],
            "tv_archive": i % 4 == 0,
            "tv_archive_duration": "3" if i % 4 == 0 else 0,
        }
        if i % 50 == 13:   # right-to-left names, mixed with Latin and digits
            s["name"] = ["قناة الجزيرة HD", "MBC 1 مصر", "العربية (عاجل)",
                         "شبکه سه", "כאן 11", "لا إله"][i // 50 % 6]
            s["category_id"] = "8"
        if i % 50 == 7:
            s["name"] = "Первый канал %d" % i
            s["category_id"] = "7"
        if i % 97 == 0:
            s["category_id"] = None           # -> Uncategorized
        if i % 89 == 0:
            del s["category_id"]
            s["category_ids"] = [6]
        out.append(s)
    out.append({"name": "", "stream_id": 9999})          # skipped: no name
    out.append({"name": "No id", "stream_id": None})     # skipped: no id
    return out

BASE = "http://127.0.0.1:%d" % (int(sys.argv[1]) if len(sys.argv) > 1 else 8099)
VIDEO = sys.argv[2] if len(sys.argv) > 2 else None

PLOT = ("A retired cartographer is pulled back into one last survey when a "
        "map of a coastline that does not exist turns up in her late "
        "brother's papers. What starts as a favour becomes a race across "
        "three countries, two languages and one very stubborn lighthouse "
        "keeper who knows more than he lets on. ") * 2

VOD_CATS = [
    {"category_id": "10", "category_name": "Drama", "parent_id": 0},
    {"category_id": 11, "category_name": "Comedy", "parent_id": 0},
    {"category_id": "12", "category_name": "Documentary", "parent_id": 0},
]
TITLES = ["The Long Coast", "Paper Lanterns", "Night Ferry", "Saltwater",
          "Odd Numbers", "Ten Small Rooms", "Glasswork", "The Survey"]


def vod():
    out = []
    for i in range(300 * SCALE):
        sid = 20000 + i
        if i % 11 == 5:
            icon = ""                          # -> initials
        elif i % 13 == 7:
            icon = BASE + "/nope.jpg"          # -> 404, initials
        else:
            icon = BASE + "/img/%d.bmp" % i
        out.append({
            "num": i + 1,
            "name": "%s %d (%d)" % (TITLES[i % 8], i // 8 + 1, 1990 + i % 35),
            "stream_type": "movie",
            "stream_id": sid if i % 2 else str(sid),
            "stream_icon": icon,
            "rating": "%.1f" % (5 + (i * 7) % 50 / 10) if i % 4 else "",
            "added": "1700000000",
            "category_id": ["10", 11, "12"][i % 3],
            "container_extension": "mp4",
        })
    return out


def vodinfo(sid):
    i = sid - 20000
    if i % 9 == 4:
        return {"info": [], "movie_data": {"stream_id": sid}}   # panels do this
    return {"info": {
        "plot": PLOT, "genre": ["Drama, Mystery", "Comedy", "Documentary"][i % 3],
        "releasedate": "%d-05-01" % (1990 + i % 35), "duration_secs": 6720,
        "duration": "01:52:00", "cast": "Ana Ruiz, Tom Okafor, Mei Lin",
        "director": "J. Doe", "backdrop_path": [BASE + "/img/b%d.bmp" % i]},
        "movie_data": {"stream_id": sid, "container_extension": "mp4"}}


SERIES_CATS = [
    {"category_id": "20", "category_name": "Crime", "parent_id": 0},
    {"category_id": "21", "category_name": "Sci-fi", "parent_id": 0},
]
SHOWS = ["Harbour Street", "Orbitals", "Quiet Town"]


def series():
    out = []
    for i in range(40 * SCALE):
        out.append({
            "num": i + 1, "name": "%s %d" % (SHOWS[i % 3], i + 1),
            "series_id": 30000 + i, "cover": BASE + "/img/s%d.bmp" % i,
            "plot": PLOT[:220], "cast": "Sam Reed, Ola Berg",
            "genre": ["Crime", "Sci-fi"][i % 2],
            "releaseDate": "%d-01-10" % (2005 + i % 18), "rating": "%.1f" % (6 + i % 4),
            "backdrop_path": [BASE + "/img/sb%d.bmp" % i],
            "category_id": ["20", "21"][i % 2],
        })
    return out


def seriesinfo(sid):
    i = sid - 30000
    name = "%s %d" % (SHOWS[i % 3], i + 1)
    seasons = {}
    for s in range(1, 4):
        seasons[str(s)] = [{
            "id": str(40000 + i * 100 + s * 10 + e), "episode_num": e,
            "title": "%s - S%02dE%02d - Chapter %d" % (name, s, e, e),
            "container_extension": "mp4", "season": s,
            "info": {"duration_secs": 2580 + e * 60,
                     "plot": "Episode %d of season %d. " % (e, s) + PLOT[:160]},
        } for e in range(1, 7 + s)]
    eps = seasons if i % 2 == 0 else list(seasons.values())   # both shapes exist
    return {"seasons": [{"season_number": s} for s in range(1, 4)],
            "info": {"name": name, "cover": BASE + "/img/s%d.bmp" % i, "plot": PLOT,
                     "genre": "Crime", "releaseDate": "2011-01-10",
                     "cast": "Sam Reed, Ola Berg",
                     "backdrop_path": [BASE + "/img/sb%d.bmp" % i]},
            "episodes": eps}


EPG = [("News", "The headlines and the weather."),
       ("The Long Interview", "An hour with someone who has done something."),
       ("Weather", "Where the rain is going."),
       ("World Today", "Reports from four continents."),
       ("Documentary: Islands", "Life on islands too small for maps."),
       ("Late Film: Night Ferry", "A crossing that takes longer than planned.")]
DURS = [30, 60, 45, 90, 30, 120]


def programmes(sid):
    """(start, stop, title, desc) from yesterday 00:00, local time"""
    t = datetime.datetime.now().replace(hour=0, minute=0, second=0, microsecond=0)
    t -= datetime.timedelta(days=1)
    end = t + datetime.timedelta(days=3)
    out, k = [], sid % 6
    while t < end:
        stop = t + datetime.timedelta(minutes=DURS[k % 6])
        out.append((t, stop) + EPG[k % 6])
        t, k = stop, k + 1
    return out


def listing(sid, start, stop, title, desc, full):
    now = time.time()
    if sid % 5 == 0:
        enc = lambda x: x                       # some panels send plain text
    else:
        enc = lambda x: base64.b64encode(x.encode()).decode()
    e = {"id": "1", "epg_id": "1", "title": enc(title), "lang": "en",
         "start": start.strftime("%Y-%m-%d %H:%M:%S"),
         "end": stop.strftime("%Y-%m-%d %H:%M:%S"),
         "description": enc(desc), "channel_id": "ch%d.example" % sid,
         "start_timestamp": str(int(start.timestamp())),
         "stop_timestamp": str(int(stop.timestamp()))}
    if full:
        e["now_playing"] = int(start.timestamp() <= now < stop.timestamp())
        e["has_archive"] = int(sid % 4 == 0 and stop.timestamp() < now)
    return e


def shortepg(sid, limit):
    now = time.time()
    progs = [p for p in programmes(sid) if p[1].timestamp() > now][:limit]
    return {"epg_listings": [listing(sid, *p, full=False) for p in progs]}


def datatable(sid):
    return {"epg_listings": [listing(sid, *p, full=True) for p in programmes(sid)]}


def bmp(seed, w, h):
    """a diagonal two-colour gradient, different per seed"""
    a = ((seed * 67) % 200 + 30, (seed * 131) % 200 + 30, (seed * 29) % 200 + 30)
    b = (a[2] // 3, a[0] // 3, a[1] // 3)
    row = (w * 3 + 3) & ~3
    px = bytearray()
    for y in range(h):
        line = bytearray()
        for x in range(w):
            t = (x / w + (h - 1 - y) / h) / 2
            line += bytes(int(b[2 - k] + (a[2 - k] - b[2 - k]) * t) for k in range(3))
        px += line + b"\0" * (row - w * 3)
    return (b"BM" + struct.pack("<IHHI", 54 + len(px), 0, 0, 54) +
            struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(px), 2835, 2835, 0, 0) +
            bytes(px))


STREAMS = json.dumps(streams())
# real panels escape slashes
STREAMS = STREAMS.replace("/", "\\/")


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("fakextream: " + (fmt % args) + "\n")

    def video(self):
        size = os.path.getsize(VIDEO)
        start, end = 0, size - 1
        rng = self.headers.get("Range", "")
        if rng.startswith("bytes="):
            a, _, b = rng[6:].partition("-")
            start = int(a or 0)
            end = min(int(b), size - 1) if b else size - 1
        self.send_response(206 if rng else 200)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        if rng:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        with open(VIDEO, "rb") as f:
            f.seek(start)
            left = end - start + 1
            try:
                while left > 0:
                    chunk = f.read(min(65536, left))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    left -= len(chunk)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass

    def send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/player_api.php":
            if q.get("username") != USER or q.get("password") != PASS:
                return self.send(200, json.dumps({"user_info": {"auth": 0}}))
            action = q.get("action")
            if not action:
                return self.send(200, json.dumps({
                    "user_info": {
                        "username": USER, "password": PASS, "message": "",
                        "auth": 1, "status": "Active",
                        "exp_date": "1803945600", "is_trial": "0",
                        "active_cons": "0", "max_connections": "2",
                        "allowed_output_formats": ["m3u8", "ts"],
                    },
                    "server_info": {"url": "127.0.0.1", "port": "8099",
                                    "timezone": "UTC", "timestamp_now": int(time.time())},
                }))
            if action == "get_live_categories":
                return self.send(200, json.dumps(CATS))
            if action == "get_live_streams":
                time.sleep(1)   # make the loading state visible
                return self.send(200, STREAMS)
            if action == "get_vod_categories":
                return self.send(200, json.dumps(VOD_CATS))
            if action == "get_vod_streams":
                return self.send(200, json.dumps(vod()).replace("/", "\\/"))
            if action == "get_vod_info":
                return self.send(200, json.dumps(vodinfo(int(q.get("vod_id", "0")))))
            if action == "get_series_categories":
                return self.send(200, json.dumps(SERIES_CATS))
            if action == "get_series":
                return self.send(200, json.dumps(series()))
            if action == "get_short_epg":
                return self.send(200, json.dumps(shortepg(int(q.get("stream_id", "0")),
                                                          int(q.get("limit", "4")))))
            if action == "get_simple_data_table":
                return self.send(200, json.dumps(datatable(int(q.get("stream_id", "0")))))
            if action == "get_series_info":
                return self.send(200, json.dumps(seriesinfo(int(q.get("series_id", "0")))))
            return self.send(200, "[]")
        if u.path.startswith("/img/"):
            name = u.path[5:-4]
            seed = int("".join(c for c in name if c.isdigit()) or 0)
            wide = name.startswith(("b", "sb"))
            img = bmp(seed + (500 if wide else 0), *((480, 270) if wide else (200, 300)))
            return self.send(200, img, "image/bmp")
        parts = [unquote(p) for p in u.path.split("/")]
        # /timeshift/user/pass/minutes/YYYY-MM-DD:HH-MM/id.ts
        if len(parts) == 7 and parts[1] == "timeshift":
            if parts[2] != USER or parts[3] != PASS or not VIDEO:
                return self.send(403, "forbidden", "text/plain")
            sys.stderr.write("fakextream: timeshift %s min from %s of %s\n" % (parts[4], parts[5], parts[6]))
            return self.video()
        if len(parts) == 5 and parts[1] in ("movie", "series"):
            if parts[2] != USER or parts[3] != PASS or not VIDEO:
                return self.send(403, "forbidden", "text/plain")
            return self.video()
        # /live/user/pass/id.ext
        if len(parts) == 5 and parts[1] == "live":
            if parts[2] != USER or parts[3] != PASS:
                return self.send(403, "forbidden", "text/plain")
            try:
                sid = int(parts[4].split(".")[0])
            except ValueError:
                return self.send(404, "not found", "text/plain")
            self.send_response(302)
            self.send_header("Location", LIVE[sid % len(LIVE)])
            self.end_headers()
            return
        self.send(404, "not found", "text/plain")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
