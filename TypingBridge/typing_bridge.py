#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Typing Meter & Solenoid Controller (Hybrid: keyboard + RAW HID-layer)

・Tkinter GUI
・グローバルキーフックで CPM 計測（keyboard ライブラリ）
・QMK RAW HID からレイヤー状態だけを受信して Core2 に転送

  QMK 側 RAW HID プロトコル想定：
    data[0] = cmd
      0x02 : レイヤー変更
          data[1] = layer (0..n)

・CPM ロジック（QMK と同等の方式）：
    - 1秒 window_count を貯める
    - 1秒ごとに current_cpm = window_count * 60
    - 500ms ごとに Core2 へ current_cpm を送信

・Core2 への USB シリアル送信：
    - 0x01, LSB, MSB  : CPM
    - 0x02, layer     : レイヤー番号
    - 0x10            : 軽いソレノイド
    - 0x11            : 強いソレノイド

・モード切り替え：
    - CPM only
    - Solenoid only
    - Both

・Connect ボタン / keyboard hook / RAW HID はすべて別スレッド or 非ブロッキングで動作

・追加: Auto reconnect 機能
    - 最後に成功した COM ポートを記憶し、切断されている場合に 5秒おきに自動再接続
    - 「Auto reconnect」チェックボックスで ON/OFF
"""

import sys
import time
import threading
import tkinter as tk
from tkinter import ttk, messagebox
from serial.tools import list_ports
import json
import os
import re
from tkinter.scrolledtext import ScrolledText

import psutil
import platform


# ---------------------------------------------------------
# シリアル (Core2 通信用)
# ---------------------------------------------------------
try:
    import serial
    import serial.tools.list_ports as list_ports
except ImportError:
    print("pyserial が必要です: pip install pyserial")
    sys.exit(1)


# ================================
# Core2 Serial Handshake Protocol
# ================================
HELLO_MAGIC = 0xF0
HELLO_CMD   = 0x00

DEV_MAGIC   = 0x7F
DEV_CMD_DEVICE_ID = 0x01

# Core2 の DEVICE_ID 応答は 6 bytes 想定
# [0]=0x7F, [1]=0x01, [2]=protocol, [3]=deviceType(=Core2), [4]=features, [5]=reserved
DEVICE_ID_LEN = 6

CORE2_DEVICE_TYPE = 0x01


def probe_core2_bt_passive(port: str, wait=0.4):
    """
    消極的成功判定:
    - openできる
    - writeがtimeoutしない
    - 例外が出ない
    """
    try:
        print(f"[BT PASSIVE PROBE] open {port}")

        ser = serial.Serial(
            port,
            115200,
            timeout=0.1,
            write_timeout=0.1
        )

        # RFCOMM 安定待ち
        time.sleep(wait)

        # write が通るかだけ確認（readは見ない）
        ser.write(bytes([HELLO_MAGIC, HELLO_CMD]))

        # ★ close しない！
        print(f"[BT PASSIVE PROBE] likely success {port}")
        return True, ser   # ser を保持して返す

    except Exception as e:
        print(f"[BT PASSIVE PROBE] failed {port}: {e}")
        return False, None


def extract_bt_mac(port_info):
    """
    BTHENUM\\{00001101-0000-1000-8000-00805F9B34FB}_XXXX
    → 08:00:5F:9B:34:FB
    """
    hwid = (port_info.hwid or "").upper()
    m = re.search(r'([0-9A-F]{12})', hwid.replace("-", ""))
    if not m:
        return None
    mac = m.group(1)
    return ":".join(mac[i:i+2] for i in range(0, 12, 2))


def probe_core2_port(port: str, timeout=0.15):
    """
    指定された COM ポートに HELLO を送り、
    Core2 の DEVICE_ID 応答が返るか確認する
    """
    try:
        ser = serial.Serial(
            port,
            115200,
            timeout=0.05,
            write_timeout=0.05
        )
        # ★★ これを入れる ★★
        time.sleep(0.6)   # ← ESP32 USB CDC 安定待ち
        
        # 受信バッファをクリア
        ser.reset_input_buffer()

        # HELLO 送信
        ser.write(bytes([HELLO_MAGIC, HELLO_CMD]))

        deadline = time.time() + timeout
        buf = bytearray()

        while time.time() < deadline:
            buf += ser.read(32)

            # DEVICE_ID パケット検出
            idx = buf.find(bytes([DEV_MAGIC, DEV_CMD_DEVICE_ID]))
            if idx != -1 and len(buf) >= idx + DEVICE_ID_LEN:
                pkt = buf[idx:idx + DEVICE_ID_LEN]

                # deviceType == Core2 ?
                if pkt[3] == CORE2_DEVICE_TYPE:
                    ser.close()
                    return True

            time.sleep(0.01)

        ser.close()

    except Exception:
        pass

    return False


def auto_detect_core2_port(preferred_port=None, link_type=None):
    ports = list_ports_by_link_type(link_type)

    # ---------- USB は probe しない ----------
    if link_type == "USB":
        # last_port 優先
        if preferred_port and preferred_port in ports:
            return preferred_port, None

        # 先頭をそのまま返す（= UIと同じ）
        if ports:
            return ports[0], None

        return None, None

    # ---------- Bluetooth は今まで通り ----------
    if preferred_port and preferred_port in ports:
        ok, ser = probe_core2_bt_passive(preferred_port)
        if ok:
            return preferred_port, ser

    for port in ports:
        if port == preferred_port:
            continue
        ok, ser = probe_core2_bt_passive(port)
        if ok:
            return port, ser

    return None, None


def is_bluetooth_port_info(p):
    desc = (p.description or "").upper()
    hwid = (p.hwid or "").upper()
    return (
        "BLUETOOTH" in desc
        or "BTHENUM" in hwid
        or "RFCOMM" in hwid
        or "STANDARD SERIAL OVER BLUETOOTH" in desc
    )


def auto_detect_core2_bt(preferred_port=None):
    ports = list(list_ports.comports())

    print(f"[BT AUTO] preferred_port = {preferred_port}")

    # --- ① last_port 最優先（Bluetooth判定しない） ---
    if preferred_port:
        for p in ports:
            if p.device == preferred_port:
                print(f"[BT AUTO] trying last_port {p.device}")
                ok, ser = probe_core2_bt_passive(p.device)
                if ok:
                    return p.device, ser
                else:
                    print(f"[BT AUTO] last_port failed: {p.device}")
                break   # ★ 1回だけ

    # --- ② Bluetooth COM を順に総当たり ---
    for p in ports:
        if not is_bluetooth_port_info(p):
            continue
        if p.device == preferred_port:
            continue

        print(f"[BT PROBE] trying {p.device}")
        ok, ser = probe_core2_bt_passive(p.device)
        if ok:
            return p.device, ser

    return None, None





# ---------------------------------------------------------
# グローバルキーフック
# ---------------------------------------------------------
try:
    import keyboard
except ImportError:
    print("keyboard ライブラリが必要です: pip install keyboard")
    sys.exit(1)

# ---------------------------------------------------------
# RAW HID (QMK → レイヤー取得用)
# ---------------------------------------------------------
try:
    import hid  # hidapi
    HID_AVAILABLE = True
except ImportError:
    print("hidapi が見つかりません (pip install hidapi) → RAW HID レイヤー連動は無効で起動します。")
    HID_AVAILABLE = False

# ==== QMK 側で設定している VID/PID ====
# config.h の "usb" セクションと合わせる：
#   "vid": "0xFEED",
#   "pid": "0x0005",
QMK_VENDOR_ID = 0xFF60
QMK_PRODUCT_ID = 0x0005

# Debug: HID enumerate 全リスト表示
if HID_AVAILABLE:
    print("=== HID ENUMERATE LIST ===")
    for d in hid.enumerate():
        print(d)
    print("=== END ENUM ===")


# ================================
# Serial Sender (Core2 通信)
# ================================
class SerialSender:
 
    def __init__(self):
        self.ser = None
        self.lock = threading.Lock()
        self.is_bluetooth = False 


    
    def connect(self, port: str, baudrate: int = 115200):
        with self.lock:
            if self.ser and self.ser.is_open:
                self.ser.close()
                self.ser = None

            # --- Bluetooth 判定（description / hwid から） ---
            self.is_bluetooth = False
            for p in list_ports.comports():
                if p.device != port:
                    continue

                desc = (p.description or "").upper()
                hwid = (p.hwid or "").upper()

                # 強めに Bluetooth 判定
                if (
                    "BLUETOOTH" in desc
                    or "BTHENUM" in hwid
                    or "RFCOMM" in hwid
                    or "STANDARD SERIAL OVER BLUETOOTH" in desc
                ):
                    self.is_bluetooth = True
                break


            print(
                f"[SERIAL] open {port} "
                f"({'Bluetooth' if self.is_bluetooth else 'USB'})"
            )

            self.ser = serial.Serial(
                port,
                baudrate=baudrate,
                timeout=0.05 if self.is_bluetooth else 0,
                write_timeout=0.05 if self.is_bluetooth else 0
            )

              # 🔽 BT の場合だけ HELLO 待ち
            if self.is_bluetooth:
                time.sleep(0.5)           # ← 超重要
                self.ser.reset_input_buffer()
                self.ser.write(bytes([HELLO_MAGIC, HELLO_CMD]))



    def is_connected(self) -> bool:
        if self.ser is None:
            return False

        # USB / BT 共通：open状態ならOK
        return self.ser.is_open


    def is_connected(self) -> bool:
        if self.ser is None:
            return False

        # Bluetooth は open 状態だけで OK
        if self.is_bluetooth:
            return self.ser.is_open

        # USB は「例外を起こさず write できるか」で判定
        try:
            self.ser.write(b"")
            return True
        except Exception:
            self.ser = None
            return False


    def _write(self, data: bytes):
        with self.lock:
            if not self.is_connected():
                return
            try:
                self.ser.write(data)
            except serial.SerialTimeoutException:
                # 書き込みタイムアウト → 無視
                pass
            except serial.SerialException:
                # ポートが突然消えたなど
                self.ser = None

    # ---- プロトコル送信 ----
    def send_cpm(self, cpm: int):
        """0x01, LSB, MSB (LSB先。Core2のUSBステートマシンに合わせる)"""
        if not self.is_connected():
            return
        if cpm < 0:
            cpm = 0
        if cpm > 2000:
            cpm = 2000
        lsb = cpm & 0xFF
        msb = (cpm >> 8) & 0xFF
        packet = bytes([0x01, lsb, msb])
        self._write(packet)

    def send_layer(self, layer: int):
        """0x02, layer"""
        if not self.is_connected():
            return
        layer &= 0xFF
        packet = bytes([0x02, layer])
        self._write(packet)

    def solenoid_light(self):
        if not self.is_connected():
            return
        self._write(bytes([0xA5, 0x80]))   # Light

    def solenoid_strong(self):
        if not self.is_connected():
            return
        self._write(bytes([0xA5, 0x81]))   # Strong
    
    def get_link_type(self) -> str:
        if not self.is_connected():
            return "None"
        return "Bluetooth" if self.is_bluetooth else "USB"
    
    def attach_existing_serial(self, ser, is_bluetooth=True):
        with self.lock:
            self.ser = ser
            self.is_bluetooth = is_bluetooth

    def send_pc_status(self, stats, disk_r_mb, disk_w_mb):
        """
        PC Status を Core2 に送信
        """
        if not self.is_connected():
            return

        def clamp(v, lo=0, hi=255):
            return max(lo, min(hi, int(v)))

               # ---- 量子化 ----
        r_mbps_q = clamp(disk_r_mb * 10)   # 0.1MB/s 単位
        w_mbps_q = clamp(disk_w_mb * 10) 

        packets = [
            (0x20, clamp(stats["cpu_usage"])),
            (0x21, clamp(stats["ram_usage"])),
            (0x22, clamp(stats["disk_usage"])),
            (0x23, clamp(disk_r_mb)),   # MB/s
            (0x24, clamp(disk_w_mb)),
            (0x25, r_mbps_q),                     # ★ MB/s 実値
            (0x26, w_mbps_q),                     # ★ MB/s 実値
        ]

        if stats.get("cpu_temp") is not None:
            packets.append((0x27, clamp(stats["cpu_temp"], 0, 100)))

        for cmd, val in packets:
            self._write(bytes([cmd, val]))


# ================================
# CPM Counter (QMK互換ロジック)
# ================================
class CPMCounter:
    """
    QMK の CPM ロジックに極力寄せる:

    QMK側:
        - 1秒で window_count を貯める
        - 1秒経過時に current_cpm = window_count * 60
        - window_count リセット
        - 送信は 500ms ごとに現在の current_cpm を送る
    """

    def __init__(self):
        self.window_start = time.perf_counter()
        self.window_count = 0
        self.current_cpm = 0
        self.last_send = time.perf_counter()

    def reset(self):
        self.window_start = time.perf_counter()
        self.window_count = 0
        self.current_cpm = 0
        self.last_send = time.perf_counter()

    def on_key_press(self):
        """キーダウン1回ごとに呼ぶ"""
        self.window_count += 1

    def update(self):
        """
        定期的 (例: 50msごと) に呼ぶ。
        戻り値: (current_cpm, should_send)
        """
        now = time.perf_counter()

        # 1秒経過したら CPM 更新
        if now - self.window_start >= 1.0:
            self.current_cpm = self.window_count * 60
            self.window_count = 0
            self.window_start = now

        # 500ms ごとに送信フラグ ON
        should_send = False
        if now - self.last_send >= 0.5:
            self.last_send = now
            should_send = True

        return self.current_cpm, should_send


# ================================
# RAW HID Receiver (QMK → Layer)
# ================================
class RawHIDReceiver:
    """
    QMK キーボードから RAW HID でレイヤー状態だけ受信するクラス
    """

    def __init__(self, app):
        self.app = app
        self.device = None
        self.thread = None
        self.running = False

    def start(self):
        """RawHID デバイスを探して受信開始"""
        if not HID_AVAILABLE:
            print("[RAW HID] hidapi が無効")
            return

        try:
            dev = find_rawhid_device(
                vid=QMK_VENDOR_ID,
                pid=QMK_PRODUCT_ID
            )

            if dev is None:
                print("[RAW HID] RawHID デバイスが見つかりません")
                return

            self.device = hid.device()
            self.device.open_path(dev["path"])
            self.device.set_nonblocking(True)
            self.running = True

            self.thread = threading.Thread(
                target=self._loop,
                daemon=True
            )
            self.thread.start()

            print(
                f"[RAW HID] Opened: VID=0x{QMK_VENDOR_ID:04X}, "
                f"PID=0x{QMK_PRODUCT_ID:04X}, path={dev['path']}"
            )

        except Exception as e:
            print("[RAW HID] open error:", e)
            self.device = None
            self.running = False

    def stop(self):
        self.running = False
        if self.device:
            try:
                self.device.close()
            except Exception:
                pass
            self.device = None

    def _loop(self):
        READ_SIZE = 32

        while self.running and self.device:
            try:
                data = self.device.read(READ_SIZE)
            except Exception as e:
                print("[RAW HID] read error:", e)
                break

            if not data:
                time.sleep(0.005)
                continue

            cmd = data[0]
            if cmd == 0x02 and len(data) >= 2:
                layer = data[1]
                self.app.on_qmk_layer_change(layer)

            time.sleep(0.001)


# ================================
# RawHID device auto detection
# ================================
def find_rawhid_device(vid=None, pid=None):
    """
    RawHID デバイスを自動検出する
    """
    candidates = []

    for d in hid.enumerate():
        usage_page = d.get("usage_page", 0)
        usage = d.get("usage", 0)

        # RawHID 条件
        if not (0xFF00 <= usage_page <= 0xFFFF):
            continue
        if usage != 0x61:
            continue

        if vid is not None and d.get("vendor_id") != vid:
            continue
        if pid is not None and d.get("product_id") != pid:
            continue

        candidates.append(d)

    if len(candidates) == 1:
        return candidates[0]

    for d in candidates:
        name = (d.get("product_string") or "").lower()
        if "ete" in name or "qmk" in name:
            return d

    return candidates[0] if candidates else None


# ================================
# RawHID device auto detection_json
# ================================

CONFIG_FILE = "typing_bridge.json"

def load_last_ports():
    if not os.path.exists(CONFIG_FILE):
        return {"usb": None, "bt": None}
    try:
        with open(CONFIG_FILE, "r", encoding="utf-8") as f:
            d = json.load(f)
            return {
                "usb": d.get("usb", {}).get("last_port"),
                "bt":  d.get("bt", {}).get("last_port"),
            }
    except Exception:
        return {"usb": None, "bt": None}


def save_last_port(link: str, port: str):
    data = {"usb": {}, "bt": {}}

    if os.path.exists(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                data.update(json.load(f))
        except Exception:
            pass

    if link == "USB":
        data["usb"]["last_port"] = port
    elif link == "Bluetooth":
        data["bt"]["last_port"] = port

    with open(CONFIG_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)


def save_bt_identity(port):
    for p in list_ports.comports():
        if p.device != port:
            continue

        mac = extract_bt_mac(p)
        if not mac:
            return

        data = {}
        if os.path.exists(CONFIG_FILE):
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)

        data.setdefault("bt", {})
        data["bt"].update({
            "mac": mac,
            "name": p.description,
            "last_port": port
        })

        with open(CONFIG_FILE, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)

        print(f"[BT SAVE] {mac} @ {port}")

# ================================
# CPM Counter (QMK互換ロジック)
# ================================
class PCStatsCollector:
    """
    PC の状態をまとめて取得するクラス
    Core2 送信前提のデータ構造
    """

    def __init__(self):
        self.last_update = 0

    def collect(self):
        stats = {}

        # CPU
        stats["cpu_usage"] = int(psutil.cpu_percent(interval=None))

        # RAM
        mem = psutil.virtual_memory()
        stats["ram_usage"] = int(mem.percent)

        # Disk（C: 固定。将来変更可）
        try:
            disk = psutil.disk_usage("C:/")
            stats["disk_usage"] = int(disk.percent)
        except Exception:
            stats["disk_usage"] = 0

        # 温度（取れない環境も多い）
        stats["cpu_temp"] = None
        try:
            temps = psutil.sensors_temperatures()
            if temps:
                for name, entries in temps.items():
                    if entries:
                        stats["cpu_temp"] = int(entries[0].current)
                        break
        except Exception:
            pass

        return stats

class DiskIOMeter:
    """
    Disk I/O を 1秒差分で MB/s に変換する
    """
    def __init__(self):
        self.prev_read = None
        self.prev_write = None
        self.prev_time = None

    def update(self):
        now = time.time()
        io = psutil.disk_io_counters()

        # 初回は値を保存するだけ
        if self.prev_time is None:
            self.prev_read = io.read_bytes
            self.prev_write = io.write_bytes
            self.prev_time = now
            return 0.0, 0.0

        dt = now - self.prev_time
        if dt <= 0:
            return 0.0, 0.0

        read_mb_s = (io.read_bytes - self.prev_read) / 1024 / 1024 / dt
        write_mb_s = (io.write_bytes - self.prev_write) / 1024 / 1024 / dt

        self.prev_read = io.read_bytes
        self.prev_write = io.write_bytes
        self.prev_time = now

        return read_mb_s, write_mb_s


# ================================
# Serial port auto selection
# ================================

# Core2(ESP32)のUSBシリアルは環境により表記が変わるので、
# "Silicon Labs" / "CP210" / "CH340" / "USB-SERIAL" 等を広く拾う
USB_HINT_KEYWORDS = [
    "CP210", "SILICON LABS", "USB SERIAL", "USB-SERIAL", "CH340", "CH910", "FTDI", "UART"
]

BT_HINT_KEYWORDS = [
    "BLUETOOTH", "BTHENUM"
]

def list_ports_by_link_type(link_type: str):
    """
    link_type: "USB" or "Bluetooth"
    """
    ports = []
    for p in list_ports.comports():
        desc = (p.description or "").upper()
        hwid = (p.hwid or "").upper()

        is_bt = (
            "BLUETOOTH" in desc
            or "BTHENUM" in hwid
            or "RFCOMM" in hwid
            or "STANDARD SERIAL OVER BLUETOOTH" in desc
        )

        if link_type == "Bluetooth" and is_bt:
            ports.append(p.device)
        elif link_type == "USB" and not is_bt:
            ports.append(p.device)

    return ports



# ================================
# Tkinter GUI 本体
# ================================
class TypingMeterApp:
    def __init__(self, root):
        self.root = root
        self.root.title("Typingbridge")

        self.sender = SerialSender()
        self.cpm_counter = CPMCounter()
        self.rawhid = RawHIDReceiver(self)
        self.last_ports = load_last_ports()
        self._resume_reconnect_pending = False
        self._has_connected_once = False
        self._autoconnect_running = False
        self._autoconnect_anim_step = 0
        self._autoconnect_label_base = ""
        self.root.geometry("450x550")
        self._pressed_keys = set()

        self.pcstats = PCStatsCollector()

        self.cpu_var  = tk.StringVar(value="--")
        self.ram_var  = tk.StringVar(value="--")
        self.disk_var = tk.StringVar(value="--")
        self.temp_var = tk.StringVar(value="--")

        self.disk_io = DiskIOMeter()

        self.disk_r_var = tk.StringVar(value="0.0 MB/s")
        self.disk_w_var = tk.StringVar(value="0.0 MB/s")

        self.always_on_top = tk.BooleanVar(value=True)
        # 起動時に最前面設定を反映
        self.update_topmost()

        

        # ==== GUI 変数 ====
        self.cpm_var = tk.StringVar(value="0")
        self.layer_var = tk.IntVar(value=0)
        #self.mode_var = tk.IntVar(value=2)  # 0=CPM only, 1=Solenoid only, 2=Both
        self.layer_label_var = tk.StringVar(value="Layer: -")
    

        # 自動再接続関連
        # ✅ 前回成功したCOMを読み出し（なければNone）
        self.last_ports= load_last_ports()
        self.last_connect_try = 0.0
        self.RECONNECT_INTERVAL_SEC = 5.0

        # ==== UI レイアウト ====
        self._build_ui()

        # 接続方式選択
        self.link_mode = tk.StringVar(value="USB")

        mode_frame = ttk.LabelFrame(self.frm, text="Link Type", padding=5)
        mode_frame.grid(row=0, column=0, columnspan=4, sticky="w", pady=(0, 5))

        self.link_mode = tk.StringVar(value="USB")

        ttk.Radiobutton(
            mode_frame,
            text="USB",
            variable=self.link_mode,
            value="USB",
            command=self.on_link_mode_changed
        ).pack(side="left", padx=4)

        ttk.Radiobutton(
            mode_frame,
            text="Bluetooth",
            variable=self.link_mode,
            value="Bluetooth",
            command=self.on_link_mode_changed
        ).pack(side="left", padx=4)

        ttk.Checkbutton(
            mode_frame,
            text="Always on Top",
            variable=self.always_on_top,
            command=self.update_topmost
        ).pack(side="left", padx=(12, 0))


        # ✅ 起動時：COM候補から自動選択して Combobox を合わせる
        self._auto_select_port_into_combobox()

        # 起動時に Link Type に応じて COM を絞る
        self.on_link_mode_changed()



        # ==== RAW HID 受信開始（レイヤー連動）====
        self.rawhid.start()

        # ==== keyboard フックを別スレッドで開始 ====
        self._setup_keyboard_hook()

        # ==== 定期更新ループ ====
        self._tick()

        # 終了ハンドラ
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
    

    # -----------------------------
    # UI 部分
    # -----------------------------
    def _build_ui(self):
        self.frm = ttk.Frame(self.root, padding=5)
        self.frm.grid(row=0, column=0, sticky="nsew")


        # COMポート選択
        ttk.Label(self.frm, text="COM Port:").grid(row=1, column=0, sticky="w")
    

        self.port_cb = ttk.Combobox(
            self.frm,
            width=8,
            values=self._list_serial_ports(),
            state="readonly"
        )
        self.port_cb.grid(row=1, column=1, sticky="w", padx=4)
        if self.port_cb["values"]:
            self.port_cb.current(0)

        self.btn_refresh = ttk.Button(self.frm, text="Refresh", command=self.on_refresh_ports)
        self.btn_refresh.grid(row=1, column=2, sticky="w", padx=(1,1))

        self.btn_connect = ttk.Button(
            self.frm,
            text="Manual Connect",
            command=self.on_connect
        )
        self.btn_connect.grid(row=1, column=3, sticky="w", padx=(1,1))

        self.btn_autoconnect = ttk.Button(
            self.frm,
            text="Auto Connect",
            command=self.on_autoconnect
        )
        self.btn_autoconnect.grid(row=1, column=4, sticky="w", padx=(1,1))



        # ステータス + Auto reconnect チェック
        self.lbl_status = ttk.Label(self.frm, text="Disconnected")
        self.lbl_status.grid(row=2, column=0, columnspan=3, sticky="w", pady=(4, 10))

        
        # 接続種別表示
        self.link_var = tk.StringVar(value="Link: -")
        self.lbl_link = ttk.Label(self.frm, textvariable=self.link_var)
        self.lbl_link.grid(row=3, column=0, columnspan=4, sticky="w")

        # CPM 表示
        cpm_frame = ttk.LabelFrame(self.frm, text="CPM (QMK互換)", padding=10)
        cpm_frame.grid(row=4, column=0, columnspan=4, sticky="ew")

        ttk.Label(cpm_frame, text="Current CPM:").grid(row=0, column=0, sticky="w")
        lbl_cpm = ttk.Label(
            cpm_frame,
            textvariable=self.cpm_var,
            font=("Segoe UI", 18, "bold")
        )
        lbl_cpm.grid(row=0, column=1, sticky="w", padx=10)

        # 現在レイヤー表示
        ttk.Label(cpm_frame, textvariable=self.layer_label_var).grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(5, 0)
        )

        # レイヤー送信用（手動デバッグ）
        layer_frame = ttk.LabelFrame(self.frm, text="Layer (Manual send)", padding=10)
        layer_frame.grid(row=5, column=0, columnspan=4, sticky="ew", pady=(10, 0))

        ttk.Label(layer_frame, text="Layer:").grid(row=0, column=0, sticky="w")
        spn_layer = tk.Spinbox(
            layer_frame,
            from_=0,
            to=15,
            textvariable=self.layer_var,
            width=5
        )
        spn_layer.grid(row=0, column=1, sticky="w", padx=4)

        self.btn_send_layer = ttk.Button(
            layer_frame,
            text="Send Layer",
            command=self.on_send_layer
        )
        self.btn_send_layer.grid(row=0, column=2, sticky="w", padx=6)

        # Stretch
        for i in range(4):
            self.frm.columnconfigure(i, weight=1)
            self.root.rowconfigure(0, weight=1)
            self.root.columnconfigure(0, weight=1)

        # ---- Log ----
        log_frame = ttk.LabelFrame(self.frm, text="Log", padding=5)
        log_frame.grid(row=6, column=0, columnspan=5, sticky="ew", pady=(10, 0))

        self.log_box = ScrolledText(
            log_frame,
            height=8,
            state="disabled",
            font=("Consolas", 9)
        )
        self.log_box.pack(fill="both", expand=True)
        #-----PC Status---
        stats_frame = ttk.LabelFrame(self.frm, text="PC Status", padding=5)
        stats_frame.grid(row=7, column=0, columnspan=5, sticky="ew", pady=(8, 0))

        ttk.Label(stats_frame, text="CPU").grid(row=0, column=0, sticky="w")
        ttk.Label(stats_frame, textvariable=self.cpu_var).grid(row=0, column=1, sticky="w")

        ttk.Label(stats_frame, text="RAM").grid(row=1, column=0, sticky="w")
        ttk.Label(stats_frame, textvariable=self.ram_var).grid(row=1, column=1, sticky="w")

        ttk.Label(stats_frame, text="Disk U").grid(row=2, column=0, sticky="w")
        ttk.Label(stats_frame, textvariable=self.disk_var).grid(row=2, column=1, sticky="w")

        ttk.Label(stats_frame, text="Disk R").grid(row=3, column=0, sticky="w")
        ttk.Label(stats_frame, textvariable=self.disk_r_var).grid(row=3, column=1, sticky="w")

        ttk.Label(stats_frame, text="Disk W").grid(row=4, column=0, sticky="w")
        ttk.Label(stats_frame, textvariable=self.disk_w_var).grid(row=4, column=1, sticky="w")




        # Row weight（Log を伸ばす）
        self.frm.rowconfigure(6, weight=1)  # Log
        self.frm.rowconfigure(7, weight=0)  # PC Status

    def on_autoconnect(self):
        link = self.link_mode.get()
        self.log(f"AutoConnect ({link})")

        self._autoconnect_running = True
        self._autoconnect_anim_step = 0
        self._autoconnect_label_base = f"Searching Core2 ({link})"
        self._autoconnect_anim_tick()

        self.btn_autoconnect.config(state="disabled")

        threading.Thread(
            target=self._autoconnect_thread,
            daemon=True
        ).start()

       
    def _autoconnect_thread(self):
        try:
            link = self.link_mode.get()
            preferred = (
                self.last_ports.get("bt")
                if link == "Bluetooth"
                else self.last_ports.get("usb")
            )

            port, ser = auto_detect_core2_port(
                preferred_port=preferred,
                link_type=link
            )

            if not port:
                self.root.after(0, self._autoconnect_failed)
                return

            self.root.after(
                0,
                lambda: self._autoconnect_success(port, ser, link)
            )

        except Exception as e:
            self.log(f"AutoConnect error: {e}")
            self.root.after(0, self._autoconnect_failed)


    def _autoconnect_success(self, port, ser, link):
        self.log(f"success: {port}")
        self.port_cb.set(port)
        self.link_mode.set(link)

        if ser:
            self.sender.attach_existing_serial(
                ser,
                is_bluetooth=(link == "Bluetooth")
            )
            self._on_connected_ok(port)
        else:
            threading.Thread(
                target=self._connect_thread,
                args=(port, False),
                daemon=True
            ).start()

        self.btn_autoconnect.config(state="normal")
    
    
    def _autoconnect_failed(self):
        self._autoconnect_running = False
        self.log("failed: Core2 not found")
        self.lbl_status.config(text="Disconnected")
        self.btn_autoconnect.config(state="normal")

    
    def log(self, msg: str):
        ts = time.strftime("%H:%M:%S")
        self.log_box.configure(state="normal")
        self.log_box.insert("end", f"[{ts}] {msg}\n")
        self.log_box.see("end")
        self.log_box.configure(state="disabled")


    def _list_serial_ports(self):
        ports = []
        for p in list_ports.comports():
            ports.append(p.device)
        return ports
    

    def on_refresh_ports(self):
        self.on_link_mode_changed()


    def update_topmost(self):
        self.root.attributes(
            "-topmost",
            self.always_on_top.get()
        )

    
    def on_link_mode_changed(self):
        link = self.link_mode.get()

        ports = list_ports_by_link_type(link)
        self.port_cb["values"] = ports

        if not ports:
            self.port_cb.set("")
            self.lbl_status.config(text=f"No {link} COM ports found")
            return

        # 現在の選択が無効なら先頭に
        current = self.port_cb.get()
        if current not in ports:
            self.port_cb.set(ports[0])


    def _auto_select_port_into_combobox(self):
        ports = self._list_serial_ports()
        self.port_cb["values"] = ports
        if not ports:
            return

        # BT / USB に分ける
        bt_ports = []
        usb_ports = []

        for p in list_ports.comports():
            if p.device not in ports:
                continue
            desc = (p.description or "").upper()
            hwid = (p.hwid or "").upper()

            if "BLUETOOTH" in desc or "BTHENUM" in hwid:
                bt_ports.append(p.device)
            else:
                usb_ports.append(p.device)

        # 優先順位
        if bt_ports and self.last_ports["bt"] in bt_ports:
            self.port_cb.set(self.last_ports["bt"])
            return

        if usb_ports and self.last_ports["usb"] in usb_ports:
            self.port_cb.set(self.last_ports["usb"])
            return

        # fallback
        self.port_cb.set(bt_ports[0] if bt_ports else usb_ports[0])



    # -----------------------------
    # Connect / Disconnect (スレッド化)
    # -----------------------------
    def on_connect(self):
        if self.btn_connect.cget("text") == "Disconnect":
            self.sender.disconnect()
            self.lbl_status.config(text="Disconnected")
            self.link_var.set("Link: -")
            self.btn_connect.config(text="Connect")
            return

        port = self.port_cb.get().strip()
        if not port:
            messagebox.showwarning("Warning", "COMポートを選択してください。")
            return

        # ★ 手動接続 → JSONより優先
        threading.Thread(
            target=self._connect_thread,
            args=(port, True),
            daemon=True
        ).start()


    def _connect_thread(self, port: str | None, is_manual: bool):
        try:
            ser = None

            # auto-detect 経由の場合
            if not is_manual:
                result = auto_detect_core2_port(
                    preferred_port=port,
                    link_type=self.link_mode.get()
                )

                if not result:
                    print("[AUTO-DETECT] Core2 not found")
                    return

                # result は port だけ or (port, ser)
                if isinstance(result, tuple):
                    port, ser = result
                else:
                    port = result

            # ---- ここから接続 ----
            if ser:
                self.sender.attach_existing_serial(ser, is_bluetooth=True)
                self.root.after(0, self._on_connected_ok, port)
                return

            if not port:
                return   # ← ★ これがないと None open する

            self.sender.connect(port)
            self.root.after(0, self._on_connected_ok, port)

        except Exception as e:
            self.root.after(0, self._on_connected_fail, str(e), is_manual)

    
    def _on_connected_ok(self, port: str):
        link = self.sender.get_link_type()
        self._has_connected_once = True
        self.lbl_status.config(text=f"Connected: {port}")
        self.link_var.set(f"Link: {link}")
        self.btn_connect.config(text="Disconnect")
        self._autoconnect_running = False

        # ★ 成功した場合のみ保存
        save_last_port(link, port)

        self.last_ports = load_last_ports()

        if link == "Bluetooth":
            save_bt_identity(port)


    def _on_connected_fail(self, msg: str, is_manual: bool):
        if is_manual:
            self.lbl_status.config(text="Connect failed")
            messagebox.showerror("Error", f"接続に失敗しました:\n{msg}")
        else:
            # 自動再接続の場合はダイアログは出さず、ステータスだけ更新
            self.lbl_status.config(text=f"Connect failed (auto): {msg}")
        self._autoconnect_running = False
        self.lbl_status.config(text="Core2 not found")
 

    # 自動再接続チェック（_tick から呼ばれる）  

    def _on_resume_from_sleep(self):
        self.log("Resume from sleep")

        if not self._was_connected_before_sleep:
            return

        self._resume_reconnect_pending = True
        self.last_connect_try = 0

    def _set_status(self, text: str):
        self.lbl_status.config(text=text)
   
    def _autoconnect_anim_tick(self):
        if not self._autoconnect_running:
            return

        dots = "." * (self._autoconnect_anim_step % 4)
        self.lbl_status.config(
            text=f"{self._autoconnect_label_base}{dots}"
        )

        self._autoconnect_anim_step += 1
        self.root.after(400, self._autoconnect_anim_tick)


    def _auto_reconnect_check(self):
        if self._autoconnect_running:
            return
        
        # ★ すでに接続中なら何もしない（USBも含む）
        if self.sender.is_connected():
            return

        if not self._has_connected_once:
            return

        link = self.link_mode.get()

        # ★ USB は「接続確認を甘くしない」
        if link == "Bluetooth" and self.sender.is_connected():
            return

        now = time.time()
        if now - self.last_connect_try < self.RECONNECT_INTERVAL_SEC:
            return

        self.last_connect_try = now

        preferred = (
            self.last_ports.get("bt") if link == "Bluetooth"
            else self.last_ports.get("usb")
        )

        self._autoconnect_running = True
        self._autoconnect_anim_step = 0
        self._autoconnect_label_base = f"Searching Core2 ({link})"
        self._autoconnect_anim_tick()

        self.log(f"AutoReconnect ({link})")

        port, ser = auto_detect_core2_port(
            preferred_port=preferred,
            link_type=link
        )

        if not port:
            self._autoconnect_running = False
            self.lbl_status.config(text="Core2 not found")
            self.log("AutoReconnect failed")
            return

        if ser:
            self.sender.attach_existing_serial(
                ser,
                is_bluetooth=(link == "Bluetooth")
            )
            self.root.after(0, self._on_connected_ok, port)
            return

        threading.Thread(
            target=self._connect_thread,
            args=(port, False),
            daemon=True
        ).start()

    

    # -----------------------------
    # レイヤー送信 / Solenoid テスト
    # -----------------------------
    def on_send_layer(self):
        if not self.sender.is_connected():
            messagebox.showinfo("Info", "未接続です。先に Connect してください。")
            return
        layer = self.layer_var.get()
        self.sender.send_layer(layer)

    # -----------------------------
    # RAW HID からのレイヤー変更イベント
    # -----------------------------
    def on_qmk_layer_change(self, layer: int):
        """RawHID Receiver スレッドから呼ばれる → main thread に渡して処理"""

        def _update():
            self.layer_var.set(layer)
            self.layer_label_var.set(f"Layer: {layer}")
            if self.sender.is_connected():
                self.sender.send_layer(layer)

        # Tkinter メインスレッドで処理
        self.root.after(0, _update)

    # -----------------------------
    # グローバルキーフック (別スレッド) - CPM & ソレノイド
    # -----------------------------
    def _setup_keyboard_hook(self):
        def hook_thread():
            def on_key(event):
                key = (event.name or "").lower()

                if event.event_type == "down":
                    # すでに押されている → OSリピートなので無視
                    if key in self._pressed_keys:
                        return

                    # 初回押下だけカウント
                    self._pressed_keys.add(key)
                    self.cpm_counter.on_key_press()

                    # Solenoid
                    if self.sender.is_connected():
                        strong_keys = {"enter", "space", "backspace", "delete", "tab"}
                        if key in strong_keys:
                            self.sender.solenoid_strong()
                        else:
                            self.sender.solenoid_light()

                elif event.event_type == "up":
                    # キーを離したら解除
                    self._pressed_keys.discard(key)

            keyboard.hook(on_key)

        threading.Thread(target=hook_thread, daemon=True).start()


    # -----------------------------
    # 定期更新 (CPM計算 & 送信 + 自動再接続)
    # -----------------------------
    def _tick(self):
        now = time.time()

        # ---- PC Stats 更新（1秒に1回） ----
        if not hasattr(self, "_last_pcstats"):
            self._last_pcstats = 0

        if now - self._last_pcstats >= 1.0:
            self._last_pcstats = now

            stats = self.pcstats.collect()

            self.cpu_var.set(f"{stats['cpu_usage']} %")
            self.ram_var.set(f"{stats['ram_usage']} %")
            self.disk_var.set(f"{stats['disk_usage']} %")

            # 🔽 Disk I/O
            r_mb, w_mb = self.disk_io.update()
            self.disk_r_var.set(f"{r_mb:.1f} MB/s")
            self.disk_w_var.set(f"{w_mb:.1f} MB/s")

            if stats["cpu_temp"] is None:
                self.temp_var.set("--")
            else:
                self.temp_var.set(f"{stats['cpu_temp']} °C")

             # ★ Core2 へ送信
            self.sender.send_pc_status(stats, r_mb, w_mb)


       # CPM ロジックを更新（QMK互換）
        cpm, should_send = self.cpm_counter.update()

        # GUI表示更新
        self.cpm_var.set(str(int(cpm)))

        # CPM 送信
        self.sender.send_cpm(int(cpm))

        # 自動再接続チェック
        self._auto_reconnect_check()

        # 次回呼び出し
        self.root.after(50, self._tick)

    # -----------------------------
    # 終了処理
    # -----------------------------
    def on_close(self):
        try:
            keyboard.unhook_all()
        except Exception:
            pass
        self.rawhid.stop()
        self.sender.disconnect()
        self.root.destroy()


# ================================
# エントリポイント
# ================================
def main():
    root = tk.Tk()
    app = TypingMeterApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()