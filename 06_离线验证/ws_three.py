#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""A/B the three sliders on the REAL GPU chain, using a still image source.

Why a still image: the camera points at the ceiling right now, so no face is
in frame.  A still source with the known golden face lets us
  1) exercise the real obs-beauty GPU chain (same filter kind, same shaders),
  2) compare it pixel-by-pixel against the CPU offline台 output for the same
     input, which is the only way to prove GPU and CPU did not diverge.

Usage: python ws_three.py
Writes _gpu_<tag>.png for each case.
"""
import base64
import json
import os
import sys
import time

for _k in ("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy"):
    os.environ.pop(_k, None)
os.environ["NO_PROXY"] = os.environ["no_proxy"] = "127.0.0.1,localhost"

import websocket  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = "ws_three_src"
IMG = os.path.join(HERE, "_wssrc_face.png")
FILT = "OBS美颜_ws"


class OBS:
    def __init__(self):
        self.ws = websocket.create_connection(
            "ws://127.0.0.1:4455", timeout=20,
            http_no_proxy=["127.0.0.1", "localhost"])
        h = json.loads(self.ws.recv())
        assert h["op"] == 0, h
        self.ws.send(json.dumps({"op": 1, "d": {"rpcVersion": 1}}))
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
                raise RuntimeError("%s: %s (code %s)" %
                                   (req, st.get("comment"), st.get("code")))
            return d.get("responseData", {})

    def shot(self, name, out):
        d = self.call("GetSourceScreenshot", sourceName=name,
                      imageFormat="png")
        raw = base64.b64decode(d["imageData"].split(",", 1)[-1])
        open(out, "wb").write(raw)
        return len(raw)


def ensure_source(o, scene):
    try:
        o.call("CreateInput", inputName=SRC, inputKind="image_source",
               sceneName=scene, inputSettings={"file": IMG},
               sceneItemEnabled=True)
        print("source created")
    except RuntimeError as e:
        if "already" in str(e).lower() or "exists" in str(e).lower() or "601" in str(e):
            print("source exists, reuse")
        else:
            raise
    try:
        o.call("CreateSourceFilter", sourceName=SRC, filterName=FILT,
               filterKind="obs_beauty_filter", filterSettings={})
        print("filter created")
    except RuntimeError as e:
        print("filter reuse:", e)


BASE = {"blur_level": 0.0, "clarity": 0.0, "color_level": 0.0,
        "red_level": 0.0, "sharpen": 0.0, "global_sharpen": 0.0,
        "face_threed": 0.0, "eye_bright": 0.0,
        "remove_pouch_strength": 0.0, "remove_nasolabial_folds_strength": 0.0,
        "delspot_level": 0.0, "thin_face": 0.0, "big_eye": 0.0, "chin": 0.0,
        "max_faces": 1.0, "enabled": True}

CASES = [
    ("off",        {}),
    ("sharp_only", {"sharpen": 100.0}),
    ("gsharp_only", {"global_sharpen": 100.0}),
    ("sharp",      {"sharpen": 100.0, "global_sharpen": 100.0}),
    ("delspot",    {"delspot_level": 100.0}),
    ("chin",       {"chin": 100.0}),
    ("all3",       {"sharpen": 100.0, "global_sharpen": 100.0,
                    "delspot_level": 100.0, "chin": 100.0}),
    ("all3_blur",  {"blur_level": 100.0, "sharpen": 100.0,
                    "global_sharpen": 100.0, "delspot_level": 100.0,
                    "chin": 100.0}),
]


def main():
    o = OBS()
    sc = o.call("GetSceneList")["currentProgramSceneName"]
    print("scene:", sc)
    ensure_source(o, sc)

    for tag, over in CASES:
        st = dict(BASE)
        st.update(over)
        o.call("SetSourceFilterSettings", sourceName=SRC, filterName=FILT,
               filterSettings=st, overlay=True)
        o.call("SetSourceFilterEnabled", sourceName=SRC, filterName=FILT,
               filterEnabled=(tag != "off"))
        # face detection + EMA needs a few frames to settle
        time.sleep(2.5)
        n = o.shot(SRC, os.path.join(HERE, "_gpu_%s.png" % tag))
        print("  %-10s %8d bytes" % (tag, n))
    o.ws.close()
    print("done")


if __name__ == "__main__":
    main()
