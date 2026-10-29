"""Checks the Supabase setup using the values in firmware/gas_monitor/secrets.h.
Run from the repo root:  python tools/check_supabase.py

1. HTTPS to the project verifies with ONLY the root certificates the ESP32 has (tools/ca_certs.pem)
2. the public key can read `latest` and call `history`
3. the public key can NOT insert into `readings` directly
4. `ingest` rejects a wrong device key and accepts the right one
   (the test row is dated 2000-01-01, so it never shows up on the dashboard)"""
import json
import re
import ssl
import sys
import urllib.error
import urllib.request
from pathlib import Path

root = Path(__file__).resolve().parent.parent
secrets = (root / "firmware" / "gas_monitor" / "secrets.h").read_text(encoding="utf-8")
cfg = {k: v for k, v in re.findall(r'#define\s+(\w+)\s+"([^"]*)"', secrets)}
URL, KEY, DEVICE_KEY = cfg["SUPABASE_URL"].rstrip("/"), cfg["SUPABASE_ANON_KEY"], cfg["DEVICE_KEY"]
if "xxxxxxxx" in URL:
    sys.exit("Fill in SUPABASE_URL and SUPABASE_ANON_KEY in secrets.h first.")

esp32_ctx = ssl.create_default_context(cafile=str(root / "tools" / "ca_certs.pem"))
results = []


def call(path, body=None, ctx=esp32_ctx):
    headers = {"apikey": KEY, "Content-Type": "application/json"}
    if KEY.startswith("eyJ"):
        headers["Authorization"] = "Bearer " + KEY
    req = urllib.request.Request(URL + "/rest/v1/" + path, headers=headers, method="POST" if body is not None else "GET",
                                 data=json.dumps(body).encode() if body is not None else None)
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=20) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def check(name, ok, detail=""):
    results.append(ok)
    print(("PASS " if ok else "FAIL ") + name + (": " + detail if detail else ""))


try:
    code, body = call("latest?select=tank,ts")
    check("TLS verifies with the ESP32's root certificates", True)
except ssl.SSLError as e:
    check("TLS verifies with the ESP32's root certificates", False, str(e))
    code, body = call("latest?select=tank,ts", ctx=ssl.create_default_context())
check("anon can read latest", code == 200, "%d %s" % (code, body[:120]))

code, body = call("rpc/history", {"p_hours": 24, "p_bucket_min": 15})
check("anon can call history", code == 200, "%d, %d buckets" % (code, len(json.loads(body)) if code == 200 else -1))

code, body = call("readings", [{"tank": 1, "ts": "2000-01-01T00:00:00Z"}])
check("anon can NOT insert into readings", code in (401, 403), "%d %s" % (code, body[:120]))

row = [{"ts": "2000-01-01T00:00:00Z", "tank": 1, "level_mm": None, "quality": None, "samples": 0}]
code, body = call("rpc/ingest", {"p_key": "wrong-key", "p_rows": row})
check("ingest rejects a wrong device key", code >= 400, "%d %s" % (code, body[:120]))

code, body = call("rpc/ingest", {"p_key": DEVICE_KEY, "p_rows": row})
check("ingest accepts the device key", code == 200,
      "%d %s" % (code, body[:120]) + ("" if code == 200 else " (did you run supabase/add_device_key.local.sql?)"))

print("\nAll checks passed." if all(results) else "\nSome checks FAILED.")
sys.exit(0 if all(results) else 1)
