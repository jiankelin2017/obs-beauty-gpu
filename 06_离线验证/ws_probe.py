#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""obs-websocket v5 probe: list scene/source/filters and grab a GPU-rendered frame.

Why this exists: the plugin's real behaviour can only be measured on the GPU
chain that OBS actually runs.  Screen/GUI automation is blocked in this
session, but OBS ships obs-websocket, and GetSourceScreenshot returns what the
filter chain produced (filters are part of the source render).  So one can do
A/B on the live engine without touching the UI.

Usage:
  python ws_probe.py info
  python ws_probe.py shot  <source> <out.png> [w] [h]
  python ws_probe.py setf  <source> "filter" key=val [key=val ...]
  python ws_probe.py enable <source> "filter" 0|1
"""
import base64
import json
import os
import sys

# The shell here exports HTTP_PROXY=127.0.0.1:4433, so a loopback websocket
# would otherwise be tunnelled through the proxy and fail with 502.
for _k in ("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy"):
    os.environ.pop(_k, None)
os.environ["NO_PROXY"] = os.environ["no_proxy"] = "127.0.0.1,localhost"

import websocket  # noqa: E402  (must come after the proxy scrub)

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HOST = "127.0.0.1:4455"


class OBS:
    def __init__(self):
        self.ws = websocket.create_connection(
            "ws://" + HOST, timeout=15,
            http_no_proxy=["127.0.0.1", "localhost"])
        hello = json.loads(self.ws.recv())
        assert hello["op"] == 0, hello
        ident = {"op": 1, "d": {"rpcVersion": 1}}
        self.ws.send(json.dumps(ident))
        # op=2 is Identified
        r = json.loads(self.ws.recv())
        if r["op"] != 2:
            raise RuntimeError("identify failed: %r" % (r,))
        self.i = 0

    def call(self, req, **kw):
        self.i += 1
        self.ws.send(json.dumps({"op": 6, "d": {
            "requestType": req, "requestId": str(self.i), "requestData": kw}}))
        while True:
            m = json.loads(self.ws.recv())
            if m["op"] != 7:
                continue
            d = m["d"]
            if d["requestId"] != str(self.i):
                continue
            st = d["requestStatus"]
            if not st["result"]:
                raise RuntimeError("%s failed: %s (code %s)" %
                                   (req, st.get("comment"), st.get("code")))
            return d.get("responseData", {})

    def close(self):
        try:
            self.ws.close()
        except Exception:
            pass


def cmd_info(o):
    v = o.call("GetVersion")
    print("obs        :", v.get("obsVersion"), "ws", v.get("obsWebSocketVersion"))
    sc = o.call("GetSceneList")
    print("current    :", sc.get("currentProgramSceneName"))
    for s in sc.get("scenes", []):
        print("scene      :", s.get("sceneName"))
    print("--- inputs")
    for inp in o.call("GetInputList").get("inputs", []):
        name, kind = inp.get("inputName"), inp.get("inputKind")
        print("  %-24s %s" % (name, kind))
        try:
            fl = o.call("GetSourceFilterList", sourceName=name).get("filters", [])
        except Exception as e:
            print("     filters? ", e)
            continue
        for f in fl:
            print("     filter: %-10s enabled=%s kind=%s" %
                  (f.get("filterName"), f.get("filterEnabled"), f.get("filterKind")))
            if "beauty" in str(f.get("filterKind", "")):
                st = o.call("GetSourceFilter", sourceName=name,
                            filterName=f["filterName"])["filterSettings"]
                print("     settings:", json.dumps(st, ensure_ascii=False))


def cmd_shot(o, src, out, w=0, h=0):
    kw = {"sourceName": src, "imageFormat": "png"}
    if w:
        kw["imageWidth"] = int(w)
    if h:
        kw["imageHeight"] = int(h)
    d = o.call("GetSourceScreenshot", **kw)
    raw = base64.b64decode(d["imageData"].split(",", 1)[-1])
    open(out, "wb").write(raw)
    print("saved", out, len(raw), "bytes")


def cmd_setf(o, src, filt, pairs):
    st = o.call("GetSourceFilter", sourceName=src, filterName=filt)["filterSettings"]
    for p in pairs:
        k, v = p.split("=", 1)
        st[k] = float(v)
    o.call("SetSourceFilterSettings", sourceName=src, filterName=filt,
           filterSettings=st, overlay=True)
    print("settings ->", json.dumps({k: st[k] for k in st if k in
                                     ("blur_level", "sharpen", "global_sharpen",
                                      "delspot_level", "chin", "thin_face",
                                      "big_eye")}, ensure_ascii=False))


def cmd_enable(o, src, filt, on):
    o.call("SetSourceFilterEnabled", sourceName=src, filterName=filt,
           filterEnabled=(on == "1"))
    print("enabled =", on)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    o = OBS()
    try:
        c = a[0]
        if c == "info":
            cmd_info(o)
        elif c == "shot":
            cmd_shot(o, a[1], a[2], a[3] if len(a) > 3 else 0,
                     a[4] if len(a) > 4 else 0)
        elif c == "setf":
            cmd_setf(o, a[1], a[2], a[3:])
        elif c == "enable":
            cmd_enable(o, a[1], a[2], a[3])
        else:
            print("unknown cmd", c)
            return 2
    finally:
        o.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
