"""WorkingTimer 用 PC エージェント。

PC の負荷・温度と ComfyUI の状態を MQTT で WorkingTimer (ESP32) に送る。
トピックとペイロードは ../MQTT_TOPICS.md に従う。アラートの判断は ESP32 側の仕事で、
ここでは状態を正直に送るだけにする。

使い方:
    pythonw agent.py            ウィンドウなしで常駐 (普段はタスクスケジューラから)
    python agent.py             ログを画面にも出して動かす (Ctrl+C で終了)
    python agent.py --stop      常駐しているエージェントを止める (offline を送ってから終わる)
    python agent.py --config X  設定ファイルを指定する (既定は同じフォルダの config.toml)
"""

import argparse
import json
import logging
import re
import socket
import sys
import threading
import time
import tomllib
import uuid
from logging.handlers import RotatingFileHandler
from pathlib import Path

import paho.mqtt.client as mqtt
import psutil
import requests
import win32api
import win32con
import win32event
import win32gui
import winerror

try:
    import pynvml  # nvidia-ml-py
except ImportError:
    pynvml = None
try:
    import websocket  # websocket-client
except ImportError:
    websocket = None

HERE = Path(__file__).resolve().parent
WINDOW_CLASS = "WorkingTimerPcAgent"

# 電源イベント (win32con にない版もあるので自前で持つ)
WM_POWERBROADCAST = 0x0218
PBT_APMSUSPEND = 0x0004
PBT_APMRESUMESUSPEND = 0x0007
PBT_APMRESUMEAUTOMATIC = 0x0012

log = logging.getLogger("agent")

DEFAULTS = {
    "pc_name": "",  # 既定値は持たない (見本のまま 2 台で同じ名前になると、接続と切断を繰り返すため)
    "broker": "192.168.11.32",
    "port": 1883,
    "comfy_url": "auto",
    "lhm_url": "",
    "sys_interval_sec": 5,
    "stall_minutes": 15,
    "max_job_minutes": 120,
}


def setup_logging():
    (HERE / "logs").mkdir(exist_ok=True)
    fmt = logging.Formatter("%(asctime)s %(levelname)s [%(threadName)s] %(message)s")
    fh = RotatingFileHandler(HERE / "logs" / "agent.log", maxBytes=1_000_000, backupCount=3, encoding="utf-8")
    fh.setFormatter(fmt)
    log.addHandler(fh)
    if sys.stderr is not None:  # pythonw では None
        sh = logging.StreamHandler()
        sh.setFormatter(fmt)
        log.addHandler(sh)
    log.setLevel(logging.INFO)


def load_config(path):
    with open(path, "rb") as f:
        cfg = {**DEFAULTS, **tomllib.load(f)}
    return cfg


def ascii_only(s, n):
    """ESP32 のフォントは ASCII だけなので、それ以外は ? にして n 文字で切る。"""
    return (s or "").encode("ascii", "replace").decode("ascii")[:n]


# ---------------------------------------------------------------- PC の負荷と温度

class SysMonitor:
    def __init__(self, lhm_url):
        self.lhm_url = lhm_url
        self.gpu = None
        self.lhm_ok = None  # 前回 LibreHardwareMonitor が読めたか (ログを状態の変化だけにする)
        psutil.cpu_percent(None)  # 1 回目は 0 が返るので空読みしておく
        if pynvml is None:
            log.warning("nvidia-ml-py が入っていないので GPU の値は -1 にする")
            return
        try:
            pynvml.nvmlInit()
            self.gpu = pynvml.nvmlDeviceGetHandleByIndex(0)
            log.info("GPU: %s", pynvml.nvmlDeviceGetName(self.gpu))
        except Exception as e:
            log.warning("NVML を使えない (%s)。GPU の値は -1 にする", e)

    def read(self):
        d = {"cpu": -1, "cpu_t": -1, "ram": -1, "gpu": -1, "gpu_t": -1, "vram": -1, "vram_tot": -1}
        try:
            d["cpu"] = round(psutil.cpu_percent(None))
            d["ram"] = round(psutil.virtual_memory().percent)
        except Exception as e:
            log.warning("psutil: %s", e)
        if self.gpu is not None:
            try:
                d["gpu"] = pynvml.nvmlDeviceGetUtilizationRates(self.gpu).gpu
                d["gpu_t"] = pynvml.nvmlDeviceGetTemperature(self.gpu, pynvml.NVML_TEMPERATURE_GPU)
                mem = pynvml.nvmlDeviceGetMemoryInfo(self.gpu)
                d["vram"] = round(mem.used / 2**30, 1)
                d["vram_tot"] = round(mem.total / 2**30, 1)
            except Exception as e:
                log.warning("NVML: %s", e)
        d["cpu_t"] = self.cpu_temp()
        return d

    def cpu_temp(self):
        """LibreHardwareMonitor の data.json から CPU 温度を探す。取れなければ -1。

        センサー名は CPU の世代で違うので決め打ちせず、
        「CPU のハードウェア配下の温度で、名前に Package を含むもの → なければ最大値」の順で選ぶ。
        """
        if not self.lhm_url:
            return -1
        try:
            data = requests.get(self.lhm_url, timeout=1).json()
        except Exception as e:
            if self.lhm_ok is not False:
                log.warning("LibreHardwareMonitor を読めない (%s)。CPU 温度は -1 にする", e)
            self.lhm_ok = False
            return -1
        if self.lhm_ok is not True:
            log.info("LibreHardwareMonitor を読めた")
        self.lhm_ok = True

        temps = []

        def walk(node, in_cpu, in_temp):
            text = node.get("Text", "")
            children = node.get("Children") or []
            # CPU のハードウェアの節は ImageURL が cpu.png。GPU などはその兄弟の別の枝なので混ざらない
            if node.get("ImageURL", "").lower().endswith("cpu.png"):
                in_cpu = True
            if text == "Temperatures":
                in_temp = True
            sid = node.get("SensorId", "").lower()  # 新しい版には "/intelcpu/0/temperature/0" のような ID もある
            is_cpu_temp = "cpu/" in sid and "/temperature/" in sid
            if not children and (is_cpu_temp or (in_cpu and (in_temp or node.get("Type") == "Temperature"))):
                m = re.search(r"(-?\d+(?:[.,]\d+)?)\s*°C", node.get("Value", ""))
                if m:
                    temps.append((text, float(m.group(1).replace(",", "."))))
            for ch in children:
                walk(ch, in_cpu, in_temp)

        walk(data, False, False)
        if not temps:
            return -1
        pkg = [v for name, v in temps if "package" in name.lower()]
        return round(pkg[0] if pkg else max(v for _, v in temps))


# ---------------------------------------------------------------- ComfyUI の監視

class ComfyMonitor:
    """ComfyUI の状態を /system_stats・/queue・/history (と、あれば WebSocket) で判定する。"""

    STATS_EVERY = 5  # /system_stats を見る間隔 (秒)
    DOWN_FAILS = 3   # これだけ続けて失敗したら down
    # comfy_url = "auto" のときに試すポート。デスクトップ版は 8000 から (使われていると 8001… へずれる)、普通の ComfyUI は 8188
    AUTO_PORTS = list(range(8000, 8010)) + [8188]

    def __init__(self, url, stall_minutes, max_job_minutes):
        self.auto = url.strip().lower() in ("", "auto")
        self.base = None if self.auto else url.rstrip("/")
        self.stall_sec = stall_minutes * 60
        self.max_job_sec = max_job_minutes * 60
        self.lock = threading.Lock()
        self.http = requests.Session()

        self.ever_ok = False  # 起動以来一度でも応答したか (なければ off)
        self.fails = 0
        self.last_stats = 0.0
        self.q = -1

        self.running_pid = None  # 実行中のジョブの prompt_id
        self.job_start = 0.0
        self.nodes = {}          # ノード ID → class_type
        self.error = None        # (node, err)。次のジョブが始まるまで保つ

        # WebSocket から来る進捗。ws_pid のジョブのもの
        self.ws_pid = None
        self.ws_node = ""
        self.ws_step = -1
        self.ws_steps = -1
        self.ws_progress_at = 0.0
        self.ws = None

    def _stats_ok(self, base, timeout):
        """base が ComfyUI として応答するか (/system_stats が "system" を含む JSON を返すか)。"""
        r = self.http.get(base + "/system_stats", timeout=timeout)
        r.raise_for_status()
        if "system" not in r.json():
            raise ValueError("ComfyUI の応答ではない")

    def _find_port(self):
        """auto のとき、127.0.0.1 の候補ポートから ComfyUI を探す。見つからなければ None。"""
        for port in self.AUTO_PORTS:
            base = f"http://127.0.0.1:{port}"
            try:
                self._stats_ok(base, 0.5)  # 開いていないポートはすぐ断られるので速い
            except Exception:
                continue
            if base != self.base:
                log.info("ComfyUI を %s で見つけた", base)
            return base
        return None

    # ---- A. ポーリング
    def poll(self):
        now = time.monotonic()
        if now - self.last_stats >= self.STATS_EVERY:
            self.last_stats = now
            err = None
            try:
                if self.base is None:
                    raise ConnectionError("ComfyUI が見つからない")
                self._stats_ok(self.base, 2)
            except Exception as e:
                err = e
            # auto なら、応答しないときはポートを探し直す (ComfyUI を起動し直すとずれることがある)
            if err and self.auto:
                found = self._find_port()
                if found:
                    self.base, err = found, None
            if err is None:
                if not self.ever_ok or self.fails:
                    log.info("ComfyUI が応答した (%s)", self.base)
                self.ever_ok = True
                self.fails = 0
            else:
                self.fails += 1
                if self.ever_ok and self.fails == self.DOWN_FAILS:
                    log.warning("ComfyUI が応答しなくなった (%s)", err)
        if not self.ever_ok or self.fails:
            return
        try:
            queue = self.http.get(self.base + "/queue", timeout=2).json()
        except Exception as e:
            log.info("/queue: %s", e)
            return
        self._update_queue(queue)

    def _update_queue(self, queue):
        running = queue.get("queue_running") or []
        pending = queue.get("queue_pending") or []
        pid = running[0][1] if running else None
        with self.lock:
            self.q = len(pending)
            if pid == self.running_pid:
                return
            prev = self.running_pid
        if prev:
            self._finish(prev)
        with self.lock:
            self.running_pid = pid
            if pid:
                self.job_start = time.monotonic()
                prompt = running[0][2] if len(running[0]) > 2 and isinstance(running[0][2], dict) else {}
                self.nodes = {str(k): v.get("class_type", "") for k, v in prompt.items() if isinstance(v, dict)}
                self.error = None  # 次のジョブが始まったので error は解除
                log.info("job start %s (%d nodes)", pid, len(self.nodes))

    def _finish(self, pid):
        """終わったジョブの結果を /history で確かめる。エラーなら error を保つ。"""
        try:
            h = self.http.get(f"{self.base}/history/{pid}", timeout=3).json().get(pid, {})
        except Exception as e:
            log.info("/history/%s: %s", pid, e)
            return
        status = h.get("status") or {}
        names = {}
        for msg in status.get("messages") or []:
            if isinstance(msg, list) and len(msg) == 2:
                names[msg[0]] = msg[1] or {}
        if "execution_interrupted" in names:
            log.info("job interrupted %s", pid)  # ユーザーの中断はエラーにしない
            return
        if status.get("status_str") == "error" or "execution_error" in names:
            d = names.get("execution_error", {})
            node = d.get("node_type", "")
            err = str(d.get("exception_type", "")).split(".")[-1]  # torch.OutOfMemoryError → OutOfMemoryError
            with self.lock:
                self.error = (node, err)
            log.warning("job error %s: %s %s", pid, node, err)
        else:
            log.info("job done %s", pid)

    # ---- B. WebSocket (届けば進捗が細かくなる。届かなくても A だけで動く)
    def on_ws_message(self, _ws, msg):
        if isinstance(msg, bytes):
            return  # プレビュー画像
        try:
            m = json.loads(msg)
        except ValueError:
            return
        t, d = m.get("type"), m.get("data") or {}
        now = time.monotonic()
        with self.lock:
            pid = d.get("prompt_id")
            if t in ("progress", "executing") and pid and pid != self.ws_pid:
                self.ws_pid, self.ws_node, self.ws_step, self.ws_steps = pid, "", -1, -1
            if t == "progress":
                self.ws_step, self.ws_steps = int(d.get("value", -1)), int(d.get("max", -1))
                if d.get("node") is not None:
                    self.ws_node = self.nodes.get(str(d["node"]), self.ws_node)
                self.ws_progress_at = now
            elif t == "executing":
                nid = d.get("node")
                self.ws_node = self.nodes.get(str(nid), "") if nid is not None else ""
                self.ws_step = self.ws_steps = -1  # ノードが変わると進捗は数え直し
                self.ws_progress_at = now
            elif t == "execution_error":
                self.error = (d.get("node_type", ""), str(d.get("exception_type", "")).split(".")[-1])
            elif t == "execution_interrupted":
                self.error = None

    def ws_loop(self, stop):
        if websocket is None:
            log.warning("websocket-client が入っていないので進捗はポーリングだけで見る")
            return
        client_id = uuid.uuid4().hex
        while not stop.is_set():
            if self.ever_ok and not self.fails and self.base:
                # ポートが変わることがあるので、つなぐたびに今の URL から作る
                url = re.sub(r"^http", "ws", self.base) + "/ws?clientId=" + client_id
                self.ws = websocket.WebSocketApp(
                    url,
                    on_open=lambda _ws: log.info("WebSocket つながった"),
                    on_message=self.on_ws_message,
                    on_close=lambda _ws, *_: log.info("WebSocket 切れた"),
                )
                try:
                    self.ws.run_forever(ping_interval=20, ping_timeout=10)
                except Exception as e:
                    log.info("WebSocket: %s", e)
                self.ws = None
            stop.wait(30)  # 切れたら 30 秒ごとにやり直す

    def close(self):
        if self.ws:
            self.ws.close()

    # ---- 送る内容
    def payload(self):
        now = time.monotonic()
        with self.lock:
            p = {"st": "idle", "q": self.q, "step": -1, "steps": -1, "node": "", "el": -1, "err": ""}
            if not self.ever_ok:
                p.update(st="off", q=-1)
            elif self.fails >= self.DOWN_FAILS:
                p.update(st="down", q=-1)
            elif self.running_pid:
                el = now - self.job_start
                ws_mine = self.ws_pid == self.running_pid and self.ws_progress_at > 0
                if ws_mine:
                    stalled = now - self.ws_progress_at > self.stall_sec
                    p.update(step=self.ws_step, steps=self.ws_steps, node=self.ws_node)
                else:
                    stalled = el > self.max_job_sec  # 進捗が取れないときは 1 ジョブの長さで判断
                p.update(st="stalled" if stalled else "run", el=int(el))
            elif self.error:
                p.update(st="error", node=self.error[0], err=self.error[1])
            p["node"] = ascii_only(p["node"], 24)
            p["err"] = ascii_only(p["err"], 40)
            return p


# ---------------------------------------------------------------- MQTT と全体

class Agent:
    def __init__(self, cfg):
        self.cfg = cfg
        name = cfg["pc_name"]
        self.t_status = f"info/pc/{name}/status"
        self.t_sys = f"info/pc/{name}/sys"
        self.t_comfy = f"info/pc/{name}/comfy"
        self.stop = threading.Event()
        self.stopped = False
        self.sleeping = False
        self.connected_at = 0.0
        self.quick_drops = []  # つながってすぐ切れた時刻 (同じ pc_name の PC がほかにいないかの目安)
        self.resend_comfy = threading.Event()  # 接続し直したら comfy をすぐ送り直す

        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"pcagent_{name}")
        # 予期せぬ切断のときはブローカーが lost を出す (接続前に設定する)
        self.client.will_set(self.t_status, "lost", qos=1, retain=True)
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)
        self.client.on_connect = self.on_connect
        self.client.on_connect_fail = lambda *_: log.info("MQTT につながらない (%s:%s)。裏でやり直す", cfg["broker"], cfg["port"])
        self.client.on_disconnect = self.on_disconnect

        self.sys = SysMonitor(cfg["lhm_url"])
        self.comfy = ComfyMonitor(cfg["comfy_url"], cfg["stall_minutes"], cfg["max_job_minutes"])

    def on_connect(self, client, _userdata, _flags, reason_code, _props):
        if reason_code.is_failure:
            log.warning("MQTT 接続を断られた: %s", reason_code)
            return
        log.info("MQTT つながった %s:%s", self.cfg["broker"], self.cfg["port"])
        self.connected_at = time.monotonic()
        client.publish(self.t_status, "online", qos=1, retain=True)
        self.resend_comfy.set()

    def on_disconnect(self, _client, _userdata, _flags, reason_code, _props):
        log.info("MQTT 切れた: %s", reason_code)
        if self.stopped or self.sleeping:
            return
        # つながって 5 秒もたたずに切れるのが 1 分に 3 回以上続くのは、同じクライアント ID の取り合いのことが多い
        now = time.monotonic()
        if now - self.connected_at < 5:
            self.quick_drops = [t for t in self.quick_drops if now - t < 60] + [now]
            if len(self.quick_drops) == 3:
                log.warning("接続してすぐ切れるのを繰り返している。ほかの PC の config.toml にも pc_name = \"%s\" が"
                            "書かれていないか確かめる (同じ名前だと取り合いになり、そのたびに lost が出る)", self.cfg["pc_name"])

    def publish(self, topic, payload, qos, retain):
        # つながっていないときは送らない (古い値をため込まない。つながり直したら送り直す)
        if self.client.is_connected():
            self.client.publish(topic, payload, qos=qos, retain=retain)

    def start(self):
        self.connect()
        threading.Thread(target=self.sys_loop, name="sys", daemon=True).start()
        threading.Thread(target=self.comfy_loop, name="comfy", daemon=True).start()
        threading.Thread(target=self.comfy.ws_loop, args=(self.stop,), name="ws", daemon=True).start()

    def connect(self):
        # ブローカーに届かなくても落ちずに、裏で繰り返しつなぎに行く
        self.client.connect_async(self.cfg["broker"], int(self.cfg["port"]), keepalive=30)
        self.client.loop_start()

    def say_and_disconnect(self, status):
        """status を retain で送ってから正しく切断する (正しく切断すれば LWT の lost は出ない)。"""
        sent = False
        if self.client.is_connected():
            info = self.client.publish(self.t_status, status, qos=1, retain=True)
            try:
                info.wait_for_publish(timeout=2)
                sent = info.is_published()
            except Exception as e:
                log.warning("%s を送れなかった: %s", status, e)
        self.client.disconnect()
        self.client.loop_stop()
        log.info("%s を%s切断した", status, "送って" if sent else "送れないまま")

    def sys_loop(self):
        interval = float(self.cfg["sys_interval_sec"])
        while not self.stop.wait(interval):
            self.publish(self.t_sys, json.dumps(self.sys.read(), separators=(",", ":")), qos=0, retain=False)

    def comfy_loop(self):
        last_st, last_sent = None, 0.0
        while not self.stop.wait(2):
            try:
                self.comfy.poll()
                p = self.comfy.payload()
            except Exception:
                log.exception("ComfyUI の監視で例外")
                continue
            now = time.monotonic()
            busy = p["st"] in ("run", "stalled")
            # 状態が変わったら即、実行中は 2 秒ごと、それ以外は 30 秒ごと
            if p["st"] != last_st or busy or now - last_sent >= 30 or self.resend_comfy.is_set():
                if p["st"] != last_st:
                    log.info("comfy %s -> %s %s", last_st, p["st"], p)
                if self.client.is_connected():
                    self.resend_comfy.clear()
                self.publish(self.t_comfy, json.dumps(p, separators=(",", ":")), qos=1, retain=True)
                last_st, last_sent = p["st"], now

    # ---- 電源イベントと終了
    def suspend(self):
        if self.sleeping or self.stopped:
            return
        self.sleeping = True
        log.info("スリープに入る")
        # つないだまま眠るとキープアライブ切れで lost が出るので、sleep を送って切断しておく
        self.say_and_disconnect("sleep")

    def resume(self):
        if not self.sleeping or self.stopped:
            return
        self.sleeping = False
        log.info("スリープから戻った")
        self.connect()  # つながったら on_connect が online を送る

    def shutdown(self, reason):
        if self.stopped:
            return
        self.stopped = True
        log.info("終了する (%s)", reason)
        self.stop.set()
        self.comfy.close()
        if self.sleeping:
            self.client.loop_stop()
        else:
            self.say_and_disconnect("offline")


def run_window(agent):
    """非表示のウィンドウでシャットダウン・スリープ・終了の知らせを受ける。

    メッセージ専用ウィンドウには WM_QUERYENDSESSION や WM_POWERBROADCAST が届かないので、
    普通のトップレベルウィンドウを作って表示しないでおく。
    """

    def on_query_end(hwnd, msg, wparam, lparam):
        return True

    def on_end(hwnd, msg, wparam, lparam):
        if wparam:
            agent.shutdown("Windows の終了")
        return 0

    def on_power(hwnd, msg, wparam, lparam):
        if wparam == PBT_APMSUSPEND:
            agent.suspend()
        elif wparam in (PBT_APMRESUMEAUTOMATIC, PBT_APMRESUMESUSPEND):
            agent.resume()
        return True

    def on_close(hwnd, msg, wparam, lparam):
        agent.shutdown("終了の指示")
        win32gui.DestroyWindow(hwnd)
        return 0

    def on_destroy(hwnd, msg, wparam, lparam):
        win32gui.PostQuitMessage(0)
        return 0

    wc = win32gui.WNDCLASS()
    wc.lpszClassName = WINDOW_CLASS
    wc.hInstance = win32api.GetModuleHandle(None)
    wc.lpfnWndProc = {
        win32con.WM_QUERYENDSESSION: on_query_end,
        win32con.WM_ENDSESSION: on_end,
        WM_POWERBROADCAST: on_power,
        win32con.WM_CLOSE: on_close,
        win32con.WM_DESTROY: on_destroy,
    }
    win32gui.RegisterClass(wc)
    hwnd = win32gui.CreateWindow(WINDOW_CLASS, f"WorkingTimer PC agent {agent.cfg['pc_name']}",
                                 0, 0, 0, 0, 0, 0, 0, wc.hInstance, None)

    # コンソールから動かしたときの Ctrl+C も、ウィンドウを閉じる形で同じ終わり方にする
    def on_console(_ctrl):
        win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
        return True

    try:
        win32api.SetConsoleCtrlHandler(on_console, True)
    except Exception:
        pass  # pythonw ではコンソールがない

    win32gui.PumpMessages()


def stop_running():
    hwnd = win32gui.FindWindow(WINDOW_CLASS, None)
    if not hwnd:
        print("動いているエージェントは見つからなかった")
        return 1
    win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
    print("エージェントに終了を伝えた")
    return 0


def main():
    ap = argparse.ArgumentParser(description="WorkingTimer 用 PC エージェント")
    ap.add_argument("--config", default=str(HERE / "config.toml"))
    ap.add_argument("--stop", action="store_true", help="常駐しているエージェントを止める")
    args = ap.parse_args()
    if args.stop:
        return stop_running()

    setup_logging()
    try:
        cfg = load_config(args.config)
    except FileNotFoundError:
        log.error("設定ファイル %s がない。config.example.toml をコピーして作る", args.config)
        return 1
    if not str(cfg["pc_name"]).strip():
        log.error("config.toml の pc_name が空。この PC の名前 (PC1 / PC2 / PC3) を書く")
        return 1

    # 二重起動すると接続と切断を繰り返すので、1 台の PC で 1 つだけにする (pc_name を書き換えた後でも)
    mutex = win32event.CreateMutex(None, False, "Local\\WorkingTimerPcAgent")
    if win32api.GetLastError() == winerror.ERROR_ALREADY_EXISTS:
        log.error("もう動いているので終了する")
        return 1

    # ログを見て、どの PC がどの名前で動いているか分かるようにする
    log.info("起動 %s on %s (broker %s:%s, comfy %s)", cfg["pc_name"], socket.gethostname(),
             cfg["broker"], cfg["port"], cfg["comfy_url"])
    agent = Agent(cfg)
    agent.start()
    try:
        run_window(agent)
    finally:
        agent.shutdown("メインループ終了")
        del mutex
    log.info("終了")
    return 0


if __name__ == "__main__":
    sys.exit(main())
