#include <M5Unified.h>
#include <Wire.h>
#include <Preferences.h>
#include <BluetoothSerial.h>

#define I2C_ADDRESS 0x20
// ==== Solenoid Command (isolated zone) ====
#define SOL_CMD_LIGHT   0x80
#define SOL_CMD_STRONG  0x81
#define SOL_HDR 0xA5

#define SOL_CMD_ENT 0x0D

// ==== コア分離 ====
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

QueueHandle_t audioQueue;

// ==== Battery status ====
uint8_t batteryPct   = 0;
float   batteryVolt  = 0.0f;
bool    batteryChg   = false;
uint32_t lastBattMs  = 0;
const uint32_t BATT_UPDATE_MS = 2000;  // 2秒に1回
static int  lastBatteryPct = -1;
static bool lastBatteryChg = false;
static bool batteryDirty   = true;  // 初回描画用

// ==== 起動時セッティング禁止 ====
uint32_t bootTimeMs = 0;
const uint32_t CONFIG_ENABLE_DELAY_MS = 1500;
Preferences prefs;
BluetoothSerial SerialBT;
static bool ignoreNextCRelease = false;

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

//=====コクピットUI=====
#include <M5GFX.h>
static LGFX_Sprite hudSprite(&M5.Display);

enum UiTheme : uint8_t {
  THEME_SOLENOID = 0,
  THEME_COCKPIT  = 1,
};

// Ace風グリーン
#define HUD_GREEN  M5.Display.color565(0, 255, 80)
#define HUD_DIM    M5.Display.color565(0, 120, 40)
#define HUD_SOFT   M5.Display.color565(0, 200, 60)

UiTheme uiTheme = THEME_SOLENOID;
// カメラワーク
static float camYaw = 0.0f;    // 左右旋回
static float camPitch = 0.0f;  // 上下
static float camYawVel = 0.0f;
static float camPitchVel = 0.0f;
static uint32_t nextCamEventMs = 0;
static float camYawTarget = 0.0f;
static float camPitchTarget = 0.0f;

static inline float clampf(float v, float a, float b) {
  return v < a ? a : (v > b ? b : v);
}
static float hudSpeed = 600;
static float hudAlt   = 7800;

//武装モード
enum WeaponType {
  WEAPON_GUN = 0,
  WEAPON_MISSILE = 1
};

WeaponType currentWeapon = WEAPON_GUN;
bool lockActive = false;
uint32_t lockUntilMs = 0;

volatile uint8_t activeSource   = SRC_NONE;  // 現在の入力ソース種別
AppMode          appMode        = MODE_NONE; // 起動時に選ぶモード

//settingmode切替
bool invertMode = false;
bool settingsTogglePage = false;

// ★ 反転(=HUDミラー)適用
// 通常: rotationNormal
// HUDミラー: rotationMirror
static uint8_t rotationNormal = 1;  // ←普段の向きに合わせて(0/1/2/3)
static uint8_t rotationMirror = 7;  // 1に対するミラー版（1→7が定番）

void applyInvertMode() {
  M5.Display.setRotation(invertMode ? rotationMirror : rotationNormal);
}

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
uint8_t soundVolume = 120;       // ソレノイド音量(🔴スピーカー保護のリミッターMAX80)

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
  prefs.putBool("invert", invertMode);
  prefs.putUChar("theme", (uint8_t)uiTheme);
  prefs.end();
}

void loadConfig() {
  prefs.begin("solenoid", true);
  vibStrength  = prefs.getUChar("vib", 180);
  vibEnabled   = prefs.getBool("vibOn", true);
  toneBase     = prefs.getFloat("tone", 4000.0f);  // デフォルトも4000Hzに揃える
  soundVolume  = prefs.getUChar("vol", 80);        // デフォルト80
  invertMode = prefs.getBool("invert", false);
  uiTheme = (UiTheme)prefs.getUChar("theme", (uint8_t)THEME_SOLENOID);

  // 🔴 安全リミッタ（古い設定が残っていても 80 を超えないように）
  soundVolume = constrain(soundVolume, 0, 255);

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
inline void playClick()
{
    uint8_t evt = 1;
    xQueueSend(audioQueue, &evt, 0);
}

// ======================================================
// 音タスク（コア分離用）
// ======================================================
void audioTask(void* arg)
{
    uint8_t evt;

    while (1)
    {
        if (xQueueReceive(audioQueue, &evt, portMAX_DELAY))
        {
            if (evt == 1)   // click
            {
                makeClickWave();
                M5.Speaker.playRaw(
                    clickBuffer,
                    sizeof(clickBuffer)/sizeof(int16_t),
                    16000,
                    true,
                    1
                );
            }
        }
    }
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
  
  if (configMode) return;  // ★追加：設定中は描画しない
  if (uiTheme == THEME_COCKPIT) return; // ★追加
  
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
const int SOL_NORMAL_STEP_INTERVAL_MS = 5;   // ピストン1ステップ(描画)の間隔
const int SOL_FAST_GAP_MS             = 17;  // 2段クリック間のギャップ
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
// ==== FX event ====
volatile uint16_t fxFireBurst = 0;   // 弾幕量
volatile uint32_t fxFlashUntilMs = 0;

inline void onFireVisualFX(bool strong=false) {
  fxFireBurst = strong ? 18 : 10;
  fxFlashUntilMs = millis() + (strong ? 70 : 40);

// ★ recoilは「速度」に加算する
camPitchVel -= strong ? 2.8f : 1.8f;
camYawVel   += (random(-100, 101) / 100.0f) * (strong ? 1.4f : 1.0f);

// 目標が範囲外に飛びすぎないように
  camYawTarget   = clampf(camYawTarget,   -1.15f, 1.15f);
  camPitchTarget = clampf(camPitchTarget, -1.8f, 1.8f);
}

inline void fireSolenoidByTiming() {
if (currentWeapon != WEAPON_MISSILE)
    currentWeapon = WEAPON_GUN;
  onFireVisualFX(false);

  if (uiTheme == THEME_COCKPIT) {
      solenoidEffect();
      return;
  }

  solenoidEffect();
}

// ---- WARNING system ----
bool warningActive = false;
uint32_t warningUntilMs = 0;

// 通常色と警告色
uint16_t HUD_NORMAL  = M5.Display.color565(0,255,80);
uint16_t HUD_WARNING = M5.Display.color565(180,0,0);

// 現在のHUD色（毎フレーム更新）
uint16_t hudColor = HUD_NORMAL;



void drawReticle(float cx, float cy) {

  uint16_t c = hudColor;

  int r = 18;

  // 外円
  M5.Display.drawCircle(cx, cy, r, c);

  // 十字
  M5.Display.drawLine(cx - 26, cy, cx - 8, cy, c);
  M5.Display.drawLine(cx + 8, cy, cx + 26, cy, c);
  M5.Display.drawLine(cx, cy - 26, cx, cy - 8, c);
  M5.Display.drawLine(cx, cy + 8, cx, cy + 26, c);

  // 中央点
  //M5.Display.fillCircle(cx, cy, 2, c);
}

void drawLockOnReticle(float cx, float cy)
{
  uint16_t c = M5.Display.color565(255, 80, 0);

  int r = 22;

  // ---- 外側ブレード（四方向）----
  int gap = 12;

  M5.Display.drawLine(cx - r, cy - gap, cx - r, cy - r, c);
  M5.Display.drawLine(cx - r, cy + gap, cx - r, cy + r, c);

  M5.Display.drawLine(cx + r, cy - gap, cx + r, cy - r, c);
  M5.Display.drawLine(cx + r, cy + gap, cx + r, cy + r, c);

  M5.Display.drawLine(cx - gap, cy - r, cx - r, cy - r, c);
  M5.Display.drawLine(cx + gap, cy - r, cx + r, cy - r, c);

  M5.Display.drawLine(cx - gap, cy + r, cx - r, cy + r, c);
  M5.Display.drawLine(cx + gap, cy + r, cx + r, cy + r, c);

  // ---- 中央小リング ----
  M5.Display.drawCircle(cx, cy, 8, c);

  // ---- 点滅ロック表示 ----
  if ((millis() / 200) % 2 == 0)
  {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(c);
    M5.Display.setCursor(cx - 10, cy - 36);
    M5.Display.print("LOCK");
  }
}



void drawCompass(float heading)
{
  // ---- ステータスバーの下に配置 ----
  int x = 80;
  int w = 160;
  int y = 54;   // ← ここが重要（50pxより下）
  int h = 22;

  // 部分クリア（コンパス専用エリア）
  M5.Display.fillRect(x-4, y-4, w+8, h+8, BLACK);

  uint16_t c = hudColor;
  int centerX = x + w/2;

  // 中央マーカー
  M5.Display.fillTriangle(centerX-4, y,
                          centerX+4, y,
                          centerX,   y-6,
                          c);

  const int minorStep = 5;
  const int majorStep = 30;
  const float pixelPerDeg = 1.6f;

  int base = ((int)heading / minorStep) * minorStep;

  for (int v = base - 90; v <= base + 90; v += minorStep)
  {
    float diff = heading - v;
    int dx = diff * pixelPerDeg;
    int tx = centerX - dx;

    if (tx < x || tx > x + w) continue;

    bool major = (v % majorStep == 0);
    int len = major ? 10 : 6;

    M5.Display.drawLine(tx, y + h - len, tx, y + h, c);

    if (major)
    {
      int deg = (v % 360 + 360) % 360;

      const char* label = nullptr;
      if (deg == 0) label = "N";
      else if (deg == 90) label = "E";
      else if (deg == 180) label = "S";
      else if (deg == 270) label = "W";

      M5.Display.setTextSize(1);
      M5.Display.setTextColor(c);

      if (label)
      {
        M5.Display.setCursor(tx - 3, y + 2);
        M5.Display.print(label);
      }
      else
      {
        M5.Display.setCursor(tx - 6, y + 2);
        M5.Display.printf("%03d", deg);
      }
    }
  }
}

// ==== Cockpit scene objects ====
struct Star {
  float x;   // -1..+1
  float y;   // -1..+1
  float z;   //  0..1  (0に近いほど手前)
};

struct Bullet {
  float x, y, z;     // ★ -1..+1, -1..+1,  z: 奥行き（大きいほど奥）
  float px, py, pz;  // ★前回（トレーサー用）
  float vx, vy, vz;          // ★奥へ進む速度
  float speed;      // ← 追加
  float age;        // ← 追加
  uint16_t life;
  uint8_t type; 
};

struct Explosion {
  float x, y, z;
  uint8_t life;

  float sparkVX[6];
  float sparkVY[6];
};
static const int EXP_MAX = 12;
static Explosion explosions[EXP_MAX];


static inline void getVanishingPoint(float &cx, float &cy) {
  // drawStarsWarp() と完全に同じ係数にする
  cx = 160.0f + camYaw * 28.0f;   // ←あなたが使ってる値に合わせて
  cy = 140.0f + camPitch * 22.0f;
}

static inline void project3(float x, float y, float z, float cx, float cy, float &sx, float &sy) {
  const float fov = 0.65f;                 // drawStarsWarp() と同じ
  float inv = fov / z;                     // z大＝奥→inv小＝中心へ寄る
  sx = cx + x * inv * 160.0f;
  sy = cy + y * inv * 120.0f;
}

static const int STAR_N = 70;
static Star stars[STAR_N];

static const int BULLET_MAX = 60;
static Bullet bullets[BULLET_MAX];
static int bulletHead = 0;

// 任意（HUD傾き等に将来使える）
static float camBank = 0.0f;
static float camBankVel = 0.0f;
static float camBankTarget = 0.0f;
static inline float frand(float a, float b) {
  return a + (b - a) * (random(0, 10000) / 10000.0f);
}

void triggerWarning(uint32_t durationMs = 2000)
{
  warningActive = true;
  warningUntilMs = millis() + durationMs;
}


void respawnStar(int i) {
  // 画面中心から少し散らす（完全中心密集を避ける）
  stars[i].x = frand(-1.0f, 1.0f);
  stars[i].y = frand(-1.0f, 1.0f);

  // zは奥に配置（遠い星を多めに）
  stars[i].z = frand(0.35f, 1.0f);
}

// 速度パラメータ（お好みで）
static float warpSpeed = 0.55f;
static float warpSpeedTarget = 0.55f;

void updateStars(float dt) {
  // カメラワークで中心点をずらす（旋回/上昇下降）
  // camYaw/camPitch は -1..+1 程度に抑えてある前提
  float yawShift   = camYaw   * 0.35f;
  float pitchShift = camPitch * 0.28f;

  for (int i = 0; i < STAR_N; i++) {
    // 前進：zが減る（近づく）
    stars[i].z -= warpSpeed * dt;

    // zが手前に来すぎたら奥に戻す
    if (stars[i].z <= 0.05f) respawnStar(i);

    // 旋回/上昇下降で“進行方向”をずらす
    // 星自体を少し逆方向へずらすと、視点が向いた感じになる
    stars[i].x -= yawShift * dt * 0.6f;
    stars[i].y -= pitchShift * dt * 0.6f;

    // 範囲外に出た星は再配置（端で不自然に溜まらない）
    if (stars[i].x < -1.2f || stars[i].x > 1.2f ||
        stars[i].y < -1.2f || stars[i].y > 1.2f) {
      respawnStar(i);
    }
  }
}

void drawStarsWarp() {
  
  //控え目
  //float cx = 160.0f + camYaw * 18.0f;
  //float cy = 120.0f + camPitch * 14.0f;
  
  // 例（ダイナミック寄り）
  float cx = 160.0f + camYaw * 60.0f;
  float cy = 140.0f + camPitch * 45.0f;

  // 画角（値が小さいほど広角）
  const float fov = 0.65f;

  for (int i = 0; i < STAR_N; i++) {
    // zが小さいほど大きく投影される → 外側へ伸びる
    float inv = fov / stars[i].z;

    float sx = cx + stars[i].x * inv * 160.0f;
    float sy = cy + stars[i].y * inv * 120.0f;

    // 前フレーム相当の位置を推定して尾を描く（簡易）
    float inv2 = fov / (stars[i].z + 0.06f);
    float px = cx + stars[i].x * inv2 * 160.0f;
    float py = cy + stars[i].y * inv2 * 120.0f;

    // 画面外はスキップ
   if (sx < 0 || sx >= 320 || sy < 0 || sy >= 240) continue;

    // 明るさ：近いほど明るい
    float near01 = 1.0f - constrain((stars[i].z - 0.05f) / 0.95f, 0.0f, 1.0f);
    uint8_t v = (uint8_t)(70 + 185 * near01);
    uint16_t c = M5.Display.color565(v, v, v);

    // 尾（短い線）＋点
    M5.Display.drawLine((int)px, (int)py, (int)sx, (int)sy, c);
    M5.Display.drawPixel((int)sx, (int)sy, c);
  }
}

void initCockpitScene() {
  for (int i = 0; i < STAR_N; i++) respawnStar(i);

  for (int i = 0; i < BULLET_MAX; i++) bullets[i].life = 0;

  camYaw = camPitch = 0;
  camYawVel = camPitchVel = 0;
  nextCamEventMs = millis() + 1500;
}

void updateCamera(float dt) {
  uint32_t now = millis();

  // ---- イベント（たまに大きく振る）----
  if (now >= nextCamEventMs) {
    int r = random(0, 100);

    float y = 0.0f, p = 0.0f, b = 0.0f;
    uint32_t interval = random(1200, 3000);

    if (r < 25) {
      // 旋回（パン）
      y = (random(0, 2) ? 1 : -1) * (0.55f + random(0, 45) / 100.0f); // 0.55〜1.0
      p = (random(-25, 26) / 100.0f);
      b = -y * 0.6f;
      interval = random(900, 1600);
    } else if (r < 45) {
      // 上昇/下降（チルト）
      p = (random(0, 2) ? 1 : -1) * (0.8f + random(0, 60) / 100.0f); // 0.45〜0.85
      y = (random(-30, 31) / 100.0f);
      b = y * 0.35f;
      interval = random(900, 1700);
    } else if (r < 75) {
      // センターへ戻す休止
      y = 0.0f; p = 0.0f; b = 0.0f;
      interval = random(1200, 2500);
    } else if (r < 90) {
      // パン+チルト同時（派手）
      y = (random(0, 2) ? 1 : -1) * (0.75f + random(0, 35) / 100.0f); // 0.75〜1.10
      p = (random(0, 2) ? 1 : -1) * (0.9f + random(0, 50) / 100.0f); // 0.40〜0.75
      b = -y * 0.75f;
      interval = random(700, 1400);
    } else {
      // タービュランス（短時間ガタガタ）
      y = (random(-110, 111) / 100.0f) * 0.35f;
      p = (random(-110, 111) / 100.0f) * 0.30f;
      b = (random(-110, 111) / 100.0f) * 0.25f;
      interval = random(350, 700);
    }

    camYawTarget   = clampf(y, -2.2f, 2.2f);
    camPitchTarget = clampf(p, -0.95f, 0.95f);

    nextCamEventMs = now + interval;
  }

  // ---- スプリング追従（“ぐいっ→揺れ→戻る”）----
  const float k = 10.0f;  // ★上げるとキレる
  const float d = 3.8f;   // ★下げると揺れる

  { // yaw
    float a = k * (camYawTarget - camYaw) - d * camYawVel;
    camYawVel += a * dt;
    camYaw    += camYawVel * dt;
  }
  { // pitch
    float a = k * (camPitchTarget - camPitch) - d * camPitchVel;
    camPitchVel += a * dt;
    camPitch    += camPitchVel * dt;
  }
  { // bank（任意）
    float a = (k*0.8f) * (camBankTarget - camBank) - (d*0.9f) * camBankVel;
    camBankVel += a * dt;
    camBank    += camBankVel * dt;
  }

  camYaw   = clampf(camYaw,   -1.15f, 1.15f);
  camPitch = clampf(camPitch, -0.95f, 0.95f);
  float turnStrength = fabs(camYawVel);
  camBankTarget = -camYaw * (0.6f + turnStrength * 0.25f);
  camBank  = clampf(camBank,  -0.90f, 0.90f);
}

void spawnBarrage(uint16_t n) {
  float cx, cy;
  getVanishingPoint(cx, cy);

  // 砲口（画面下：中央寄りが正面感強い）
  float gunLx, gunRx;
  //中央機関砲
    if (currentWeapon == WEAPON_GUN) {
      gunLx = 160 - 10;
      gunRx = 160 + 10;
    } else {
      // ミサイル発射時
      gunLx = 160 - 70;
      gunRx = 160 + 70;
    }

  const float gunY  = 216;

  const float z0 = 0.20f;          // ★発射時の手前（小さいほど近い＝太く見える）
  const float vz = 1.70f;          // ★奥へ進む速さ（大きいほど奥へ飛ぶ感）
  const float spread = 0.050f;      // ★散り（小さいほど真っ直ぐ正面）

  // 逆投影で、スクリーン座標→正規化(x,y)に戻す
  const float fov = 0.65f;
  float inv0 = fov / z0;

  for (uint16_t k = 0; k < n; k++) {
    Bullet &b = bullets[bulletHead];
    bulletHead = (bulletHead + 1) % BULLET_MAX;
    b.type = currentWeapon;

    bool right = (k & 1);

    float sx = (right ? gunRx : gunLx) + random(-3, 4);
    float sy = gunY + random(-2, 3);

    // ★正規化座標（消失点中心の座標系）
    float nx = (sx - cx) / (inv0 * 160.0f);
    float ny = (sy - cy) / (inv0 * 120.0f);

    // 真っ直ぐ正面に寄せる散り（小さめ）
    nx += frand(-spread, spread);
    ny += frand(-spread, spread);

    b.x = nx; b.y = ny; b.z = z0;
    b.px = b.x; b.py = b.y; b.pz = b.z;
    if (currentWeapon == WEAPON_GUN)
    {
      b.age   = 0.0f;
      b.speed = 0.0f;     // 使わないけど初期化して安全に
      b.vx = 0.0f;
      b.vy = 0.0f;
      b.vz = vz * (0.85f + random(0,31)/100.0f);

      b.life = 26 + random(0, 12);   // 寿命（描画の生死）
    }
    else
    {
      // ---- ミサイル初期挙動 ----
      b.age   = 0.0f;
      b.speed = 1.0f;     // 初速
      float side = right ? 0.4f : -0.4f;  // ★ 横広がりは残すが弱め（0.6→0.55）
      b.vx = side;
      b.vy = frand(-0.01f, 0.01f);          // ★ 少しだけ上下の“噴かし”
      b.vz = 1.0f;                          // ★ 前進のベースも上げる
      b.life = 32 + random(0, 10);          // ★ 少し長めに飛ばす（任意）
    }
  }
}

void spawnExplosion(float x, float y, float z)
{
  for (int i = 0; i < EXP_MAX; i++)
  {
    if (explosions[i].life == 0)
    {
      explosions[i].x = x;
      explosions[i].y = y;
      explosions[i].z = z;
      explosions[i].life = 20;

      // 🔥 火花方向ランダム生成
      for (int s = 0; s < 6; s++)
      {
        float ang = frand(0.0f, 2.0f * PI);
        float spd = frand(0.02f, 0.06f);
        explosions[i].sparkVX[s] = cosf(ang) * spd;
        explosions[i].sparkVY[s] = sinf(ang) * spd;
      }

      break;
    }
  }
}

bool isExplosionActive()
{
  for (int i = 0; i < EXP_MAX; i++)
  {
    if (explosions[i].life > 0)
      return true;
  }
  return false;
}

void consumeFXEvents() {
  uint16_t n = fxFireBurst;
  if (n == 0) return;
  fxFireBurst = 0;

  // 上限かける（暴走防止）
  if (n > 30) n = 30;

  if (currentWeapon == WEAPON_MISSILE)
{
    spawnBarrage(2);           // ★左右2発
    
}
else
{
    spawnBarrage(n);           // 通常機関砲
}
}

void drawSpeedTape(float speed)
{
  int x = 13;
  int y = 90;
  int w = 55;
  int h = 130;

  M5.Display.fillRect(x-4, y-4, w+8, h+8, BLACK);

  uint16_t c = hudColor;

  int centerY = y + h/2;

  const int minorStep = 20;     // ★ 20刻み
  const int majorStep = 100;    // ★ 100刻みを太線
  const float pixelPerUnit = 0.5f; // ★ ALTと見た目揃え用

  int base = ((int)speed / minorStep) * minorStep;

  for (int v = base - 400; v <= base + 400; v += minorStep)
  {
    int dy = (speed - v) * pixelPerUnit;
    int ty = centerY + dy;

    if (ty < y || ty > y+h) continue;

    bool major = (v % majorStep == 0);
    int len = major ? 20 : 10;

    M5.Display.drawLine(x + w - len, ty, x + w, ty, c);

    if (major)
    {
      M5.Display.setTextSize(1);
      M5.Display.setCursor(x + 4, ty - 4);
      M5.Display.printf("%d", v);
    }
  }

  // 中央表示
  M5.Display.fillRect(x, centerY-12, w, 24, BLACK);
  M5.Display.drawRect(x, centerY-12, w, 24, c);

  M5.Display.setTextSize(2);
  M5.Display.setCursor(x+15, centerY-8);
  M5.Display.printf("%d", (int)speed);
}


void drawAltTape(int alt)
{
  int x = 250;
  int y = 90;
  int w = 55;
  int h = 130;

  // ---- 部分クリア（重要）----
  M5.Display.fillRect(x-4, y-4, w+8, h+8, BLACK);

  uint16_t c = hudColor;

  int centerY = y + h/2;

  // ===== テープ設定 =====
  const int majorStep = 500;      // 太目盛り
  const int minorStep = 100;      // 細目盛り
  const float pixelPerUnit = 0.10f; // 高度1あたりのpx（調整可）

  // 基準（majorStep単位）
  int base = (alt / minorStep) * minorStep;

  for (int v = base - 5000; v <= base + 5000; v += minorStep)
  {
    int dy = (alt - v) * pixelPerUnit;
    int ty = centerY + dy;

    if (ty < y || ty > y + h) continue;

    bool major = (v % majorStep == 0);

    int len = major ? 20 : 10;

    // 左側に目盛り
    M5.Display.drawLine(x, ty, x + len, ty, c);

    if (major)
    {
      M5.Display.setTextSize(1);
      M5.Display.setTextColor(c);
      M5.Display.setCursor(x + 22, ty - 4);
      M5.Display.printf("%d", v);
    }
  }

  // ===== 中央固定ウィンドウ =====
  M5.Display.fillRect(x, centerY - 12, w, 24, BLACK);
  M5.Display.drawRect(x, centerY - 12, w, 24, c);

  M5.Display.setTextSize(2);
  M5.Display.setCursor(x + 6, centerY - 8);
  M5.Display.printf("%d", alt);
}

void drawCockpit() {
  // 背景
  M5.Display.fillRect(0, 0, 320, 240, BLACK);

  drawStarsWarp();

  // ステータス（ソースとバッテリをそれっぽく）
  hudSprite.setTextColor(hudColor);
  hudSprite.setTextSize(1);
  uint16_t barColor = warningActive
    ? M5.Display.color565(40, 0, 0)    // 暗い赤（透かし風）
    : M5.Display.color565(0, 30, 0);   // 通常グリーン

  hudSprite.fillRect(0, 0, 320, 50, barColor);

  hudSprite.setCursor(16, 18);
  hudSprite.printf("LINK:%s",
    (activeSource==SRC_USB)?"USB":
    (activeSource==SRC_BT)?"BT":
    (activeSource==SRC_I2C)?"I2C":"NONE");

  hudSprite.setCursor(240, 18);
  hudSprite.printf("PWR:%d%%", batteryPct);
  hudSprite.drawRect(10, 10, 300, 25, hudColor);

  // 最後に一括転送
  hudSprite.pushSprite(0, 0);

// 弾（正面奥へ：投影トレーサー）
{
  float cx, cy;
  getVanishingPoint(cx, cy);

  for (int i = 0; i < BULLET_MAX; i++) {
    Bullet &b = bullets[i];
    if (b.life == 0) continue;

    float x1, y1, x2, y2;
    project3(b.px, b.py, b.pz, cx, cy, x1, y1);
    project3(b.x,  b.y,  b.z,  cx, cy, x2, y2);

    // 画面内だけ
    if (x2 < -10 || x2 > 330 || y2 < -10 || y2 > 230) continue;

    // 弾描画
    if (b.type == WEAPON_GUN)
    {
        M5.Display.drawLine((int)x1, (int)y1, (int)x2, (int)y2, ORANGE);
    }
    else
    {
    // ---- ミサイル本体（太め）----
    for (int w = -1; w <= 1; w++)
    {
        M5.Display.drawLine(
            (int)x1 + w,
            (int)y1,
            (int)x2 + w,
            (int)y2,
            RED
        );
    }

      // ---- 白煙（長め＋拡散）----
      for (int t = 1; t <= 5; t++)
      {
          float ratio = t * 0.05f;   // 伸び具合

          float tx = x1 + (x2 - x1) * ratio;
          float ty = y1 + (y2 - y1) * ratio;

          // 少し横に拡散
          float spread = t * 0.35f;

          // ===== 青白グラデ =====
          uint8_t r = 180 - t * 15;
          uint8_t g = 220 - t * 10;
          uint8_t b = 255;

          if (r < 40) r = 40;
          if (g < 80) g = 80;

          uint16_t plumeColor = M5.Display.color565(r, g, b);

          M5.Display.drawCircle((int)tx, (int)ty, spread, plumeColor);
      }
    }
  }
}
// ---- Explosion draw (Ring + Sparks) ----
{
  float cx, cy;
  getVanishingPoint(cx, cy);

  for (int i = 0; i < EXP_MAX; i++)
  {
    if (explosions[i].life == 0) continue;

    float sx, sy;
    project3(explosions[i].x,
             explosions[i].y,
             explosions[i].z,
             cx, cy, sx, sy);

    int age = 20 - explosions[i].life;

    // 🔥 リング
    int radius = age * 2.5;
    uint8_t glow = 255 - age * 12;
    uint16_t ringColor = M5.Display.color565(glow, glow/2, 0);

    M5.Display.drawCircle((int)sx, (int)sy, radius, ringColor);

    // ✨ 火花
    for (int s = 0; s < 6; s++)
    {
      float px = explosions[i].x + explosions[i].sparkVX[s] * age;
      float py = explosions[i].y + explosions[i].sparkVY[s] * age;

      float sx2, sy2;
      project3(px, py, explosions[i].z,
               cx, cy, sx2, sy2);

      uint16_t sparkColor =
        M5.Display.color565(255, 200, 80);

      M5.Display.drawPixel((int)sx2, (int)sy2, sparkColor);
    }
  }
}


  // フラッシュ：半透明がないので “薄い矩形” で疑似
  if ((int32_t)(millis() - fxFlashUntilMs) < 0) {
    // コクピット中央だけ光らせる（画面全体より気持ちいい）
    M5.Display.drawRect(14, 44, 292, 182, WHITE);
    M5.Display.drawRect(15, 45, 290, 180, WHITE);
  }
  
  float cx, cy;
  getVanishingPoint(cx, cy);
    if (lockActive && (int32_t)(millis() - lockUntilMs) >= 0)
  {
      lockActive = false;
  }
  
  if (lockActive)
      drawLockOnReticle(cx, cy);
  else
      drawReticle(cx, cy);

  // ★ カメラと連動させる
  float targetSpeed = 600 + camPitch * 120.0f;
  float targetAlt   = 7800 + camYaw   * 500.0f;

  // 慣性（0.05〜0.15くらいが良い）
  hudSpeed += (targetSpeed - hudSpeed) * 0.08f;
  hudAlt   += (targetAlt   - hudAlt)   * 0.08f;

  int speed = constrain((int)hudSpeed, 200, 1200);
  int alt   = constrain((int)hudAlt,   1000, 15000);

  // ---- ワープ速度ターゲット ----
  // 200〜1200 を 0.2〜1.8 にマッピング
  warpSpeedTarget = 0.2f + (speed - 200) * (1.6f / 1000.0f);

  // ---- SPD ラベル ----
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(hudColor);
  M5.Display.setCursor(28, 72);
  M5.Display.print("[SPD]");

  // ---- ALT ラベル ----
  M5.Display.setCursor(260, 72);
  M5.Display.print("[ALT]");
  
  drawSpeedTape(speed);
  drawAltTape(alt);
  float heading = camYaw * 90.0f;
  drawCompass(heading);

  if (warningActive)
  {
    int wx = 95;
    int wy = 185;
    int ww = 130;
    int wh = 32;

    // 赤の透かし帯
    uint16_t bg = M5.Display.color565(50, 0, 0);
    M5.Display.fillRect(wx, wy, ww, wh, bg);

    M5.Display.setTextSize(2);
    M5.Display.setTextColor(hudColor);

    if ((millis()/200)%2 == 0) {
      M5.Display.setCursor(wx + 20, wy + 8);
      M5.Display.print("WARNING");
    }
  }

  // ---- Weapon HUD ----
  int wx = 145;
  int wy = 215;

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(hudColor);
  uint16_t c = M5.Display.color565(255, 80, 0);

  // 枠なし・シンプル表示
  if (currentWeapon == WEAPON_GUN)
  {
      M5.Display.setCursor(wx, wy);
      M5.Display.print("[GUN]");
  }
  else
  {
      // ミサイルは点滅
      if ((millis()/150)%2==0)
      {
          M5.Display.setCursor(wx, wy);
          M5.Display.setTextColor(c);
          M5.Display.print("[MSL]");
      }
  }
}

void updateBullets(float dt) {
  for (int i = 0; i < BULLET_MAX; i++) {
    Bullet &b = bullets[i];
    if (b.life == 0) continue;

    b.px = b.x; b.py = b.y; b.pz = b.z;

    b.age += dt;

    if (b.type == WEAPON_MISSILE)
    {
      // ---- 徐々に加速 ----
      b.speed += dt * (1.8f + b.age * 4.0f);
      if (b.speed > 4.5f) b.speed = 4.5f;

      // ---- 0.18秒後に前へ収束 ----
      if (b.age > 0.10f)
      {
        float pull = -b.x * 3.0f;
        b.vx += pull * dt;
        b.vx *= 0.90f;
      }

      b.x += b.vx * b.speed * dt;
      b.y += b.vy * b.speed * dt;
      b.z += b.vz * b.speed * dt;
    }
    else
    {
      b.z += b.vz * dt;
    }   // ★奥へ進む
    b.life--;

    // 奥へ行き過ぎ or 寿命
    if (b.z > 1.20f)
    {
      // ★ ミサイルのみ爆発
      if (b.type == WEAPON_MISSILE)
      {
        spawnExplosion(b.x, b.y, b.z);
      }

      b.life = 0;
    }
  }

  for (int i = 0; i < EXP_MAX; i++)
  {
    if (explosions[i].life > 0)
    {
      explosions[i].life--;

      // 火花を広げる
      for (int s = 0; s < 6; s++)
      {
        explosions[i].sparkVX[s] *= 1.05f;
        explosions[i].sparkVY[s] *= 1.05f;
      }
    }
  }
  bool missileAlive = false;
  bool explosionAlive = false;

    for (int i = 0; i < BULLET_MAX; i++)
    {
        if (bullets[i].life > 0 &&
            bullets[i].type == WEAPON_MISSILE)
            missileAlive = true;
    }

    for (int i = 0; i < EXP_MAX; i++)
    {
        if (explosions[i].life > 0)
            explosionAlive = true;
    }

    if (!missileAlive && !explosionAlive)
    {
        currentWeapon = WEAPON_GUN;
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

volatile uint32_t lastI2CFireMs = 0;
const uint32_t I2C_MIN_INTERVAL_MS = 8;  // ← 調整可
volatile uint8_t i2cFireCount = 0;

void onReceiveEvent(int numBytes) {
    if (numBytes <= 0) return;

    uint8_t cmd = Wire.read();
    while (Wire.available()) Wire.read();

    // ---- Enterだけミサイル ----
    if (cmd == SOL_CMD_ENT) {
        currentWeapon = WEAPON_MISSILE;
        lockActive = true;
        lockUntilMs = millis() + 1200;

        onFireVisualFX(true);
        solenoidEffect();

        return;
    }

    // ---- それ以外は常に通常弾 ----
    currentWeapon = WEAPON_GUN;

    if (cmd == SOL_CMD_LIGHT || cmd == SOL_CMD_STRONG) {
        if (i2cFireCount < 10) {
            i2cFireCount++;
        }
    }
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
      mmap(soundVolume, 0, 150, 0, 220)
      15, ORANGE);

  // 上に通信インジケータも表示
  drawCommIndicator();
}

void drawToggleUI()
{
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(ORANGE);
  M5.Display.setCursor(40, 20);
  M5.Display.println("HUD CONTROL");

  M5.Display.setTextSize(2);
  M5.Display.setTextColor(WHITE);

  // ---- Invert ----
  M5.Display.setCursor(40, 70);
  M5.Display.printf("Invert : %s", invertMode ? "ON" : "OFF");

  // ---- UI Mode ----
  M5.Display.setCursor(40, 120);
  M5.Display.printf("UI Mode : %s",
      uiTheme == THEME_COCKPIT ? "Cockpit" : "Solenoid");

  // ---- Hint ----
  M5.Display.setTextSize(1);
  M5.Display.setCursor(40, 200);
  M5.Display.print("Tap item to toggle");
}

//メイン画面描画関数
void drawMainScreen() {
  M5.Display.fillScreen(BLACK);

  M5.Display.setTextSize(2);
  M5.Display.setTextColor(ORANGE);
  M5.Display.setCursor(0, 30);
  M5.Display.println("Solenoid Emulator");

  M5.Display.setTextColor(GREEN);

  if (appMode == MODE_USB_BT) {
    M5.Display.println("Mode: USB / Bluetooth");
  } else if (appMode == MODE_I2C) {
    M5.Display.println("Mode: I2C");
  } else {
    M5.Display.println("Mode: Demo (local only)");
  }

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE);
  M5.Display.println("Hold screen to open settings");

  drawSolenoid(0);
  drawCommIndicator();

  batteryDirty = true;
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
      soundVolume = constrain(map(t.x, 20, 240, 0, 200), 0, 200);
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
      drawMainScreen();
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
  if (millis() - bootTimeMs < CONFIG_ENABLE_DELAY_MS) return;
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
static bool sol_wait_header = false;

void handleSerialByte(uint8_t b, CommSource src) {
  activeSource = src;

  // ===== Solenoid 2-byte frame =====
  if (!sol_wait_header) {
    if (b == SOL_HDR) {
      sol_wait_header = true;
    }
    return;
  }

  // header を受けた次の1byteだけ評価
  sol_wait_header = false;

  if (b == SOL_CMD_LIGHT || b == SOL_CMD_STRONG) {
    currentWeapon = WEAPON_GUN;
    lockActive = false;
    fireSolenoidByTiming();
    usb_state = 0;   // 他のステートを壊してOK
    return;
  }
  // ---- Enterキー処理（ミサイル発射）----
  if (b == SOL_CMD_ENT) {
      if (uiTheme == THEME_COCKPIT) {
          lockActive = true;
          lockUntilMs = millis() + 1200;   // ★1.2秒ロック維持
          currentWeapon = WEAPON_MISSILE;
          onFireVisualFX(true);
          solenoidEffect();
      }
      return;
  }

  // ===== ここから下は CPM / Layer 用 =====
  switch (usb_state) {
    case 0:
      if (b == 0x01) usb_state = 1;   // CPM
      else if (b == 0x02) usb_state = 3; // Layer
      break;

    case 1:
      usb_state = 2;  // LSB 捨て
      break;

    case 2:
      usb_state = 0;  // MSB 捨て
      break;

    case 3:
      usb_state = 0;  // layer 捨て
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

static bool hudSpriteReady = false;

void ensureHudSprite() {
  if (hudSpriteReady) return;
  hudSprite.setColorDepth(16);
  hudSprite.createSprite(320, 50);
  hudSpriteReady = true;
}


// ======================================================
// 初期化
// ======================================================
void setup() {
  auto cfg = M5.config();
  // USB シリアルを有効化（USB/BTモード時に利用）
  cfg.serial_baudrate = 115200;
  cfg.output_power    = true;

  M5.begin(cfg);

  M5.Power.setExtOutput(false);
  
  //画面輝度最大
  M5.Lcd.setBrightness(255);
  
  loadConfig();

  applyInvertMode();

  audioQueue = xQueueCreate(8, sizeof(uint8_t));

   xTaskCreatePinnedToCore(
     audioTask,
    "audioTask",
     4096,
     NULL,
     3,      // 優先度
     NULL,
     1       // Core1固定（重要）
  );

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
  if (uiTheme == THEME_COCKPIT) initCockpitScene();
  drawMainScreen();
  bootTimeMs = millis();
}

// ======================================================
// メインループ
// ======================================================
static uint32_t lastI2CProcessMs = 0;
const uint32_t I2C_PROCESS_INTERVAL_MS = 8;

void loop() {
  M5.update();

  // battery
  updateBatteryStatus();
  updateBatteryUI();
  if (batteryDirty) { drawBatteryIndicator(); batteryDirty = false; }

 if (uiTheme != THEME_COCKPIT) {
    updateSolenoid();
  }
  updateVibrationPulse();

  if (solenoidRequest) { solenoidRequest = false; fireSolenoidByTiming(); }

  // ==== Frame limiter (60fps) ====
const uint32_t FRAME_MS = 16;  // 1000/60 ≒ 16ms
static uint32_t lastDrawMs = 0;

  // ---- WARNING update ----
if (random(0,1000) < 2) {
triggerWarning(5000);
}

if (warningActive)
{
  if ((int32_t)(millis() - warningUntilMs) >= 0)
  {
    warningActive = false;
  }
}

if (i2cFireCount > 0) {
    uint32_t now = millis();
    if (now - lastI2CProcessMs >= I2C_PROCESS_INTERVAL_MS) {
        i2cFireCount--;
        lastI2CProcessMs = now;
        fireSolenoidByTiming();
    }
}


// HUD色切替
hudColor = warningActive ? HUD_WARNING : HUD_NORMAL;
  
  
  // テーマ更新＆描画
  static uint32_t lastFrameMs = millis();
  uint32_t now = millis();
  float dt = (now - lastFrameMs) / 1000.0f;
  if (dt > 0.05f) dt = 0.05f; // ワープ防止
  lastFrameMs = now;
  if (uiTheme == THEME_COCKPIT && !configMode) {

      uint32_t nowMs = millis();
  if (nowMs - lastDrawMs < FRAME_MS) {
      return;   // ← これが絶対必要
  }

  float dt = (nowMs - lastDrawMs) / 1000.0f;
  if (dt > 0.05f) dt = 0.05f;

  lastDrawMs = nowMs;

      // ---- ワープ速度制御 ----
      float diff = warpSpeedTarget - warpSpeed;

      const float accelRate = 0.14f;
      const float decelRate = 0.035f;

      if (diff > 0.0f) warpSpeed += diff * accelRate;
      else             warpSpeed += diff * decelRate;

      float speed01 = constrain((warpSpeed - 0.2f) / 1.6f, 0.0f, 1.0f);
      warpSpeed += speed01 * 0.015f;

      if (warpSpeed > 1.4f) camPitchVel += 0.02f;


      ensureHudSprite();
      consumeFXEvents();
      updateCamera(dt);
      updateStars(dt);
      updateBullets(dt);
      drawCockpit();
  }

  // indicator (ソレノイドテーマでは既存通り)
  static uint8_t prevSource = 0;
  if (prevSource != activeSource) { prevSource = activeSource; drawCommIndicator(); }

  if (appMode == MODE_USB_BT) pollSerialInputs();

if (configMode)
{
    // ===============================
    // ★ Cボタンでトグル画面切替
    // ===============================
if (M5.BtnC.wasPressed())
    {
    ignoreNextCRelease = true;          // ★追加：このCのリリースは通常モードで拾わない
    settingsTogglePage = !settingsTogglePage;

    if (settingsTogglePage) drawToggleUI();
    else                    drawConfigUI();

    return;
 }
    // ===============================
    // ★ トグル画面処理
    // ===============================
    if (settingsTogglePage)
    {
        if (M5.Touch.getCount() > 0)
        {
            auto t = M5.Touch.getDetail(0);

            if (t.wasPressed())
            {
                // ---- Invert Toggle ----
                if (t.y > 60 && t.y < 100)
                {
                    invertMode = !invertMode;
                    applyInvertMode();
                    saveConfig();   
                    drawToggleUI();
                    solenoidFastClick();
                }

                // ---- UI Theme Toggle ----
                if (t.y > 110 && t.y < 160)
                {
                    uiTheme = (uiTheme == THEME_COCKPIT)
                        ? THEME_SOLENOID
                        : THEME_COCKPIT;

                    if (uiTheme == THEME_COCKPIT)
                        initCockpitScene();

                        saveConfig();        // ★即保存
                        drawToggleUI();
                        solenoidFastClick();
                }
            }
        }
        return;
    }

    // ===============================
    // ★ 通常Setting画面処理
    // ===============================
    handleConfigTouch();

    if (triggerPending) {
        triggerPending = false;
        solenoidFastClick();
    }

    // A/B長押しでSetting終了（Cは除外）
    if (M5.BtnA.isHolding() || M5.BtnB.isHolding())
    {
        configMode = false;
        settingsTogglePage = false;   // ★戻す
        saveConfig();
        vibEnabled = true;
        M5.Power.setVibration(0);
        drawMainScreen();
    }

    return;
}

  checkTouchToConfig();

  // デバッグボタン
  if (M5.BtnA.wasPressed())
  {
      if (uiTheme == THEME_COCKPIT)
      {
          // 武装切替
          currentWeapon = WEAPON_MISSILE;
          // 発射エフェクト
          spawnBarrage(2);     // 直接出す
          // 自動でGUNに戻す
          solenoidEffect(); 
      }
      else
      {
          solenoidEffect();  // 既存動作
      }
  }
  if (M5.BtnB.wasPressed()) solenoidEffect();

  // 例：BtnC短押し＝強め演出、長押し＝テーマ切替
  static uint32_t cDownMs = 0;
  if (M5.BtnC.wasPressed()) cDownMs = millis();
  if (M5.BtnC.wasReleased()) {
      if (ignoreNextCRelease) {             // ★追加
    ignoreNextCRelease = false;
    cDownMs = 0;                        // ★ついでにクリアして事故防止
    return;                             // ★このフレームはC操作を無視
  }
    if (millis() - cDownMs > 600) {
      uiTheme = (uiTheme == THEME_SOLENOID) ? THEME_COCKPIT : THEME_SOLENOID;
      if (uiTheme == THEME_COCKPIT) initCockpitScene();
      drawMainScreen(); // ベースUI更新（コクピットなら中で上書きされる）
    } else {
      // 短押しは “強打” の気分
      onFireVisualFX(true);
      solenoidEffect();
      lastFireMs = millis();
      startFastSolenoid();
    }
  }
}