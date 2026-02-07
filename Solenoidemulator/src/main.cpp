#include <M5Unified.h>
#include <Wire.h>
#include <Preferences.h>
#include <BluetoothSerial.h>

#define I2C_ADDRESS 0x20

// ==== Battery status ====
uint8_t batteryPct   = 0;
float   batteryVolt  = 0.0f;
bool    batteryChg   = false;
uint32_t lastBattMs  = 0;
const uint32_t BATT_UPDATE_MS = 2000;  // 2秒に1回
static int  lastBatteryPct = -1;
static bool lastBatteryChg = false;
static bool batteryDirty   = true;  // 初回描画用




Preferences prefs;
BluetoothSerial SerialBT;

// ==== 通信ソース種別 ====
enum CommSource : uint8_t {
  SRC_NONE = 0,
  SRC_USB  = 1,
  SRC_BT   = 2,
  SRC_I2C  = 3,
};

// ==== 起動時モード種別 ====
enum AppMode : uint8_t {
  MODE_NONE   = 0,
  MODE_USB_BT = 1,
  MODE_I2C    = 2,
  MODE_DEMO   = 3,
};

volatile uint8_t activeSource   = SRC_NONE;  // 現在の入力ソース種別
AppMode          appMode        = MODE_NONE; // 起動時に選ぶモード

// ==== 状態管理 ====
volatile bool     triggerPending = false;   // I2C用トリガフラグ（USB/BTでは使用しない）
volatile uint32_t lastReceiveUs  = 0;
volatile uint8_t  lastCmd        = 0;

bool     configMode      = false;
uint32_t touchStart      = 0;
uint32_t configEntryTime = 0;                 // 設定モード突入時刻
const uint32_t CONFIG_INPUT_DELAY = 500;      // 設定モード切替直後の無視時間(ms)

// ==== 設定値 ====
// ※ デフォルト値（初期起動時の安全寄り設定）
uint8_t vibStrength = 180;
bool    vibEnabled  = true;
float   toneBase    = 4000.0f;  // UIで3500〜7000の範囲
uint8_t soundVolume = 80;       // ソレノイド音量(🔴スピーカー保護のリミッターMAX80)

// ==== サウンドバッファ ====
static int16_t clickBuffer[200];

// ==== 直近の発火時刻（ms）…高速連打判定用 ====
uint32_t lastFireMs = 0;

// ==== USB/BT 受信用ステートマシン（CPM/Layerパケットを無視するため） ====
// 0x01: [0x01][LSB][MSB]  CPM
// 0x02: [0x02][layer]     Layer
// 0x10: Light solenoid    (ここでだけソレノイド発火)
// 0x11: Strong solenoid   (同上)
static uint8_t usb_state = 0;

// ======================================================
// 設定保存/読込
// ======================================================
void saveConfig() {
  prefs.begin("solenoid", false);
  prefs.putUChar("vib",  vibStrength);
  prefs.putBool("vibOn", vibEnabled);
  prefs.putFloat("tone", toneBase);
  prefs.putUChar("vol",  soundVolume);
  prefs.end();
}

void loadConfig() {
  prefs.begin("solenoid", true);
  vibStrength  = prefs.getUChar("vib", 180);
  vibEnabled   = prefs.getBool("vibOn", true);
  toneBase     = prefs.getFloat("tone", 4000.0f);  // デフォルトも4000Hzに揃える
  soundVolume  = prefs.getUChar("vol", 80);        // デフォルト80

  // 🔴 安全リミッタ（古い設定が残っていても 80 を超えないように）
  if (soundVolume > 80) soundVolume = 80;

  // toneBaseも範囲内に収める（UIと同じ3500〜7000）
  toneBase = constrain(toneBase, 3500.0f, 7000.0f);

  prefs.end();
}

// ======================================================
// 金属クリック波形生成
// ======================================================
void makeClickWave() {
  const int   sampleRate = 16000;
  const float baseFreq   = constrain(toneBase, 3500.0f, 7000.0f);  // 安全レンジ
  const float decay      = 0.998f;
  const float lowFreq    = 250.0f;
  const float mix        = 0.30f;
  const int   samples    = 160;

  float phase1 = 0.0f, phase2 = 0.0f;
  for (int i = 0; i < samples; i++) {
    float env = powf(decay, i);
    float sig = sinf(phase1) * env + sinf(phase2) * env * mix;
    clickBuffer[i] = (int16_t)(sig * 30000);
    phase1 += 2.0f * PI * baseFreq / sampleRate;
    phase2 += 2.0f * PI * lowFreq  / sampleRate;
  }
}

// ======================================================
// 音再生（非ブロッキング）
// ======================================================
inline void playClick() {
  M5.Speaker.stop();
  makeClickWave();
  M5.Speaker.setVolume(soundVolume);  // 既に80上限で制限済み
  M5.Speaker.playRaw(
      clickBuffer,
      sizeof(clickBuffer) / sizeof(int16_t),
      16000,   // sample rate
      true,    // stereo LR
      1        // pitch
  );
}

// ======================================================
// バイブレーション
// ======================================================
// ---- Vibration pulse (non-blocking) ----
static bool     vibPulsing = false;
static uint32_t vibOffAtUs = 0;

inline void startVibrationPulseUs(uint32_t duration_us) {
  if (!vibEnabled) return;
  M5.Power.setVibration(vibStrength);
  vibPulsing = true;
  vibOffAtUs = micros() + duration_us;
}

inline void updateVibrationPulse() {
  if (!vibPulsing) return;
  // micros() はオーバーフローするけど差分判定ならOK
  if ((int32_t)(micros() - vibOffAtUs) >= 0) {
    M5.Power.setVibration(0);
    vibPulsing = false;
  }
}

inline void pulseVibrationFast() {
  startVibrationPulseUs(45000); 
}

// ======================================================
// ソレノイド描画
// ======================================================
void drawSolenoid(int pos) {
  int baseX  = 60;
  int baseY  = 140;
  int width  = 200;
  int height = 30;

  M5.Display.fillRect(baseX - 10, baseY - 10,
                      width + 20, height + 20, BLACK);
  M5.Display.drawRect(baseX, baseY, width, height, ORANGE);

  int springStartX = baseX + 35 + pos;
  int springEndX   = baseX + width - 5;
  int springY1     = baseY + 5;
  int springY2     = baseY + height - 5;
  uint16_t springColor = M5.Display.color565(150, 150, 150);
  int springPitch = map(pos, 0, 20, 10, 5);

  for (int i = springStartX; i < springEndX; i += springPitch) {
    int x1 = i;
    int x2 = i + springPitch / 2;
    bool up = ((i / springPitch) % 2 == 0);
    M5.Display.drawLine(x1, up ? springY1 : springY2,
                        x2, up ? springY2 : springY1,
                        springColor);
  }

  // ピストン
  M5.Display.fillRect(baseX + pos + 2, baseY + 2,
                      35, height - 4, RED);
  // ストッパー
  M5.Display.fillRect(baseX + width - 4, baseY,
                      4, height, YELLOW);
}

// ======================================================
// 通信状態インジケータ
// ======================================================
void drawCommIndicator() {
  int cx = 310;
  int cy = 12;
  int r  = 6;

  uint16_t color = RED;
  switch (activeSource) {
    case SRC_USB: color = GREEN;  break;   // USB
    case SRC_BT:  color = CYAN;   break;   // Bluetooth
    case SRC_I2C: color = YELLOW; break;   // I2C
    case SRC_NONE:
    default:      color = RED;    break;   // 未接続/デモ
  }

  M5.Display.fillCircle(cx, cy, r + 2, BLACK);
  M5.Display.fillCircle(cx, cy, r, color);
}

// ======================================================
// ⭐ ソレノイド ステートマシン
//   - NORMAL: ピストンアニメ＋2段目クリック
//   - FAST  : 音だけ2段クリック（10ms 間隔）
// ======================================================

enum SolenoidState {
  SOL_STATE_IDLE = 0,
  SOL_STATE_NORMAL_FORWARD,
  SOL_STATE_NORMAL_BACK,
  SOL_STATE_FAST_CLICK1,
  SOL_STATE_FAST_CLICK2,
};

SolenoidState solState      = SOL_STATE_IDLE;
int           solPos        = 0;         // 0 ～ 15
uint32_t      solLastStepMs = 0;

// パラメータ
const int SOL_NORMAL_STEP_INTERVAL_MS = 3;   // ピストン1ステップ(描画)の間隔
const int SOL_FAST_GAP_MS             = 16;  // 2段クリック間のギャップ
const int FAST_THRESHOLD_MS           = 30;  // これより短い間隔なら FAST モード

// NORMAL モード開始（ピストンアニメ＋2段目クリック）
void startNormalSolenoid() {
  solPos  = 0;
  drawSolenoid(solPos);
  solState      = SOL_STATE_NORMAL_FORWARD;
  solLastStepMs = millis();
}

// FAST モード開始（音だけ2段クリック / 描画なし）
void startFastSolenoid() {
  // 1発目を即時鳴らす
  playClick();
  pulseVibrationFast();
  solState      = SOL_STATE_FAST_CLICK1;
  solLastStepMs = millis();
}

// ステートマシン更新（loop() から毎フレーム呼ぶ）
void updateSolenoid() {
  uint32_t now = millis();

  switch (solState) {
    case SOL_STATE_IDLE:
      // 何もしない
      break;

    case SOL_STATE_NORMAL_FORWARD:
      if (now - solLastStepMs >= SOL_NORMAL_STEP_INTERVAL_MS) {
        solLastStepMs += SOL_NORMAL_STEP_INTERVAL_MS;
        solPos += 5;
        if (solPos >= 15) {
          solPos = 15;
          // ピストンが奥に到達したタイミングで 2段目クリック
          playClick();
          pulseVibrationFast();
          solState = SOL_STATE_NORMAL_BACK;
        }
        drawSolenoid(solPos);
      }
      break;

    case SOL_STATE_NORMAL_BACK:
      if (now - solLastStepMs >= SOL_NORMAL_STEP_INTERVAL_MS) {
        solLastStepMs += SOL_NORMAL_STEP_INTERVAL_MS;
        solPos -= 5;
        if (solPos <= 0) {
          solPos  = 0;
          solState = SOL_STATE_IDLE;
        }
        drawSolenoid(solPos);
      }
      break;

    case SOL_STATE_FAST_CLICK1:
      // 1発目 → 10ms 後に2発目
      if (now - solLastStepMs >= SOL_FAST_GAP_MS) {
        playClick();
        pulseVibrationFast();
        solState      = SOL_STATE_FAST_CLICK2;
        solLastStepMs = now;
      }
      break;

    case SOL_STATE_FAST_CLICK2:
      // 2発目 → 10ms 後に完全終了
      if (now - solLastStepMs >= SOL_FAST_GAP_MS) {
        solState = SOL_STATE_IDLE;
      }
      break;
  }
}

// 旧API相当ラッパ（NORMAL モード起動）
inline void solenoidEffect() {
  // 先行クリック
  playClick();
  pulseVibrationFast();
  // ピストンアニメ＋2段目クリック
  startNormalSolenoid();
}

// 設定モード等で使う、音だけ高速2段クリック
inline void solenoidFastClick() {
  startFastSolenoid();
}
//ソレノイド起動用の共通関数を作る
inline void fireSolenoidByTiming() {
  uint32_t nowMs   = millis();
  uint32_t deltaMs = nowMs - lastFireMs;
  lastFireMs       = nowMs;

  if (deltaMs < FAST_THRESHOLD_MS) {
    startFastSolenoid();
   //solenoidEffect();
  } else {
    solenoidEffect();
  }
}

// ======================================================
// I2C受信 ISR
// I2C モードのときのみ登録する
// ======================================================
volatile bool solenoidRequest = false;
volatile uint32_t solenoidRequestTime = 0;
// I2C受信 → 依頼だけ積む（ISR/コールバック内は軽く）
volatile uint8_t solenoidReqCount = 0;

// ---- I2C trigger gate ----
volatile bool     solenoidPending = false;
volatile uint32_t lastI2CUs = 0;

void onReceiveEvent(int numBytes) {
  if (numBytes <= 0) return;

  uint8_t cmd = Wire.read();

  
  // 余分なバイトが来ても詰まらないように捨てる
  while (Wire.available()) (void)Wire.read();

  lastCmd        = cmd;
  lastReceiveUs  = micros();
  activeSource   = SRC_I2C;

  if (cmd != 0x10) return;   // ★ Light solenoid 以外は無視
    if (solenoidReqCount < 10) solenoidReqCount++; // キュー（上限付き）
  uint32_t nowMs   = millis();
  uint32_t deltaMs = nowMs - lastFireMs;
  lastFireMs       = nowMs;
  activeSource = SRC_I2C;
  solenoidRequest = true;
  solenoidRequestTime = millis();  // ISR でも OK（読むだけ）
}

// ======================================================
// 設定UI
// ======================================================
void drawConfigUI() {
  const int offsetY = -15;
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(ORANGE);
  M5.Display.setCursor(20, 20);
  M5.Display.println("SETTINGS MODE");

  M5.Display.setTextColor(WHITE);
  // Vibration
  M5.Display.setCursor(20, 60 + offsetY);
  M5.Display.printf("Vibration: %s (%d)", vibEnabled ? "ON" : "OFF", vibStrength);
  M5.Display.drawRect(20, 85 + offsetY, 220, 20, BLUE);
  M5.Display.fillRect(20, 85 + offsetY,
                      map(vibStrength, 0, 255, 0, 220), 20, CYAN);

  // Tone (3500〜7000)
  M5.Display.setCursor(20, 130 + offsetY);
  M5.Display.printf("Tone: %.0f Hz", toneBase);
  M5.Display.drawRect(20, 155 + offsetY, 220, 20, GREEN);
  M5.Display.fillRect(20, 155 + offsetY,
                      map((int)toneBase, 3500, 7000, 0, 220), 20, GREENYELLOW);

  // Volume（★ 最大値80に対応）
  M5.Display.setCursor(20, 200 + offsetY);
  M5.Display.printf("Volume: %d", soundVolume);
  M5.Display.drawRect(20, 225 + offsetY, 220, 15, RED);
  M5.Display.fillRect(
      20, 225 + offsetY,
      map(soundVolume, 0, 80, 0, 220),
      15, ORANGE);

  // 上に通信インジケータも表示
  drawCommIndicator();
}

// ======================================================
// 設定操作（入力遅延付き）
// ======================================================
void handleConfigTouch() {
  // モード突入直後は誤タッチ防止で無視
  if (millis() - configEntryTime < CONFIG_INPUT_DELAY) return;

  if (M5.Touch.getCount() == 0) return;
  auto t = M5.Touch.getDetail(0);
  const int offsetY = -15;

  if (t.isPressed()) {
    // Vibration slider
    if (t.y > 80 + offsetY && t.y < 115 + offsetY) {
      vibStrength = constrain(map(t.x, 20, 240, 0, 255), 0, 255);
      M5.Power.setVibration(vibStrength);
      drawConfigUI();
    }
    // Tone slider（3500〜7000Hz）
    else if (t.y > 150 + offsetY && t.y < 185 + offsetY) {
      toneBase = constrain(map(t.x, 20, 240, 3500, 7000), 3500.0f, 7000.0f);
      drawConfigUI();
      solenoidFastClick();
    }
    // Volume slider（最大80）
    else if (t.y > 220 + offsetY && t.y < 245 + offsetY) {
      soundVolume = constrain(map(t.x, 20, 240, 0, 80), 0, 80);
      M5.Speaker.setVolume(soundVolume);
      drawConfigUI();
      solenoidFastClick();
    }
  }

  if (t.wasReleased()) {
    M5.Power.setVibration(0);
    // 画面下端タップで設定終了
    if (t.y > 245 + offsetY) {
      configMode = false;
      saveConfig();
      M5.Display.fillScreen(BLACK);
      M5.Display.setTextSize(2);
      M5.Display.setTextColor(ORANGE);
      M5.Display.setCursor(0, 30);
      M5.Display.println("Solenoid Emulator");
      M5.Display.setTextSize(1);
      M5.Display.setTextColor(WHITE);
      M5.Display.println("Hold screen to open settings");
      drawSolenoid(0);
      drawCommIndicator();
      batteryDirty = true;
    }
  }

  // Vib ON/OFF トグル（ラベル部分タップ）
  if (t.wasPressed() && t.y > 40 + offsetY && t.y < 70 + offsetY) {
    vibEnabled = !vibEnabled;
    drawConfigUI();
    solenoidFastClick();
  }
}

// ======================================================
// 設定モード切替（長押し）
// ======================================================
void checkTouchToConfig() {
  if (M5.Touch.getCount() > 0) {
    auto t = M5.Touch.getDetail(0);
    if (!configMode && t.wasPressed()) {
      touchStart = millis();
    }
    if (!configMode && t.isPressed() && (millis() - touchStart > 800)) {
      configMode      = true;
      configEntryTime = millis();
      drawConfigUI();
      batteryDirty = true;
    }
  } else {
    touchStart = 0;
  }
}

// ======================================================
// USB / BT 受信バイト 1個を処理するステートマシン
//   - 0x10 / 0x11 → ソレノイド発火
//   - 0x01 / 0x02 → 以降の LSB/MSB/Layer を読み捨て（ソレノイドは鳴らさない）
// ======================================================
void handleSerialByte(uint8_t b, CommSource src) {
  activeSource = src;  // インジケータ更新用

  switch (usb_state) {
    // ---- ヘッダ待ち ----
    case 0:
      if (b == 0x01) {          // CPM パケット開始
        usb_state = 1;
      }
      else if (b == 0x02) {     // Layer パケット開始
        usb_state = 3;
      }
      // ★★★ Solenoid コマンド（1バイト完結）★★★
      else if (b == 0x10 || b == 0x11) {
      fireSolenoidByTiming();
      }
      // その他のバイトは無視
      break;

    // ---- CPM LSB ----
    case 1:
      // LSB を読み捨て
      usb_state = 2;
      break;

    // ---- CPM MSB ----
    case 2:
      // MSB を読み捨て
      usb_state = 0;
      break;

    // ---- Layer 1バイト ----
    case 3:
      // layer 値を読み捨て
      usb_state = 0;
      break;

    default:
      usb_state = 0;
      break;
  }
}

// ======================================================
// USB / BT シリアル入力チェック
// ※ MODE_USB_BT のときだけ呼ぶ
// ※ CPM/Layer フレームは「食べて捨てる」、0x10/0x11 だけでソレノイド発火
// ======================================================
void pollSerialInputs() {
  // USB シリアル優先で全て読む
  while (Serial.available() > 0) {
    uint8_t b = Serial.read();
    handleSerialByte(b, SRC_USB);
  }

  // BT シリアル側も同様に読む
  while (SerialBT.available() > 0) {
    uint8_t b = SerialBT.read();
    handleSerialByte(b, SRC_BT);
  }
}

// ======================================================
// 起動時の接続モード選択UI
// ======================================================
void drawModeSelectScreen() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(ORANGE);
  M5.Display.setCursor(20, 20);
  M5.Display.println("Select Connection Mode");

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE);
  M5.Display.setCursor(20, 45);
  M5.Display.println("Tap a box or press A/B/C:");

  // USB/BT
  int x = 20, w = 280;
  int y1 = 70, h = 40;
  M5.Display.drawRect(x, y1, w, h, CYAN);
  M5.Display.setCursor(x + 10, y1 + 10);
  M5.Display.setTextSize(2);
  M5.Display.print("USB / Bluetooth");

  // I2C
  int y2 = 130;
  M5.Display.drawRect(x, y2, w, h, YELLOW);
  M5.Display.setCursor(x + 10, y2 + 10);
  M5.Display.setTextSize(2);
  M5.Display.print("I2C(TheExtrend_stack)");

  // Demo
  int y3 = 190;
  M5.Display.drawRect(x, y3, w, h, GREEN);
  M5.Display.setCursor(x + 10, y3 + 10);
  M5.Display.setTextSize(2);
  M5.Display.print("Demo (local only)");

  // ボタン説明
  M5.Display.setTextSize(1);
  M5.Display.setCursor(20, 240);
  M5.Display.print("BtnA: USB/BT   BtnB: I2C   BtnC: Demo");
}

bool axpReadReg(uint8_t reg, uint8_t &val) {
    Wire.beginTransmission(0x34);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;

    Wire.requestFrom(0x34, 1);
    if (!Wire.available()) return false;
    val = Wire.read();
    return true;
}

// 起動時モード選択処理
void selectStartupMode() {
  drawModeSelectScreen();
  appMode = MODE_NONE;

  while (appMode == MODE_NONE) {
    M5.update();

    // 物理ボタンでも選択可
    if (M5.BtnA.wasPressed()) {
      appMode = MODE_USB_BT;
      break;
    }
    if (M5.BtnB.wasPressed()) {
      appMode = MODE_I2C;
      break;
    }
    if (M5.BtnC.wasPressed()) {
      appMode = MODE_DEMO;
      break;
    }

    // タッチUI
    if (M5.Touch.getCount() > 0) {
      auto t = M5.Touch.getDetail(0);
      if (t.wasPressed()) {
        int tx = t.x;
        int ty = t.y;

        // USB/BT box
        if (tx >= 20 && tx <= 300 && ty >= 70 && ty <= 110) {
          appMode = MODE_USB_BT;
          break;
        }
        // I2C box
        if (tx >= 20 && tx <= 300 && ty >= 130 && ty <= 170) {
          appMode = MODE_I2C;
          break;
        }
        // Demo box
        if (tx >= 20 && tx <= 300 && ty >= 190 && ty <= 230) {
          appMode = MODE_DEMO;
          break;
        }
      }
    }

    delay(10);
  }
}

//バッテリー描画消去関数
void clearBatteryIndicator() {
  int x = 30;
  int y = 5;
  int h = 10;
  // 数値表示エリアだけ消す
  M5.Display.fillRect(x - 32, y, 30, h, BLACK);
  // ゲージ内部も消す（枠は残す設計）
  M5.Display.fillRect(x + 1, y + 1, 25 - 2, h - 2, BLACK);
}

//バッテリー更新関数
void updateBatteryStatus() {
  uint32_t now = millis();
  if (now - lastBattMs < BATT_UPDATE_MS) return;
  lastBattMs = now;

  batteryPct  = M5.Power.getBatteryLevel();
  batteryVolt = M5.Power.getBatteryVoltage() / 1000.0f;
  batteryChg  = M5.Power.isCharging();
}

void drawBatteryIndicator() {
  int x = 30;
  int y = 5;
  int w = 25;
  int h = 10;

  uint16_t color;
  if (batteryChg)          color = CYAN;
  else if (batteryPct > 30) color = GREEN;
  else if (batteryPct > 10) color = YELLOW;
  else                      color = RED;

  uint16_t textcolor;
  if (batteryChg)          textcolor = CYAN;
  else if (batteryPct > 30) textcolor = GREEN;
  else if (batteryPct > 10) textcolor = YELLOW;
  else                      textcolor = RED;

  // 枠
  M5.Display.drawRect(x, y, w, h, color);
  M5.Display.fillRect(x + w, y + 4, 3, h - 8, color); // 端子

  // 中身
  int fill = map(batteryPct, 0, 100, 0, w - 2);
  M5.Display.fillRect(x + 1, y + 1, fill, h - 2, color);

  // 数値（小）
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(textcolor);
  M5.Display.setCursor(x - 28, y + 2);
  M5.Display.printf("%d%%", batteryPct);
}


void updateBatteryUI() {
  // 変化検出
  if (batteryPct != lastBatteryPct ||
      batteryChg != lastBatteryChg) {


    clearBatteryIndicator();   // ★ 変化時だけ消す
    batteryDirty = true;


    lastBatteryPct = batteryPct;
    lastBatteryChg = batteryChg;
  }
}


// ======================================================
// 初期化
// ======================================================
void setup() {
  auto cfg = M5.config();
  // USB シリアルを有効化（USB/BTモード時に利用）
  cfg.serial_baudrate = 115200;
  cfg.output_power    = true;

  M5.Power.setExtOutput(false);

  M5.begin(cfg);

  M5.Power.setExtOutput(false);
  
  loadConfig();

  // 起動時モード選択（毎回選ぶ仕様）
  selectStartupMode();

  // モードごとの初期化
  if (appMode == MODE_USB_BT) {
    Serial.begin(115200);           // USB シリアル
    SerialBT.begin("TypingBridge"); // Bluetooth SPP 名
    activeSource = SRC_NONE;        // 最初は未接続
  } else if (appMode == MODE_I2C) {
    // I2Cのみ有効（USB/BTは開始しない）
    Wire.begin(I2C_ADDRESS, 32, 33, 400000);
    Wire.onReceive(onReceiveEvent);
    activeSource = SRC_I2C;         // インジケータは黄色に近い状態
  } else { // MODE_DEMO
    // 通信なし（Demoモード）
    activeSource = SRC_NONE;
  }

  M5.Speaker.setVolume(soundVolume);
  M5.Power.setVibration(0);

  // メイン画面
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(ORANGE);
  M5.Display.fillScreen(BLACK);
  M5.Display.setCursor(0, 30);
  M5.Display.println("Solenoid Emulator");
  M5.Display.setTextColor(WHITE);

  if (appMode == MODE_USB_BT) {
    M5.Display.setTextColor(GREEN);
    M5.Display.println("Mode: USB / Bluetooth");
  } else if (appMode == MODE_I2C) {
    M5.Display.setTextColor(GREEN);
    M5.Display.println("Mode: I2C");
  } else {
    M5.Display.setTextColor(GREEN);
    M5.Display.println("Mode: Demo (local only)");
  }
  M5.Display.setTextSize(1);
   M5.Display.setTextColor(WHITE);
  M5.Display.println("Hold screen to open settings");
  drawSolenoid(0);
  drawCommIndicator();
}

// ======================================================
// メインループ
// ======================================================
void loop() {
  M5.update();
  //
  updateBatteryStatus();
  updateBatteryUI();
  if (batteryDirty) {
  drawBatteryIndicator();
  batteryDirty = false;   // ← ★ここで false に戻る
}


  // ⭐ 毎フレーム ソレノイド ステートマシン更新
  updateSolenoid();
  updateVibrationPulse();  // ★追加：振動OFF制御

    if (solenoidRequest) {
    solenoidRequest = false;
    fireSolenoidByTiming();
  }

  // 通信インジケータ更新（ソース変化時のみ）
  static uint8_t prevSource = 0;
  if (prevSource != activeSource) {
    prevSource = activeSource;
    drawCommIndicator();
  }

  // モードに応じた入力取得
  if (appMode == MODE_USB_BT) {
    // USB / BT からの入力をポーリング（CPMは無視、0x10/0x11だけ使用）
    pollSerialInputs();
  }
  // MODE_I2C は onReceiveEvent のみ
  // MODE_DEMO は外部トリガなし

  if (configMode) {
    // 設定 UI 操作
    handleConfigTouch();

    // 設定画面中もソレノイド音は進行させる（描画なし FAST のみ）
    if (triggerPending) {
      triggerPending = false;
      solenoidFastClick();
    }

    // A/B/C で設定モード終了
    if (M5.BtnA.isHolding() || M5.BtnB.isHolding() || M5.BtnC.isHolding()) {
      configMode = false;
      saveConfig();
      vibEnabled = true;                 // ★ 強制ON
      M5.Power.setVibration(0);          // 念のため

      M5.Display.fillScreen(BLACK);
      M5.Display.setTextSize(2);
      M5.Display.setTextColor(ORANGE);
      M5.Display.setCursor(0, 30);
      M5.Display.println("Solenoid Emulator");
      M5.Display.setTextColor(WHITE);

      if (appMode == MODE_USB_BT) {
        M5.Display.println("Mode: USB / Bluetooth");
      } else if (appMode == MODE_I2C) {
        M5.Display.println("Mode: I2C");
      } else {
        M5.Display.println("Mode: Demo (local only)");
      }

      M5.Display.println("Hold screen to open settings");
      drawSolenoid(0);
      drawCommIndicator();
      batteryDirty = true; 
    }
    return;
  }

  // 設定モード突入チェック
  checkTouchToConfig();

  // 通信トリガでソレノイド動作（通常画面）
  // ※ ここは「I2C からの 1打鍵トリガ」のみ
  if (triggerPending) {
    triggerPending = false;
  }

  // ボタン操作（デバッグ用 / Demoモードでも使用可）
  if (M5.BtnA.wasPressed()) {
    solenoidEffect();
  }
  if (M5.BtnB.wasPressed()) {
    solenoidEffect();
  }
  if (M5.BtnC.wasPressed()) {
    // ボタンCは「ダブルソレノイド」お試し用
    solenoidEffect();
    lastFireMs = millis();
    startFastSolenoid();
  }
}
