#!/usr/bin/env python3
"""A local stand-in for Microsoft's software-download service, strict about the same
things the real one is (as documented by Fido): a session id must be whitelisted through
vlscppe /tags and the ov-df request/reply before the SKU lookup works, the link request
needs a Referer, and ISO downloads honour Range. Usage: mock_msdl.py PORTFILE LOGFILE [mode]
modes: ok | banned | flaky | httplink | http503"""
import hashlib, http.server, json, re, sys, urllib.parse, threading

portfile, logfile = sys.argv[1], sys.argv[2]
mode = sys.argv[3] if len(sys.argv) > 3 else "ok"
ISO = (hashlib.sha256(b"rufux-iso").digest() * 100000)[:3 * 1024 * 1024 + 123]   # 3 MiB and a bit
state = {}   # session -> {"tag": bool, "mdt": bool, "ov": bool}
flaky = {"n": 0}
# product edition id -> (arch type, languages)
LANGS = [("English", "English (United States)"), ("English International", "English International"),
         ("French", "French"), ("French Canadian", "French Canadian"), ("German", "German")]
EDITIONS = {"3321": 1, "3324": 2, "2618": 1, "2378": 1, "3322": 1, "3325": 2, "3323": 1, "3326": 2}
skus = {}    # sku id -> (edition arch type)

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def rec(self, s):
        open(logfile, "a").write(s + "\n")
    def send_json(self, o, code=200):
        b = json.dumps(o).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        u = urllib.parse.urlsplit(self.path); q = urllib.parse.parse_qs(u.query)
        g = lambda k: q.get(k, [""])[0]
        self.rec(f"GET {u.path} ua={self.headers.get('User-Agent','')[:20]} ref={self.headers.get('Referer','')} range={self.headers.get('Range','')}")
        if u.path == "/vlscppe/tags":
            if mode == "http503": return self.send_json({"err": "busy"}, 503)
            assert g("org_id") == "y6jn8c31"
            state.setdefault(g("session_id"), {})["tag"] = True
            self.send_response(200); self.send_header("Content-Length", "0"); self.send_header("Set-Cookie", "MC1=abc; Path=/"); self.end_headers(); return
        if u.path == "/ov-df/mdt.js":
            s = state.get(g("session_id"), {})
            if not s.get("tag") or g("instanceId") != "560dc9f3-1aa5-4a2f-b63c-9e18f8d0e175": return self.send_json({"err": "no tag"}, 403)
            s["mdt"] = True
            js = ('var a=function(){var u="https://ov-df.microsoft.com/?session_id=%s&CustomerId=x&PageId=si&w=A1B2C3D4E5F6&mdt=" + Date.now() + '
                  '"&rticks="+987654321;};' % g("session_id")).encode()
            self.send_response(200); self.send_header("Content-Length", str(len(js))); self.end_headers(); self.wfile.write(js); return
        if u.path == "/ov-df/":
            s = state.get(g("session_id"), {})
            if not s.get("mdt") or g("w") != "A1B2C3D4E5F6" or g("rticks") != "987654321" or not g("mdt").isdigit(): return self.send_json({"err": "bad ov-df"}, 403)
            s["ov"] = True
            self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers(); return
        if u.path == "/www/software-download-connector/api/getskuinformationbyproductedition":
            s = state.get(g("sessionID"), {})
            if not s.get("ov"):
                return self.send_json({"Errors": [{"Type": 0, "Value": "session not whitelisted"}]})
            if g("profile") != "606624d44113" or g("Locale") != "en-US": return self.send_json({"Errors": [{"Type": 0, "Value": "bad profile"}]})
            if mode == "flaky":
                flaky["n"] += 1
                if flaky["n"] == 1: return self.send_json({"Errors": [{"Type": 0, "Value": "try again"}]})
            pe = g("productEditionId"); arch = EDITIONS.get(pe)
            if arch is None: return self.send_json({"Errors": [{"Type": 0, "Value": "unknown edition"}]})
            out = []
            for i, (lang, disp) in enumerate(LANGS):
                sid = f"{pe}{i:02d}"; skus[sid] = (arch, lang); out.append({"Id": sid, "Language": lang, "LocalizedLanguage": disp})
            return self.send_json({"Skus": out})
        if u.path == "/www/software-download-connector/api/GetProductDownloadLinksBySku":
            s = state.get(g("sessionID"), {})
            if not s.get("ov") or self.headers.get("Referer") != "https://www.microsoft.com/software-download/windows11":
                return self.send_json({"Errors": [{"Type": 0, "Value": "denied"}]})
            if mode == "banned": return self.send_json({"Errors": [{"Type": 9, "Value": "715-123130"}]})
            if g("SKU") not in skus: return self.send_json({"Errors": [{"Type": 0, "Value": "unknown sku"}]})
            arch, lang = skus[g("SKU")]
            host = self.headers.get("Host")
            name = "Win11_25H2_%s_%s.iso" % (lang.replace(" ", ""), {1: "x64", 2: "Arm64"}[arch])
            uri = ("http://example.com/x/" + name) if mode == "httplink" else f"http://{host}/files/{name}?t=1&P1=2"
            return self.send_json({"ProductDownloadOptions": [{"Uri": uri, "DownloadType": arch}]})
        if u.path.startswith("/files/"):
            data = ISO; rng = self.headers.get("Range")
            if rng:
                m = re.match(r"bytes=(\d+)-", rng); start = int(m.group(1))
                self.send_response(206); self.send_header("Content-Range", f"bytes {start}-{len(data)-1}/{len(data)}"); body = data[start:]
            else:
                self.send_response(200); body = data
            self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body); return
        self.send_json({"err": "not found"}, 404)

srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
open(portfile, "w").write(str(srv.server_address[1]))
srv.serve_forever()
