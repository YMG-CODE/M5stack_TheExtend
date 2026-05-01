#include <Arduino.h>
#include <M5Unified.h>
#include <vector>
#include <algorithm>
#include <math.h>
#include <BluetoothSerial.h>

// =============================
// 設定
// =============================
const int GRID_SIZE = 8;
const float SPHERE_RADIUS = 100.0f;

const int CANVAS_WIDTH = 200;
const int CANVAS_HEIGHT = 200;

const float CAMERA_Z = SPHERE_RADIUS * 3.0f;
const float FOV = 200.0f;

float sensGain = 0.005f;   // 感度（まずここ上げる）
float SMOOTH = 0.0f;   // 入力の滑らかさ
float FRICTION = 0.75f; // 慣性

float accDX = 0;
float accDY = 0;

bool movementStarted = false;

BluetoothSerial SerialBT;

bool padMode = false;   // false=Trackball / true=Trackpad

bool ballPrecisionActive = false;

float fxX = 160;
float fxY = 120;

float fxPrevX = 160;
float fxPrevY = 120;

float fxPower = 0.0f;
bool  fxTouch = false;

float trailX = 160;
float trailY = 120;

float trailVX = 0;
float trailVY = 0;

bool tapArmed = false;
uint32_t tapStartMs = 0;
int tapStartX = 0;
int tapStartY = 0;

float clickFx = 0.0f;

float tapTravel = 0.0f;


// =============================
// ベクトル
// =============================
struct Vec3 {
  float x, y, z;
  Vec3(float x=0, float y=0, float z=0) : x(x), y(y), z(z) {}
  Vec3 operator+(const Vec3& v) const { return Vec3(x+v.x,y+v.y,z+v.z); }
  Vec3 operator-(const Vec3& v) const { return Vec3(x-v.x,y-v.y,z-v.z); }
  Vec3 operator*(float s) const { return Vec3(x*s,y*s,z*s); }

  void normalize() {
    float m = sqrt(x*x+y*y+z*z);
    if (m > 0) { x/=m; y/=m; z/=m; }
  }
};

struct Vec2 { float x,y; };
Vec3 angularVel = {0,0,0};

float dot(const Vec3& a,const Vec3& b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(const Vec3& a,const Vec3& b){
  return Vec3(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x);
}

// =============================
// Quad構造（★ここが重要）
// =============================
struct Quad {
  int v[4];
  Vec3 center;
  int team;   // 0:透明 1:緑
};

std::vector<Vec3> vertices;
std::vector<Quad> quads;

// ===== 回転（新方式） =====
Vec3 rotAxis = {0, 0, 0};
Vec3 stableAxis = {0,0,0};
int lastTouchX = 0;
int lastTouchY = 0;
bool touching = false;

// =============================
// Canvas
// =============================
M5Canvas canvas(&M5.Display);

// =============================
// 球生成
// =============================
void createQuadSphere() {
  vertices.clear();
  quads.clear();

  Vec3 normals[] = {
    {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
  };

  Vec3 tangents[] = {
    {0,1,0},{0,1,0},{1,0,0},{1,0,0},{1,0,0},{1,0,0}
  };

  int offset = 0;

  for (int f=0; f<6; f++) {
    Vec3 n = normals[f];
    Vec3 t = tangents[f];
    Vec3 b = cross(n,t);

    for(int j=0;j<=GRID_SIZE;j++){
      for(int i=0;i<=GRID_SIZE;i++){
        float u = (float)i/GRID_SIZE*2-1;
        float v = (float)j/GRID_SIZE*2-1;

        Vec3 p = n + t*u + b*v;
        p.normalize();
        vertices.push_back(p*SPHERE_RADIUS);
      }
    }

    for(int j=0;j<GRID_SIZE;j++){
      for(int i=0;i<GRID_SIZE;i++){
        Quad q;

        q.v[0]=offset+j*(GRID_SIZE+1)+i;
        q.v[1]=offset+j*(GRID_SIZE+1)+i+1;
        q.v[2]=offset+(j+1)*(GRID_SIZE+1)+i+1;
        q.v[3]=offset+(j+1)*(GRID_SIZE+1)+i;

        q.center = (vertices[q.v[0]]+
                    vertices[q.v[1]]+
                    vertices[q.v[2]]+
                    vertices[q.v[3]])*0.25f;

        // ★半分だけ緑
        q.team = (f < 3) ? 1 : 0;

        quads.push_back(q);
      }
    }

    offset += (GRID_SIZE+1)*(GRID_SIZE+1);
  }
}

// =============================
// 回転
// =============================
Vec3 rotate(Vec3 v){
  Vec3 axis = rotAxis;
  axis.normalize();

  float angle = sqrt(rotAxis.x*rotAxis.x + rotAxis.y*rotAxis.y + rotAxis.z*rotAxis.z);

  if (angle < 0.0001f) return v;

  float c = cos(angle);
  float s = sin(angle);

  return v * c +
         cross(axis, v) * s +
         axis * dot(axis, v) * (1 - c);
}

// =============================
// タッチ操作（トラックボール）
// =============================
void updateTouch(){

  auto t = M5.Touch.getDetail();

  // =====================
  // TAP CLICK DETECTOR
  // =====================
  if (t.isPressed()) {

    if (!tapArmed) {
      tapArmed = true;
      tapTravel = 0.0f;
      tapStartMs = millis();
      tapStartX = t.x;
      tapStartY = t.y;
    }

  } else {

    if (tapArmed) {

      uint32_t dt = millis() - tapStartMs;

      int moveX = abs(t.x - tapStartX);
      int moveY = abs(t.y - tapStartY);
      float speedNow = sqrt(
        angularVel.x * angularVel.x +
        angularVel.y * angularVel.y
      );

      if (
          dt < 170 &&
          moveX < 10 &&
          moveY < 10 &&
          tapTravel < 18 &&
          speedNow < 0.55f
      ) {
        Serial.print("CLICK\n");
        Serial.flush();
        clickFx = 1.0f;
      }
    }

    tapArmed = false;
  }
  

  // =========================
  // TRACKPAD MODE
  // =========================
  if (padMode) {

  if (t.isPressed()) {
  float speed = sqrt(
  angularVel.x * angularVel.x +
  angularVel.y * angularVel.y
);

// 連続慣性
float inertia = 0.08f + speed * 0.18f;

// 制限
if (inertia > 0.88f) inertia = 0.88f;
if (inertia < 0.05f) inertia = 0.05f;

angularVel.x *= inertia;
angularVel.y *= inertia;

    if (!touching) {
      touching = true;
      lastTouchX = t.x;
      lastTouchY = t.y;

      // ★ returnしない
    }

    int dx = t.x - lastTouchX;
    int dy = t.y - lastTouchY;

    tapTravel += abs(dx) + abs(dy);

    lastTouchX = t.x;
    lastTouchY = t.y;

    //float speed = sqrt(dx * dx + dy * dy);

    // ======================
    // 微動ブースト（重要）
    // ======================
    float gain = 0.22f;

    if (speed < 2.0f) gain = 0.34f;
    if (speed < 1.0f) gain = 0.48f;
    if (speed < 0.5f) gain = 0.60f;

  float targetX = dy * gain;
  float targetY = -dx * gain;

// 初動はゆっくり立ち上がる
float smooth;

if (speed < 0.6f) smooth = 0.96f;
else if (speed < 1.2f) smooth = 0.78f;
else smooth = 0.42f;

if (speed < 0.65f) {

  // BALL的精密制御
  angularVel.x += dy * 0.10f;
  angularVel.y += -dx * 0.10f;

if (speed < 0.65f) {
  angularVel.x *= 0.92f;
  angularVel.y *= 0.92f;
}

fxPrevX = fxX;
fxPrevY = fxY;

fxX = t.x;
fxY = t.y;

float ddx = fxX - fxPrevX;
float ddy = fxY - fxPrevY;

fxPower = sqrt(ddx * ddx + ddy * ddy);
fxTouch = true;

} else {

  // 通常PAD
  angularVel.x = angularVel.x * 0.35f + dy * gain * 0.65f;
  angularVel.y = angularVel.y * 0.35f + -dx * gain * 0.65f;
}


} else {

    touching = false;

  static float microX = 0;
  static float microY = 0;

  fxTouch = false;
  fxPower *= 0.90f;

int dx = t.x - lastTouchX;
int dy = t.y - lastTouchY;

lastTouchX = t.x;
lastTouchY = t.y;

// 微動蓄積
microX += dx;
microY += dy;

float outX = 0;
float outY = 0;

    // しきい値超えたら少しずつ出す
    if (fabs(microX) > 0.15f) {
      outX = microX * 0.25f;
      microX *= 0.4f;
    }

    if (fabs(microY) > 0.15f) {
      outY = microY * 0.25f;
      microY *= 0.4f;
    }

    angularVel.x = outY;
    angularVel.y = -outX;

    if (fabs(angularVel.x) < 0.04f) angularVel.x = 0;
    if (fabs(angularVel.y) < 0.04f) angularVel.y = 0;
  }

  return;
}
  
  // =========================
  // TRACKBALL MODE
  // =========================

  if(t.isPressed()){

    // ===== 初タッチ =====
    if (!touching) {
      touching = true;

      angularVel = {0, 0, 0};
      stableAxis = {0, 0, 0};
      rotAxis = {0, 0, 0};
      movementStarted = false;

      accDX = 0;
      accDY = 0;

      lastTouchX = t.x;
      lastTouchY = t.y;

      return; // 初回は差分なし
    }

    // ===== 差分取得 =====
    int rawDX = t.x - lastTouchX;
    int rawDY = t.y - lastTouchY;

    lastTouchX = t.x;
    lastTouchY = t.y;

    // =========================
    // BALL 精密操作モード
    // 慣性OFF / 軸固定なし / PAD的な直入力
    // =========================
    float rawSpeed = sqrt(rawDX * rawDX + rawDY * rawDY);

    if (rawSpeed > 0 && rawSpeed < 1.4f) {

      ballPrecisionActive = true;

      // 軸固定を使わない
      stableAxis = {0, 0, 0};

      // 微細操作用ゲイン
      float microGain = 0.11f;

      if (rawSpeed < 0.7f) microGain = 0.075f;
      if (rawSpeed < 0.4f) microGain = 0.055f;

      // PADと同じ感覚：指の動きをそのまま回転速度へ
      angularVel.x = angularVel.x * 0.20f + rawDY * microGain;
      angularVel.y = angularVel.y * 0.20f + -rawDX * microGain;

      // 回転適用
      Vec3 axisX = rotate({1,0,0});
      Vec3 axisY = rotate({0,1,0});

      Vec3 worldAxis = axisX * angularVel.x + axisY * angularVel.y;

      float angle = sqrt(
        angularVel.x * angularVel.x +
        angularVel.y * angularVel.y
      );

      if (angle > 0.0001f) {
        worldAxis.normalize();

        rotAxis.x += worldAxis.x * angle;
        rotAxis.y += worldAxis.y * angle;
        rotAxis.z += worldAxis.z * angle;
      }

      return;
    }

    ballPrecisionActive = false;

    accDX += (float)rawDX;
    accDY += (float)rawDY;

    int dx = (int)accDX;
    int dy = (int)accDY;

    accDX -= dx;
    accDY -= dy;

    float moveAmount = fabs(dx) + fabs(dy);

    // ===== 初動ロック =====
    if (!movementStarted) {
    if (moveAmount < 0.25f) {
      return;
    }

    movementStarted = true;

    // 初動ジャンプ防止を弱める
    accDX *= 0.7f;
    accDY *= 0.7f;
  }

    // ===== スピード =====
    float speed = sqrt(dx*dx + dy*dy);

    // ===== 軸生成 =====
    Vec3 inputAxis = {(float)dy, (float)-dx, 0.0f};

    float len = sqrt(inputAxis.x*inputAxis.x + inputAxis.y*inputAxis.y);
    if (len > 0.0f) {
      inputAxis.x /= len;
      inputAxis.y /= len;
    }

    // ===== 初動で軸を固定 =====
    if (stableAxis.x == 0 && stableAxis.y == 0) {
      stableAxis = inputAxis;
    }

    // ===== なめらか追従 =====
        // ===== 速度カーブ =====
    float v = speed;
    float stability;

    if (v < 1.2f) {
      stability = 0.25f;
    } else if (v < 2.0f) {
      stability = 0.65f;
    } else {
      stability = 0.92f;
    }

    stableAxis.x = stableAxis.x * stability + inputAxis.x * (1.0f - stability);
    stableAxis.y = stableAxis.y * stability + inputAxis.y * (1.0f - stability);

    float slen = sqrt(stableAxis.x*stableAxis.x + stableAxis.y*stableAxis.y);
    if (slen > 0.0f) {
      stableAxis.x /= slen;
      stableAxis.y /= slen;
    }

    Vec3 axis = stableAxis;


   // デッドゾーン圧縮
    if (v < 0.5f) {
      v = v * 0.5f;  // ← 完全ゼロにしない
    }
    

    // 低速ブースト（滑らかに）
    float lowBoost = 1.0f;
    if (v > 0 && v < 2.0f) {
      float t = v / 2.0f;          // 0〜1
      lowBoost = 1.5f - 0.5f * t;  // 1.5 → 1.0 に減衰
    }

    float microBoost = 0.0f;
    if (v < 1.0f) {
      float t = 1.0f - (v / 1.0f);  // 1→0
      microBoost = pow(t, 2.0f) * 0.05f;           // ←ここが重要
    }

    float power;

    if (v < 1.0f) {
      // ★微動ゾーン（ほぼリニア）
      power = v * sensGain * 2.8f;
    } else {
      // ★加速ゾーン
      float t = v - 1.0f;
      power = (1.0f + t * t*0.60f) * sensGain;
    }
    
    if (speed < 0.5f) {
    // 何もしない（減衰しない）
    } else if (speed < 1.5f) {
      angularVel.x *= 0.9f;
      angularVel.y *= 0.9f;
    }

    // ===== 合成 =====
    float follow;

    if (v < 1.0f) {
      follow = 0.90f;
    } else if (v < 2.0f) {
      follow = 0.70f;
    } else {
      follow = 0.45f;
    }


    angularVel.x = angularVel.x * (1.0f - follow) + axis.x * power * follow;
    angularVel.y = angularVel.y * (1.0f - follow) + axis.y * power * follow;
    float directPower = 0.0f;

      if (v < 0.8f) {
        float t = (0.8f - v) / 0.8f;
        directPower = t * 0.05f;
      }

      float finalPower = power + directPower;
    
    
    // ===== 微動補助 =====
    if (v < 1.2f) {
      float direct = (1.2f - v) / 1.2f;
      angularVel.x += axis.x * direct * 0.10f;
      angularVel.y += axis.y * direct * 0.10f;
    }
    
    
      // ===== 回転適用 =====
    Vec3 axisX = rotate({1,0,0});
    Vec3 axisY = rotate({0,1,0});

    Vec3 worldAxis = axisX * angularVel.x + axisY * angularVel.y;

    float angle = sqrt(angularVel.x*angularVel.x + angularVel.y*angularVel.y);

    if (angle > 0.0001f) {
      worldAxis.normalize();

      rotAxis.x += worldAxis.x * angle;
      rotAxis.y += worldAxis.y * angle;
      rotAxis.z += worldAxis.z * angle;
    }

  } else {

    // ===== タッチ終了 =====
    touching = false;
    stableAxis = {0,0,0};

    // 精密操作中に離した場合は慣性を残さない
    if (ballPrecisionActive) {
      angularVel = {0,0,0};
      ballPrecisionActive = false;
    }
  }

  // ===== 慣性 =====
  if(!touching){

    Vec3 axisX = rotate({1,0,0});
    Vec3 axisY = rotate({0,1,0});

    Vec3 worldAxis = axisX * angularVel.x + axisY * angularVel.y;

    float angle = sqrt(angularVel.x*angularVel.x + angularVel.y*angularVel.y);

    if (angle > 0.0001f) {
      worldAxis.normalize();

      rotAxis.x += worldAxis.x * angle;
      rotAxis.y += worldAxis.y * angle;
      rotAxis.z += worldAxis.z * angle;
    }

    float speed = sqrt(angularVel.x*angularVel.x + angularVel.y*angularVel.y);

    float f;
    if (speed > 4.0f) f = 0.75f;
    else if (speed > 1.0f) f = 0.70f;
    else f = 0.65f;

    angularVel.x *= f;
    angularVel.y *= f;

    if (fabs(angularVel.x) < 0.05f) angularVel.x = 0;
    if (fabs(angularVel.y) < 0.05f) angularVel.y = 0;
  }
}

void drawCore() {

  if (clickFx <= 0.01f) return;

  static float phase = 0.0f;
  phase += 0.12f;

  // =====================
  // 3D中心
  // =====================
  Vec3 corePos = {0,0,0};
  Vec3 v = rotate(corePos);

  v.z += CAMERA_Z;
  if (v.z <= 0) return;

  float s = FOV / v.z;

  int cx = v.x * s + CANVAS_WIDTH  / 2;
  int cy = v.y * s + CANVAS_HEIGHT / 2;

  // 揺らぎ
  cx += sin(phase * 0.9f) * 2.5f * clickFx;
  cy += cos(phase * 1.2f) * 2.5f * clickFx;

  // =====================
  // 球色に合わせた抽象光
  // =====================
  for (int i = 5; i >= 0; i--) {

    int rr = 7 + i * 4 + clickFx * 6;

    float fade = (float)i / 5.0f;

    uint8_t r = 0;
    uint8_t g = 40 + clickFx * 90 * fade;
    uint8_t b = 18 + clickFx * 45 * fade;

    uint16_t col = canvas.color565(r, g, b);

    canvas.fillCircle(cx, cy, rr, col);
  }

  // 中心気配だけ
  canvas.fillCircle(
    cx,
    cy,
    2 + clickFx,
    canvas.color565(20, 180, 60)
  );
}

void drawSphere(){

  canvas.fillSprite(TFT_BLACK);

  std::vector<Vec2> proj(vertices.size());
  std::vector<Vec3> rotv(vertices.size());

  // 頂点変換
  for(size_t i=0;i<vertices.size();i++){
    Vec3 v = rotate(vertices[i]);
    v.z += CAMERA_Z;

    if(v.z<=0){
      proj[i]={-9999,-9999};
      continue;
    }

    float s = FOV/v.z;

    proj[i].x = v.x*s + CANVAS_WIDTH/2;
    proj[i].y = v.y*s + CANVAS_HEIGHT/2;

    rotv[i]=v;
  }

  // Zソート
  std::vector<std::pair<float,int>> zsort;
  for(size_t i=0;i<quads.size();i++){
    Vec3 c = rotate(quads[i].center);
    zsort.push_back({c.z, i});
  }
std::sort(zsort.begin(), zsort.end(),
          [](const std::pair<float,int>& a,
             const std::pair<float,int>& b) {
            return a.first > b.first;
          });

  // 描画
  for(auto &p:zsort){
    Quad &q = quads[p.second];

    // ★透明は描かない
    //if(q.team==0) continue;

    Vec2 pt[4];
    Vec3 rv[4];

    for(int i=0;i<4;i++){
      pt[i]=proj[q.v[i]];
      rv[i]=rotv[q.v[i]];
    }

    if(pt[0].x==-9999) continue;

    Vec3 e1 = rv[1]-rv[0];
    Vec3 e2 = rv[2]-rv[0];
    Vec3 n  = cross(e1,e2);

    // 裏面


float shade = (rv[0].z - CAMERA_Z)/SPHERE_RADIUS;
shade = constrain(shade,0,1);

float glow = pow(shade, 2.0f);
float rim  = pow(1.0f - fabs(n.z), 3.0f);

uint8_t g = 120 + glow * 135 + rim * 120;
uint8_t b = glow * 80;

uint16_t col = canvas.color565(0, g, b);


// ===== 非対称マーカー追加 =====

// 極（上下）
float pole = vertices[q.v[0]].y / SPHERE_RADIUS;

if (pole > 0.6f) {
  float t = (pole - 0.6f) / 0.4f;

  uint8_t g = 180 + t * 75;
  uint8_t r = t * 40;
  uint8_t b = t * 10;

  float speed = sqrt(angularVel.x*angularVel.x + angularVel.y*angularVel.y);

  if (speed > 0.2f && random(0,100) < 30) {
    g = constrain(g + 100, 0, 255); // メイン発光
    r = constrain(r + 20, 0, 255);  // 少しだけ暖色
    // bは上げない → 緑維持
  }

  col = canvas.color565(r, g, b);
}

if (pole < -0.6f) {
  float t = (-pole - 0.6f) / 0.4f;

  uint8_t g = 180 + t * 75;
  uint8_t r = 0;
  uint8_t b = t * 30;  // ←青をかなり抑える

  float speed = sqrt(angularVel.x*angularVel.x + angularVel.y*angularVel.y);

  if (speed > 0.2f && random(0,100) < 30) {
    g = constrain(g + 100, 0, 255);
    // bはほぼ上げない
  }

  col = canvas.color565(r, g, b);
}

// ===== ベース座標（球に貼り付く） =====
Vec3 base = vertices[q.v[0]];
    canvas.fillTriangle(pt[0].x,pt[0].y,pt[1].x,pt[1].y,pt[2].x,pt[2].y,col);
    canvas.fillTriangle(pt[0].x,pt[0].y,pt[2].x,pt[2].y,pt[3].x,pt[3].y,col);

    canvas.drawLine(pt[0].x,pt[0].y,pt[1].x,pt[1].y, TFT_BLACK);
    canvas.drawLine(pt[1].x,pt[1].y,pt[2].x,pt[2].y, TFT_BLACK);
    canvas.drawLine(pt[2].x,pt[2].y,pt[3].x,pt[3].y, TFT_BLACK);
    canvas.drawLine(pt[3].x,pt[3].y,pt[0].x,pt[0].y, TFT_BLACK);

  }


// // =====================
// // CLICK RIPPLE (BALL thin)
// // =====================
// if (clickFx > 0.01f) {

//   int cx = CANVAS_WIDTH / 2;
//   int cy = CANVAS_HEIGHT / 2;

//   int rr = SPHERE_RADIUS + (1.0f - clickFx) * 38;

//   uint16_t col = canvas.color565(
//     90 + clickFx * 50,
//     180 + clickFx * 50,
//     180 + clickFx * 40
//   );

//   // 1本線のみ（細く）
//   canvas.drawCircle(cx, cy, rr, col);
// }


// =====================
// CLICK CORE ONLY
// =====================
if (clickFx > 0.01f) {
  drawCore();
}


  canvas.pushSprite(
    (M5.Display.width()-CANVAS_WIDTH)/2,
    (M5.Display.height()-CANVAS_HEIGHT)/2
  );
}


void drawKaossPadFX() {

  // 背景
  M5.Display.fillScreen(TFT_BLACK);

  // グリッド
  for (int x = 0; x < 320; x += 40)
    M5.Display.drawFastVLine(x, 0, 240, TFT_DARKGREY);

  for (int y = 0; y < 240; y += 40)
    M5.Display.drawFastHLine(0, y, 320, TFT_DARKGREY);

  // 色変化
  uint16_t col = TFT_CYAN;

  if (fxPower > 2.0f) col = TFT_GREEN;
  if (fxPower > 4.0f) col = TFT_YELLOW;
  if (fxPower > 7.0f) col = TFT_RED;

  // 軌跡
  M5.Display.drawLine(
    (int)fxPrevX,
    (int)fxPrevY,
    (int)fxX,
    (int)fxY,
    col
  );

  // リング
  int r = 12 + fxPower * 2.0f;

  M5.Display.drawCircle((int)fxX, (int)fxY, r, col);
  M5.Display.drawCircle((int)fxX, (int)fxY, r + 1, col);

  // 中心点
  M5.Display.fillCircle((int)fxX, (int)fxY, 3, TFT_WHITE);
  
  // =====================
// CLICK RIPPLE
// =====================
if (clickFx > 0.01f) {

  int r = 20 + (1.0f - clickFx) * 80;

  uint16_t col = M5.Display.color565(
    255,
    255 * clickFx,
    255 * clickFx
  );

  M5.Display.drawCircle((int)trailX, (int)trailY, r, col);
  M5.Display.drawCircle((int)trailX, (int)trailY, r + 2, col);
}



  M5.Display.setCursor(10,10);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.println("TRACKPAD MODE");
}

void drawCursorTrailFX() {

  // 残像フェード
  M5.Display.fillScreen(TFT_BLACK);

  // 中心基点
  M5.Display.fillCircle(160, 120, 3, TFT_WHITE);

  // 軌道線
  M5.Display.drawLine(
    160, 120,
    (int)trailX,
    (int)trailY,
    TFT_CYAN
  );

  // 先端リング
  int r = 6 + sqrt(trailVX * trailVX + trailVY * trailVY) * 2;

  uint16_t col = TFT_CYAN;

  if (r > 10) col = TFT_YELLOW;
  if (r > 14) col = TFT_RED;

  M5.Display.drawCircle((int)trailX, (int)trailY, r, col);
  M5.Display.fillCircle((int)trailX, (int)trailY, 3, TFT_WHITE);

  if (clickFx > 0.01f) {

  int rr = 8 + (1.0f - clickFx) * 60;

  uint16_t fxCol = M5.Display.color565(
    255,
    255 * clickFx,
    255 * clickFx
  );

  M5.Display.drawCircle((int)trailX, (int)trailY, rr, fxCol);
  M5.Display.drawCircle((int)trailX, (int)trailY, rr + 2, fxCol);
}
  
  
  // 自然復帰
  trailX += (160 - trailX) * 0.08f;
  trailY += (120 - trailY) * 0.08f;
}


//マウスレポート送信
// void sendMouseDelta() {

//   static float accumX = 0;
//   static float accumY = 0;
 
//   float speed = sqrt(angularVel.x*angularVel.x + angularVel.y*angularVel.y);

//   // ★スケールを自動調整
//   float scale = 32.0f / (1.0f + speed * 0.45f);

//   // ★最低保証（これが超重要）
//   if (scale < 10.0f) scale = 10.0f;

//   accumX += -angularVel.y * scale;
//   accumY +=  angularVel.x * scale;

//   int dx = (int)round(accumX);
//   int dy = (int)round(accumY);

//     // 微動アシスト
//     if (dx == 0 && fabs(accumX) > 0.25f) {
//       dx = (accumX > 0) ? 1 : -1;
//     }

//     if (dy == 0 && fabs(accumY) > 0.25f) {
//       dy = (accumY > 0) ? 1 : -1;
//     }

//   accumX -= dx;
//   accumY -= dy;

//   // USB
//   Serial.write(0x30);
//   Serial.write((int8_t)dx);
//   Serial.write((int8_t)dy);

//   // 🔥 Bluetooth追加
//   if (SerialBT.hasClient()) {
//     SerialBT.write(0x30);
//     SerialBT.write((int8_t)dx);
//     SerialBT.write((int8_t)dy);
//   }
// }
void sendMouseDelta() {
  static float accumX = 0.0f;
  static float accumY = 0.0f;

  static float outX = 0;
  static float outY = 0;

  outX = outX * 0.65f + accumX * 0.35f;
  outY = outY * 0.65f + accumY * 0.35f;

  int dx = (int)round(outX);
  int dy = (int)round(outY);

  // 疑似高Hz化: 1フレームの移動を複数サブステップに分ける
  const int subSteps = 2;

  float speed = sqrt(angularVel.x * angularVel.x + angularVel.y * angularVel.y);

  // 速度に応じたスケール
float mid = speed * speed;

// 中速だけ追加減衰
if (speed > 0.8f && speed < 1.8f) {
  mid *= 1.55f;
}

float scale = 14.0f / (1.0f + speed * 0.75f);

if (scale < 4.2f) scale = 4.2f;

  // 1フレーム分の移動量
  float frameMoveX = -angularVel.y * scale * 1.6f;
  float frameMoveY =  angularVel.x * scale * 1.6f;

  // サブステップに分割して accum に積む
  if (fabs(angularVel.x) < 0.03f &&
    fabs(angularVel.y) < 0.03f) {
    accumX = 0;
    accumY = 0;
  }

  for (int i = 0; i < subSteps; i++) {
    accumX += frameMoveX / subSteps;
    accumY += frameMoveY / subSteps;

    int dx = (int)round(accumX);
    int dy = (int)round(accumY);

    // 微動アシスト
    if (dx == 0 && fabs(accumX) > 0.42f) {
      dx = (accumX > 0) ? 1 : -1;
    }
    if (dy == 0 && fabs(accumY) > 0.42f) {
      dy = (accumY > 0) ? 1 : -1;
    }

    accumX -= dx;
    accumY -= dy;

    if (dx != 0 || dy != 0) {
      Serial.write(0x30);
      Serial.write((int8_t)dx);
      Serial.write((int8_t)dy);
      
      trailVX = dx;
      trailVY = dy;
      trailX += dx * 6.0f;
      trailY += dy * 6.0f;
      // 範囲制限
      trailX = constrain(trailX, 20, 300);
      trailY = constrain(trailY, 20, 220);

      if (SerialBT.hasClient()) {
        SerialBT.write(0x30);
        SerialBT.write((int8_t)dx);
        SerialBT.write((int8_t)dy);
      }
    }
  }
}

// =============================
// SETUP
// =============================
void setup(){
  auto cfg=M5.config();
  M5.begin(cfg);
  SerialBT.begin("TrackballCore2");  // 名前は自由
  canvas.createSprite(CANVAS_WIDTH,CANVAS_HEIGHT);
  M5.Display.setBrightness(180);

  Serial.begin(115200); 

  createQuadSphere();
}

// =============================
// LOOP
// =============================
void loop(){
  M5.update();

  if (M5.BtnC.wasPressed()) {

  padMode = !padMode;

  touching = false;
  movementStarted = false;

  angularVel = {0,0,0};
  rotAxis    = {0,0,0};
  stableAxis = {0,0,0};

  trailX  = 160;
  trailY  = 120;
  trailVX = 0;
  trailVY = 0;

  fxX = 160;
  fxY = 120;
  fxPrevX = 160;
  fxPrevY = 120;

  fxPower = 0;
  fxTouch = false;

  clickFx = 0;
  tapArmed = false;

  // ★ここ重要
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.endWrite();

  canvas.fillSprite(TFT_BLACK);
  canvas.pushSprite(0,0);

  delay(20);
}

updateTouch();

// ★追加
clickFx *= 0.5f;
if (clickFx < 0.01f) clickFx = 0.0f;

if (!padMode) {
  drawSphere();
} else {
  drawCursorTrailFX();
}
  sendMouseDelta();  

  delay(5);
}