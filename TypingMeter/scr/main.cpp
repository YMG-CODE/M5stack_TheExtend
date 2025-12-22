#include <M5Unified.h>
#define M5_Lcd M5.Display  // Legacy alias for M5Core2 compatibility
#include <Wire.h>
#include <math.h>
#include <Preferences.h>

// ★ 追加
#include <BluetoothSerial.h>
// ==== USB/BT Connection Flags ====
bool btConnected = false;   // セントラルがいれば true
bool usbActive   = false;   // 今フレームで USB Serial に何か来たら true
unsigned long lastCPMTime = 0;  // ← これだけでOK

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

volatile uint8_t activeSource = SRC_NONE;  // 現在の入力ソース
AppMode          appMode      = MODE_I2C;  // デフォルトは I2C

// ==== 定数 ====
constexpr uint8_t I2C_SLAVE_ADDR = 0x0B;
constexpr int CENTER_X = 160;
constexpr int CENTER_Y = 200;
constexpr int RADIUS   = 120;
constexpr int VALUE_MAX = 2000;
constexpr int NEEDLE_STEP = 2;

// ==== 色関連 ====
const uint16_t METER_COLORS[] = {
    GREEN, GREENYELLOW, OLIVE, YELLOW, WHITE,
    RED, MAROON, ORANGE, MAGENTA, BLUE, CYAN, NAVY
};
constexpr uint16_t NEEDLE_COLOR = RED;

// ==== 状態 ====
uint16_t targetValue = 0;
uint16_t prevValue   = 0;
int displayedValue = 0;  
int colorIndex       = 0;
uint16_t meterColor  = METER_COLORS[0];

// ==== CPM統計 ====
int cpmHistory[300];
int historyIndex = 0;
uint32_t sumValue = 0;
int sampleCount = 0;
uint16_t maxCPM = 0;
uint64_t totalKeystrokes = 0;
unsigned long startTime;

// ==== 永続化 ====
unsigned long lastGraphUpdate = 0;
unsigned long lastSaveTime = 0;
constexpr unsigned long SAVE_INTERVAL = 5000; // 5秒ごと保存
const unsigned long GRAPH_UPDATE_INTERVAL = 1000; // 更新間隔 (ms)

#define REPLAY_BLOCK_DURATION 600000  // 10分単位（ミリ秒）
#define REPLAY_SPEED 10000               // 再生速度（ms/frame）
bool isReplaying = false;
unsigned long replayStartTime = 0;
int replayFrameIndex = 0;
uint32_t sumCPM = 0;
uint32_t countCPM = 0;

// ==== ログ画面グラフ棒調整 ====
const int GRAPH_X = 20;
const int GRAPH_Y = 220;
const int GRAPH_WIDTH = 300;
const int GRAPH_HEIGHT = 70;
const int MOVING_AVG_WINDOW = 60;   // 移動平均のサンプル数（直近60サンプル）
int cpmGraph[GRAPH_WIDTH];  // 表示用リングバッファ
int logAvgCPM = 0;   // LOGモード開始時点の固定平均

// ==== フェード用 ====
uint8_t avgFadeAlpha = 0;  // 平均線フェード用
unsigned long lastFadeUpdate = 0;

// ==== 表示モード ====
enum DisplayMode { MODE_METER, MODE_LOG };
DisplayMode displayMode = MODE_METER;

// ==== ユーティリティ ====
inline int valueToAngle(int value) {
    return map(value, 0, VALUE_MAX, -120, 120);
}

inline void polarToXY(int angle, int r, int &x, int &y) {
    float rad = angle * PI / 180.0;
    x = CENTER_X + cos(rad) * r;
    y = CENTER_Y + sin(rad) * r;
}

// ==== レイヤーインジケーター関連 ==== 
int currentLayer = 0; const int MAX_LAYERS = 6; 
// 対応レイヤー数 
const int LAYER_BLOCK_WIDTH = 40; 
const int LAYER_BLOCK_HEIGHT = 12; 
const int LAYER_BASE_Y = 45; 
const uint16_t LAYER_ON_COLOR = TFT_CYAN; 
const uint16_t LAYER_OFF_COLOR = TFT_DARKGREY;
int activeLayer = 0;  // 現在アクティブなレイヤー番号

// ==== ⛽ ポモドーロ関連 ====
bool pomodoroActive = false;
bool pomodoroBreak = false;
bool pomodoroLongMode = false;  // false=25分, true=45分
unsigned long pomodoroStart = 0;
int fuelLevel = 100;
unsigned long lastFuelDraw = 0;

enum PomodoroMode { POMO_OFF, POMO_SHORT, POMO_LONG, POMO_BREAK };
PomodoroMode pomoMode = POMO_OFF;

unsigned long pomoStartTime = 0;
bool fueling = false; // 給油アニメ中フラグ
bool pomoActive = false;

const int SHORT_DURATION = 25 * 60 * 1000;  // 25分
const int LONG_DURATION  = 45 * 60 * 1000;  // 45分
const int BREAK_DURATION = 5 * 60 * 1000;   // 5分休憩

int pomoCycle = 0;  
// 0=OFF, 1=SHORT(25), 2=LONG(45), 3=DEMO


// ==== スクリーンセーバー ====
bool screenSaverActive = false;
unsigned long lastActivityTime = 0;
const unsigned long SCREENSAVER_TIMEOUT = 3000; // 30秒無操作で開始

// 背景スクロール関連
int roadOffset = 0;
int skylineOffset = 0;
unsigned long lastFrameTime = 0;

// ボタンピン
const int btnA_pin = 39;
const int btnB_pin = 38;
const int btnC_pin = 37;

// 割り込みフラグ
volatile bool btnA_pressed = false;
volatile bool btnB_pressed = false;
volatile bool btnC_pressed = false;

// ==== 設定系 ====
bool vibrationEnabled = true;  // デフォルト ON
Preferences prefsVibe;  // ← バイブ専用

// ==== バイブレーション関数（ON時のみ動作） ====
void pulseVibration(int level = 150, int duration = 200) {
    if (!vibrationEnabled) return; // 設定OFFなら無視
    M5.Power.setVibration(level);
    delay(duration);
    M5.Power.setVibration(0);
}

// ==== 色を返す関数 ====
// 800以上なら固定赤、それ以外は選択された色
uint16_t getScaleColor(int value) {
    if (value >= 800) {
        if (meterColor == RED) return ORANGE;
        else return RED;
    }
    return meterColor;
}
uint16_t getCPMColor(int cpm) {
    cpm = constrain(cpm, 0, VALUE_MAX);
    uint8_t r = map(cpm, 0, VALUE_MAX, 0, 255);
    uint8_t g = 0;
    uint8_t b = map(cpm, 0, VALUE_MAX, 255, 0);
    return M5.Display.color565(r, g, b);
}

// ==== メーター背景描画 ====
void drawMeterBackground() {
    M5.Display.setTextDatum(TL_DATUM);
    M5.Display.startWrite();

    M5.Display.fillScreen(BLACK);
    

    // 外周アーク（色スケール）
    for (int a = -120; a <= 120; a++) {
        int px, py;
        polarToXY(a, RADIUS, px, py);
        int v = map(a, -120, 120, 0, VALUE_MAX);
        uint16_t col = getScaleColor(v);
        M5.Display.drawPixel(px, py, col);
    }

    // メモリ数字と補助線
    for (int i = 0; i <= 5; i++) {
        int value = i * 200;
        int angle = valueToAngle(value);

        int tx, ty, lx1, ly1, lx2, ly2;
        polarToXY(angle, RADIUS + 20, tx, ty);
        polarToXY(angle, RADIUS - 10, lx1, ly1);
        polarToXY(angle, RADIUS - 2,  lx2, ly2);

        uint16_t c = getScaleColor(value);

        M5.Display.setTextSize(2);
        M5.Display.setTextColor(c);
        M5.Display.setTextDatum(TL_DATUM);  // もう一度明示
        M5.Display.setCursor(tx - 10, ty - 10);
        M5.Display.drawLine(lx1, ly1, lx2, ly2, c);

        if (value == 1000) {
            M5.Display.print("1K");
        } else {
            M5.Display.printf("%d", value);
        }
        M5.Display.endWrite();
    }
}

void drawNeedle(int value, int oldValue) {
    // 古い針を消す
    int oldAngle = valueToAngle(oldValue);
    int oldX, oldY;
    polarToXY(oldAngle, RADIUS, oldX, oldY);
    M5.Display.drawLine(CENTER_X - 1, CENTER_Y, oldX - 1, oldY, BLACK);
    M5.Display.drawLine(CENTER_X,     CENTER_Y, oldX,     oldY, BLACK);
    M5.Display.drawLine(CENTER_X + 1, CENTER_Y, oldX + 1, oldY, BLACK);
    M5.Display.fillCircle(CENTER_X, CENTER_Y, 5, BLACK);

    // 新しい針
    int angle = valueToAngle(value);
    int x, y;
    polarToXY(angle, RADIUS, x, y);
    M5.Display.drawLine(CENTER_X - 1, CENTER_Y, x - 1, y, NEEDLE_COLOR);
    M5.Display.drawLine(CENTER_X + 1, CENTER_Y, x + 1, y, NEEDLE_COLOR);
    M5.Display.drawLine(CENTER_X,     CENTER_Y, x,     y, NEEDLE_COLOR);

    // 根元とハブ
    M5.Display.fillCircle(CENTER_X, CENTER_Y, 5, NEEDLE_COLOR);
    M5.Display.fillCircle(CENTER_X, CENTER_Y, 2, meterColor);

    // 中央に CPM 表示
    uint16_t c = getScaleColor(value);  // ここで色を判定
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(c, BLACK);
    M5.Display.setCursor(CENTER_X - 40, CENTER_Y + 10);
    M5.Display.printf("%d CPM  ", value);

    // 外形線再描画
for (int a = -120; a <= 120; a++) {
    int px, py;
    polarToXY(a, RADIUS, px, py);
    int v = map(a, -120, 120, 0, VALUE_MAX);
    uint16_t c = getScaleColor(v);
    M5.Display.drawPixel(px, py, c);
}
for (int i = 0; i <= 5; i++) {
    int value = i * 200;
    int angle = valueToAngle(value);

    int tx, ty, lx1, ly1, lx2, ly2;
    polarToXY(angle, RADIUS + 20, tx, ty);
    polarToXY(angle, RADIUS - 10, lx1, ly1);
    polarToXY(angle, RADIUS - 2,  lx2, ly2);

    uint16_t c = getScaleColor(value);

    M5.Display.setTextSize(2);
    M5.Display.setTextColor(c);
    M5.Display.setCursor(tx - 10, ty - 10);
    M5.Display.drawLine(lx1, ly1, lx2, ly2, c);

    if (value == 1000) {
        M5.Display.print("1K");
    } else {
        M5.Display.printf("%d", value);
    }
}
}






// ====統計数値の桁切り====
String formatWithK(uint64_t num) {
    if (num >= 1000000) return String((float)num / 1000000.0, 2) + "M";
    if (num >= 1000)    return String((float)num / 1000.0, 2) + "K";
    return String(num);
}

// ==== 統計更新 + decay処理 ====  
void updateStats(int cpm) {
    static unsigned long lastUpdate = millis();
    static int lastCPM = 0;           // 最後の打鍵値保持
    static unsigned long lastKeystroke = 0; // 最後の打鍵時刻

    unsigned long now = millis();
    float elapsedMin = (now - lastUpdate) / 60000.0;  // 経過分数
    lastUpdate = now;

    // ==== decay保持処理 ====
    if (cpm > 0) {
        lastCPM = cpm;
        lastKeystroke = now;
    } else {
        // 打鍵停止後5秒間は前の値を保持
        if (now - lastKeystroke <= 5000) {
            cpm = lastCPM;
        } else {
            lastCPM = 0; // decay完了
        }
    }

    // ==== 統計更新 ====
    totalKeystrokes += (uint64_t)(cpm * elapsedMin) / 2;  // CPM → 打鍵数換算
    if (cpm > maxCPM) maxCPM = cpm;
    sumValue += cpm;
    sampleCount++;

    // 履歴に保存（常に最新が右端にくる）
    cpmHistory[historyIndex] = cpm;
    historyIndex = (historyIndex + 1) % 300;

    // === 各Modeで記録 ===
        for (int i = 0; i < GRAPH_WIDTH - 1; i++) {
            cpmGraph[i] = cpmGraph[i + 1];
        }
        cpmGraph[GRAPH_WIDTH - 1] = cpm;
}

// ==== CPM / Layer 共通適用ヘルパ ====
// ==== 打鍵中だけ統計を取る平均CPM専用カウンタ ====
// すべての外部入力（I2C / USB / BT / DEMO）からの CPM はここを経由させる
void applyCPM(uint16_t cpm) {

    if (cpm > VALUE_MAX) cpm = VALUE_MAX;

    // メインメーター更新
    targetValue = cpm;
    lastActivityTime = millis();

    // USB/BT の通信時刻更新（針戻し用）
    if (appMode == MODE_USB_BT) {
        lastCPMTime = millis();
    }

    // ==== ★ 統計処理（打鍵している間だけ）====
    if (cpm > 0) {
        // 平均CPM用の積算（0 は除外）
        sumCPM += cpm;
        countCPM++;

        // KS（キーストローク）集計
        totalKeystrokes += (cpm / 60);
    }

    // ==== 最大CPM更新 ====
    if (cpm > maxCPM) {
        maxCPM = cpm;
    }

    // ==== グラフ/リプレイ用ヒストリ ====
    cpmHistory[historyIndex] = cpm;
    historyIndex = (historyIndex + 1) % 300;
}



void updateGraphHistory(int cpm) {
    static unsigned long lastGraphUpdate = 0;
    unsigned long now = millis();

    if (now - lastGraphUpdate >= GRAPH_UPDATE_INTERVAL) {
        lastGraphUpdate = now;

        // 左シフト
        for (int i = 0; i < GRAPH_WIDTH - 1; i++) {
            cpmGraph[i] = cpmGraph[i + 1];
        }
        cpmGraph[GRAPH_WIDTH - 1] = cpm;
    }
        // 一部のみ再描画（ちらつき防止）
    int baseX = GRAPH_X, baseY = GRAPH_Y, graphW = GRAPH_WIDTH, graphH = GRAPH_HEIGHT;
    M5.Display.fillRect(baseX, baseY - graphH, graphW, graphH, BLACK);

    // グリッド線
    for (int i = 0; i <= 4; i++) {
        int y = baseY - (graphH * i / 4);
        M5.Display.drawLine(baseX, y, baseX + graphW, y, TFT_DARKGREY);
    }

    // 折れ線
    int prevY = baseY - map(cpmGraph[0], 0, VALUE_MAX, 0, graphH);
    for (int i = 1; i < GRAPH_WIDTH; i++) {
        int x1 = baseX + i - 1;
        int x2 = baseX + i;
        int y2 = baseY - map(cpmGraph[i], 0, VALUE_MAX, 0, graphH);
        uint16_t col = getCPMColor(cpmGraph[i]);
        M5.Display.drawLine(x1, prevY, x2, y2, col);
        M5.Display.drawLine(x1, prevY+1, x2, y2+1, col);
        prevY = y2;
    }
}

// ==== 起動してからの生涯平均CPM ====
// ==== 実打鍵の平均CPM（0を除外） ====
// cpmGraph[] の中で 0 でない値だけ平均化する
int getMovingAverageCPM() {
return (countCPM > 0) ? (sumCPM / countCPM) : 0;
}


// ==== ログ画面 ====
void drawLogScreen() {
    M5.Display.fillScreen(BLACK);
     // ==== LOGモードの平均値を確定 ====
    logAvgCPM = getMovingAverageCPM();  // ←新しく作る関数

    // タイトルバー
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setTextSize(3);
    M5.Display.drawString("LOG MODE", 160, 20);
    M5.Display.drawLine(10, 40, 310, 40, TFT_DARKGREY);

    // === 統計情報 ===
    int avgCPM = getMovingAverageCPM();  // リアルタイム平均（直近60サンプル）
    unsigned long elapsed = (millis() - startTime) / 1000;

    M5.Display.setTextDatum(TL_DATUM);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_YELLOW);
    M5.Display.setCursor(15, 47); 
    M5.Display.printf("AvgCPM: %d", avgCPM);

    M5.Display.setTextColor(TFT_RED);
    M5.Display.setCursor(15, 72); 
    M5.Display.printf("MaxCPM: %d", maxCPM);

    M5.Display.setTextColor(TFT_GREEN);
    M5.Display.setCursor(15, 97); 
    M5.Display.printf("TotalKS: %s", formatWithK(totalKeystrokes).c_str());

    M5.Display.setTextColor(TFT_LIGHTGREY);
    M5.Display.setCursor(15, 122);
    M5.Display.printf("Uptime %02lu:%02lu:%02lu", elapsed/3600, (elapsed%3600)/60, elapsed%60);

    // ==== リプレイ開始 ====
    isReplaying = true;
    replayStartTime = millis();
    replayFrameIndex = 0;
}

// ==== グラフ描画（リプレイ再生アニメーション＋時間スケール対応＋余白調整） ====
void drawReplayFrameAnimated(int baseX, int baseY, int graphW, int graphH) {
    static unsigned long lastFrameTime = 0;
    static bool initialized = false;
    const int frameInterval = 80;       // フレーム間隔（ms）
    const int valueRangeMax = max(1000, (maxCPM / 200 + 1) * 200);  // 動的スケール
    const int SAMPLE_INTERVAL_MS = 2000; // サンプリング間隔（例: 2秒）
    const int TIME_STEP_SEC = 10;       // X軸ラベル間隔（秒）
    const int MARGIN_LEFT = 30;         // 左の余白（ラベル分）

// ==== フレーム更新間隔制御 ====
// 初回（initialized=false）はスキップ禁止
if (initialized && millis() - lastFrameTime < frameInterval) return;
lastFrameTime = millis();

    int totalHistory = min(300, historyIndex);
    if (totalHistory == 0) return;

    // ==== Cボタンで再生中断 ====
    if (M5.BtnC.wasPressed()) {
        isReplaying = false;
        replayFrameIndex = 0;
        initialized = false;
        return;
    }

    // ==== 初期化（LOG画面切替時のみ） ====
    if (!initialized) {
        M5.Display.fillRect(baseX - 1, baseY - graphH - 1, graphW + 2, graphH + 25, BLACK);
        
        int graphStartX = baseX + MARGIN_LEFT;
        int graphEndX = baseX + graphW - 1;

        // 軸線
        M5.Display.drawLine(graphStartX, baseY, graphEndX, baseY, TFT_DARKGREY); // X軸
        M5.Display.drawLine(graphStartX, baseY - graphH, graphStartX, baseY, TFT_DARKGREY); // Y軸

        // ==== Y軸目盛り ====
        const int step = valueRangeMax / 5;  // 5分割
         for (int v = 0; v <= valueRangeMax; v += step) {
         int y = baseY - map(v, 0, valueRangeMax, 0, graphH);
         M5.Display.setTextSize(1);
         M5.Display.setTextColor(TFT_DARKGREY, BLACK);
         M5.Display.setCursor(baseX + 2, y - 3);
         M5.Display.printf("%d", v);
         M5.Display.drawLine(graphStartX - 3, y, graphStartX, y, TFT_DARKGREY);
        }

        // ==== X軸時間スケール ====
        int totalSec = totalHistory * (SAMPLE_INTERVAL_MS / 1000);
        for (int t = 0; t <= totalSec; t += TIME_STEP_SEC) {
            int x = graphStartX + map(t, 0, totalSec, 0, graphW - MARGIN_LEFT);
            M5.Display.drawLine(x, baseY, x, baseY + 3, TFT_DARKGREY);
        }

        replayFrameIndex = 0;
        initialized = true;
    }

    // ==== 折れ線グラフ描画 ====
    delay(100);
    int graphStartX = baseX + MARGIN_LEFT;
    int graphEndX = baseX + graphW - 1;

    int index = (historyIndex - totalHistory + replayFrameIndex + 300) % 300;
    int cpm = cpmHistory[index];
    int prevIndex = (index - 1 + 300) % 300;
    int prevCPM = cpmHistory[prevIndex];

    float timePerSample = (float)(graphW - MARGIN_LEFT) / totalHistory; // 横方向スケール
    int x1 = graphStartX + (replayFrameIndex - 1) * timePerSample;
    int x2 = graphStartX + replayFrameIndex * timePerSample;
    int y1 = baseY - map(prevCPM, 0, valueRangeMax, 0, graphH);
    int y2 = baseY - map(cpm,0, valueRangeMax, 0, graphH);
    uint16_t col = getCPMColor(cpm);

    if (replayFrameIndex > 0 && x2 <= graphEndX)
        M5.Display.drawLine(x1, y1, x2, y2, col);

    replayFrameIndex++;

    // ==== 平均線 ====
    int avgCPM = getMovingAverageCPM();
    int avgY = baseY - map(avgCPM, 0, valueRangeMax, 0, graphH);
    for (int x = graphStartX; x < graphEndX; x += 6)
        M5.Display.drawPixel(x, avgY, TFT_WHITE);

    // ==== 平均ラベル（右端固定） ====
    int labelX = graphEndX - 70;
    int labelY = constrain(avgY - 6, baseY - graphH + 5, baseY - 10);
    M5.Display.setTextSize(1);
    M5.Display.fillRoundRect(labelX - 4, labelY - 2, 55, 12, 3, BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(labelX, labelY);
    M5.Display.printf("Avg:%d", avgCPM);

    // ==== グラフ下ラベル ====
    int displayWidth = min(graphW, totalHistory);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_DARKGREY, BLACK);
    M5.Display.setCursor(graphStartX + (graphW - MARGIN_LEFT) / 2 - 30, baseY + 10);
    M5.Display.printf("Last %d sec", displayWidth * (SAMPLE_INTERVAL_MS / 1000));

    // ==== 再生終了処理（1回のみ再生） ====
    if (replayFrameIndex >= totalHistory) {
        isReplaying = false;
        replayFrameIndex = 0;
        initialized = false;
        // 🔸 LOG切替時バイブ（短く弱め）
        pulseVibration(150, 200);
        return;
    }
}


// ==== 保存関数 ====
void saveStats() {
    prefs.putULong64("totalKeystrokes", totalKeystrokes);
    prefs.putUInt("maxCPM", maxCPM);
    prefs.putUInt("sumValue", sumValue);
    prefs.putUInt("sampleCount", sampleCount);
}

// ==== 針削除単独実行====
void ClearNeedle(int value, int oldValue) {
    int oldAngle = valueToAngle(oldValue);
    int oldX, oldY;
    polarToXY(oldAngle, RADIUS, oldX, oldY);
    M5.Display.drawLine(CENTER_X - 1, CENTER_Y, oldX - 1, oldY, BLACK);
    M5.Display.drawLine(CENTER_X,     CENTER_Y, oldX,     oldY, BLACK);
    M5.Display.drawLine(CENTER_X + 1, CENTER_Y, oldX + 1, oldY, BLACK);
    M5.Display.fillCircle(CENTER_X, CENTER_Y, 5, BLACK);
}
// ガソリンアイコン
void drawFuelIcon(int x, int y, uint16_t color) {
    // ベース：ポンプ本体
    M5.Display.fillRoundRect(x, y, 14, 20, 2, color);
    
    // ノズル部分
    M5.Display.fillRect(x + 10, y + 2, 6, 3, color);
    M5.Display.drawLine(x + 15, y + 3, x + 17, y + 6, color);
    M5.Display.drawLine(x + 17, y + 6, x + 13, y + 9, color);
    
    // ディスプレイ（小窓）
    M5.Display.fillRect(x + 3, y + 3, 6, 6, BLACK);
    
    // ホース（黒線）
    M5.Display.drawLine(x + 13, y + 6, x + 10, y + 18, BLACK);
}
// バッテリーアイコン
void drawBatteryIcon(int x, int y, int level, uint16_t color) {
    // 外枠
    M5.Display.drawRect(x, y, 20, 10, color);
    M5.Display.fillRect(x + 20, y + 3, 2, 4, color); // 端子

    // バッテリーレベル（0～100）
    int fillWidth = map(level, 0, 100, 0, 18);
    if (fillWidth > 0) {
        M5.Display.fillRect(x + 1, y + 1, fillWidth, 8, color);
    }
}
// オイル圧（または警告）アイコン
void drawPressureIcon(int x, int y, uint16_t color) {
    // オイル缶の形
    M5.Display.drawLine(x, y + 6, x + 8, y + 6, color);
    M5.Display.drawLine(x + 8, y + 6, x + 10, y + 3, color);
    M5.Display.drawLine(x + 10, y + 3, x + 14, y + 3, color);
    M5.Display.drawLine(x + 14, y + 3, x + 14, y + 9, color);
    M5.Display.drawLine(x + 14, y + 9, x, y + 9, color);
    M5.Display.drawLine(x, y + 9, x, y + 6, color);

    // 注ぎ口
    M5.Display.drawLine(x + 10, y + 3, x + 12, y + 1, color);

    // 一滴
    M5.Display.fillCircle(x + 16, y + 10, 2, color);
}
// サイドブレーキアイコン
void drawHandBrakeIcon(int x, int y, uint16_t color) {
    // 丸（背景）
    M5.Display.fillCircle(x + 10, y + 10, 10, BLACK);   // 背景黒
    M5.Display.drawCircle(x + 10, y + 10, 10, color);   // 外枠

    // "!"マーク
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(color, BLACK);
    M5.Display.setCursor(x + 5, y + 2);  // 丸の中心に合わせる
    M5.Display.print("!");
}

// ==== タイトルをタイプ演出で描画 ====
void drawTitleTyping() {
    const char* title = "TheExtEnd_Drive";
    uint16_t textColor = meterColor;  // 既存のメーター色を使用
    int x = 65;
    int y = 20;
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(meterColor);

    // 1文字ずつ描画
    String buffer = "";
    for (int i = 0; title[i] != '\0'; i++) {
        buffer += title[i];
        M5.Display.fillRect(0, 0, 160, 40, BLACK);  // 前の文字を消して再描画（ちらつき防止）
        M5.Display.setCursor(x, y);
        M5.Display.print(buffer);
        delay(25);  // ← タイピング速度（調整可）
    }
}

// ==== 🚗 シフトノブ インジケータ ====
enum ShiftMode { SHIFT_P, SHIFT_R, SHIFT_N, SHIFT_D, SHIFT_M };
ShiftMode currentShift = SHIFT_P;
ShiftMode targetShift  = SHIFT_P;
unsigned long lastShiftAnim = 0;
const int SHIFT_ANIM_DURATION = 300; // アニメーション時間(ms)

// シフト位置のX座標（ノブ中心座標）
const int shiftX[] = { 300, 280, 260, 240, 220 }; // P,R,N,D,M
const int shiftY = 30;

// ギア文字描画
const char* shiftLabel[] = { "0", "1", "2", "3", "R" };

// ==== シフトインジケータ描画 ====
void drawShiftIndicator_light() {
    static ShiftMode lastDrawnShift = SHIFT_P;
    static int lastKnobX = -1;

    // アニメーション進行
    float t = min(1.0f, (millis() - lastShiftAnim) / (float)SHIFT_ANIM_DURATION);
    int fromX = shiftX[currentShift];
    int toX   = shiftX[targetShift];
    int knobX = fromX + (toX - fromX) * t;
    
        // 背景部分のみクリア
        M5.Display.fillRect(210, 0, 120, 50, BLACK);

        // 各ギア文字
        for (int i = 0; i < 5; i++) {
            uint16_t color = (i == targetShift) ? meterColor : TFT_DARKGREY;
            M5.Display.setTextSize(2);
            M5.Display.setTextColor(color, BLACK);
            M5.Display.setCursor(shiftX[i] - 5, shiftY - 25);
            M5.Display.print(shiftLabel[i]);
        }

        // ノブ描画
        M5.Display.fillCircle(knobX, shiftY, 5, meterColor);
        //M5.Display.drawCircle(knobX, shiftY, 9, TFT_WHITE);

    }

void drawShiftIndicator() {
    static ShiftMode lastDrawnShift = SHIFT_P;
    static int lastKnobX = -1;

    // アニメーション進行
    float t = min(1.0f, (millis() - lastShiftAnim) / (float)SHIFT_ANIM_DURATION);
    int fromX = shiftX[currentShift];
    int toX   = shiftX[targetShift];
    int knobX = fromX + (toX - fromX) * t;
    

    // ノブが移動した or シフトが変わったときだけ再描画
    if (knobX != lastKnobX || currentShift != lastDrawnShift) {
        // 背景部分のみクリア
        M5.Display.fillRect(210, 0, 120, 50, BLACK);

        // 各ギア文字
        for (int i = 0; i < 5; i++) {
            uint16_t color = (i == targetShift) ? meterColor : TFT_DARKGREY;
            M5.Display.setTextSize(2);
            M5.Display.setTextColor(color, BLACK);
            M5.Display.setCursor(shiftX[i] - 5, shiftY - 25);
            M5.Display.print(shiftLabel[i]);
        }

        // ノブ描画
        M5.Display.fillCircle(knobX, shiftY, 5, meterColor);
        //M5.Display.drawCircle(knobX, shiftY, 9, TFT_WHITE);

        lastKnobX = knobX;
        lastDrawnShift = currentShift;
    }

    // アニメーション完了
    if (t >= 0.05f && currentShift != targetShift) {
        currentShift = targetShift;
    }
}
// ==== シフト変更関数 ====
void changeShift(ShiftMode next) { 
    if (next != currentShift) { 
        targetShift = next; lastShiftAnim = millis(); 
    }
 }

unsigned long lastLayerChange = 0;

void setActiveLayer(int newLayer) {
    if (newLayer == activeLayer) return;

    activeLayer = newLayer;
    lastLayerChange = millis();

    // シフト更新
    switch (newLayer) {
        case 0: changeShift(SHIFT_P); break;
        case 1: changeShift(SHIFT_R); break;
        case 2: changeShift(SHIFT_N); break;
        case 3: changeShift(SHIFT_D); break;
        case 4: changeShift(SHIFT_M); break;
    }
}


// Layer も共通
void applyLayer(uint8_t layer) {
    if (layer > 4) return;  // 0〜4 を許容
    
    // Typing Meter の正式レイヤ更新関数へ委譲
    setActiveLayer(layer);

    // 通信ソース別インジケータ更新（任意）
    activeSource = (appMode == MODE_I2C) ? SRC_I2C :
                   (appMode == MODE_USB_BT) ? SRC_USB :
                   (appMode == MODE_DEMO) ? SRC_NONE : SRC_NONE;
}


// ==== 小型ガソリンメーター描画（右下E・左上F配置・針反転＋モード別数値ラベル） ====
void drawFuelMeter(int level) {
    int cx = 45;     // 中心X
    int cy = 230;    // 中心Y
    int r = 46 ;      // 半径
    const int ANGLE_OFFSET = 15;  
    const int START_ANGLE = -140 + ANGLE_OFFSET-6;  // F位置（左上）
    const int END_ANGLE   = -40 + ANGLE_OFFSET-20;   // E位置（右下）
    const int SWEEP = END_ANGLE - START_ANGLE;
    const int RED_ZONE_PERCENT = 25;   // 残量15%以下を赤エリアに設定
    const int RED_ZONE_ANGLE   = (SWEEP * RED_ZONE_PERCENT) / 100; // 約15°相当

    // --- 背景 ---
    M5.Display.fillRect(cx - r - 6, cy - r - 6, r * 2 + 12, r * 2 + 12, BLACK);

    // --- 外円弧 ---
    for (int i = 0; i <= SWEEP; i++) {
        int a = START_ANGLE + i;
        // E側15%分をレッドゾーン
        uint16_t col = (i < RED_ZONE_ANGLE) ? TFT_RED : meterColor;
        int x = cx + cos(a * PI / 180.0) * r;
        int y = cy + sin(a * PI / 180.0) * r;
        M5.Display.drawPixel(x, y, col);
    }

// --- メモリ線 ---
const int tickCount = 4;  // 0%, 25%, 50%, 75%, 100%
for (int i = 0; i <= tickCount; i++) {
    int val = i * 25;
    int a = END_ANGLE - (val * (SWEEP) / 100);

    // 🔴 E側2本分（0%と25%）をレッドゾーン化
    uint16_t col = (i >= tickCount - 1) ? TFT_RED :  meterColor;

    int x1 = cx + cos(a * PI / 180.0) * (r - 5);
    int y1 = cy + sin(a * PI / 180.0) * (r - 5);
    int x2 = cx + cos(a * PI / 180.0) * (r + 1);
    int y2 = cy + sin(a * PI / 180.0) * (r + 1);
    M5.Display.drawLine(x1, y1, x2, y2, col);
}
    // --- 針（反時計回り F→E）---
    // F=100, E=0 → 値が小さくなるほど右へ回る
    int a = END_ANGLE - ((100 - level) * SWEEP / 100);
    int nx = cx + cos(a * PI / 180.0) * (r - 10);
    int ny = cy + sin(a * PI / 180.0) * (r - 10);
    M5.Display.drawLine(cx, cy, nx, ny, TFT_RED);
    M5.Display.fillCircle(cx, cy, 3, TFT_RED);

    // --- E / F ラベル ---
    int fX = cx + cos(START_ANGLE * PI / 180.0) * (r + 10);
    int fY = cy + sin(START_ANGLE * PI / 180.0) * (r + 10);
    int eX = cx + cos(END_ANGLE * PI / 180.0) * (r + 10);
    int eY = cy + sin(END_ANGLE * PI / 180.0) * (r + 10);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_RED, BLACK);
    M5.Display.setCursor(fX + 4, fY +18);
    M5.Display.print("E");
    M5.Display.setTextColor(meterColor, BLACK);
    M5.Display.setCursor(eX - 7, eY +16);
    M5.Display.print("F");
}

// ==== 残り時間表示（燃料計の上にオーバーレイ） ====
void drawFuelTimeOverlay(unsigned long remainingMs, bool isDemo) {
    // Fuelメーターの位置・サイズ（今の実装に合わせて固定）
    const int cx = 45;
    const int cy = 80;
    const int r  = 46;

    // クリア領域（燃料計の上の帯を消去）
    // 幅広めに取って前回描画を確実に消す
    int clearX = cx - (r + 25);
    int clearY = cy - (r + 30);
    int clearW = (r * 2) + 50;
    int clearH = 18;
    M5.Display.fillRect(clearX, clearY, clearW, clearH, BLACK);

    // 残り時間のフォーマット
    char buf[24];
    if (isDemo) {
        // 秒表示（切り上げ）
        unsigned long sec = (remainingMs + 999) / 1000;
        snprintf(buf, sizeof(buf), "%lu sec", sec);
    } else {
        // 分表示（切り上げ）
        unsigned long min = (remainingMs + 59999) / 60000;
        snprintf(buf, sizeof(buf), "%lu min", min);
    }

    // 中央寄せで表示
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(meterColor, BLACK);
    M5.Display.drawString(buf, cx, cy - r - 20);
    M5.Display.setTextDatum(TL_DATUM); // 以降の描画に影響しないよう戻す
}



// ==== ポモドーロ進行・描画統合関数（BREAK中に⛽点滅アニメ付き） ====
void updatePomodoro() {
    static unsigned long lastFuelDraw = 0;
    static bool fillingVisible = false;
    static unsigned long lastBlink = 0;

    // 無効・給油中なら更新しない
    if (pomoMode == POMO_OFF || fueling) return;

    unsigned long elapsed = millis() - pomoStartTime;
    unsigned long total = 0;

    // --- 各モードごとの時間設定 ---
    if (pomoCycle == 3) {                     // デモモード
        total = 5000;                         // 5秒でEmpty
    } else if (pomoMode == POMO_SHORT) {      // 25分
        total = SHORT_DURATION;
    } else if (pomoMode == POMO_LONG) {       // 45分
        total = LONG_DURATION;
    } else if (pomoMode == POMO_BREAK) {      // 休憩中
        total = (pomoCycle ==3 ) ? 3000 : BREAK_DURATION;  // デモ時のみ3秒でFill
    }

    // --- 燃料ゲージ進行 ---
    int newLevel = (pomoMode == POMO_BREAK)
        ? map(elapsed, 0, total, 0, 100)
        : map(elapsed, 0, total, 100, 0);
    newLevel = constrain(newLevel, 0, 100);

    fuelLevel = newLevel;  // 値だけ更新（常に進行）

    // ==== 🔸描画はメーターモードのときのみ ====
    if (displayMode != MODE_LOG) {
        if (millis() - lastFuelDraw > 200) {
            drawFuelMeter(newLevel);
            lastFuelDraw = millis();
        }

         // --- 残り時間オーバーレイ（燃料計の上） ---
    {        
        unsigned long remaining =
            (elapsed >= total) ? 0UL : (total - elapsed);
        bool isDemo = (pomoCycle == 3); // デモモードなら秒表示
        drawFuelTimeOverlay(remaining, isDemo);
    }
        // --- レッドゾーン警告（燃料残量が少ないとき） ---
        if (fuelLevel <= 20 && pomoMode != POMO_BREAK) {  // 残量20%以下かつBREAK中でない
            const int iconX = CENTER_X - 140;  // 左側（Fuelメーター位置）
            const int iconY = CENTER_Y -45;   // メーター上部
            drawFuelIcon(iconX, iconY, TFT_ORANGE); // ⛽アイコン
            // 🔸 バイブ通知（弱め1回・短く）
            static bool lowFuelVibeDone = false;
            if (!lowFuelVibeDone) {
                pulseVibration(150, 200);
                lowFuelVibeDone = true;
            }
            else {
            // 燃料回復 → フラグ解除
            static bool lowFuelVibeDone = false;
            lowFuelVibeDone = false;
            }
        }
        if (pomoMode == POMO_BREAK) {
            const unsigned long BLINK_INTERVAL = 800;  // 点滅間隔 (ms)
            static bool fillingVisible = false;
            static unsigned long lastBlink = 0;
            if (millis() - lastBlink > BLINK_INTERVAL) {
                lastBlink = millis();
                fillingVisible = !fillingVisible;

                if (fillingVisible) {
                // === 表示フェーズ ===
                    M5.Display.setTextSize(2);
                    M5.Display.setTextColor(TFT_GREEN, BLACK);
                    M5.Display.setCursor(CENTER_X -150, CENTER_Y-50); // テキストをメーター上に
                    M5.Display.print("Refueling!");
                } else {
                    // === 消去フェーズ ===
                    M5.Display.fillRect(CENTER_X - 150, CENTER_Y-50, 120, 30, BLACK);
                }
            }
            // === 最終的に確実に消去（BREAK終了時など） ===
            if (elapsed >= total && fillingVisible) {
            fillingVisible = false;
            M5.Display.fillRect(CENTER_X - 150, CENTER_Y - 50, 120, 30, BLACK);
            }
        }

    }
    // --- モード遷移 ---
    if (elapsed >= total) {
        if (pomoMode == POMO_SHORT || pomoMode == POMO_LONG) {
            // 作業終了 → BREAK
            pomoMode = POMO_BREAK;
            pomoStartTime = millis();

            if (displayMode != MODE_LOG) {
                // === EMPTY! 点滅演出 ===
                const int blinkCount = 6;      // 点滅回数
                const int blinkInterval = 250; // 点滅間隔(ms)
                for (int i = 0; i < 2; i++) {
                    pulseVibration(150, 300);
                    delay(100);
                }

                for (int i = 0; i < blinkCount; i++) {
                    if (i % 2 == 0) {
                        // 表示
                        M5.Display.setTextColor(TFT_RED, BLACK);
                        M5.Display.setTextSize(3);
                        M5.Display.setCursor(CENTER_X - 50, CENTER_Y + 10);
                        M5.Display.print("EMPTY!");
                    } else {
                        // 消去（背景塗りつぶし）
                        M5.Display.fillRect(CENTER_X - 50, CENTER_Y + 10, 190, 40, BLACK);
                    }
                    delay(blinkInterval);
                }
                // 完了後にクリア
                M5.Display.fillRect(CENTER_X - 50, CENTER_Y + 7, 190, 40, BLACK);
                M5.Display.fillRect(CENTER_X - 140, CENTER_Y - 45, 30, 25, BLACK);
            }
        }
        else if (pomoMode == POMO_BREAK) {
    // BREAK終了 → 給油
    if (displayMode != MODE_LOG) {
        const int blinkCount = 6;
        const int blinkInterval = 300;
        const int textX = CENTER_X - 55;
        const int textY = CENTER_Y + 10;
        // Fuelメーターの位置・サイズ（今の実装に合わせて固定）
        const int cx = 45;
        const int cy = 80;
        const int r  = 46;
        // クリア領域（燃料計の上の帯を消去）
        // 幅広めに取って前回描画を確実に消す
        int clearX = cx - (r + 25);
        int clearY = cy - (r + 30);
        int clearW = (r * 2) + 50;
        int clearH = 18;

        // 🔸 READY到達時バイブレーション通知（1回長め）
        pulseVibration(150, 300);
        
        for (int i = 0; i < blinkCount; i++) {
            if (i % 2 == 0) {
                M5.Display.setTextColor(TFT_GREEN, BLACK);
                M5.Display.setTextSize(3);
                M5.Display.setCursor(textX, textY);
                M5.Display.print("READY_");
            } else {
                M5.Display.fillRect(textX, textY, 150, 40, BLACK);
            }
            delay(blinkInterval);
        }

        M5.Display.fillRect(textX, textY, 150, 40, BLACK);
        M5.Display.fillRect(clearX, clearY, clearW, clearH, BLACK);
    }

    // 🔁 OFFでなければ次の作業へ戻る
    if (pomoCycle != 0) {
        pomoMode = (pomoCycle == 2) ? POMO_LONG : POMO_SHORT; // 前回と同じ長さ
        pomoStartTime = millis();
        fuelLevel = 100;
        drawFuelMeter(fuelLevel);

        // 「NEXT SESSION!」を一瞬表示
        M5.Display.setTextColor(TFT_ORANGE, BLACK);
        M5.Display.setTextSize(2);
        M5.Display.fillRect(5, 5, 210, 40, BLACK);
        M5.Display.setCursor(10, 10);
        M5.Display.print("NEXT SESSION_");
        delay(1000);
        M5.Display.fillRect(5, 5, 210, 40, BLACK);
    } else {
        pomoMode = POMO_OFF;
    }
}
    }
}

void registerActivity() {
    lastActivityTime = millis();
    if (screenSaverActive) {
        screenSaverActive = false;
        M5.Display.fillScreen(BLACK);
        drawMeterBackground();
        drawFuelMeter(fuelLevel);
    }
}


void drawNightCityDrive() {
    static float zOffset = 0;
    static float roadCurve = 0;
    static float bgCurve = 0;
    static bool curveRight = true;
    static unsigned long lastFrame = 0;
    static bool Weathinitialized = false;

    unsigned long now = millis();
    if (now - lastFrame < 33) return;  // 約30fps
    lastFrame = now;

// === 信号状態管理 ===
static unsigned long signalStartTime = millis();
static int signalPhase = 0;
static int signalColor = TFT_GREEN;
static float speed = 0;

// ---- カーブ状態を先にチェック ----
static int curveTimer = 0;
static float curveTarget = 0.0f;
static bool isCurving = false;
static float roadCurvePrev = 0.0f;  // ← 前フレームの角度を保持して変化を確認

// === 信号フェーズ進行（カーブ中は一時停止） ===
if (!isCurving) {
    unsigned long elapsed = (millis() - signalStartTime) / 1000;
    signalPhase = elapsed % 37;
} else {
    signalStartTime = millis() - (signalPhase * 1000);
}

// === 信号状態を先に決定（※ speed をここで更新） ===
if (signalPhase < 30) {
    signalColor = TFT_GREEN;
    speed = 12.5f;
} else if (signalPhase < 33) {
    signalColor = TFT_YELLOW;
    speed = 8.5f;
} else {
    signalColor = TFT_RED;
    speed = 0.0f;
}

// === 信号表示ロジック（カーブ中は非表示） ===
bool drawSignal = false;
if (!isCurving) {
    if ((signalColor == TFT_GREEN && signalPhase >= 28 && signalPhase < 30) ||
        signalColor == TFT_YELLOW ||
        signalColor == TFT_RED ||
        (signalColor == TFT_GREEN && signalPhase >= 0 && signalPhase < 4)) {
        drawSignal = true;
    }
}

// === カーブ挙動（左右ランダム化・自然な流れ） ===
curveTimer++;
if (speed > 0.1f) {
    zOffset += speed;
    if (zOffset > 1000) zOffset = 0;

// ---- カーブ開始トリガー ----| 
//直線をもっと長く | `curveTimer > 250` とする |
//| カーブ発生を減らす | `random(0,100)<30` とする |
//| 緩やかなカーブ中心に | `random(15,30)` に変更 |
//| 大きくうねる峠道風 | `random(30,60)` に変更 |
    if (!isCurving && curveTimer > 200) {
        if (random(0, 100) < 70) {  // 70% の確率でカーブ開始この確率を下げれば「直線が長くなる」
            curveTarget = ((random(0, 2) == 0) ? -1.0f : 1.0f) * random(20, 50);
            isCurving = true;
        }
        curveTimer = 0;
    }
    // ---- カーブ角の更新（非対称イージング）----
    float easing = 0.025f; // 通常の滑らかさ
    if (!isCurving && fabs(roadCurve) > 1.0f) {
        // カーブ後の復帰は速く
        easing = 0.05f;
        curveTarget = 0.0f;  // 自動的に直進へ戻す
    }
    roadCurve += (curveTarget - roadCurve) * easing;

    // --- 直線復帰瞬間にゼロ化補正（新規追加）---
if (!isCurving && curveTarget == 0.0f && fabs(roadCurve) < 2.0f) {
    roadCurve = 0.0f;
    roadCurvePrev = 0.0f;
}
    // --- 微小値を強制リセット（ゼロ化補正）---
if (fabs(curveTarget - roadCurve) < 0.3f && fabs(roadCurve) < 1.5f) {
    roadCurve = 0.0f;
}
    // ---- カーブ終了条件 ----
    if (isCurving && fabs(curveTarget - roadCurve) < 1.0f) {
        isCurving = false;
        curveTimer = -random(120, 200); // 次のカーブまでクールタイム
    }

    // 前回角度を記録
    roadCurvePrev = roadCurve;
}

// ==== パララックス背景追従 ====
//float targetBgCurve = roadCurve * 0.8f;
//bgCurve += (targetBgCurve - bgCurve) * 0.08f;
//float bgPerspective = constrain(bgCurve / 80.0f, -0.5f, 0.5f);

// ==== カメラ回転・パース設定 ====
// パース倍率を強化（視覚的な湾曲を増やす）
float curvePerspective = constrain(roadCurve / 55.0f, -0.9f, 0.9f);
// 背景パース角を道路と一致させる
float bgPerspective = curvePerspective * 0.85f;  // ← 同一方向へ連動
//float bgPerspective    = constrain(bgCurve  / 65.0f, -0.7f, 0.7f);

// ==== 車体ロールとパースを同期させる ====
float roll = (signalColor == TFT_RED)
    ? 0.0f
    : radians(constrain(roadCurve * 0.25f, -5, 5));

// カメラチルトを roll に連動（逆方向）
float cameraTilt = roll * 0.8f;  // ← 逆位相でカメラが追従、自然なカーブ視点に

// カメラオフセット（傾き強調）
int camOffsetX = (int)(sin(cameraTilt) * 26);
int camOffsetY = (int)((1 - cos(cameraTilt)) * 10);

// 消失点を roll にも基づかせる（車の傾きと一致）
int vanishingX    = 160 + (int)(sin(roll) * 120);
int bgVanishingX  = 160 + (int)(sin(roll) * 120);


// === 背景 ===
M5.Display.fillScreen(TFT_BLACK);

// === 🌤 天候設定 ===
enum WeatherType { WEATHER_CLEAR, WEATHER_CRESCENT, WEATHER_RAIN, WEATHER_THUNDER, WEATHER_FOG };
static int weather = WEATHER_CLEAR;
static int moonX = 0, moonY = 0, moonR = 0;
static float starDrift = 0;
static int starX[60], starY[60];

if (!Weathinitialized) {
    Weathinitialized = true;
    weather = random(0, 5);  // ランダム天候
    moonX = (random(0, 2) == 0) ? 60 : 260;
    moonY = 35 + random(-8, 8);
    moonR = (weather == WEATHER_CLEAR) ? 14 : 11;

    // 星座初期化（完全固定）
    for (int i = 0; i < 60; i++) {
        starX[i] = random(0, 320);
        starY[i] = random(10, 100);
    }
}

// === 星空ドリフト更新 ===
if (speed > 0.1f) {
    // 星が流れるスピードを緩やかに（直進時に自然な流れ）
    starDrift += speed * 0.10f;
    if (starDrift > 320.0f) starDrift -= 320.0f;
}

// === 星の描画（スムーズドリフト＋固定パターン）===
for (int i = 0; i < 60; i++) {
    float sx = fmodf(starX[i] + starDrift, 320.0f);
    float sy = starY[i];

    // カーブによる視差補正
    sx += bgPerspective * (sy - 120) * 0.25f + camOffsetX / 12;
    sy += camOffsetY / 15;

    uint16_t col = (i % 7 == 0)
        ? M5.Display.color565(255, 240, 150)  // 明るい星
        : M5.Display.color565(180, 180, 220);  // 通常の星

    M5.Display.drawPixel((int)sx, (int)sy, col);
}

// === 月の描画（常時再描画・ちらつきなし）===
{
    // 月の色をやや落ち着かせる（柔らかい白黄色）
uint16_t moonColor = M5.Display.color565(220, 210, 140);

if (weather == WEATHER_CRESCENT) {
    // 三日月なら黒で右側を削る
    M5.Display.fillCircle(moonX + 4, moonY, moonR - 3, TFT_BLACK);
}
    M5.Display.fillCircle(moonX, moonY, moonR, moonColor);

    if (weather == WEATHER_CRESCENT)
        M5.Display.fillCircle(moonX + 4, moonY, moonR - 3, TFT_BLACK);

    // 雷：一瞬光る
    if (weather == WEATHER_THUNDER && (millis() % 3000 < 80))
        M5.Display.fillCircle(moonX, moonY, moonR + 3, TFT_WHITE);

    // 霧：ぼかし効果
    if (weather == WEATHER_FOG) {
        for (int r = moonR + 2; r < moonR + 6; r++) {
            uint8_t fade = 60 - (r - moonR) * 10;
            M5.Display.drawCircle(moonX, moonY, r, M5.Display.color565(fade, fade, 0));
        }
    }
}


// === ビル群（静止窓＋屋上ネオン点滅）===
const int buildingCount = 12;
const int buildingSpacing = 60;
const int totalDepth = buildingCount * buildingSpacing;

// --- 窓の点灯パターン固定 ---
static bool windowOn[buildingCount][8][6];
static bool initialized = false;

if (!initialized) {
    initialized = true;
    for (int i = 0; i < buildingCount; i++) {
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 6; c++) {
                windowOn[i][r][c] = (random(0, 100) < 30);  // 30% 点灯
            }
        }
    }
}
for (int i = 0; i < buildingCount; i++) {
    int depth = (int)(totalDepth - fmod((zOffset + i * buildingSpacing), totalDepth));
    float scale = 1.0f - (float)depth / totalDepth;
    bool isLeft = (i % 2 == 0);

    float parallax = bgPerspective * (1.0f - scale) * 120.0f;
    int offset = (int)(bgCurve * scale * 2.8f);
    int shift = (int)(parallax) + camOffsetX;
    int curveWarp = (int)(sin(scale * 0.25f) * curvePerspective * 180.0f);

    // --- ランダム形状 ---
    int heightBase = 35 + (i * 13 % 40);
    int height = heightBase + (int)(scale * 60);
    int width = 20 + (i * 7 % 12);
    int baseY = 120 + (int)(scale * (100 - fabs(roadCurve) * 0.6f)) + camOffsetY;
    int x = isLeft
        ? 40 + offset - (int)(scale * 80) + shift - curveWarp
        : 260 + offset + (int)(scale * 80) + shift - curveWarp;

    // === ビル外枠 ===
    uint8_t tone = 60 + (uint8_t)(scale * 150);
    uint16_t frameCol = M5.Display.color565(tone, tone, tone + 20);
    M5.Display.drawRect(x, baseY - height, width, height, frameCol);

    // === フロア区切り（減らした） ===
    int floorSpacing = 14;
    for (int h = floorSpacing; h < height; h += floorSpacing) {
        M5.Display.drawFastHLine(x + 1, baseY - h, width - 2, frameCol);
    }

    // === 窓（固定点灯）===
    int floors = min(8, height / floorSpacing);
    int cols = min(6, width / 4);
    for (int r = 0; r < floors; r++) {
        float brightness = 1.0f - (float)r / floors;
        uint8_t val = 130 + (uint8_t)(100 * brightness * 0.7f);
        uint16_t lightCol = M5.Display.color565(val, val, val / 2);
        for (int c = 0; c < cols; c++) {
            if (windowOn[i][r][c]) {
                int wx = x + 3 + c * 3;
                int wy = baseY - 4 - r * floorSpacing;
                M5.Display.fillRect(wx, wy, 2, 2, lightCol);
            }
        }
    }

    // === 屋上ネオン（赤・青フェード点滅）===
    if (height > 45 && (i % 3 == 0)) {
        float t = (millis() % 4000) / 4000.0f;  // ややゆっくり周期
        float fade = 0.4f + 0.6f * fabs(sin(TWO_PI * t));
        uint16_t neonCol = (i % 2 == 0)
            ? M5.Display.color565((uint8_t)(255 * fade), 0, 0)     // 赤
            : M5.Display.color565(0, 0, (uint8_t)(255 * fade));    // 青
        M5.Display.fillRect(x + width / 2 - 4, baseY - height - 5, 8, 2, neonCol);
    }

    // === 地面反射ライン ===
    if (scale > 0.5f) {
        uint16_t rc = M5.Display.color565(30, 30, 50);
        M5.Display.drawFastHLine(x, baseY, width, rc);
    }
}


// === 道路パース（カーブ連動・幅可変Ver）===
for (int z = 0; z < 18; z++) {
    float p1 = (float)z / 18.0f;
    float p2 = (float)(z + 1) / 18.0f;
    int y1 = 240 - (int)(p1 * 120) + camOffsetY;
    int y2 = 240 - (int)(p2 * 120) + camOffsetY;

    // --- 基本幅 ---
    int baseW1 = 140 + (int)(p1 * -60);
    int baseW2 = 140 + (int)(p2 * -60);

    // --- カーブ時の幅補正 ---
    //float widthAdjust = 1.0f - fabs(-curvePerspective) * 0.04f;
    float innerExpand = 1.0f + fabs(curvePerspective) * 0.18f;
    float outerShrink = 1.0f - fabs(curvePerspective) * 0.8f;

    float leftFactor  = (-curvePerspective > 0) ? innerExpand : outerShrink;
    float rightFactor = (-curvePerspective > 0) ? outerShrink : innerExpand;

    int roadHalfWidth1L = (int)(baseW1 * leftFactor);
    int roadHalfWidth1R = (int)(baseW1 * rightFactor);
    int roadHalfWidth2L = (int)(baseW2 * leftFactor);
    int roadHalfWidth2R = (int)(baseW2 * rightFactor);

    // --- カーブ湾曲オフセット ---
    float curveFactor = curvePerspective * 160.0f;
    int curveOffset1 = (int)(sin(p1 * 1.1f) * curveFactor * (1.0f - p1));
    int curveOffset2 = (int)(sin(p2 * 1.1f) * curveFactor * (1.0f - p2));

    // --- 座標計算 ---
    int x1L = vanishingX - roadHalfWidth1L + camOffsetX - curveOffset1;
    int x1R = vanishingX + roadHalfWidth1R + camOffsetX - curveOffset1;
    int x2L = vanishingX - roadHalfWidth2L + camOffsetX - curveOffset2;
    int x2R = vanishingX + roadHalfWidth2R + camOffsetX - curveOffset2;

// --- 路肩描画（太線Ver.） ---
int thickness = 2;  // 太さ（2〜4くらいで調整）

for (int t = 0; t < thickness; t++) {
    M5.Display.drawLine(x1L + t, y1, x2L + t, y2, TFT_DARKGREY);
    M5.Display.drawLine(x1R - t, y1, x2R - t, y2, TFT_DARKGREY);
}
}


   // === センターライン（曲がり追従＋奥で収束）===
const int segmentCount = 6;
const int segmentLen = 26;
int scroll = ((int)(zOffset * 0.5f)) % segmentLen;

for (int i = 0; i < segmentCount; i++) {
    float t = (float)i / segmentCount;
    int y = 240 - (int)(t * 120) - scroll + camOffsetY;
    if (y < 120 || y > 240) continue;

    float scale = 1.0f - t * 0.75f;
       // --- センターライン（遠近で細く）---
float lineFade = 1.0f - t * 0.8f;  // 奥で細く
int w = max(1, (int)(2.0f * lineFade));  // 太さ1〜2px程度
int len = max(6, (int)(10 + scale * 10));

    // --- カーブ角度を反映 ---
    float curveFactor = sin(t * 0.4f) * (curvePerspective * 180.0f);
    int xCenter = vanishingX + (int)(curveFactor) + camOffsetX;

    int rectY = y - len / 2;
    if (rectY < 120) continue;

uint8_t fadeVal = 180 - (int)(t * 100);  // 奥で暗く
uint16_t centerCol = M5.Display.color565(fadeVal, fadeVal, fadeVal);

M5.Display.fillRect(xCenter - w, rectY, w * 2, len, centerCol);
}

// === 信号機描画 ===
if (drawSignal) {
    int sigBaseX = vanishingX + camOffsetX;
    int sigBaseY = 100 + camOffsetY;

    // 支柱
    M5.Display.fillRect(sigBaseX - 2, sigBaseY - 25, 4, 25, TFT_DARKGREY);

    // 本体
    int bodyW = 36, bodyH = 12;
    int bodyX = sigBaseX - bodyW / 2;
    int bodyY = sigBaseY - bodyH / 2;
    M5.Display.fillRoundRect(bodyX, bodyY, bodyW, bodyH, 2, TFT_DARKGREY);

    // ランプ配置
    int lampR_X = bodyX + 6;
    int lampY_X = bodyX + 16;
    int lampG_X = bodyX + 26;
    int lampY = bodyY + bodyH / 2;

    // ランプ点灯制御
    M5.Display.fillCircle(lampR_X, lampY, 3,
                      (signalColor == TFT_RED) ? TFT_RED : TFT_DARKGREY);
    M5.Display.fillCircle(lampY_X, lampY, 3,
                      (signalColor == TFT_YELLOW) ? TFT_YELLOW : TFT_DARKGREY);
    M5.Display.fillCircle(lampG_X, lampY, 3,
                      (signalColor == TFT_GREEN) ? TFT_GREEN : TFT_DARKGREY);
}

// === 自車（高級セダン・スポーツカー風ワイヤーフレーム）===
int baseY = 210;
int carCenterX = 160 + (int)(curvePerspective * 40);
int carWidth = 64;
int carHeight = 26;  // ← スポーティに少し低く

//float roll = (signalColor == TFT_RED) ? 0.0f : radians(constrain(roadCurve * 0.22f, -5, 5));
float sinR = sin(roll), cosR = cos(roll);

auto rotX = [&](int x, int y) { return carCenterX + (int)((x - carCenterX) * cosR - (y - baseY) * sinR); };
auto rotY = [&](int x, int y) { return baseY + (int)((x - carCenterX) * sinR + (y - baseY) * cosR); };

int leftX = carCenterX - carWidth / 2;
int rightX = carCenterX + carWidth / 2;
int topY = baseY - carHeight;

M5.Display.fillRect(leftX - 3, topY - 3, carWidth + 6, carHeight + 12, TFT_BLACK);

uint16_t lineCol = TFT_LIGHTGREY;
uint16_t accentCol = TFT_WHITE;
uint16_t glassCol = TFT_CYAN;

// === 下ライン（ルーフと対称になる流線型カーブ）===
{
    int midX = carCenterX;

    // 外形にフィットする左右端（はみ出し防止）
    int leftBaseX  = leftX + 8;
    int rightBaseX = rightX - 8;
    int baseYOffset = -3;

    // 中心をやや持ち上げることでルーフに対称なカーブを描く
    int midYOffset = -1;  // ← 上向きカーブ量（対称性を強調）

    // 左→中央→右（2分割構成で自然な滑らかさ）
    M5.Display.drawLine(
        rotX(leftBaseX, baseY + baseYOffset),
        rotY(leftBaseX, baseY + baseYOffset),
        rotX(midX, baseY + midYOffset),
        rotY(midX, baseY + midYOffset),
        lineCol
    );
    M5.Display.drawLine(
        rotX(midX, baseY + midYOffset),
        rotY(midX, baseY + midYOffset),
        rotX(rightBaseX, baseY + baseYOffset),
        rotY(rightBaseX, baseY + baseYOffset),
        lineCol
    ); 

    // 左右フェンダーへの接続補強（角の浮きを防ぐ）
    M5.Display.drawLine(
        rotX(leftBaseX, baseY + baseYOffset),
        rotY(leftBaseX, baseY + baseYOffset),
        rotX(leftBaseX + 3, baseY - 1),
        rotY(leftBaseX + 3, baseY - 1),
        lineCol
    );
    M5.Display.drawLine(
        rotX(rightBaseX, baseY + baseYOffset),
        rotY(rightBaseX, baseY + baseYOffset),
        rotX(rightBaseX - 3, baseY - 1),
        rotY(rightBaseX - 3, baseY - 1),
        lineCol
    );
}

// === 下部水平ライン＋ディフューザー＋4本マフラー ===
{
    int leftBaseX  = leftX + 5 ;
    int rightBaseX = rightX - 5;
    int lineY = baseY-1;   // 車高ライン（基準）
    uint16_t accentCol = TFT_DARKGREY;

    // --- メイン水平ライン ---
    
    M5.Display.drawLine(

    rotX(leftBaseX+18, lineY),
        
    rotY(leftBaseX+18, lineY+3),
        
    rotX(rightBaseX-18, lineY),
        
    rotY(rightBaseX-18, lineY+3),
        
    accentCol
    
    );

    // --- 左右の台形（ディフューザー）---
    int trapWidth = 16;   // 台形の幅
    int trapHeight = 6;   // 下がり量
    int trapOffsetY = -1;  // 全体の下方向オフセット

    // 左台形
    {
        int topL = leftBaseX + 2;
        int topR = topL + trapWidth;
        int topY = lineY;
        int botL = topL + 2;
        int botR = topR - 2;
        int botY = lineY + trapHeight + trapOffsetY;

        // 上辺
        M5.Display.drawLine(rotX(topL, topY), rotY(topL, topY), rotX(topR, topY), rotY(topR, topY), accentCol);
        // 左斜辺
        M5.Display.drawLine(rotX(topL, topY), rotY(topL, topY), rotX(botL, botY), rotY(botL, botY), accentCol);
        // 右斜辺
        M5.Display.drawLine(rotX(topR, topY), rotY(topR, topY), rotX(botR, botY), rotY(botR, botY), meterColor);
        // 下辺
        M5.Display.drawLine(rotX(botL, botY), rotY(botL, botY), rotX(botR, botY), rotY(botR, botY), meterColor);
    }

    // 右台形（左右反転）
    {
        int topR = rightBaseX - 2;
        int topL = topR - trapWidth;
        int topY = lineY;
        int botR = topR - 2;
        int botL = topL + 2;
        int botY = lineY + trapHeight + trapOffsetY;

        M5.Display.drawLine(rotX(topL, topY), rotY(topL, topY), rotX(topR, topY), rotY(topR, topY), accentCol);
        M5.Display.drawLine(rotX(topL, topY), rotY(topL, topY), rotX(botL, botY), rotY(botL, botY), meterColor);
        M5.Display.drawLine(rotX(topR, topY), rotY(topR, topY), rotX(botR, botY), rotY(botR, botY), accentCol);
        M5.Display.drawLine(rotX(botL, botY), rotY(botL, botY), rotX(botR, botY), rotY(botR, botY), meterColor);
    }
    

    // --- マフラー（左右各2本）---
    uint16_t pipeOuter = TFT_LIGHTGREY;
    uint16_t pipeInner = TFT_DARKGREY;

    int pipeR = 2;              // 外周半径
    int pipeInnerR = 1;         // 内側（黒）

    int leftPipeBaseX = leftBaseX + 7;
    int rightPipeBaseX = rightBaseX - 7;
    int pipeY = lineY + trapHeight + trapOffsetY - 3;  // 台形下端に沿わせる
    
    // 左側 2本（外→内）
    M5.Display.fillCircle(rotX(leftPipeBaseX, pipeY), rotY(leftPipeBaseX, pipeY), pipeR, pipeOuter);
    M5.Display.fillCircle(rotX(leftPipeBaseX + 6, pipeY - 1), rotY(leftPipeBaseX + 6, pipeY ), pipeR, pipeOuter);
    M5.Display.fillCircle(rotX(leftPipeBaseX, pipeY), rotY(leftPipeBaseX, pipeY), pipeInnerR, pipeInner);
    M5.Display.fillCircle(rotX(leftPipeBaseX + 6, pipeY - 1), rotY(leftPipeBaseX + 6, pipeY ), pipeInnerR, pipeInner);

    // 右側 2本（外→内）
    M5.Display.fillCircle(rotX(rightPipeBaseX, pipeY), rotY(rightPipeBaseX, pipeY), pipeR, pipeOuter);
    M5.Display.fillCircle(rotX(rightPipeBaseX - 6, pipeY ), rotY(rightPipeBaseX - 5, pipeY) , pipeR, pipeOuter);
    M5.Display.fillCircle(rotX(rightPipeBaseX, pipeY), rotY(rightPipeBaseX, pipeY), pipeInnerR, pipeInner);
    M5.Display.fillCircle(rotX(rightPipeBaseX - 6, pipeY ), rotY(rightPipeBaseX - 5, pipeY ), pipeInnerR, pipeInner);

// --- 中央ブレーキランプ（ディフューザー中央の赤矩形・ブレーキ連動）---
{
    int lampW = 11;   // 横幅
    int lampH = 2;    // 縦幅
    int lampX = carCenterX - lampW / 2;
    int lampY = lineY + trapHeight + trapOffsetY - 4; // ディフューザー上部に

    // ==== 状態連動 ====
    bool brakeOn = false;
    if (signalColor == TFT_RED || signalColor == TFT_YELLOW) {
        brakeOn = true;  // ブレーキ or 減速時
    }

    // オプション：青信号時も減速中だけ点灯したいなら
    // if (speed < 0.3f && signalColor == TFT_GREEN) brakeOn = true;

    // ==== 色設定 ====
    uint16_t lampColor  = brakeOn ? TFT_RED : M5.Display.color565(150, 0, 0); // 消灯時は暗赤
    uint16_t lampBorder = M5.Display.color565(150, 0, 0); // 濃赤縁取り

    // ==== 描画 ====
    if (brakeOn) {
        // 点灯時（明るい赤）
        M5.Display.fillRoundRect(
            rotX(lampX + 1, lampY + 1),
            rotY(lampX + 1, lampY + 1),
            lampW - 2, lampH, 1,
            lampColor
        );
    } else {
        // 消灯時（暗赤で残光）
        M5.Display.fillRoundRect(
            rotX(lampX + 1, lampY + 1),
            rotY(lampX + 1, lampY + 1),
            lampW - 2, lampH, 1,
            lampColor
        );
    }
}
}

// === 側面ライン（リアボディ矩形＋ルーフへ繋がるCピラー台形）===
// --- 左側 ---
{
    int rearTopX_L = leftX + 9;
    int rearTopY_L = topY + 9;
    int rearBotX_L = leftX + 9;
    int rearBotY_L = baseY - 2;
    int curveBulge = -3;   // 外側への膨らみ量
    int segments = 20;     // 分割数（滑らかさ）

    for (int i = 0; i < segments; i++) {
        float t1 = i / (float)segments;
        float t2 = (i + 1) / (float)segments;

        // 補間
        int y1 = rearTopY_L + (int)((rearBotY_L - rearTopY_L) * t1);
        int y2 = rearTopY_L + (int)((rearBotY_L - rearTopY_L) * t2);

        // カーブ形状（緩やか補正）
        auto curve = [&](float t, float power = 3.0f) {
            // t:0〜1, power: 大きいほど緩やか
            float mid = (t - 0.5f);
            return (1.0f - pow(fabs(mid * 2.0f), power));
        };

        int x1 = rearTopX_L + (int)(curveBulge * curve(t1));
        int x2 = rearTopX_L + (int)(curveBulge * curve(t2));

        M5.Display.drawLine(rotX(x1, y1), rotY(x1, y1),
                        rotX(x2, y2), rotY(x2, y2),
                        lineCol);
    }

    // 上辺（トランク接続ライン）
    int rearFrontX_L = leftX + 14;
    int rearFrontY_L = topY +8 ;
    M5.Display.drawLine(rotX(rearTopX_L, rearTopY_L), rotY(rearTopX_L, rearTopY_L),
                    rotX(rearFrontX_L, rearFrontY_L), rotY(rearFrontX_L, rearFrontY_L),
                    lineCol);
}

// --- 右側 ---
{
    int rearTopX_R = rightX - 9;
    int rearTopY_R = topY + 9;
    int rearBotX_R = rightX - 9;
    int rearBotY_R = baseY - 2;
    int curveBulge = 3;    // 右は逆方向に膨らむ
    int segments = 20;

    for (int i = 0; i < segments; i++) {
        float t1 = i / (float)segments;
        float t2 = (i + 1) / (float)segments;

        // 補間（上から下へ）
        int y1 = rearTopY_R + (int)((rearBotY_R - rearTopY_R) * t1);
        int y2 = rearTopY_R + (int)((rearBotY_R - rearTopY_R) * t2);

        // カーブ形状（緩やか補正）
        auto curve = [&](float t, float power = 3.0f) {
            float mid = (t - 0.5f);
            return (1.0f - pow(fabs(mid * 2.0f), power));
        };

        int x1 = rearTopX_R + (int)(curveBulge * curve(t1));
        int x2 = rearTopX_R + (int)(curveBulge * curve(t2));

        M5.Display.drawLine(
            rotX(x1, y1), rotY(x1, y1),
            rotX(x2, y2), rotY(x2, y2),
            lineCol
        );
    }

    // 上辺（トランク接続ライン）
    int rearFrontX_R = rightX - 14;
    int rearFrontY_R = topY + 8;
    M5.Display.drawLine(
        rotX(rearTopX_R, rearTopY_R), rotY(rearTopX_R, rearTopY_R),
        rotX(rearFrontX_R, rearFrontY_R), rotY(rearFrontX_R, rearFrontY_R),
        lineCol
    );
}

// === リアウインドウ（左右対称台形＋トランクライン接続）===
{
    int winTopW = carWidth - 38;   // 上辺の長さ（短め）
    int winBotW = carWidth - 30;   // 下辺の長さ（やや広め）
    int winH = 5;                  // 高さ
    int winTopY = topY + 4;        // 上辺のY座標
    uint16_t glassCol = TFT_CYAN;

    // --- 傾き補正 ---
    int tiltOffset = (int)(sin(roll) * (winTopW * 0.4f));

    // --- 台形座標計算（左右対称）---
    int topLeftX  = carCenterX - winTopW / 2;
    int topRightX = carCenterX + winTopW / 2;
    int botLeftX  = carCenterX - winBotW / 2;
    int botRightX = carCenterX + winBotW / 2;

    int topLeftY  = winTopY - tiltOffset / 2;
    int topRightY = winTopY + tiltOffset / 2;
    int botLeftY  = winTopY + winH - tiltOffset / 2;
    int botRightY = winTopY + winH + tiltOffset / 2;

    // --- 回転変換 ---
    int x1 = rotX(topLeftX, topLeftY);
    int y1 = rotY(topLeftX, topLeftY);
    int x2 = rotX(topRightX, topRightY);
    int y2 = rotY(topRightX, topRightY);
    int x3 = rotX(botRightX, botRightY);
    int y3 = rotY(botRightX, botRightY);
    int x4 = rotX(botLeftX, botLeftY);
    int y4 = rotY(botLeftX, botLeftY);

    // === 外枠 ===
    M5.Display.drawLine(x1, y1, x2, y2, glassCol);
    M5.Display.drawLine(x2, y2, x3, y3, glassCol);
    M5.Display.drawLine(x3, y3, x4, y4, glassCol);
    M5.Display.drawLine(x4, y4, x1, y1, glassCol);

       // === 外枠 ===
    M5.Display.drawLine(x1, y1, x2, y2, lineCol);
    M5.Display.drawLine(x2, y2, x3, y3, lineCol);
    M5.Display.drawLine(x3, y3, x4, y4, lineCol);
    M5.Display.drawLine(x4, y4, x1, y1, lineCol);

    // === 反射効果 ===
    for (int i = 0; i < winH; i++) {
        float t = (float)i / winH;
        int yTop = y1 + (int)((y4 - y1) * t);
        int yBottom = y2 + (int)((y3 - y2) * t);
        float fade = (1.0f - t) * 0.5f;
        int colVal = 80 + (int)(40 * fade);
        uint16_t col = M5.Display.color565(colVal, colVal + 30, colVal + 60);
        if (i % 2 == 0)
            M5.Display.drawLine(x1 + 2, yTop, x2 - 2, yBottom, col);
    }

// === リアウインドウ外形（ルーフ外縁ライン：縦に広げた版） ===
{
    float scaleX = 1.15f;
    float scaleY = 1.6f;

    int bodyTopW = (int)(winTopW * scaleX);
    int bodyBotW = (int)(winBotW * scaleX);
    int bodyH    = (int)(winH * scaleY);
    int bodyTopY = winTopY - 3;

    int tiltOffset = (int)(sin(roll) * (bodyTopW * 0.4f));

    // 台形座標
    int topLeftX  = carCenterX - bodyTopW / 2;
    int topRightX = carCenterX + bodyTopW / 2;
    int botLeftX  = carCenterX - bodyBotW / 2;
    int botRightX = carCenterX + bodyBotW / 2;

    int topLeftY  = bodyTopY - tiltOffset / 2;
    int topRightY = bodyTopY + tiltOffset / 2;
    int botLeftY  = bodyTopY + bodyH - tiltOffset / 2;
    int botRightY = bodyTopY + bodyH + tiltOffset / 2;

    // 回転変換
    int bx1 = rotX(topLeftX, topLeftY);
    int by1 = rotY(topLeftX, topLeftY);
    int bx2 = rotX(topRightX, topRightY);
    int by2 = rotY(topRightX, topRightY);
    int bx3 = rotX(botRightX, botRightY);
    int by3 = rotY(botRightX, botRightY);
    int bx4 = rotX(botLeftX, botLeftY);
    int by4 = rotY(botLeftX, botLeftY);

    uint16_t bodyCol = lineCol;

    // === 上辺は直線 ===
    M5.Display.drawLine(bx1, by1, bx2, by2, bodyCol);

    // === コーナー丸み（上辺と斜辺の接続部） ===
    auto drawCornerCurve = [&](int x1, int y1, int x2, int y2, bool inwardLeft) {
        // 曲率の強さ（ピクセル数）
        const int radius = 4; // 丸み強度
        int segs = 4;         // 分割数（増やすほど滑らか）

        for (int i = 0; i < segs; i++) {
            float t1 = i / (float)segs;
            float t2 = (i + 1) / (float)segs;

            // 線形補間
            int sx1 = x1 + (int)((x2 - x1) * t1);
            int sy1 = y1 + (int)((y2 - y1) * t1);
            int sx2 = x1 + (int)((x2 - x1) * t2);
            int sy2 = y1 + (int)((y2 - y1) * t2);

            // カーブオフセットを加えて角を丸める
            int dir = inwardLeft ? 1 : -1;
            int cx1 = sx1 + dir * (int)(sin(t1 * M_PI_2) * radius);
            int cy1 = sy1 + (int)(1 - cos(t1 * M_PI_2)) * radius;
            int cx2 = sx2 + dir * (int)(sin(t2 * M_PI_2) * radius);
            int cy2 = sy2 + (int)(1 - cos(t2 * M_PI_2)) * radius;

            M5.Display.drawLine(cx1, cy1, cx2, cy2, bodyCol);
        }
    };

    // 左右上コーナーを丸める
    drawCornerCurve(bx1, by1, bx4, by4, false);   // 左
    drawCornerCurve(bx2, by2, bx3, by3, true);  // 右

    // 下辺（ウインドウ下端）
    M5.Display.drawLine(bx3, by3, bx4, by4, bodyCol);
}



// === リアスポイラー（線画・ワイヤーフレーム風＋太線＋ブレーキランプ）===
{
    int spoilerWidthTop  = carWidth - 34;         // 上辺（短め）-36
    int spoilerWidthBot  = spoilerWidthTop+2;   // 下辺（奥行き）+2
    int spoilerHeight    = 3;                     // 厚み
    int spoilerYTop      = topY + 6;              // 上辺Y位置
    int spoilerYBot      = spoilerYTop + spoilerHeight;

    int spoilerXLeftTop  = carCenterX - spoilerWidthTop / 2;
    int spoilerXRightTop = carCenterX + spoilerWidthTop / 2;
    int spoilerXLeftBot  = carCenterX - spoilerWidthBot / 2;
    int spoilerXRightBot = carCenterX + spoilerWidthBot / 2;

    // ライン色
    uint16_t lineBright = TFT_WHITE;                 // 上辺明線
    uint16_t lineNormal = M5.Display.color565(180,180,180);
    uint16_t lineEdge   = M5.Display.color565(240,240,240);
    uint16_t lampColor  = TFT_RED;

    // --- 上辺（滑らかな弧状ライン：太め二重ライン）---
    int midX = carCenterX;
    int midY = spoilerYTop-1 ; // 中央をわずかに持ち上げて弧を作る(-1)

    auto drawCurvedLine = [&](int yOffset, uint16_t color) {
        // 左〜中央
        M5.Display.drawLine(
            rotX(spoilerXLeftTop, spoilerYTop + yOffset),
            rotY(spoilerXLeftTop, spoilerYTop + yOffset),
            rotX(midX, midY + yOffset),
            rotY(midX, midY + yOffset),
            color
        );
        // 中央〜右
        M5.Display.drawLine(
            rotX(midX, midY + yOffset),
            rotY(midX, midY + yOffset),
            rotX(spoilerXRightTop, spoilerYTop + yOffset),
            rotY(spoilerXRightTop, spoilerYTop + yOffset),
            color
        );
    };

    // 太さを2〜3ライン重ねて表現
    drawCurvedLine(0, meterColor);
    drawCurvedLine(-1, lineNormal);
    // --- 左斜辺（奥行き側） ---
    M5.Display.drawLine(
        rotX(spoilerXLeftTop, spoilerYTop),
        rotY(spoilerXLeftTop, spoilerYTop),
        rotX(spoilerXLeftBot, spoilerYBot),
        rotY(spoilerXLeftBot, spoilerYBot),
        lineNormal
    );

    // --- 右斜辺（手前側） ---
    M5.Display.drawLine(
        rotX(spoilerXRightTop, spoilerYTop),
        rotY(spoilerXRightTop, spoilerYTop),
        rotX(spoilerXRightBot, spoilerYBot),
        rotY(spoilerXRightBot, spoilerYBot),
        lineEdge
    );

    // --- 中央支柱（短い縦棒） ---
    int pillarX = carCenterX;
    int pillarTopY = spoilerYBot;
    int pillarBotY = spoilerYBot - 2;
    M5.Display.drawLine(
        rotX(pillarX, pillarTopY),
        rotY(pillarX, pillarTopY),
        rotX(pillarX, pillarBotY),
        rotY(pillarX, pillarBotY),
        lineNormal
    );

    // --- ハイマウントブレーキランプ ---
    {
        int lampW = 12;   // 横幅
        int lampH = 2;   // 縦幅
        int lampX = carCenterX - lampW / 2;
        int lampY = midY +1;  // 弧の中央付近

        // 外枠（明線）
        M5.Display.drawRect(
            rotX(lampX, lampY),
            rotY(lampX, lampY),
            lampW,
            lampH,
            lineEdge
        );

    // 内部（発光部）
    bool brakeOn = false;
    if (signalColor == TFT_RED || signalColor == TFT_YELLOW) {
        brakeOn = true;  // ブレーキ or 減速時
    }

    // ==== 色設定 ====
    uint16_t lampColor  = brakeOn ? TFT_RED : M5.Display.color565(150, 0, 0); // 消灯時は暗赤
    uint16_t lampBorder = M5.Display.color565(150, 0, 0); // 濃赤縁取り

if (brakeOn) {
        M5.Display.fillRect(
            rotX(lampX + 1, lampY + 1),
            rotY(lampX + 1, lampY + 1),
            lampW - 2,
            lampH - 1,
            lampColor
        );
        } else {
        // 消灯時（暗赤で残光）
        M5.Display.fillRoundRect(
            rotX(lampX + 1, lampY + 1),
            rotY(lampX + 1, lampY + 1),
            lampW - 2, lampH, 1,
            lampColor
        );
    }
    }
}

// === トランクライン（リアウインドウ下端から縦に接続）===
int trunkY = (y3 + y4) / 2 + 6; // リアウインドウ下端より少し下
int trunkLeftX  = x4 + 5;
int trunkRightX = x3 - 5;
uint16_t trunkCol = TFT_DARKGREY;

// --- ロール連動オフセット ---
int rollAmp = 4;                               // ロールによる上下変化量（px）
int rollOffsetLeft  = (int)(sin(roll) * rollAmp);   // 左カーブで左が沈む
int rollOffsetRight = (int)(-sin(roll) * rollAmp);  // 右カーブで右が沈む

// --- トランク上辺（逆ハの字＋ロール傾き連動）---
int midX = (trunkLeftX + trunkRightX) / 2;
int midY = trunkY;  // 中央を少し下げてスポーツカー的な逆ハの字

// 左右端の高さをロール角に応じて変化させる
int leftY  = trunkY + rollOffsetLeft;
int rightY = trunkY + rollOffsetRight;

// 線を3点で構成（左→中央→右）
M5.Display.drawLine(
    rotX(trunkLeftX, leftY), rotY(trunkLeftX, leftY),
    rotX(midX, midY),        rotY(midX, midY),
    trunkCol
);
M5.Display.drawLine(
    rotX(midX, midY),        rotY(midX, midY),
    rotX(trunkRightX, rightY), rotY(trunkRightX, rightY),
    trunkCol
);

// --- 左右縦ライン（rotX/rotYで傾き反映）---
int tailTopY = baseY - 10;  // テールランプ上位置
int tiltX = 3;              // 外傾量（鋭角化用）

// 左側ライン（ロールで自然傾き）
M5.Display.drawLine(
    rotX(trunkLeftX, leftY), rotY(trunkLeftX, leftY),
    rotX(trunkLeftX - tiltX, tailTopY - 4 + rollOffsetLeft),
    rotY(trunkLeftX - tiltX, tailTopY - 4 + rollOffsetLeft),
    trunkCol
);

// 右側ライン（ロールで自然傾き）
M5.Display.drawLine(
    rotX(trunkRightX, rightY), rotY(trunkRightX, rightY),
    rotX(trunkRightX + tiltX, tailTopY - 4 + rollOffsetRight),
    rotY(trunkRightX + tiltX, tailTopY - 4 + rollOffsetRight),
    trunkCol
);
}

// === タイヤ（角丸スクエア＋白縁＋シャドウ）===
// int tireW = 6;     // 横幅（タイヤの太さ）
// int tireH = 10;     // 縦幅
// int tireR = 2;     // 角の丸み

// 左タイヤ（外枠）
// M5.Display.drawRoundRect(
   //  rotX(leftX + 8, baseY - 4), rotY(leftX + 8, baseY - 4),
   //  tireW, tireH, tireR, TFT_DARKGREY); 
// 右タイヤ（外枠）
// M5.Display.drawRoundRect(
  // rotX(rightX - 13, baseY - 4), rotY(rightX - 13, baseY - 4),
   // tireW, tireH, tireR, TFT_DARKGREY);

//ドアミラー
int mirrorY = topY + 8;
// 左ミラー
int lx = rotX(leftX + 11, mirrorY);
int ly = rotY(leftX + 11, mirrorY);
M5.Display.drawEllipse(lx, ly, 4, 2, TFT_LIGHTGREY);   // 外枠
//M5.Display.fillEllipse(lx, ly, 3, 1, TFT_WHITE);        // 内部の反射部

// 右ミラー
int rx = rotX(rightX - 11, mirrorY);
int ry = rotY(rightX - 11, mirrorY);
M5.Display.drawEllipse(rx, ry, 4, 2, TFT_LIGHTGREY);
//M5.Display.fillEllipse(rx, ry, 3, 1, TFT_WHITE);

// === テールランプ（信号優先＋ウインカー点滅対応）===
{
    // テールランプ位置
    int tailLX = leftX + 16;
    int tailRX = rightX - 12;
    int tailY = baseY - 10;

    // === 点滅制御 ===
    bool blinkState = (millis() / 500) % 2;   // 500ms周期でON/OFF
    bool turnRight = roll > radians(2.0f);    // カーブ右傾きで右ウインカー
    bool turnLeft  = roll < radians(-2.0f);   // カーブ左傾きで左ウインカー

    // === 信号優先制御 ===
    if (signalColor == TFT_RED || signalColor == TFT_YELLOW) {
        // 🔴 赤 or 🟡 黄 のとき → ブレーキランプ優先（常時点灯）
        uint16_t brakeColor = (signalColor == TFT_RED || signalColor == TFT_YELLOW || !isCurving) ? TFT_RED : TFT_ORANGE;

       
        M5.Display.fillCircle(rotX(tailLX - 5, tailY), rotY(tailLX - 5, tailY), 3, brakeColor); //左外   
        M5.Display.fillCircle(rotX(tailLX + 2, tailY+1), rotY(tailLX + 2, tailY+1), 2, brakeColor);//左内
        M5.Display.fillCircle(rotX(tailRX - 7, tailY+1), rotY(tailRX - 7, tailY+1), 2, brakeColor);//右内
        M5.Display.fillCircle(rotX(tailRX, tailY), rotY(tailRX, tailY), 3, brakeColor);//右外
    }
    else {
        // 🟢 青信号時 → 傾き連動ウインカー点滅
        uint16_t tailDim = M5.Display.color565(170, 20, 20);
        M5.Display.fillCircle(rotX(tailLX + 2, tailY+1), rotY(tailLX + 2, tailY+1), 2, tailDim);//左内
        M5.Display.fillCircle(rotX(tailRX - 7, tailY+1), rotY(tailRX - 7, tailY+1), 2,tailDim);//右内

        // 左右ウインカーの点滅判定/左外・右外がウインカー
        if (turnLeft && blinkState) {
            M5.Display.fillCircle(rotX(tailLX - 5, tailY), rotY(tailLX - 5, tailY), 3, TFT_ORANGE);

        } else {
           M5.Display.fillCircle(rotX(tailLX - 5, tailY), rotY(tailLX - 5, tailY), 3, tailDim);
        }

        if (turnRight && blinkState) {
            M5.Display.fillCircle(rotX(tailRX, tailY), rotY(tailRX, tailY), 3, TFT_ORANGE);

        } else {
            M5.Display.fillCircle(rotX(tailRX, tailY), rotY(tailRX, tailY), 3, tailDim);
        }
    }
}

// === ナンバープレート（コンパクト＋外枠付き）===
    int plateW = 13;  // 幅を少し絞る（従来:14）
    int plateH = 7;
    int plateX = carCenterX - plateW / 2;
    int plateY = baseY - 6;
    // --- 本体（白地＋黒縁）---
    M5.Display.fillRoundRect(plateX, plateY, plateW, plateH, 2, TFT_WHITE);
    M5.Display.drawRoundRect(plateX, plateY, plateW, plateH, 2, TFT_BLACK);
}


// ==== 統計リセット ====
void resetStats() {
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(RED);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(50, 100);
    M5.Display.print("Resetting stats...");
    delay(1000);

    totalKeystrokes = 0;
    maxCPM = 0;
    sumValue = 0;
    sampleCount = 0;
    memset(cpmHistory, 0, sizeof(cpmHistory));
    historyIndex = 0;
    
    // ✅ 統計だけ消す（vibrationEnabled等は保持）
    prefs.remove("totalKeystrokes");
    prefs.remove("maxCPM");
    prefs.remove("sumValue");
    prefs.remove("sampleCount");

    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(TFT_GREEN);
    M5.Display.setCursor(30, 100);
    M5.Display.print("Stats reset complete!!");
    // 🔸 LOG切替時バイブ（短く弱め）
    pulseVibration(150, 300);
    delay(1000);
    M5.Display.fillScreen(BLACK);
  if (displayMode == MODE_LOG) {
    drawLogScreen();
  } else {    
    drawMeterBackground();   
    changeShift(SHIFT_M);
    drawShiftIndicator_light();
    drawFuelMeter(fuelLevel);
  }
}

volatile int newLayerReceived = -1;  // ← 割り込みから受け取る
unsigned long lastDrawTime = 0;

// I2C受信ハンドラ
void receiveEvent(int bytes) {
    if (bytes < 1) return;

    uint8_t cmd = Wire.read();
    bytes--;

    if (cmd == 0x01 && bytes >= 2) { // CPM
        uint8_t high = Wire.read();
        uint8_t low  = Wire.read();
        uint16_t newValue = (high << 8) | low;
        applyCPM(newValue);
        activeSource = SRC_I2C;
        Serial.printf("I2C Received CPM=%d\n", newValue);
    }
    else if (cmd == 0x02 && bytes >= 1) {
        uint8_t layer = Wire.read();
        applyLayer(layer);
        activeSource = SRC_I2C;
        Serial.printf("I2C Received Layer=%d\n", layer);
    }
}

// ==== USB Serial からの受信処理 ====
/// =========================
// USB CPMステートマシン
// =========================
static uint8_t usb_state = 0;
static uint8_t usb_lsb   = 0;
static uint8_t usb_msb   = 0;

void processUSBSerial() {

    while (Serial.available() > 0) {
        uint8_t b = Serial.read();

        Serial.printf("[RAW] %02X \n", b);

        switch (usb_state) {

        // ---- ヘッダ待ち ----
        case 0:  
            if (b == 0x01) {       // CPM
                usb_state = 1;
            } 
            else if (b == 0x02) {  // Layer
                usb_state = 3;
            }
            break;

        // ---- CPM LSB ----
        case 1:
            usb_lsb = b;
            usb_state = 2;
            break;

        // ---- CPM MSB ----
        case 2:
            usb_msb = b;
            {
                uint16_t cpm = (usb_msb << 8) | usb_lsb;
                applyCPM(cpm);
                activeSource = SRC_USB;
            }
            usb_state = 0;
            break;

        // ---- Layer ----
        case 3:
            applyLayer(b);
            activeSource = SRC_USB;
            usb_state = 0;
            break;
        }
    }
}




// ==== Bluetooth Serial からの受信処理（超・非ブロッキング） ====
void processBTSerial() {
    if (!SerialBT.hasClient()) return;

    while (SerialBT.available() > 0) {
        int cmd = SerialBT.read();

        if (cmd == 0x01) {
            if (SerialBT.available() < 2) return;
            uint8_t lsb = SerialBT.read();
            uint8_t msb = SerialBT.read();
            uint16_t cpm = (static_cast<uint16_t>(msb) << 8) | lsb;
            applyCPM(cpm);
            activeSource = SRC_BT;
        }

        else if (cmd == 0x02) {
            if (SerialBT.available() < 1) return;
            uint8_t layer = SerialBT.read();
            applyLayer(layer);
            activeSource = SRC_BT;
        }
    }
}



//  割り込みハンドラ
void IRAM_ATTR btnA_ISR() { btnA_pressed = true; }
void IRAM_ATTR btnB_ISR() { btnB_pressed = true; }
void IRAM_ATTR btnC_ISR() { btnC_pressed = true; }


//起動シーケンス
// ==== 起動シーケンス（スピード＋ガソリンメータースウィープ）====
void startupSweep() {
    M5.Display.fillScreen(BLACK);

    // 記録された色インデックスを読み出し
    colorIndex = prefs.getInt("meterColorIdx", 0);
    meterColor = METER_COLORS[colorIndex];

    // 背景とタイトル
    drawMeterBackground();
    fuelLevel = 0;
    drawFuelMeter(fuelLevel);
    delay(500);

    // 警告アイコン描画
    drawFuelIcon(CENTER_X -155, CENTER_Y -45, TFT_ORANGE);
    drawHandBrakeIcon(CENTER_X -124, CENTER_Y -45, TFT_RED);
    drawPressureIcon(CENTER_X -88, CENTER_Y -40, TFT_ORANGE);
    delay(600);

    // ==== メータースウィープ開始 ====
    drawNeedle(0, 0);
    delay(500);
    // スピードメーター: 左端→右端
    // ガソリンメーター: F→E
    for (int v = 0; v <= 1000; v += 100) {
        if (v == 100) pulseVibration(100, 400);
        drawNeedle(v, (v == 0 ? 0 : v - 100));
        // F(100)→E(0) に向けて減少
        int f = map(v, 0, 1000, 0, 100);
        drawFuelMeter(f);
        delay(15);
    }
    delay(600);
    // タイトル演出
    drawTitleTyping();
    delay(590);
    M5.Display.setTextDatum(TL_DATUM);
    M5.Display.startWrite();
    M5.Display.fillRect(CENTER_X - 155, CENTER_Y-45, 120, 25, BLACK);
    M5.Display.fillRect(CENTER_X - 40, CENTER_Y - 10, 120, 40, BLACK);
    ClearNeedle(0, 0);

    // スピード針戻し＋ガソリンFへ戻し
    for (int v = 1000; v >= 0; v -= 100) {
        drawNeedle(v, (v == 1000 ? 1000 : v + 100));
        int f = map(v, 1000, 0, 0, 100);
        delay(40);
    }
    // ガソリンメーターはF位置（100）で固定
    fuelLevel = 100;
    drawFuelMeter(fuelLevel);  
    M5.Display.endWrite();  

// === READY点滅 ===
    for (int i = 0; i < 6; i++) {
        if (i % 2 == 0) {
            M5.Display.setTextDatum(TL_DATUM);
            M5.Display.startWrite();
            M5.Display.setTextSize(3);
            M5.Display.setTextColor(meterColor, BLACK);
            M5.Display.setCursor(CENTER_X - 55, CENTER_Y + 10);
            M5.Display.print("READY_");

            // 最初の点灯時に1回バイブ
           // if (i == 0) pulseVibration(100, 200);
        } else {
            M5.Display.fillRect(CENTER_X - 55, CENTER_Y + 10, 120, 40, BLACK);
        }

    delay(300);
}

    // READY消去後、ガソリン満タン固定
    drawFuelMeter(100);
    M5.Display.endWrite();
}

// ==== 起動時モード選択 ====
// A: USB/BT, B: I2C, C: DEMO（5秒無操作で I2C デフォルト）
void selectAppMode() {
    appMode = MODE_I2C;  // デフォルト

    M5.Display.fillScreen(BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_CYAN, BLACK);
    M5.Display.setCursor(20, 60);
    M5.Display.println("Select Mode:");
    M5.Display.setCursor(40, 100);
    M5.Display.println("A: USB / BT");
    M5.Display.setCursor(40, 130);
    M5.Display.println("B: I2C");
    M5.Display.setCursor(40, 160);
    M5.Display.println("C: DEMO");
    M5.Display.setTextSize(1);
    M5.Display.setCursor(40, 200);
    M5.Display.println("(5 sec timeout -> I2C)");

    unsigned long start = millis();
    while (millis() - start < 5000) {
        M5.update();
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
        delay(10);
    }

    // モード確定表示
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_GREEN, BLACK);
    M5.Display.setCursor(20, 120);
    M5.Display.print("Mode: ");
    if (appMode == MODE_USB_BT) M5.Display.print("USB/BT");
    else if (appMode == MODE_I2C) M5.Display.print("I2C");
    else if (appMode == MODE_DEMO) M5.Display.print("DEMO");
    delay(600);
}




// ==== 設定・初期化 ====
void setup() {
    // 起動時に割り込みフラグを必ずクリア
    btnA_pressed = true;
    btnB_pressed = true;
    btnC_pressed = true;

    M5.Power.setLed(false);   // 消灯

    M5.begin();
    M5.Power.setLed(0);  // Disable LED at startup for unified

    Serial.begin(115200);
    Serial.println("M5Core2 Typing Meter");

    // ★ 起動時モード選択
    selectAppMode();

    // ★ モードに応じて I2C / BT 初期化
    if (appMode == MODE_I2C) {
        Wire.begin(I2C_SLAVE_ADDR, G32, G33, 400000); // SDA=32, SCL=33, 400kHz
        Wire.onReceive(receiveEvent);
        Serial.println("Mode: I2C Slave");
    }

    if (appMode == MODE_USB_BT) {
        SerialBT.begin("TypingMeter"); // 任意の名前
        Serial.println("Mode: USB/BT (Serial + BT)");
    }

    if (appMode == MODE_DEMO) {
        Serial.println("Mode: DEMO (self-generated CPM)");
        // DEMO では I2C / BT は使用しない
    }

    prefs.begin("typingmeter", false);
    prefsVibe.begin("vibe", false);

    pinMode(btnA_pin, INPUT_PULLUP);
    pinMode(btnB_pin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(btnA_pin), btnA_ISR, FALLING);
    attachInterrupt(digitalPinToInterrupt(btnB_pin), btnB_ISR, FALLING);

    Serial.println("M5Core2 Meter Ready");


    // ...（この下は元の setup と同じ：Preferences 読み出し・色読み出し
    //      Display 初期化・drawMeterBackground()・startupSweep() など）


    
    // 記録された色インデックスを読み出し
    colorIndex = prefs.getInt("meterColorIdx", 0);
    meterColor = METER_COLORS[colorIndex];

    M5.Display.clearDisplay(TFT_BLACK);
    M5.Display.setRotation(1);  
    
    // 1) 背景を最初に完全描画
    drawMeterBackground();
    drawFuelMeter(fuelLevel);

    // 2) 背景が確定した後で StartupSweep の描画を行う
    startupSweep();

    // 3) startupSweep で汚れた背景を再度描画して復元
    drawMeterBackground();
    drawFuelMeter(fuelLevel);
    drawShiftIndicator();

}

// ==== メインループ ====
void loop() {
    M5.update();
    updatePomodoro();
// ==== 起動直後のボタン誤動作防止 ====
static bool skipButtonsOnce = true;
if (skipButtonsOnce) {
    skipButtonsOnce = false;
    return;
}

   // ==== 通信処理 ====
if (appMode == MODE_USB_BT) {
    processUSBSerial();
    processBTSerial();
}

static uint8_t prevSource = 255;
if (prevSource != activeSource) {
    prevSource = activeSource;

    if (activeSource == SRC_USB || activeSource == SRC_BT) {
        // USB または BT がアクティブ → LED 点灯
        M5.Power.setLed(true);
    } else {
        // 未接続 / I2C / Demo → 消灯
        M5.Power.setLed(false);
    }
}


// --- レイヤー更新フラグが立っていたら即描画 ---
if (newLayerReceived >= 0) {
    if (newLayerReceived != currentLayer) {
        // Logモードでは残像や描画をスキップ
        if (displayMode == MODE_METER) {
        // 描画
        currentLayer = newLayerReceived;
        setActiveLayer(currentLayer);  // ← ここに統合！
        Serial.printf("[I2C] Layer=%d\n", currentLayer);
        drawShiftIndicator(); 
        }
    }
    newLayerReceived = -1;
}
// 🚗 シフトノブ描画更新（残像補間）
drawShiftIndicator();

// === Aボタン長押しで設定メニュー ===
static bool settingsHandled = false;
  if (M5.BtnA.pressedFor(2000)) {
    if (!settingsHandled) {
        settingsHandled = true;

        // 設定画面描画
        M5.Display.fillRect(CENTER_X - 80, CENTER_Y - 20, 190, 60, BLACK);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(TFT_CYAN, BLACK);
        M5.Display.setCursor(CENTER_X - 70, CENTER_Y - 10);
        M5.Display.print("Settings");
        delay(500);

        // トグル切替
        vibrationEnabled = !vibrationEnabled;
        prefsVibe.putBool("enabled", vibrationEnabled);
        prefsVibe.end();          // ← 明示的に終了
        delay(50);             // ← 書き込み完了待ち（安全マージン）
        prefsVibe.begin("vibe", false);  // 再オープンして他の操作継続

        // 表示反映
        M5.Display.fillRect(CENTER_X - 80, CENTER_Y + 10, 190, 30, BLACK);
        M5.Display.setTextColor(TFT_YELLOW, BLACK);
        M5.Display.setCursor(CENTER_X - 65, CENTER_Y + 20);
        if (vibrationEnabled) {
            M5.Display.print("Vibration: ON");
            pulseVibration(180, 250); // 強めに1回フィードバック
        } else {
            M5.Display.print("Vibration: OFF");
        }

        delay(1000);
        M5.Display.fillRect(CENTER_X - 80, CENTER_Y - 20, 190, 60, BLACK);
    }
} else if (M5.BtnA.wasReleased()) {  // === ボタンA：次のカラー ===
    if (!settingsHandled) {
    colorIndex = (colorIndex + 1) % (sizeof(METER_COLORS) / sizeof(METER_COLORS[0]));
    meterColor = METER_COLORS[colorIndex];
    prefs.putInt("meterColorIdx", colorIndex);
    displayMode = MODE_METER;
    drawMeterBackground();
    changeShift(SHIFT_M);
    drawShiftIndicator_light();
    drawFuelMeter(fuelLevel);
    }
settingsHandled = false;
}


// === ボタンB：ポモドーロ開始・モード切替 ===
static bool longPressHandledB = false;

if (M5.BtnB.pressedFor(2000)) {
    if (!longPressHandledB) {
        longPressHandledB = true;

        // === モード循環: OFF → SHORT → LONG → DEMO → OFF ===
        pomoCycle = (pomoCycle + 1) % 4;

        // === 各モード設定 ===
        if (pomoCycle == 0) {        // OFF
            pomoMode = POMO_OFF;
            M5.Display.fillRect(5, 5, 210, 40, BLACK);
            M5.Display.setTextColor(TFT_LIGHTGREY, BLACK);
            M5.Display.setTextSize(2);
            M5.Display.setCursor(10, 10);
            M5.Display.print("Pomodoro: OFF");
            delay(800);
            M5.Display.fillRect(5, 5, 210, 40, BLACK);
            fuelLevel = 100;
            drawFuelMeter(fuelLevel);
            return;  // ここで終了（他処理に進まない）
        }

        else if (pomoCycle == 1) {   // 25分
            pomoMode = POMO_SHORT;
        }
        else if (pomoCycle == 2) {   // 45分
            pomoMode = POMO_LONG;
        }
        else if (pomoCycle == 3) {   // DEMO
            pomoMode = POMO_SHORT;   // 内部的にはSHORT扱い
        }

        // === 開始共通処理 ===
        pomoStartTime = millis();
        fuelLevel = 100;
        drawFuelMeter(fuelLevel);

        // 左上にモード名表示
        M5.Display.setTextColor(TFT_ORANGE, BLACK);
        M5.Display.setTextSize(2);
        M5.Display.fillRect(5, 5, 210, 40, BLACK);
        M5.Display.setCursor(10, 10);
        if (pomoCycle == 1) M5.Display.print("Pomodoro_25min");
        else if (pomoCycle == 2) M5.Display.print("Pomodoro_45min");
        else if (pomoCycle == 3) M5.Display.print("Pomodoro_DEMO");
        delay(1000);
        M5.Display.fillRect(5, 5, 210, 40, BLACK);
    }
}
else if (M5.BtnB.wasReleased()) {
    if (!longPressHandledB) { // ← 長押し直後の解放を無視
        // --- 短押しでメーター色変更 ---
        if (colorIndex == 0)
            colorIndex = (sizeof(METER_COLORS) / sizeof(METER_COLORS[0])) - 1;
        else
            colorIndex--;
        meterColor = METER_COLORS[colorIndex];
        prefs.putInt("meterColorIdx", colorIndex);
        displayMode = MODE_METER;
        drawMeterBackground();
        changeShift(SHIFT_M);
        drawShiftIndicator_light();
        drawFuelMeter(fuelLevel);
    }
    longPressHandledB = false; // ← フラグをリセット
    
}
// === ボタンC ===
static bool longPressHandled = false;
if (M5.BtnC.pressedFor(2000)) {
    if (!longPressHandled) {
        resetStats();  // 長押し時に統計リセット
        longPressHandled = true;
    }
} else if (M5.BtnC.wasReleased()) {
    if (!longPressHandled) {  
        // 現在のモードに基づいて次のモードを決定
        DisplayMode nextMode = (displayMode == MODE_METER) ? MODE_LOG : MODE_METER;
        displayMode = nextMode;  // モードを更新

        if (nextMode == MODE_LOG) {
            drawLogScreen();  // Logモード用描画のみ
        } else {    
            drawMeterBackground();   
            changeShift(SHIFT_M);
            drawShiftIndicator_light();
            drawFuelMeter(fuelLevel);
        }
    }
    longPressHandled = false;
}

// ==== スクリーンセーバー制御（完全安定版） ====
static bool screenSaverMode = true;         // ON/OFFトグル（長押しで切替）
static bool screenSaverActive = false;      // 実際にセーバー動作中か
static unsigned long lastActivityTime = 0;  // CPM・操作の最終時刻
static unsigned long screenSaverRecoveryUntil = 0; // 復帰後のセーフ期間
static bool touchHeld = false;
static unsigned long touchStartTime = 0;

const int TOUCH_HOLD_MS = 1500;             // 長押し時間（1.5秒）
const unsigned long AUTO_TIMEOUT_MS = 30000; // 無操作発動時間（30秒）


auto p = M5.Touch.getDetail();
bool touchPressed = p.isPressed();
int touchX = 0, touchY = 0;
if (touchPressed) {
    touchX = p.x;
    touchY = p.y;
}

// ==== 条件①：中央長押しで screenSaverMode トグル ====
if (touchPressed && (touchX > 80 && touchX < 240 && touchY > 80 && touchY < 200)) {
    if (!touchHeld) {
        touchStartTime = millis();
        touchHeld = true;
    } else if (millis() - touchStartTime > TOUCH_HOLD_MS) {
        touchHeld = false;
        screenSaverMode = !screenSaverMode;  // ON/OFF 切替
        screenSaverActive = screenSaverMode; // ← ONなら即スクリーンセーバーに入る
        M5.Display.fillScreen(BLACK);

        // --- モード状態を一時表示 ---
        M5.Display.setTextDatum(TL_DATUM);
        M5.Display.setTextColor(TFT_WHITE, BLACK);
        M5.Display.drawString(
            String("screenSaverMode: ") + (screenSaverMode ? "ON" : "OFF"),
            10, 5, 2
        );

        delay(800); // 表示保持
        M5.Display.fillRect(0, 0, 320, 20, BLACK);

        if (screenSaverMode) {
            // === 🔹 ON → スクリーンセーバー即描画 ===
            drawNightCityDrive();  // 既存セーバー描画関数
        } else {
            // === 🔹 OFF → 通常メータ画面へ復帰 ===
            displayMode = MODE_METER;
            M5.Display.fillScreen(BLACK);
            drawMeterBackground();
            drawFuelMeter(fuelLevel);
            changeShift(SHIFT_M);
            drawShiftIndicator_light();
        }

        // 🩵 長押し操作もアクティビティ扱い
        lastActivityTime = millis();
        screenSaverRecoveryUntil = millis() + 5000; // 5秒間は発動禁止
    }
} else {
    touchHeld = false;
}

// ==== 条件②：Pomodoro中はセーバー無効 ====
bool pomodoroActiveNow =
    (pomoMode == POMO_SHORT || pomoMode == POMO_LONG || pomoMode == POMO_BREAK);
if (pomodoroActiveNow) screenSaverActive = false;
// 🔥 LOGMODE の間もスクリーンセーバー禁止
if (displayMode == MODE_LOG) screenSaverActive = false;

// ==== 条件③：無操作経過時間 ====
bool idleTooLong = (millis() - lastActivityTime > AUTO_TIMEOUT_MS);

// ==== ABCボタンいずれかが押された場合無操作時間をリセット ====  
if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) {
    lastActivityTime = millis();
}

// === Meter モードで「打鍵中」はスクリーンセーバー禁止（暗転防止）===
if (displayMode == MODE_METER && targetValue > 0) {
    idleTooLong = false;
}

// ===LOG モード中は完全禁止（M5Unified でも同じ）===
if (displayMode == MODE_LOG) {idleTooLong = false;
}

// 🔸 復帰直後5秒間はセーバー禁止
if (millis() < screenSaverRecoveryUntil) {
    idleTooLong = false;
}

// ==== 自動スクリーンセーバー発動 ====
// ==== LOG モード中はスクリーンセーバー完全禁止 ====
if (displayMode != MODE_LOG) {
    if (screenSaverMode && !pomodoroActiveNow && !screenSaverActive && idleTooLong) {
        screenSaverActive = true;
        delay(100);
        M5.Display.fillScreen(BLACK);
        delay(400);
    }
}

// ==== CPM・ボタン入力・操作で復帰 ====
if (screenSaverActive) {
    if (targetValue > 0 || M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) {
        screenSaverActive = false;

        // 🩵 無操作タイマーをリセットし再発動防止
        lastActivityTime = millis();
        screenSaverRecoveryUntil = millis() + 5000; // 復帰後5秒は無効

        // 🔸 LOGMODEから復帰する場合は自動で解除
        displayMode = MODE_METER;

        // --- 背景クリア＋再描画 ---
        M5.Display.fillScreen(BLACK);
        drawMeterBackground();
        drawFuelMeter(fuelLevel);
        changeShift(SHIFT_M);
        drawShiftIndicator_light();
    } else {
        // セーバー描画継続
        drawNightCityDrive();
        return; // セーバー中は他の描画スキップ
    }
}

// ==== 定期保存 ====
    if (millis() - lastSaveTime > SAVE_INTERVAL) {
        saveStats();
        lastSaveTime = millis();
    }


// CPMを受信したとき ONLY ここを実行（後述の applyCPM 内に追加）
// lastCPMTime = millis();

if (appMode == MODE_USB_BT) {
    if (millis() - lastCPMTime > 700) {   // 700ms 無通信
        targetValue = 0;// ★ 表示値だけ落とす。統計は壊さない
        M5.Power.setLed(false);  // ★ 無通信 → LED OFF
    }
}
    
// ==== メーター針の追従 ====
int speed = (appMode == MODE_USB_BT ? NEEDLE_STEP * 3 : NEEDLE_STEP);
    if (displayMode == MODE_METER) {
        uint16_t displayedValue = prevValue;

        if (displayedValue < targetValue)
    displayedValue += speed;
else if (displayedValue > targetValue)
    displayedValue -= speed;

    
        drawNeedle(displayedValue, prevValue);
    prevValue = displayedValue;  
    }
 // ==== ログ画面で ====
    if (displayMode == MODE_LOG) {
            if (isReplaying) {
            drawReplayFrameAnimated(GRAPH_X, GRAPH_Y, GRAPH_WIDTH, GRAPH_HEIGHT);
        }
    }
}
