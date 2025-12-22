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

# ---------------------------------------------------------
# シリアル (Core2 通信用)
# ---------------------------------------------------------
try:
    import serial
    import serial.tools.list_ports as list_ports
except ImportError:
    print("pyserial が必要です: pip install pyserial")
    sys.exit(1)

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
QMK_VENDOR_ID = 0xFEED
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

    def connect(self, port: str, baudrate: int = 115200):
        """ブロッキングなので、必ず別スレッドから呼ぶこと"""
        with self.lock:
            if self.ser and self.ser.is_open:
                self.ser.close()
                self.ser = None

            # timeout / write_timeout をゼロにして完全ノンブロッキング寄りに
            self.ser = serial.Serial(
                port,
                baudrate=baudrate,
                timeout=0,
                write_timeout=0
            )

    def disconnect(self):
        with self.lock:
            if self.ser and self.ser.is_open:
                self.ser.close()
            self.ser = None

    def is_connected(self) -> bool:
        return self.ser is not None and self.ser.is_open

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
        """0x10 : 軽いソレノイド（Core2側で 0x10 を受け取ったら軽く鳴らす）"""
        if not self.is_connected():
            return
        self._write(bytes([0x10]))

    def solenoid_strong(self):
        """0x11 : 強いソレノイド（Core2側で 0x11 を受け取ったら強く鳴らす）"""
        if not self.is_connected():
            return
        self._write(bytes([0x11]))


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
    QMK キーボードから RAW HID でレイヤー状態だけ受信するクラス。

    想定プロトコル (keymap.c 側):
      void send_layer_usb(uint8_t layer) {
          uint8_t data[32] = {0};
          data[0] = 0x02;
          data[1] = layer;
          raw_hid_send(data, sizeof(data));
      }

      → Python 側:
         data[0] = 0x02
         data[1] = layer
    """

    def __init__(self, app):
        self.app = app
        self.device = None
        self.thread = None
        self.running = False

    def start(self):
        """デバイスを開いて受信スレッドを開始（RawHID 専用）"""
        if not HID_AVAILABLE:
            print("[RAW HID] hidapi が無いので無効です。")
            return

        try:
            dev = None

            # ---- Raw HID の Usage Page/Usage を持つ正しいインターフェイスを探す ----
            for d in hid.enumerate():
                # 必要ならデバッグ表示:
                # print(d)
                if (
                    d.get("vendor_id") == QMK_VENDOR_ID
                    and d.get("product_id") == QMK_PRODUCT_ID
                    and d.get("usage_page") == 0xFF00
                    and d.get("usage") == 0x61
                ):
                    dev = d
                    break

            if dev is None:
                print("[RAW HID] 対応する RawHID デバイスが見つかりません。")
                return

            # ---- 見つかった Raw HID インターフェイスを開く ----
            self.device = hid.device()
            self.device.open_path(dev["path"])
            self.device.set_nonblocking(True)
            self.running = True

            # ---- 別スレッドで受信ループを開始 ----
            t = threading.Thread(target=self._loop, daemon=True)
            t.start()
            self.thread = t

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
        if self.device is not None:
            try:
                self.device.close()
            except Exception:
                pass
            self.device = None

    def _loop(self):
        """RAW HID を常時読み続けるループ（別スレッド）"""
        READ_SIZE = 32  # QMK RawHID は 32 バイトが多い

        while self.running and self.device is not None:
            try:
                data = self.device.read(READ_SIZE)
            except Exception as e:
                print("[RAW HID] read error:", e)
                break

            if not data:
                # ノーデータ → 少し待機
                time.sleep(0.005)
                continue

            cmd = data[0]

            if cmd == 0x02 and len(data) >= 2:
                # レイヤー変更通知
                layer = data[1]
                # Tkinter はメインスレッドだけ触れるので after で投げる
                self.app.on_qmk_layer_change(layer)

            # それ以外の cmd は今は未使用 (拡張余地)
            time.sleep(0.001)


# ================================
# Tkinter GUI 本体
# ================================
class TypingMeterApp:
    def __init__(self, root):
        self.root = root
        self.root.title("Typing Meter & Solenoid Controller (Hybrid)")

        self.sender = SerialSender()
        self.cpm_counter = CPMCounter()
        self.rawhid = RawHIDReceiver(self)

        # ==== GUI 変数 ====
        self.cpm_var = tk.StringVar(value="0")
        self.layer_var = tk.IntVar(value=0)
        self.mode_var = tk.IntVar(value=2)  # 0=CPM only, 1=Solenoid only, 2=Both
        self.layer_label_var = tk.StringVar(value="Layer: -")

        # 自動再接続関連
        self.auto_reconnect = tk.BooleanVar(value=True)
        self.last_port = None
        self.last_connect_try = 0.0
        self.RECONNECT_INTERVAL_SEC = 5.0

        # ==== UI レイアウト ====
        self._build_ui()

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
        frm = ttk.Frame(self.root, padding=10)
        frm.grid(row=0, column=0, sticky="nsew")

        # COMポート選択
        ttk.Label(frm, text="COM Port:").grid(row=0, column=0, sticky="w")

        self.port_cb = ttk.Combobox(
            frm,
            width=20,
            values=self._list_serial_ports(),
            state="readonly"
        )
        self.port_cb.grid(row=0, column=1, sticky="w", padx=4)
        if self.port_cb["values"]:
            self.port_cb.current(0)

        self.btn_refresh = ttk.Button(frm, text="Refresh", command=self.on_refresh_ports)
        self.btn_refresh.grid(row=0, column=2, sticky="w", padx=4)

        self.btn_connect = ttk.Button(frm, text="Connect", command=self.on_connect)
        self.btn_connect.grid(row=0, column=3, sticky="w", padx=4)

        # ステータス + Auto reconnect チェック
        self.lbl_status = ttk.Label(frm, text="Disconnected")
        self.lbl_status.grid(row=1, column=0, columnspan=3, sticky="w", pady=(4, 10))

        self.chk_autorc = ttk.Checkbutton(
            frm,
            text="Auto reconnect",
            variable=self.auto_reconnect
        )
        self.chk_autorc.grid(row=1, column=3, sticky="e", pady=(4, 10))

        # CPM 表示
        cpm_frame = ttk.LabelFrame(frm, text="CPM (QMK互換)", padding=10)
        cpm_frame.grid(row=2, column=0, columnspan=4, sticky="ew")

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

        # モード選択 (CPM / Solenoid / Both)
        mode_frame = ttk.LabelFrame(frm, text="Mode", padding=10)
        mode_frame.grid(row=3, column=0, columnspan=4, sticky="ew", pady=(10, 0))

        tk.Radiobutton(
            mode_frame, text="CPM only", variable=self.mode_var, value=0
        ).grid(row=0, column=0, sticky="w")
        tk.Radiobutton(
            mode_frame, text="Solenoid only", variable=self.mode_var, value=1
        ).grid(row=0, column=1, sticky="w")
        tk.Radiobutton(
            mode_frame, text="Both", variable=self.mode_var, value=2
        ).grid(row=0, column=2, sticky="w")

        # レイヤー送信用（手動デバッグ）
        layer_frame = ttk.LabelFrame(frm, text="Layer (Manual send)", padding=10)
        layer_frame.grid(row=4, column=0, columnspan=4, sticky="ew", pady=(10, 0))

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

        # Solenoid テスト用ボタン
        sol_frame = ttk.LabelFrame(frm, text="Solenoid Test", padding=10)
        sol_frame.grid(row=5, column=0, columnspan=4, sticky="ew", pady=(10, 0))

        self.btn_light = ttk.Button(sol_frame, text="Light", command=self.on_sol_light)
        self.btn_light.grid(row=0, column=0, padx=4)

        self.btn_strong = ttk.Button(sol_frame, text="Strong", command=self.on_sol_strong)
        self.btn_strong.grid(row=0, column=1, padx=4)

        # Stretch
        for i in range(4):
            frm.columnconfigure(i, weight=1)
        self.root.rowconfigure(0, weight=1)
        self.root.columnconfigure(0, weight=1)

    def _list_serial_ports(self):
        ports = []
        for p in list_ports.comports():
            ports.append(p.device)
        return ports

    def on_refresh_ports(self):
        self.port_cb["values"] = self._list_serial_ports()
        if self.port_cb["values"]:
            self.port_cb.current(0)

    # -----------------------------
    # Connect / Disconnect (スレッド化)
    # -----------------------------
    def on_connect(self):
        if self.sender.is_connected():
            # Disconnect（ユーザーの明示操作 → auto reconnect は一旦止める）
            self.sender.disconnect()
            self.lbl_status.config(text="Disconnected")
            self.btn_connect.config(text="Connect")
            self.last_port = None  # 手動で切ったら自動再接続は抑止
            return

        port = self.port_cb.get().strip()
        if not port:
            messagebox.showwarning("Warning", "COMポートを選択してください。")
            return

        # ★ 接続処理は別スレッドで実行（GUIフリーズ防止）
        threading.Thread(
            target=self._connect_thread,
            args=(port, True),  # True = manual
            daemon=True
        ).start()

    def _connect_thread(self, port: str, is_manual: bool):
        try:
            self.sender.connect(port)
            # GUI 更新は main thread で行う必要があるので、afterで投げる
            self.root.after(0, lambda: self._on_connected_ok(port))
        except Exception as e:
            self.root.after(0, lambda: self._on_connected_fail(str(e), is_manual))

    def _on_connected_ok(self, port: str):
        self.lbl_status.config(text=f"Connected: {port}")
        self.btn_connect.config(text="Disconnect")
        self.last_port = port
        # 成功したので、次回以降の自動再接続の起点を今にしておく
        self.last_connect_try = time.time()

    def _on_connected_fail(self, msg: str, is_manual: bool):
        if is_manual:
            self.lbl_status.config(text="Connect failed")
            messagebox.showerror("Error", f"接続に失敗しました:\n{msg}")
        else:
            # 自動再接続の場合はダイアログは出さず、ステータスだけ更新
            self.lbl_status.config(text=f"Connect failed (auto): {msg}")

    # 自動再接続チェック（_tick から呼ばれる）
    def _auto_reconnect_check(self):
        if not self.auto_reconnect.get():
            return
        if self.sender.is_connected():
            return
        if not self.last_port:
            return

        now = time.time()
        # 前回トライから一定時間経過していなければスキップ
        if now - self.last_connect_try < self.RECONNECT_INTERVAL_SEC:
            return

        self.last_connect_try = now
        print(f"[AUTO-RECONNECT] trying to reconnect to {self.last_port}")
        threading.Thread(
            target=self._connect_thread,
            args=(self.last_port, False),  # False = auto
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

    def on_sol_light(self):
        self.sender.solenoid_light()

    def on_sol_strong(self):
        self.sender.solenoid_strong()

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
                # キーDOWNのみ
                if event.event_type != "down":
                    return

                mode = self.mode_var.get()

                # CPM カウント（keyboard フックベース）
                if mode in (0, 2):
                    self.cpm_counter.on_key_press()

                # Solenoid
                if mode in (1, 2) and self.sender.is_connected():
                    name = (event.name or "").lower()

                    strong_keys = {"enter", "space", "backspace", "delete", "tab"}
                    if name in strong_keys:
                        self.sender.solenoid_strong()
                    else:
                        self.sender.solenoid_light()

            # ここはブロッキングなので、必ず「別スレッド」で呼ぶ
            keyboard.hook(on_key)

        t = threading.Thread(target=hook_thread, daemon=True)
        t.start()

    # -----------------------------
    # 定期更新 (CPM計算 & 送信 + 自動再接続)
    # -----------------------------
    def _tick(self):
        # CPM ロジックを更新（QMK互換）
        cpm, should_send = self.cpm_counter.update()

        # GUI表示更新
        self.cpm_var.set(str(int(cpm)))

        # CPM 送信
        if should_send and self.sender.is_connected():
            mode = self.mode_var.get()
            if mode in (0, 2):  # CPMを送るモード
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
