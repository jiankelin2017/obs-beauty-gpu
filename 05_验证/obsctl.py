#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
obs-websocket v5 客户端（S1 正确性验收用，2026-11-02）。

为什么要它：
  第 1 项（像素对拍）需要「把插件输出抓成图」；第 2 项（开关 20 次）需要
  「反复切滤镜 enabled」。这两件事手动做既慢又不可复现，obs-websocket 的
  GetSourceScreenshot / SetSourceFilterEnabled 正好覆盖。

  截图走 obs_source_video_render，会**经过滤镜链**，所以抓到的就是插件输出。

依赖：websocket-client（已随环境提供 1.9.x）。图像处理用 PIL，只在 shot 时导入。

命令行（方便手工排查）：
    python obsctl.py ping
    python obsctl.py list
    python obsctl.py shot  still  out.png [w h]
    python obsctl.py enable still Beauty 0|1
    python obsctl.py settings still Beauty '{"blur_level":80}'

作为库：
    from obsctl import connect, shot, set_filter_enabled
"""
import base64
import hashlib
import json
import os
import sys
import time

try:
    import websocket  # websocket-client
except ImportError:  # pragma: no cover
    sys.exit("缺少 websocket-client：pip install websocket-client")

HOST = "127.0.0.1"
PORT = 4455


# ---------------------------------------------------------------- 配置读取
def _config_path():
    appdata = os.environ.get("APPDATA")
    if not appdata:
        return None
    return os.path.join(appdata, "obs-studio", "plugin_config",
                        "obs-websocket", "config.json")


def load_password():
    """从 obs-websocket 的 config.json 读密码；没有返回空串。"""
    p = _config_path()
    if not p or not os.path.isfile(p):
        return ""
    try:
        with open(p, encoding="utf-8") as fh:
            return json.load(fh).get("server_password") or ""
    except Exception:
        return ""


def _auth_token(password, salt, challenge):
    """v5 认证：base64(sha256(base64(sha256(pwd+salt)) + challenge))"""
    secret = base64.b64encode(
        hashlib.sha256((password + salt).encode("utf-8")).digest()).decode("ascii")
    return base64.b64encode(
        hashlib.sha256((secret + challenge).encode("utf-8")).digest()).decode("ascii")


# ---------------------------------------------------------------- 客户端
class Client(object):
    def __init__(self, host=HOST, port=PORT, password=None, timeout=30.0):
        self.timeout = timeout
        self.ws = websocket.create_connection(
            "ws://%s:%d" % (host, port), timeout=timeout)
        hello = json.loads(self.ws.recv())
        if hello.get("op") != 0:
            raise RuntimeError("期望 Hello(op=0)，实际 %r" % (hello,))
        d = hello.get("d") or {}
        ident = {"rpcVersion": 1, "eventSubscriptions": 0}
        auth = d.get("authentication")
        if auth:
            pwd = load_password() if password is None else password
            ident["authentication"] = _auth_token(pwd, auth["salt"], auth["challenge"])
        self.ws.send(json.dumps({"op": 1, "d": ident}))
        self.rid = 0
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = json.loads(self.ws.recv())
            if msg.get("op") == 2:
                return
            if msg.get("op") == 7:
                continue
        raise RuntimeError("Identify 未被确认（密码错误？）")

    # --- 底层请求 ---
    def req(self, rtype, data=None, timeout=None):
        self.rid += 1
        rid = str(self.rid)
        payload = {"op": 6, "d": {"requestType": rtype, "requestId": rid}}
        if data:
            payload["d"]["requestData"] = data
        self.ws.send(json.dumps(payload))
        deadline = time.time() + (timeout or self.timeout)
        while time.time() < deadline:
            msg = json.loads(self.ws.recv())
            if msg.get("op") != 7:
                continue
            d = msg.get("d") or {}
            if d.get("requestId") != rid:
                continue
            st = d.get("requestStatus") or {}
            if not st.get("result"):
                raise RuntimeError("%s 失败：code=%s %s" %
                                   (rtype, st.get("code"), st.get("comment")))
            return d.get("responseData") or {}
        raise RuntimeError("%s 超时" % rtype)

    def close(self):
        try:
            self.ws.close()
        except Exception:
            pass

    # --- 常用封装 ---
    def scenes(self):
        return self.req("GetSceneList").get("scenes", [])

    def inputs(self):
        return self.req("GetInputList").get("inputs", [])

    def filters(self, source):
        return self.req("GetSourceFilterList", {"sourceName": source}).get("filters", [])

    def filter_enabled(self, source, filt):
        fs = self.filters(source)
        for f in fs:
            if f.get("filterName") == filt:
                return bool(f.get("filterEnabled"))
        raise KeyError("源 %r 上没有滤镜 %r" % (source, filt))

    def set_filter_enabled(self, source, filt, on):
        self.req("SetSourceFilterEnabled",
                 {"sourceName": source, "filterName": filt,
                  "filterEnabled": bool(on)})

    def set_filter_settings(self, source, filt, settings, overlay=True):
        self.req("SetSourceFilterSettings",
                 {"sourceName": source, "filterName": filt,
                  "filterSettings": settings, "overlay": bool(overlay)})

    def filter_settings(self, source, filt):
        fs = self.filters(source)
        for f in fs:
            if f.get("filterName") == filt:
                return f.get("filterSettings") or {}
        raise KeyError("源 %r 上没有滤镜 %r" % (source, filt))

    def scene_items(self, scene):
        return self.req("GetSceneItemList", {"sceneName": scene}).get("sceneItems", [])

    def set_item_transform(self, scene, item_id, tr):
        self.req("SetSceneItemTransform",
                 {"sceneName": scene, "sceneItemId": int(item_id),
                  "sceneItemTransform": tr})

    def screenshot_bytes(self, source, width=None, height=None):
        data = {"sourceName": source, "imageFormat": "png",
                "imageCompressionQuality": -1}
        if width:
            data["imageWidth"] = int(width)
        if height:
            data["imageHeight"] = int(height)
        r = self.req("GetSourceScreenshot", data)
        b64 = r.get("imageData") or ""
        if "," in b64:  # 兼容 data:image/png;base64,xxx
            b64 = b64.split(",", 1)[1]
        return base64.b64decode(b64)

    def shot(self, source, path, width=None, height=None):
        """取源截图存盘，返回 PIL.Image。"""
        raw = self.screenshot_bytes(source, width, height)
        d = os.path.dirname(os.path.abspath(path))
        if d and not os.path.isdir(d):
            os.makedirs(d)
        with open(path, "wb") as fh:
            fh.write(raw)
        from PIL import Image
        import io
        return Image.open(io.BytesIO(raw))


def connect(host=HOST, port=PORT, timeout=30.0):
    return Client(host, port, timeout=timeout)


def wait_ready(host=HOST, port=PORT, wait=60.0, quiet=True):
    """等 OBS 把 obs-websocket 拉起来，返回 Client。"""
    t0 = time.time()
    last = None
    while time.time() - t0 < wait:
        try:
            c = connect(host, port, timeout=5.0)
            return c
        except Exception as e:  # 端口还没监听 / Hello 还没到
            last = e
            time.sleep(0.7)
    raise RuntimeError("等待 obs-websocket 超时（%.0fs）：%s" % (wait, last))


# ---------------------------------------------------------------- CLI
def _print_list(c):
    print("--- 场景 ---")
    for s in c.scenes():
        print("   %s%s" % (s.get("sceneName"), "  <-当前" if s.get("sceneIndex") == 0 else ""))
    print("--- 输入源 ---")
    for i in c.inputs():
        name = i.get("inputName")
        print("   id=%-14s name=%s" % (i.get("inputKind"), name))
        try:
            for f in c.filters(name):
                print("        filter: %-14s enabled=%r" % (f.get("filterName"), f.get("filterEnabled")))
        except Exception as e:
            print("        (滤镜读取失败: %s)" % e)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    c = connect()
    try:
        if cmd == "ping":
            v = c.req("GetVersion")
            print("OK obs-websocket %s / OBS %s" %
                  (v.get("obsWebSocketVersion"), v.get("obsVersion")))
        elif cmd == "list":
            _print_list(c)
        elif cmd == "shot":
            src, out = argv[2], argv[3]
            w = int(argv[4]) if len(argv) > 4 else None
            h = int(argv[5]) if len(argv) > 5 else None
            im = c.shot(src, out, w, h)
            print("saved %s %s" % (out, im.size))
        elif cmd == "enable":
            src, filt, on = argv[2], argv[3], argv[4] not in ("0", "false", "False")
            c.set_filter_enabled(src, filt, on)
            print("filter %r.enabled = %r" % (filt, on))
        elif cmd == "settings":
            src, filt, js = argv[2], argv[3], argv[4]
            c.set_filter_settings(src, filt, json.loads(js))
            print("filter %r settings updated" % filt)
        else:
            print("未知命令: %s" % cmd)
            return 2
    finally:
        c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
