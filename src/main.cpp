#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <font/glcdfont.h>
#include "WiFiSetup.h"

// ---- WiFi (起動時に WiFiManager の設定ポータルで設定する。WiFiSetup.h) ----
#define DEVICE_BASE_NAME   "WorkingTimer" // 後ろに MAC の末尾 4 桁を付けて AP 名・ホスト名・MQTT クライアント ID に使う (例 WorkingTimer-3FA2)
#define WIFI_PORTAL_SEC    180            // 設定モードの上限。過ぎたらオフラインでタイマーとして動く
#define MQTT_HOST_DEFAULT  "192.168.11.32" // 設定ページの「MQTT server」欄の初期値。空欄で保存すると MQTT を使わない

// ---- MQTT (呼び出しベルシステムに参加する) ----
#define MQTT_PORT         1883
#define MQTT_SUB_TOPIC    "doorbell/call/device3" // 同じ部屋の受信機と同じ (送信機のボタン B)。ベルへは何も送らない (受け取るだけ)

// ---- 液晶の配線 (TFT-VUMeterTester-170-320 と同じ) ----
// GND = GND, VCC = 3.3V, SDA-0(MISO)は未接続
#define TFT_SCK  18  // SCL
#define TFT_MOSI 23  // SDA
#define TFT_MISO -1  // 未接続
#define TFT_CS   5
#define TFT_DC   16
#define TFT_RST  17
#define TFT_BL   4
#define TFT_COL_OFFSET 35
#define TFT_ROW_OFFSET 0

// 日の出・日の入りの計算に使う場所 (岐阜市)
#define SITE_LAT 35.4233f
#define SITE_LON 136.7607f

// ---- スイッチとブザー ----
// スイッチはピンと GND の間につなぐ (内部プルアップを使うので抵抗は不要)
#define BTN_SELECT 33 // 短押し: 時間を選ぶ / 長押し: 表示モードを順に切り替え
#define BTN_START  32 // 短押し: スタート・一時停止・再開 / 長押し: リセット
#define BUZZER_PIN 25 // 圧電ブザー (+ をこのピン、- を GND)。使わないときは -1

#define SCREEN_W 320
#define SCREEN_H 170
#define STRIP_H  85 // 画面を上下 2 回に分けて描く (メモリ節約のため)

// 横置き (rotation 3: 天地逆) で 320x170 として使う
Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
Arduino_ST7789 *tft = new Arduino_ST7789(
    bus, TFT_RST, 3 /* rotation */, true /* IPS */,
    170, 320, TFT_COL_OFFSET, TFT_ROW_OFFSET, TFT_COL_OFFSET, TFT_ROW_OFFSET);

// 画面の半分 (320x85) を描くキャンバス。描き終えたら上半分・下半分の位置へ転送する。
// 全面分 (108KB) は WiFi と同居すると確保できないことがあるため半分にしている。
class StripCanvas : public Arduino_Canvas
{
public:
  using Arduino_Canvas::Arduino_Canvas;
  void setOutputY(int16_t y) { _output_y = y; }
};
StripCanvas *cv = new StripCanvas(SCREEN_W, STRIP_H, tft);
int16_t gOff = 0; // いま描いている帯の上端 (画面座標)

// ---- 配色 ----
struct RGB
{
  uint8_t r, g, b;
};
constexpr uint16_t c565(uint8_t r, uint8_t g, uint8_t b)
{
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t c565(RGB c) { return c565(c.r, c.g, c.b); }

const RGB C_CYAN = {0, 214, 255};
const RGB C_AMBER = {255, 168, 0};
const RGB C_RED = {255, 48, 64};
const RGB C_GHOST_RGB = {24, 30, 40};

const uint16_t COL_BG = c565(5, 7, 11);
const uint16_t COL_GHOST = c565(C_GHOST_RGB);
const uint16_t COL_LINE = c565(44, 52, 66);
const uint16_t COL_DIM = c565(112, 122, 138);
const uint16_t COL_TEXT = c565(225, 232, 240);
const uint16_t COL_CLOCK = c565(235, 240, 248);
const uint16_t COL_SEC = c565(C_AMBER);
const uint16_t COL_RED = c565(C_RED);

RGB lerpRGB(RGB a, RGB b, float t)
{
  t = constrain(t, 0.0f, 1.0f);
  return {(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t), (uint8_t)(a.b + (b.b - a.b) * t)};
}

// 残り割合で色を変える: 50% 以上はシアン → 20% でアンバー → 0% で赤
RGB phaseRGB(float frac)
{
  if (frac >= 0.5f)
    return C_CYAN;
  if (frac >= 0.2f)
    return lerpRGB(C_AMBER, C_CYAN, (frac - 0.2f) / 0.3f);
  return lerpRGB(C_RED, C_AMBER, frac / 0.2f);
}

// 終了時刻リングのページ用 (ほかのカウントダウンと見分けるため): 終了前 5 分は赤、その前の 10 分は黄、それより前は緑
const RGB C_GO = {40, 220, 90};
const RGB C_YELLOW = {255, 220, 0};
const float END_RED_MIN = 5, END_YELLOW_MIN = 15;
RGB endZoneRGB(float remMin)
{
  return remMin <= END_RED_MIN ? C_RED : remMin <= END_YELLOW_MIN ? C_YELLOW : C_GO;
}

// ---- 描画の下請け (帯のずれ gOff を差し引いて描く) ----
inline void FR(int x, int y, int w, int h, uint16_t c) { cv->fillRect(x, y - gOff, w, h, c); }
inline void RR(int x, int y, int w, int h, int r, uint16_t c) { cv->fillRoundRect(x, y - gOff, w, h, r, c); }
inline void DRR(int x, int y, int w, int h, int r, uint16_t c) { cv->drawRoundRect(x, y - gOff, w, h, r, c); }
inline void TRI(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) { cv->fillTriangle(x0, y0 - gOff, x1, y1 - gOff, x2, y2 - gOff, c); }
inline void HL(int x, int y, int w, uint16_t c) { cv->drawFastHLine(x, y - gOff, w, c); }
inline void VL(int x, int y, int h, uint16_t c) { cv->drawFastVLine(x, y - gOff, h, c); }
inline void CIRC(int x, int y, int r, uint16_t c) { cv->fillCircle(x, y - gOff, r, c); }
inline void QUAD(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, uint16_t c)
{
  TRI(lroundf(x0), lroundf(y0), lroundf(x1), lroundf(y1), lroundf(x2), lroundf(y2), c);
  TRI(lroundf(x0), lroundf(y0), lroundf(x2), lroundf(y2), lroundf(x3), lroundf(y3), c);
}

// 文字は 5x7 フォントを 1 ドットずつ fillRect で描く。
// ライブラリの文字描画は上端・左端からのはみ出しを切り取らずにメモリへ書き込むため、
// 帯の境目をまたぐ文字でキャンバスの外を壊してしまう (再起動を繰り返す原因だった)。
void text(int x, int y, uint8_t size, uint16_t c, const char *s)
{
  for (; *s; s++, x += 6 * size)
  {
    for (int i = 0; i < 5; i++)
    {
      uint8_t line = font[(uint8_t)*s * 5 + i];
      for (int j = 0; j < 8; j++, line >>= 1)
        if (line & 1)
          FR(x + i * size, y + j * size, size, size, c);
    }
  }
}
int textW(const char *s, uint8_t size) { return strlen(s) * 6 * size - size; }
void textR(int right, int y, uint8_t size, uint16_t c, const char *s) { text(right - textW(s, size), y, size, c, s); }
void textC(int cx, int y, uint8_t size, uint16_t c, const char *s) { text(cx - textW(s, size) / 2, y, size, c, s); }

// ---- 7 セグメント数字 ----
// 消えているセグメントも薄く描いて、本物の表示器のような見た目にする
const uint8_t SEG_MAP[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
const uint8_t SEG_DASH = 0x40;

void hSeg(int x0, int yc, int len, int t, uint16_t c)
{
  int h = t / 2;
  FR(x0 + h, yc - h, len - 2 * h + 1, 2 * h + 1, c);
  TRI(x0, yc, x0 + h, yc - h, x0 + h, yc + h, c);
  TRI(x0 + len, yc, x0 + len - h, yc - h, x0 + len - h, yc + h, c);
}

void vSeg(int xc, int y0, int len, int t, uint16_t c)
{
  int h = t / 2;
  FR(xc - h, y0 + h, 2 * h + 1, len - 2 * h + 1, c);
  TRI(xc, y0, xc - h, y0 + h, xc + h, y0 + h, c);
  TRI(xc, y0 + len, xc - h, y0 + len - h, xc + h, y0 + len - h, c);
}

void drawDigit(int x, int y, int w, int h, int t, uint8_t bits, uint16_t on, uint16_t off)
{
  int hh = t / 2;
  int gap = max(1, t / 4);
  int xl = x + hh, xr = x + w - 1 - hh;
  int yt = y + hh, yb = y + h - 1 - hh, ym = y + (h - 1) / 2;
  int hl = xr - xl - 2 * gap;
  hSeg(xl + gap, yt, hl, t, (bits & 0x01) ? on : off);       // a
  vSeg(xr, yt + gap, ym - yt - 2 * gap, t, (bits & 0x02) ? on : off); // b
  vSeg(xr, ym + gap, yb - ym - 2 * gap, t, (bits & 0x04) ? on : off); // c
  hSeg(xl + gap, yb, hl, t, (bits & 0x08) ? on : off);       // d
  vSeg(xl, ym + gap, yb - ym - 2 * gap, t, (bits & 0x10) ? on : off); // e
  vSeg(xl, yt + gap, ym - yt - 2 * gap, t, (bits & 0x20) ? on : off); // f
  hSeg(xl + gap, ym, hl, t, (bits & 0x40) ? on : off);       // g
}

struct SegStyle
{
  int w, h, t, sp; // 数字の幅・高さ・線の太さ・文字間
};

int colonW(const SegStyle &st) { return st.t + st.sp; }

int segTextW(const char *s, const SegStyle &st)
{
  int w = 0;
  for (const char *p = s; *p; p++)
    w += (*p == ':' ? colonW(st) : st.w) + st.sp;
  return w - st.sp;
}

// 数字・'-'・':' を描く。描いた幅を返す
int drawSegText(int x, int y, const char *s, const SegStyle &st, uint16_t on, uint16_t off)
{
  int x0 = x;
  for (const char *p = s; *p; p++)
  {
    if (*p == ':')
    {
      int sz = st.t;
      int cx = x + st.sp / 2;
      FR(cx, y + st.h * 3 / 10 - sz / 2, sz, sz, on);
      FR(cx, y + st.h * 7 / 10 - sz / 2, sz, sz, on);
      x += colonW(st) + st.sp;
      continue;
    }
    uint8_t bits = (*p >= '0' && *p <= '9') ? SEG_MAP[*p - '0'] : (*p == '-' ? SEG_DASH : 0);
    drawDigit(x, y, st.w, st.h, st.t, bits, on, off);
    x += st.w + st.sp;
  }
  return x - x0 - st.sp;
}

// ---- セグメント式のバー ----
void drawSegBar(int x, int y, int w, int h, int n, float frac, RGB col)
{
  float pitch = (float)w / n;
  int segW = max(2, (int)(pitch * 0.72f));
  float lit = frac * n;
  int full = (int)lit;
  uint16_t on = c565(col);
  uint16_t hi = c565(lerpRGB(col, {255, 255, 255}, 0.45f));
  for (int i = 0; i < n; i++)
  {
    int sx = x + (int)(i * pitch);
    if (i < full)
    {
      FR(sx, y, segW, h, on);
      HL(sx, y, segW, hi); // 上辺を明るくして立体感を出す
    }
    else if (i == full && lit > full)
    {
      // 最後の 1 区画は残り具合に応じて明るさを変え、なめらかに減っていくように見せる
      FR(sx, y, segW, h, c565(lerpRGB(C_GHOST_RGB, col, lit - full)));
    }
    else
    {
      FR(sx, y, segW, h, COL_GHOST);
    }
  }
}

// ---- タイマー ----
const uint16_t PRESETS[] = {1, 3, 10, 25, 40, 60, 90, 120};
const int PRESET_N = sizeof(PRESETS) / sizeof(PRESETS[0]);

enum TimerState
{
  READY,
  RUNNING,
  PAUSED,
  DONE
};

TimerState state = READY;
int presetIdx = 3; // 25 分
uint32_t totalMs = 0;
uint32_t remainMs = 0; // READY / PAUSED のときの残り
uint32_t endAt = 0;    // RUNNING のときの終了時刻 (millis)
// 表示モード (SELECT 長押しで順に切り替え)
enum ViewMode
{
  VIEW_DIGITAL,      // 大きな数字で残り時間、下段に時計
  // アナログ時計の外周は、どの表示も終了時刻リング (endRingView)
  VIEW_ANALOG_CLOCK, // アナログ時計 + 右に現在時刻の数字
  VIEW_ANALOG_TIMER, // アナログ時計 + 右に残り時間の数字
  VIEW_ANALOG_ALL,   // アナログ時計 + 右の上に現在時刻 (大)、下に残り時間 (小) とバー・曜日・日付
  VIEW_DASH,         // 小さいアナログ時計 + 現在時刻・日の入り・ポモドーロ記録・砂時計・今日の豆知識
  VIEW_DASH_END,     // VIEW_DASH の数字を残り時間にし、「MIN」の代わりに終了時刻を出したもの
  VIEW_MONITOR,      // PC1〜PC3 の負荷・温度・VRAM と ComfyUI の状態
  VIEW_WIFI,         // 2.4GHz の混み具合 (ch ごとの棒グラフと強い AP)
  VIEW_N
};
uint8_t viewMode = VIEW_DIGITAL;

// 外周を終了時刻リングにする (配色も終了の何分前かで決める) 表示か
bool endRingView()
{
  return viewMode == VIEW_ANALOG_CLOCK || viewMode == VIEW_ANALOG_TIMER || viewMode == VIEW_ANALOG_ALL ||
         viewMode == VIEW_DASH || viewMode == VIEW_DASH_END;
}

Preferences prefs;

uint32_t remaining()
{
  if (state == RUNNING)
  {
    int32_t r = (int32_t)(endAt - millis());
    return r > 0 ? r : 0;
  }
  if (state == DONE)
    return 0;
  return remainMs;
}

void resetTimer()
{
  state = READY;
  totalMs = (uint32_t)PRESETS[presetIdx] * 60000UL;
  remainMs = totalMs;
}

// 残り時間の表示 (秒は切り上げ: 開始直後は 25:00、終了で 00:00)
void formatRemain(char *buf, uint32_t ms)
{
  uint32_t s = (ms + 999) / 1000;
  if (PRESETS[presetIdx] >= 100)
    sprintf(buf, "%03lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  else
    sprintf(buf, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// ---- ボタン ----
enum BtnEvent
{
  EV_NONE,
  EV_SHORT,
  EV_LONG
};

struct Button
{
  uint8_t pin;
  bool pressed = false;
  bool raw = false;
  uint32_t changedAt = 0;
  uint32_t pressedAt = 0;
  bool longFired = false;
  bool suppress = false; // 同時押しに使われたので、離すまでイベントを出さない

  explicit Button(uint8_t p) : pin(p) {}

  BtnEvent update()
  {
    uint32_t now = millis();
    bool r = digitalRead(pin) == LOW;
    if (r != raw)
    {
      raw = r;
      changedAt = now;
    }
    if (now - changedAt < 25 || raw == pressed)
    {
      if (pressed && !longFired && !suppress && now - pressedAt >= 800)
      {
        longFired = true;
        return EV_LONG;
      }
      return EV_NONE;
    }
    pressed = raw;
    if (pressed)
    {
      pressedAt = now;
      longFired = false;
      return EV_NONE;
    }
    bool sup = suppress;
    suppress = false;
    return (longFired || sup) ? EV_NONE : EV_SHORT;
  }
};

Button btnSelect(BTN_SELECT);
Button btnStart(BTN_START);

WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);

String deviceName;            // makeDeviceName() の値 (例 WorkingTimer-3FA2)
char mqttHost[41] = "";       // 設定ページで保存したブローカー。空なら MQTT を使わない
const char *setupApName = nullptr; // WiFi の設定モード中は AP 名 (画面を設定モードの表示にする)

bool mqttEnabled() { return mqttHost[0] != 0; }

// 時刻がまだないときの表示。オフライン (設定モードが時間切れ) なら OFFLINE
const char *netWaitLabel(bool shortForm = false)
{
  if (WiFi.status() == WL_CONNECTED)
    return shortForm ? "NTP.." : "NTP SYNC...";
  if (wifiSetupOffline())
    return "OFFLINE";
  return shortForm ? "WIFI.." : "WIFI...";
}

// ---- 呼び出し (MQTT で ON を受けたとき) ----
bool callReq = false;    // MQTT の受信で立てる。処理は loop() 側で行う
bool callActive = false; // 呼び出し画面を出している
uint32_t callAt = 0;     // 最後に ON を受けた時刻 (millis)
char callTime[6] = "--:--";
const uint32_t CALL_SHOW_MS = 60000; // 呼び出し画面を出しておく時間
const uint32_t CALL_RING_MS = 30000; // 呼び出し音を鳴らす時間

// ---- ブザーの ON/OFF ----
bool buzzerEnabled = true; // OFF のときは終了アラーム・呼び出し・アラートの音を鳴らさない (ボタンのクリック音と切り替えの合図は鳴らす)

// ---- PC の監視 (info/pc/<PC>/status・sys・comfy。仕様は MQTT_TOPICS.md) ----
const char *PC_NAMES[] = {"PC1", "PC2", "PC3"}; // 台数を変えるときはここだけ直す
const int PC_N = sizeof(PC_NAMES) / sizeof(PC_NAMES[0]);
const uint32_t SYS_STALE_MS = 20000;     // sys がこれだけ来なければ NO DATA
const uint32_t BROKER_LOST_MS = 120000;  // MQTT がこれだけ切れたままなら全 PC を -- に
const int CPU_T_WARN = 80, CPU_T_HOT = 90; // 温度の色分け (以上でアンバー / 赤)
const int GPU_T_WARN = 80, GPU_T_HOT = 90;

enum PcStatus : uint8_t
{
  PS_NONE, // まだ何も届いていない
  PS_ONLINE,
  PS_OFFLINE,
  PS_SLEEP,
  PS_LOST
};

enum ComfySt : uint8_t
{
  CS_NONE, // まだ何も届いていない
  CS_OFF,
  CS_IDLE,
  CS_RUN,
  CS_ERROR,
  CS_DOWN,
  CS_STALLED
};

struct PcInfo
{
  PcStatus status = PS_NONE;
  char statusAt[6] = "--:--"; // status が変わった時刻 (HH:MM)

  bool sysSeen = false;
  uint32_t sysAt = 0; // sys を受けた時刻 (millis)
  int16_t cpu = -1, cpuT = -1, ram = -1, gpu = -1, gpuT = -1;
  float vram = -1, vramTot = -1;

  ComfySt comfy = CS_NONE;
  char comfyAt[6] = "--:--"; // comfy の st が変わった時刻
  uint32_t comfyRecv = 0;    // comfy を受けた時刻 (millis。経過秒を進めて見せるため)
  int16_t q = -1, step = -1, steps = -1;
  int32_t el = -1;
  char node[25] = "";
  char err[41] = "";

  // アラートの判定用。接続後の最初の値は基準値としてだけ使う (retain の古い lost や error で誤報しないため)
  bool statusBase = false, comfyBase = false;
  uint8_t alerted = 0; // 出したアラートの種類 (正常に戻るまで同じものは出さない)
};
PcInfo pcs[PC_N];
uint32_t mqttLostAt = 0; // MQTT が切れた時刻 (起動直後は 0 = 起動時から切れている扱い)

// ---- アラート ----
const uint32_t ALERT_RING_MS = 30000;  // 警告音を鳴らす時間 (画面は解除されるまで残す)
const int ALERT_QUEUE_N = 4;           // 上位の表示中に保留しておける数
const int ALERT_HIST_N = 5;            // MONITOR 画面で見られる履歴の数
enum : uint8_t
{
  AL_LOST = 1,
  AL_ERROR = 2,
  AL_DOWN = 4,
  AL_STALLED = 8,
  AL_BROKER = 16, // BROKER LOST (pc = -1)
  AL_COMFY = AL_ERROR | AL_DOWN | AL_STALLED
};

struct Alert
{
  char title[20];  // 大きく出す (例 "PC2 LOST")
  char detail[48]; // 小さく出す (例 "KSampler OutOfMemoryError")
  char at[6];      // 起きた時刻 HH:MM
  bool quiet;      // 軽い音 1 回だけ (BROKER LOST)
  int8_t pc;       // どの PC のものか (-1 = PC に関係しない)
  uint8_t kind;    // AL_* (0 = 自動では消さない)
  uint32_t shownAt; // 最初に画面に出した時刻 (0 = まだ)。警告音はここから数える
};
Alert alertCur;
bool alertActive = false;
Alert alertQueue[ALERT_QUEUE_N];
int alertQueueN = 0;
Alert alertHist[ALERT_HIST_N]; // [0] が新しい
int alertHistN = 0;
bool monitorHistory = false; // MONITOR 画面で履歴を出している

// ---- タイマー終了アラーム (CALL・アラートの表示中は待たせる) ----
bool alarmPending = false; // 鳴らす順番待ち
bool alarmOn = false;
uint32_t alarmAt = 0;

// ---- ブザー ----
uint16_t buzzFreq = 0; // いま鳴らしている周波数 (0 = 止めている)
uint8_t fbKind = 0;    // ブザー ON/OFF 切り替えの合図音 (0 = なし、1 = ON、2 = OFF)
uint32_t fbAt = 0;

void beep(uint16_t freq, uint32_t ms)
{
  if (BUZZER_PIN >= 0)
    tone(BUZZER_PIN, freq, ms);
}

void setBuzz(uint16_t freq)
{
  if (BUZZER_PIN < 0 || freq == buzzFreq)
    return;
  buzzFreq = freq;
  if (freq)
    tone(BUZZER_PIN, freq);
  else
    noTone(BUZZER_PIN);
}

// 鳴らす音を毎回決め直す。切り替えの合図 > (ブザー ON のとき) 呼び出し音 > アラート > 終了アラーム の順に優先する
void updateAlarm()
{
  uint32_t now = millis();
  uint16_t f = 0;

  // 終了アラームは CALL・アラートの表示中は待たせ、閉じた後に最初から鳴らす
  bool overlay = callActive || alertActive;
  if (state != DONE)
    alarmPending = alarmOn = false;
  else if (overlay && alarmOn && now - alarmAt < 30000)
  {
    alarmOn = false;
    alarmPending = true;
  }
  else if (!overlay && alarmPending)
  {
    alarmPending = false;
    alarmOn = true;
    alarmAt = now;
  }

  if (fbKind)
  {
    uint32_t t = now - fbAt;
    if (fbKind == 1) // ON: 高音 2 回
      f = (t < 70 || (t >= 130 && t < 200)) ? 3600 : 0;
    else // OFF: 低音 1 回
      f = t < 250 ? 1000 : 0;
    if (t >= 250)
      fbKind = 0;
  }
  else if (!buzzerEnabled)
    f = 0; // OFF: 切り替えの合図のほかは鳴らさない
  else if (callActive)
  {
    // 呼び出し: 「ピン (高) ポーン (低)」を 2.5 秒ごとに繰り返す (終了アラームの 2700Hz ピピピッと聞き分けられるように)
    uint32_t ph = (now - callAt) % 2500;
    if (now - callAt < CALL_RING_MS)
      f = ph < 350 ? 1760 : ph < 1100 ? 1397 : 0;
  }
  else if (alertActive && alertCur.shownAt)
  {
    uint32_t t = now - alertCur.shownAt;
    if (alertCur.quiet)
      f = t < 200 ? 880 : 0; // 軽い音 1 回
    else if (t < ALERT_RING_MS)
    {
      // 警告: 低めの「ブー・ボー」を交互に (呼び出し音・終了アラームより低い)
      uint32_t ph = t % 800;
      f = ph < 350 ? 784 : (ph >= 400 && ph < 750) ? 587 : 0;
    }
  }
  else if (alarmOn)
  {
    // 終了後 30 秒間「ピピピッ」を 1 秒ごとに繰り返す
    uint32_t t = now - alarmAt;
    uint32_t ph = t % 1000;
    if (t < 30000 && (ph < 80 || (ph >= 150 && ph < 230) || (ph >= 300 && ph < 380)))
      f = 2700;
  }
  setBuzz(f);
}

// ---- 時計 ----
bool getNow(struct tm *t)
{
  time_t now = time(nullptr);
  localtime_r(&now, t);
  return t->tm_year > (2020 - 1900); // NTP 同期前は 1970 年になっている
}

// いまの時刻を "HH:MM" で (時刻が合う前は "--:--")
void nowHHMM(char *buf)
{
  struct tm t;
  if (getNow(&t))
    sprintf(buf, "%02d:%02d", t.tm_hour, t.tm_min);
  else
    strcpy(buf, "--:--");
}

const char *WDAY[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

void drawWifiIcon(int x, int y, uint16_t on, uint16_t off)
{
  // 下端 y、左端 x の 4 本のアンテナバー
  bool ok = WiFi.status() == WL_CONNECTED;
  int bars = 0;
  if (ok)
  {
    int rssi = WiFi.RSSI();
    bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
  }
  for (int i = 0; i < 4; i++)
  {
    int h = 3 + i * 3;
    FR(x + i * 4, y - h, 3, h, i < bars ? on : off);
  }
}

// MQTT の接続状態 ("MQ") とブザーの ON/OFF (ベル)。左端 x、下端 y で幅 22 ドット
void drawNetStatus(int x, int y)
{
  // ベル: 丸い頭 + 胴 + 裾 + 舌
  int bx = x, by = y - 8;
  uint16_t bc = buzzerEnabled ? c565(C_AMBER) : COL_DIM;
  CIRC(bx + 4, by + 3, 3, bc);
  FR(bx + 1, by + 3, 7, 4, bc);
  FR(bx, by + 6, 9, 1, bc);
  FR(bx + 3, by + 8, 3, 1, bc);
  if (!buzzerEnabled)
  {
    // OFF は斜線を引く
    for (int i = 0; i < 9; i++)
      FR(bx + i, by + 8 - i, 1, 1, COL_RED);
  }
  // connected() は通信路を調べに行くので、描画では最後に分かった状態だけ見る
  text(x + 11, y - 6, 1, mqtt.state() == MQTT_CONNECTED ? c565(C_CYAN) : COL_GHOST, "MQ");
}

// ---- 画面: 共通部品 ----
bool blinkOn() { return (millis() / 500) % 2 == 0; }

RGB currentAccent()
{
  if (state == DONE)
    return C_RED;
  if (endRingView())
    return endZoneRGB(remaining() / 60000.0f);
  return phaseRGB(totalMs ? (float)remaining() / totalMs : 1.0f);
}

void drawPresetChips(RGB accent)
{
  const int pitch = SCREEN_W / PRESET_N;
  for (int i = 0; i < PRESET_N; i++)
  {
    int x = i * pitch + 2, w = pitch - 4, y = 3, h = 16;
    char buf[8];
    sprintf(buf, "%u", PRESETS[i]);
    if (i == presetIdx)
    {
      RR(x, y, w, h, 4, c565(accent));
      textC(x + w / 2, y + 5, 1, COL_BG, buf);
    }
    else
    {
      DRR(x, y, w, h, 4, COL_LINE);
      textC(x + w / 2, y + 5, 1, COL_DIM, buf);
    }
  }
}

const char *stateLabel()
{
  switch (state)
  {
  case RUNNING:
    return "RUN";
  case PAUSED:
    return "PAUSE";
  case DONE:
    return "DONE";
  default:
    return "READY";
  }
}

uint16_t stateColor(RGB accent)
{
  switch (state)
  {
  case RUNNING:
    return c565(accent);
  case PAUSED:
    return blinkOn() ? c565(C_AMBER) : COL_DIM;
  case DONE:
    return blinkOn() ? COL_RED : COL_GHOST;
  default:
    return COL_TEXT;
  }
}

// 状態ラベル。RUN のときは左に点滅する丸を付ける
void drawStateLabel(int x, int y, RGB accent)
{
  uint16_t c = stateColor(accent);
  if (state == RUNNING && (millis() / 500) % 2 == 0)
    CIRC(x + 3, y + 7, 3, c);
  text(x + 11, y, 2, c, stateLabel());
}

uint16_t timerDigitColor(RGB accent)
{
  if (state == DONE)
    return blinkOn() ? COL_RED : COL_GHOST;
  if (state == PAUSED)
    return c565(lerpRGB(accent, {120, 120, 120}, 0.5f));
  return c565(accent);
}

// ---- 画面: デジタル時計レイアウト ----
void drawDigitalLayout()
{
  RGB accent = currentAccent();
  uint32_t rem = remaining();
  float frac = totalMs ? (float)rem / totalMs : 1.0f;

  drawPresetChips(accent);

  // 残り時間 (大)
  char buf[12];
  formatRemain(buf, rem);
  SegStyle big = {30, 62, 7, 8};
  drawSegText(10, 30, buf, big, timerDigitColor(accent), COL_GHOST);

  // 右側の状態欄
  drawStateLabel(236, 32, accent);
  char sub[16];
  sprintf(sub, "%u MIN", PRESETS[presetIdx]);
  text(247, 58, 1, COL_DIM, sub);
  sprintf(sub, "%d%%", (int)ceilf(frac * 100));
  text(247, 74, 2, c565(accent), sub);

  // バー
  drawSegBar(10, 101, 300, 18, 30, frac, accent);
  HL(10, 132, 300, COL_LINE);

  // 現在時刻
  struct tm t;
  bool ok = getNow(&t);
  SegStyle mid = {17, 30, 5, 5};
  SegStyle small = {10, 17, 3, 3};
  if (ok)
  {
    sprintf(buf, "%02d:%02d", t.tm_hour, t.tm_min);
    int w = drawSegText(10, 137, buf, mid, COL_CLOCK, COL_GHOST);
    sprintf(buf, "%02d", t.tm_sec);
    drawSegText(10 + w + 8, 150, buf, small, COL_SEC, COL_GHOST);
    sprintf(buf, "%04d.%02d.%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    textR(310, 140, 1, COL_DIM, buf);
    textR(310, 152, 2, COL_TEXT, WDAY[t.tm_wday]);
  }
  else
  {
    int w = drawSegText(10, 137, "--:--", mid, COL_GHOST, COL_GHOST);
    drawSegText(10 + w + 8, 150, "--", small, COL_GHOST, COL_GHOST);
    textR(310, 140, 1, COL_DIM, netWaitLabel());
  }
  drawWifiIcon(250, 166, c565(C_CYAN), COL_GHOST);
  drawNetStatus(224, 166);
}

// ---- 画面: アナログ時計レイアウト ----
void polar(float cx, float cy, float r, float deg, float &x, float &y)
{
  float a = deg * DEG_TO_RAD;
  x = cx + r * sinf(a);
  y = cy - r * cosf(a);
}

// 中心から伸びる太さのある針 (先細り)
void drawHand(float cx, float cy, float deg, float len, float tail, float width, uint16_t c)
{
  float tx, ty, bx, by, lx, ly, rx, ry;
  polar(cx, cy, len, deg, tx, ty);
  polar(cx, cy, tail, deg + 180, bx, by);
  polar(cx, cy, width / 2, deg - 90, lx, ly);
  polar(cx, cy, width / 2, deg + 90, rx, ry);
  QUAD(tx, ty, rx, ry, bx, by, lx, ly, c);
}

// リングの 1 区画 (角度 a0〜a1 度、半径 r0〜r1) を塗る
void ringSeg(float cx, float cy, float r0, float r1, float a0, float a1, uint16_t c)
{
  float x0, y0, x1, y1, x2, y2, x3, y3;
  polar(cx, cy, r1, a0, x0, y0);
  polar(cx, cy, r1, a1, x1, y1);
  polar(cx, cy, r0, a1, x2, y2);
  polar(cx, cy, r0, a0, x3, y3);
  QUAD(x0, y0, x1, y1, x2, y2, x3, y3, c);
}

// 60 区画のリングのうち、角度 s〜e (0 <= s < 360、s <= e <= s + 360) の所だけ on で塗る (消えている区画は先に塗っておく)。
// 区画の途中で切れる所は、その区画の一部だけ塗る (弧の端がなめらかに動く)
void litArc(float cx, float cy, float r0, float r1, float s, float e, uint16_t on)
{
  for (int i = 0; i < 60; i++)
  {
    float a0 = i * 6 + 1.0f, a1 = i * 6 + 5.0f;
    for (int lap = 0; lap <= 1; lap++) // 範囲が 0° (12 時) をまたぐときは 2 周目の角度で重なる
    {
      float lo = max(a0 + lap * 360, s), hi = min(a1 + lap * 360, e);
      if (hi > lo)
        ringSeg(cx, cy, r0, r1, lo, hi, on);
    }
  }
}

// 終了時刻リング: 長針と同じく 1 周 60 分で、今の分の位置から終了時刻の分の位置までを光らせる。
// 60 分後までは外側、それより先は内側 (少し暗く)。どちらの弧も終了時刻の所で終わり、そこに印を付ける。
// 色は終了の何分前かで決まる (終了前 5 分は赤、その前 10 分は黄、それより前は緑) ので、境目は文字盤の上で止まっている。
// 待機中は「今始めたら」の予告を暗く出す。一時停止中は終了時刻が後ろへずれていくので、弧は長針と一緒に回る
void drawEndRing(float cx, float cy, float R, const struct tm &t, bool ok)
{
  bool show = ok && state != DONE;
  float remMin = remaining() / 60000.0f; // READY のときは設定した時間
  float s = (t.tm_min + t.tm_sec / 60.0f) * 6;
  // 今から u 分後の位置を、外側は u = 0〜60、内側は u = 60〜120 として塗る
  const struct
  {
    float r0, r1, u0;
  } rings[2] = {{R - 5, R, 0}, {R - 10, R - 7, 60}};
  const struct
  {
    float a, b; // 今から a〜b 分後
    RGB c;
  } zones[3] = {{0, remMin - END_YELLOW_MIN, C_GO},
                {remMin - END_YELLOW_MIN, remMin - END_RED_MIN, C_YELLOW},
                {remMin - END_RED_MIN, remMin, C_RED}};
  for (int r = 0; r < 2; r++)
  {
    const auto &g = rings[r];
    for (int i = 0; i < 60; i++)
      ringSeg(cx, cy, g.r0, g.r1, i * 6 + 1.0f, i * 6 + 5.0f, COL_GHOST);
    if (!show)
      continue;
    for (const auto &z : zones)
    {
      float lo = max(z.a, g.u0), hi = min(z.b, min(remMin, g.u0 + 60));
      if (hi <= lo)
        continue;
      RGB c = state == PAUSED  ? lerpRGB(z.c, {120, 120, 120}, 0.5f)
              : state == READY ? lerpRGB(z.c, C_GHOST_RGB, 0.55f)
                               : z.c;
      if (r == 1)
        c = lerpRGB(c, {0, 0, 0}, 0.4f);
      litArc(cx, cy, g.r0, g.r1, s + (lo - g.u0) * 6, s + (hi - g.u0) * 6, c565(c));
    }
  }

  // 終点の印: リングを横切る白い線 (一時停止中は点滅)
  if (show && (state != PAUSED || blinkOn()))
  {
    float e = s + remMin * 6;
    float x0, y0, x1, y1, dx, dy;
    polar(cx, cy, R + 1, e, x0, y0);
    polar(cx, cy, remMin > 60 ? R - 11 : R - 6, e, x1, y1);
    polar(0, 0, 1.0f, e + 90, dx, dy);
    QUAD(x0 - dx, y0 - dy, x0 + dx, y0 + dy, x1 + dx, y1 + dy, x1 - dx, y1 - dy, state == READY ? COL_DIM : COL_TEXT);
  }
}

// 左側: アナログ時計の文字盤。外周は終了時刻リングで、内側のリングのぶん目盛りと針を小さくする
void drawAnalogFace(float cx, float cy, float R, const struct tm &t, bool ok)
{
  const float Rf = R - 5;  // 目盛りを置く文字盤の半径
  const float k = Rf / 68; // 目盛りや針の長さは半径 68 のときの値を基準に縮める

  drawEndRing(cx, cy, R, t, ok);

  // 目盛り
  for (int i = 0; i < 60; i++)
  {
    float x0, y0, x1, y1;
    if (i % 5 == 0)
    {
      float a = i * 6;
      float lx, ly, rx, ry;
      polar(cx, cy, Rf - 10 * k, a, x0, y0);
      polar(cx, cy, Rf - 18 * k, a, x1, y1);
      polar(0, 0, 1.2f, a + 90, rx, ry);
      lx = -rx;
      ly = -ry;
      QUAD(x0 + rx, y0 + ry, x1 + rx, y1 + ry, x1 + lx, y1 + ly, x0 + lx, y0 + ly, i == 0 ? c565(C_AMBER) : COL_TEXT);
    }
    else
    {
      polar(cx, cy, Rf - 11 * k, i * 6, x0, y0);
      FR(lroundf(x0), lroundf(y0), 1, 1, COL_DIM);
    }
  }

  if (ok)
  {
    float sec = t.tm_sec;
    float mins = t.tm_min + sec / 60.0f;
    float hours = (t.tm_hour % 12) + mins / 60.0f;
    drawHand(cx, cy, hours * 30, 32 * k, 6 * k, 6 * k, COL_CLOCK);
    drawHand(cx, cy, mins * 6, 48 * k, 8 * k, 4 * k, COL_CLOCK);
    drawHand(cx, cy, sec * 6, 52 * k, 14 * k, 1.6f, COL_SEC);
  }
  CIRC(cx, cy, 4, COL_SEC);
  CIRC(cx, cy, 1, COL_BG);
}

// 「25 MIN」の代わりに出す終了時刻 "END HH:MM" (右端 right。待機中は今始めたときの時刻)
void drawEndLabel(int right, int y, const struct tm &t, bool ok)
{
  char buf[8] = "--:--";
  if (ok && state != DONE)
  {
    uint32_t e = (t.tm_hour * 3600UL + t.tm_min * 60 + t.tm_sec + (remaining() + 999) / 1000) % 86400;
    sprintf(buf, "%02lu:%02lu", (unsigned long)(e / 3600), (unsigned long)(e / 60 % 60));
  }
  textR(right, y, 1, COL_TEXT, buf);
  textR(right - textW(buf, 1) - 4, y, 1, COL_DIM, "END");
}

const int PANEL_X = 160; // 右側パネルの左端

// 右側下段: 日付 (アナログ時計・アナログタイマー共通)
void drawPanelDate(const struct tm &t, bool ok)
{
  const int px = PANEL_X;
  char buf[16];
  HL(px, 130, 154, COL_LINE);
  if (ok)
  {
    sprintf(buf, "%02d.%02d", t.tm_mon + 1, t.tm_mday);
    text(px, 140, 2, COL_TEXT, buf);
    text(px + 70, 140, 2, COL_SEC, WDAY[t.tm_wday]);
    sprintf(buf, "%04d", t.tm_year + 1900);
    text(px, 160, 1, COL_DIM, buf);
  }
  else
  {
    text(px, 144, 1, COL_DIM, netWaitLabel());
  }
  drawWifiIcon(298, 167, c565(C_CYAN), COL_GHOST);
  drawNetStatus(272, 167);
}

void drawAnalogLayout()
{
  RGB accent = currentAccent();
  uint32_t rem = remaining();
  float frac = totalMs ? (float)rem / totalMs : 1.0f;

  drawPresetChips(accent);

  struct tm t;
  bool ok = getNow(&t);
  drawAnalogFace(78, 97, 68, t, ok);

  // 右側上段: タイマーの状態
  const int px = PANEL_X;
  drawStateLabel(px, 30, accent);
  char buf[24];
  drawEndLabel(314, 34, t, ok);

  SegStyle med = {21, 44, 5, 6};
  SegStyle small = {10, 17, 3, 3};

  if (viewMode == VIEW_ANALOG_ALL)
  {
    // 上: 現在時刻 (時:分を大きく + 秒)
    SegStyle sec = {9, 15, 3, 2};
    if (ok)
    {
      sprintf(buf, "%02d:%02d", t.tm_hour, t.tm_min);
      int w = drawSegText(px, 48, buf, med, COL_CLOCK, COL_GHOST);
      sprintf(buf, "%02d", t.tm_sec);
      drawSegText(px + w + 6, 77, buf, sec, COL_SEC, COL_GHOST);
    }
    else
    {
      int w = drawSegText(px, 48, "--:--", med, COL_GHOST, COL_GHOST);
      drawSegText(px + w + 6, 77, "--", sec, COL_GHOST, COL_GHOST);
    }
    HL(px, 99, 154, COL_LINE);

    // 下: 残り時間 (小さめの数字) + バー、右に曜日・日付
    SegStyle rest = {15, 28, 4, 4};
    formatRemain(buf, rem);
    drawSegText(px, 106, buf, rest, timerDigitColor(accent), COL_GHOST);
    if (ok)
    {
      textR(314, 106, 2, COL_SEC, WDAY[t.tm_wday]);
      sprintf(buf, "%02d.%02d", t.tm_mon + 1, t.tm_mday);
      textR(314, 124, 1, COL_TEXT, buf);
    }
    else
      textR(314, 112, 1, COL_DIM, netWaitLabel(true));
    drawSegBar(px, 142, 154, 10, 19, frac, accent);
    drawWifiIcon(298, 167, c565(C_CYAN), COL_GHOST);
    drawNetStatus(272, 167);
    return;
  }

  if (viewMode == VIEW_ANALOG_TIMER)
  {
    // 右側の数字は残り時間
    formatRemain(buf, rem);
    drawSegText(px, 54, buf, med, timerDigitColor(accent), COL_GHOST);
  }
  else if (ok)
  {
    // 右側の数字は現在時刻
    sprintf(buf, "%02d:%02d", t.tm_hour, t.tm_min);
    int w = drawSegText(px, 54, buf, med, COL_CLOCK, COL_GHOST);
    sprintf(buf, "%02d", t.tm_sec);
    drawSegText(px + w + 7, 81, buf, small, COL_SEC, COL_GHOST);
  }
  else
  {
    int w = drawSegText(px, 54, "--:--", med, COL_GHOST, COL_GHOST);
    drawSegText(px + w + 7, 81, "--", small, COL_GHOST, COL_GHOST);
  }

  drawSegBar(px, 108, 154, 12, 19, frac, accent);
  drawPanelDate(t, ok);
}

// ---- 日の出・日の入り (NOAA の簡易式) ----
// yday は 1 月 1 日を 0 とする通し日。戻り値は日本時間の 0 時からの分
float sunMinutes(int yday, bool rise)
{
  float g = 2 * PI / 365 * (yday + ((rise ? 6 : 18) - 12) / 24.0f);
  float eqt = 229.18f * (0.000075f + 0.001868f * cosf(g) - 0.032077f * sinf(g) - 0.014615f * cosf(2 * g) - 0.040849f * sinf(2 * g));
  float decl = 0.006918f - 0.399912f * cosf(g) + 0.070257f * sinf(g) - 0.006758f * cosf(2 * g) + 0.000907f * sinf(2 * g) - 0.002697f * cosf(3 * g) + 0.00148f * sinf(3 * g);
  float lat = SITE_LAT * DEG_TO_RAD;
  float ha = acosf(cosf(90.833f * DEG_TO_RAD) / (cosf(lat) * cosf(decl)) - tanf(lat) * tanf(decl)) * RAD_TO_DEG;
  return 720 - 4 * (SITE_LON + (rise ? ha : -ha)) - eqt + 9 * 60;
}

void formatMinutes(char *buf, const char *label, float m)
{
  int mm = lroundf(m);
  sprintf(buf, "%s%02d:%02d", label, mm / 60 % 24, mm % 60);
}

// 長さを "1h23m" / "23m" の形に
void formatSpan(char *buf, int mins)
{
  if (mins >= 60)
    sprintf(buf, "%dh%02dm", mins / 60, mins % 60);
  else
    sprintf(buf, "%dm", mins);
}

// ---- ポモドーロの記録 ----
// 最後まで終わったタイマーを 1 回と数える (プリセットの長さは問わない)
uint32_t pomoDay = 0;    // 最後に終わらせた日 (日本時間の 1970-01-01 からの日数)
uint16_t pomoCount = 0;  // その日の回数
uint16_t pomoMins = 0;   // その日の合計分
uint16_t pomoStreak = 0; // その日まで何日続いているか

int32_t todayIndex()
{
  time_t now = time(nullptr);
  if (now < 1600000000)
    return -1; // 時刻がまだ合っていない
  return (now + 9 * 3600) / 86400;
}

void recordPomodoro()
{
  int32_t today = todayIndex();
  if (today < 0)
    return;
  if ((uint32_t)today != pomoDay)
  {
    pomoStreak = (pomoDay + 1 == (uint32_t)today) ? pomoStreak + 1 : 1;
    pomoCount = 0;
    pomoMins = 0;
    pomoDay = today;
  }
  pomoCount++;
  pomoMins += PRESETS[presetIdx];
  prefs.putUInt("pDay", pomoDay);
  prefs.putUShort("pCnt", pomoCount);
  prefs.putUShort("pMin", pomoMins);
  prefs.putUShort("pStrk", pomoStreak);
}

// ---- 今日の豆知識 (uselessfacts.jsph.pl から英語で 1 日 1 件) ----
char factText[400] = "";
int32_t factDay = -1;
volatile bool factFetching = false;
volatile bool factFetched = false; // 取得タスクが factNew に書き終えた
char factNew[400];
uint32_t factTriedAt = 0;
bool factTried = false;

// 表示できる ASCII に置き換えて足す (フォントは英数字だけ)
void appendCp(char *dst, size_t &n, size_t cap, uint32_t cp)
{
  const char *rep = nullptr;
  char one[2] = {0, 0};
  if (cp >= 0x20 && cp < 0x7F)
  {
    one[0] = (char)cp;
    rep = one;
  }
  else if (cp == '\n' || cp == '\t' || cp == 0xA0)
    rep = " ";
  else if (cp == 0x2018 || cp == 0x2019 || cp == 0x2032)
    rep = "'";
  else if (cp == 0x201C || cp == 0x201D)
    rep = "\"";
  else if (cp == 0x2013 || cp == 0x2014)
    rep = "-";
  else if (cp == 0x2026)
    rep = "...";
  else if (cp >= 0xC0 && cp <= 0xFF)
  {
    // アクセント付きの文字は元の英字に戻す
    static const char *LAT = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
    one[0] = LAT[cp - 0xC0];
    rep = one;
  }
  else
    rep = "?";
  for (; *rep && n + 1 < cap; rep++)
    dst[n++] = *rep;
  dst[n] = 0;
}

// JSON の "text" の値を取り出す (\uXXXX と UTF-8 の両方に対応)
bool parseFact(const String &json, char *out, size_t cap)
{
  int i = json.indexOf("\"text\":\"");
  if (i < 0)
    return false;
  i += 8;
  size_t n = 0;
  out[0] = 0;
  int len = json.length();
  while (i < len)
  {
    uint8_t c = json[i++];
    if (c == '"')
      return n > 0;
    uint32_t cp = c;
    if (c == '\\' && i < len)
    {
      char e = json[i++];
      if (e == 'u' && i + 4 <= len)
      {
        cp = strtoul(json.substring(i, i + 4).c_str(), nullptr, 16);
        i += 4;
      }
      else
        cp = (e == 'n' || e == 't' || e == 'r') ? ' ' : e;
    }
    else if (c >= 0x80)
    {
      int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
      cp = c & (0x3F >> extra);
      for (int k = 0; k < extra && i < len; k++)
        cp = (cp << 6) | (json[i++] & 0x3F);
    }
    appendCp(out, n, cap, cp);
  }
  return false;
}

// 通信は時間がかかるので、画面を止めないよう別のタスクで行う
void factTask(void *)
{
  {
    WiFiClientSecure client;
    client.setInsecure(); // 証明書は確かめない (豆知識なので問題にしない)
    HTTPClient http;
    factNew[0] = 0;
    if (http.begin(client, "https://uselessfacts.jsph.pl/api/v2/facts/today?language=en"))
    {
      http.addHeader("Accept", "application/json");
      http.setTimeout(8000);
      int code = http.GET();
      if (code == 200)
        parseFact(http.getString(), factNew, sizeof(factNew));
      else
        Serial.printf("fact: HTTP %d\n", code);
      http.end();
    }
  }
  factFetched = true;
  factFetching = false;
  vTaskDelete(nullptr);
}

void updateFact()
{
  int32_t today = todayIndex();
  if (factFetched)
  {
    factFetched = false;
    if (factNew[0])
    {
      strcpy(factText, factNew);
      factDay = today;
      Serial.printf("fact: %s\n", factText);
    }
  }
  if (factFetching || today < 0 || today == factDay || WiFi.status() != WL_CONNECTED)
    return;
  // 失敗したら 10 分おきにやり直す (日付が変わったときはすぐ取りに行く)
  if (factTried && millis() - factTriedAt < 600000 && !(factText[0] && today != factDay))
    return;
  factTried = true;
  factTriedAt = millis();
  factFetching = true;
  xTaskCreatePinnedToCore(factTask, "fact", 8192, nullptr, 1, nullptr, 0);
}

// 横に流れる文字。画面に入る文字だけ描く
void drawTicker(int y, uint8_t size, uint16_t c, const char *s, int x)
{
  int pitch = 6 * size;
  for (; *s; s++, x += pitch)
  {
    if (x >= SCREEN_W)
      break;
    if (x + pitch <= 0)
      continue;
    char one[2] = {*s, 0};
    text(x, y, size, c, one);
  }
}

// ---- 砂時計 ----
// ガラスの玉 1 つ分の半幅。u はくびれからの距離 (0 = くびれ、1 = 端の蓋)。
// 紡錘形の端から 1/3 の部分 (sin の 0〜60 度) を使い、くびれから蓋まで丸みを持って広がる
float bulbHalf(float u, float half)
{
  const float neckW = 1.0f;
  return neckW + (half - neckW) * sinf(PI / 3 * u) / sinf(PI / 3);
}

// 座標や番号から決まる疑似乱数 (毎フレーム同じ値になるので砂の模様がちらつかない)
uint32_t hash32(uint32_t a, uint32_t b)
{
  uint32_t h = a * 2654435761u ^ (b + 0x9E3779B9u) * 2246822519u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return h;
}

// 砂の粒の色。基本の色を少し暗く・明るくばらつかせる
uint16_t grainColor(RGB base, uint32_t h)
{
  switch (h % 6)
  {
  case 0:
    return c565(lerpRGB(base, {0, 0, 0}, 0.45f));
  case 1:
    return c565(lerpRGB(base, {0, 0, 0}, 0.25f));
  case 2:
    return c565(lerpRGB(base, {255, 255, 255}, 0.35f));
  default:
    return c565(base);
  }
}

// 玉 1 つを描く。dir = -1 で上の玉 (くびれから上へ)、+1 で下の玉 (くびれから下へ)。
// fillFrac は砂の量 (玉の面積に対する割合)。砂は上の玉ではくびれ側、下の玉では底側にたまる。
// 砂の表面の y を返す (砂がなければ玉の奥の端)
int drawBulb(int cx, int y0, int len, int dir, float half, float fillFrac, uint16_t glass, RGB sand)
{
  const int MAXR = 80;
  int hw[MAXR];
  float total = 0;
  len = min(len, MAXR);
  for (int i = 0; i < len; i++)
  {
    hw[i] = lroundf(bulbHalf((i + 0.5f) / len, half));
    total += 2 * hw[i] - 1;
  }

  // 砂のたまる側から行を数え、面積が足りるところまで塗る
  float want = constrain(fillFrac, 0.0f, 1.0f) * total, acc = 0;
  bool fromNeck = dir < 0;
  int surf = y0 + dir * (fromNeck ? 0 : len - 1);
  int surfI = -1;
  uint16_t base = c565(sand);
  for (int k = 0; k < len && acc + 0.5f < want; k++)
  {
    int i = fromNeck ? k : len - 1 - k;
    acc += 2 * hw[i] - 1;
    surf = y0 + dir * i;
    surfI = i;
    FR(cx - hw[i] + 1, surf, 2 * hw[i] - 1, 1, base);
    // ざらっとした粒の模様
    for (int xx = cx - hw[i] + 1; xx < cx + hw[i]; xx++)
    {
      uint32_t h = hash32(xx, surf);
      if (h % 4 == 0)
        FR(xx, surf, 1, 1, grainColor(sand, h >> 3));
    }
  }

  // 上の玉: 砂がくびれへ吸い込まれて、表面の真ん中が少しくぼむ
  if (fromNeck && surfI > 0 && state == RUNNING)
  {
    FR(cx - 1, surf, 3, 1, COL_BG);
    if (hw[surfI] > 4)
    {
      FR(cx - 3, surf, 1, 1, COL_BG);
      FR(cx + 3, surf, 1, 1, COL_BG);
    }
  }

  // 輪郭。前の行との幅の差を横線でつなぎ、急な所でも途切れないようにする
  int prev = hw[0];
  for (int i = 0; i < len; i++)
  {
    int a = min(prev, hw[i]), b = max(prev, hw[i]);
    int y = y0 + dir * i;
    FR(cx - b, y, b - a + 1, 1, glass);
    FR(cx + a, y, b - a + 1, 1, glass);
    prev = hw[i];
  }
  return surf;
}

// 落ちていく砂粒。くびれから重力で加速しながら落ち、下の山に当たると少し跳ねる
void drawFallingSand(int cx, int neck, int pile, RGB sand)
{
  const int N = 18;          // 同時に見える粒の数の目安
  const uint32_t T = 700;    // 1 粒がいちばん下まで落ちるのにかける時間 (ms)
  const float g = 2.0f * 60 / (0.7f * 0.7f); // 0.7 秒で 60 ドット落ちる重力
  uint32_t now = millis();
  for (int i = 0; i < N; i++)
  {
    uint32_t t0 = now + i * T / N + (hash32(i, 7) % (T / N));
    uint32_t cycle = t0 / T;
    float t = (t0 % T) / 1000.0f;
    uint32_t h = hash32(i, cycle);
    float y = neck + 1 + 0.5f * g * t * t;
    // 落ちるほど少し横に広がる
    int spread = (int)(h % 5) - 2;
    int x = cx + lroundf(spread * min(1.0f, (y - neck) / 40.0f));
    if (y < pile)
    {
      FR(x, lroundf(y), 1, (h >> 8) % 3 == 0 ? 2 : 1, grainColor(sand, h >> 4));
    }
    else
    {
      // 着地した直後だけ、山の表面で左右に跳ねる
      float since = t - sqrtf(2.0f * (pile - neck - 1) / g);
      if (since < 0.08f)
      {
        int dx = (h & 1 ? 1 : -1) * (1 + lroundf(since * 40));
        FR(cx + dx, pile - 1 - (since < 0.04f ? 1 : 0), 1, 1, grainColor(sand, h >> 4));
      }
    }
  }
  // くびれの所はいつも砂が通っている
  FR(cx, neck - 1, 1, 3, c565(sand));
}

// 上の砂が残り時間、下の砂が経過時間。量は面積で比例させる
void drawHourglass(int x, int y, int w, int h, float frac, RGB accent)
{
  int cx = x + w / 2;
  float half = w / 2 - 3;
  int capH = 4;
  int yt = y + capH, yb = y + h - capH; // ガラスの上端・下端
  int neck = (yt + yb) / 2;
  uint16_t glass = COL_DIM;

  RR(x, y, w, capH, 2, COL_LINE);
  RR(x, yb, w, capH, 2, COL_LINE);
  // 蓋どうしをつなぐ細い柱
  FR(x, y + capH, 1, h - 2 * capH, COL_LINE);
  FR(x + w - 1, y + capH, 1, h - 2 * capH, COL_LINE);

  drawBulb(cx, neck - 1, neck - yt, -1, half, frac, glass, accent);
  int pile = drawBulb(cx, neck + 1, yb - neck - 1, +1, half, 1 - frac, glass, accent);
  FR(cx - 1, neck, 1, 1, glass);
  FR(cx + 1, neck, 1, 1, glass);

  if (state == RUNNING && frac > 0)
    drawFallingSand(cx, neck, pile, accent);
}

// ---- 画面: 全部入りダッシュボード ----
void drawDashLayout()
{
  RGB accent = currentAccent();
  uint32_t rem = remaining();
  float frac = totalMs ? (float)rem / totalMs : 1.0f;
  char buf[32];

  drawPresetChips(accent);

  struct tm t;
  bool ok = getNow(&t);
  bool endView = viewMode == VIEW_DASH_END;
  drawAnalogFace(62, 84, 58, t, ok);

  // 中央上: 状態と、現在時刻 (VIEW_DASH) か残り時間 (VIEW_DASH_END)
  const int px = 128, pr = 278;
  drawStateLabel(px, 26, accent);
  if (endView)
  {
    drawEndLabel(pr, 30, t, ok);
    drawNetStatus(203, 37); // 「END」のぶん幅を取るので少し左へ
  }
  else
  {
    sprintf(buf, "%u MIN", PRESETS[presetIdx]);
    textR(pr, 30, 1, COL_DIM, buf);
    drawNetStatus(207, 37); // このレイアウトには WiFi アイコンがないので、状態ラベルと「MIN」の間に置く
  }
  SegStyle med = {18, 36, 5, 5};
  if (endView)
  {
    formatRemain(buf, rem);
    drawSegText(px, 46, buf, med, timerDigitColor(accent), COL_GHOST);
  }
  else if (ok)
  {
    sprintf(buf, "%02d:%02d", t.tm_hour, t.tm_min);
    drawSegText(px, 46, buf, med, COL_CLOCK, COL_GHOST);
  }
  else
    drawSegText(px, 46, "--:--", med, COL_GHOST, COL_GHOST);
  HL(px, 89, pr - px, COL_LINE);

  // 中央左: 日の入り (日没後と夜明け前は日の出) までの時間
  if (ok)
  {
    float now = t.tm_hour * 60 + t.tm_min + t.tm_sec / 60.0f;
    float rise = sunMinutes(t.tm_yday, true), set = sunMinutes(t.tm_yday, false);
    bool toSet = now >= rise && now < set;
    float target = toSet ? set : now < rise ? rise : sunMinutes(t.tm_yday + 1, true) + 1440;
    text(px, 94, 1, COL_DIM, toSet ? "TO SUNSET" : "TO SUNRISE");
    formatSpan(buf, (int)ceilf(target - now));
    text(px, 105, 2, c565(toSet ? C_AMBER : C_CYAN), buf);
    formatMinutes(buf, toSet ? "SET " : "RISE ", target);
    text(px, 123, 1, COL_DIM, buf);
    sprintf(buf, "%02d.%02d %s", t.tm_mon + 1, t.tm_mday, WDAY[t.tm_wday]);
    text(px, 135, 1, COL_TEXT, buf);
  }
  else
  {
    text(px, 94, 1, COL_DIM, "TO SUNSET");
    text(px, 105, 2, COL_GHOST, "--");
    text(px, 135, 1, COL_DIM, netWaitLabel());
  }

  // 中央右: 今日のポモドーロ数・合計時間・連続日数
  const int qx = 208;
  int32_t today = todayIndex();
  bool isToday = today >= 0 && pomoDay == (uint32_t)today;
  bool alive = today >= 0 && (isToday || pomoDay + 1 == (uint32_t)today);
  text(qx, 94, 1, COL_DIM, "POMODORO");
  CIRC(qx + 6, 112, 6, COL_RED); // トマト
  FR(qx + 4, 104, 5, 2, c565(60, 200, 90));
  sprintf(buf, "%u", isToday ? pomoCount : 0);
  text(qx + 17, 105, 2, COL_TEXT, buf);
  formatSpan(buf, isToday ? pomoMins : 0);
  text(qx, 123, 1, COL_DIM, "TIME");
  textR(pr, 123, 1, COL_TEXT, buf);
  text(qx, 135, 1, COL_DIM, "STREAK");
  sprintf(buf, "%ud", alive ? pomoStreak : 0);
  textR(pr, 135, 1, c565(C_AMBER), buf);

  drawHourglass(284, 26, 28, 118, state == DONE ? 0 : frac, accent);

  // 最下段: 今日の豆知識を流す
  HL(8, 147, SCREEN_W - 16, COL_LINE);
  const char *head = "TODAY'S FACT: ";
  const char *msg = factText[0] ? factText : (factFetching ? "LOADING..." : "WAITING FOR WIFI...");
  int lead = strlen(head) * 12;
  int span = lead + strlen(msg) * 12 + SCREEN_W;
  int x = SCREEN_W - (int)((millis() / 25) % span);
  drawTicker(151, 2, c565(C_AMBER), head, x);
  drawTicker(151, 2, factText[0] ? COL_TEXT : COL_DIM, msg, x + lead);
}

// ---- 画面: MONITOR / WIFI 共通の上部バー ----
// MQTT が長く切れたままか (このときは PC の情報を信用せず -- にする)
// MQTT を使わない設定のとき、オフラインで動いているとき (WiFi の設定モードが時間切れ) は数えない
bool brokerLost()
{
  if (!mqttEnabled() || wifiSetupOffline())
    return false;
  return mqtt.state() != MQTT_CONNECTED && millis() - mqttLostAt >= BROKER_LOST_MS;
}

// 時計が主役、タイマーは小さく。右端に MQ・ベル・WiFi
void drawTopBar()
{
  char buf[16];
  struct tm t;
  if (getNow(&t))
  {
    sprintf(buf, "%02d:%02d", t.tm_hour, t.tm_min);
    text(4, 1, 2, COL_CLOCK, buf);
  }
  else
    text(4, 1, 2, COL_GHOST, "--:--");

  const int x = 76;
  RGB accent = currentAccent();
  uint16_t c = stateColor(accent); // PAUSED・DONE は点滅
  formatRemain(buf, remaining());
  switch (state)
  {
  case READY:
    sprintf(buf, "READY %um", PRESETS[presetIdx]);
    text(x, 1, 2, COL_DIM, buf);
    break;
  case RUNNING:
  case PAUSED:
  {
    if (state == RUNNING)
      TRI(x, 2, x, 14, x + 9, 8, c);
    else
    {
      FR(x, 2, 3, 13, c);
      FR(x + 6, 2, 3, 13, c);
    }
    text(x + 14, 1, 2, c, buf);
    char sub[8];
    sprintf(sub, "(%um)", PRESETS[presetIdx]);
    text(x + 14 + textW(buf, 2) + 6, 7, 1, COL_DIM, sub);
    break;
  }
  case DONE:
    text(x, 1, 2, c, "DONE");
    break;
  }

  if (brokerLost())
    text(206, 5, 1, blinkOn() ? c565(C_AMBER) : COL_DIM, "BROKER?");
  drawNetStatus(266, 14);
  drawWifiIcon(296, 14, c565(C_CYAN), COL_GHOST);
  HL(0, 17, SCREEN_W, COL_LINE);
}

// ---- 画面: MONITOR (PC1〜PC3 の 3 段) ----
const RGB C_GREEN = {60, 200, 90};
const RGB C_BLUE = {70, 120, 255};

// 経過秒を "m:ss" / "h:mm:ss" に
void formatElapsed(char *buf, int32_t s)
{
  if (s >= 3600)
    sprintf(buf, "%ld:%02ld:%02ld", (long)(s / 3600), (long)(s / 60 % 60), (long)(s % 60));
  else
    sprintf(buf, "%ld:%02ld", (long)(s / 60), (long)(s % 60));
}

// 長い文字列を先頭から maxChars 文字までにして写す
void fitText(char *dst, size_t cap, const char *src, int maxChars)
{
  snprintf(dst, min(cap, (size_t)max(maxChars, 0) + 1), "%s", src);
}

uint16_t tempColor(int t, int warn, int hot)
{
  return t >= hot ? COL_RED : t >= warn ? c565(C_AMBER) : COL_TEXT;
}

// 実行中なら最後に受けてからの時間を足して、経過秒が進んで見えるようにする
int32_t comfyElapsed(const PcInfo &p)
{
  if (p.el < 0)
    return -1;
  bool ticking = p.comfy == CS_RUN || p.comfy == CS_STALLED;
  return p.el + (ticking ? (int32_t)((millis() - p.comfyRecv) / 1000) : 0);
}

// "CPU[バー] 55C" の 1 組。次の項目の左端を返す
int drawHwItem(int x, int y, const char *label, int pct, int temp, int warn, int hot, bool fresh)
{
  text(x, y, 1, COL_DIM, label);
  FR(x + 21, y, 30, 7, COL_GHOST);
  if (fresh && pct > 0)
    FR(x + 21, y, 30 * min(pct, 100) / 100, 7, c565(C_CYAN));
  char buf[8];
  if (fresh && temp >= 0)
  {
    sprintf(buf, "%dC", temp);
    text(x + 55, y, 1, tempColor(temp, warn, hot), buf);
  }
  else
    text(x + 55, y, 1, COL_GHOST, "--");
  return x + 55 + 18 + 10;
}

// ComfyUI の状態。y は大きい文字 (サイズ 2) の上端、その 18 ドット下に小さい行
void drawComfy(const PcInfo &p, int x, int y)
{
  const int right = SCREEN_W - 4;
  char buf[48], el[12] = "";
  int32_t e = comfyElapsed(p);
  if (e >= 0)
    formatElapsed(el, e);

  switch (p.comfy)
  {
  case CS_RUN:
  {
    text(x, y, 2, c565(C_CYAN), "RUN");
    int bx = x + 42;
    bool hasStep = p.step >= 0 && p.steps > 0;
    if (hasStep)
    {
      drawSegBar(bx, y + 3, 120, 8, 20, constrain((float)p.step / p.steps, 0.0f, 1.0f), C_CYAN);
      sprintf(buf, "%d/%d", p.step, p.steps);
      text(bx + 126, y + 3, 1, COL_TEXT, buf);
    }
    else if (el[0])
      text(bx, y, 2, COL_TEXT, el); // 進捗が取れないときは経過時間だけ大きく

    // 小さい行: ノード名 + 待ち数 + 経過。入りきらなければノード名 → 待ち数 → 経過の順に削る
    bool showQ = p.q > 0, showEl = hasStep && el[0];
    int nodeChars = 0;
    char tail[24];
    for (int pass = 0; pass < 3; pass++)
    {
      tail[0] = 0;
      if (showQ)
        sprintf(tail, "Q%d", p.q);
      if (showEl)
        sprintf(tail + strlen(tail), "%s%s", tail[0] ? "  " : "", el);
      int tailW = tail[0] ? textW(tail, 1) + 12 : 0;
      nodeChars = (right - bx - tailW) / 6;
      if (nodeChars >= 6 || (int)strlen(p.node) <= nodeChars)
        break;
      if (pass == 0 && showQ)
        showQ = false;
      else
        showEl = false;
    }
    fitText(buf, sizeof(buf), p.node, nodeChars);
    text(bx, y + 18, 1, COL_DIM, buf);
    if (tail[0])
      textR(right, y + 18, 1, COL_TEXT, tail);
    break;
  }
  case CS_IDLE:
    text(x, y, 2, COL_TEXT, "IDLE");
    if (p.q > 0)
    {
      sprintf(buf, "Q%d", p.q);
      text(x + 60, y, 2, c565(C_AMBER), buf);
    }
    break;
  case CS_OFF:
    text(x, y, 2, COL_DIM, "COMFY OFF");
    break;
  case CS_ERROR:
    text(x, y, 2, COL_RED, "ERROR");
    fitText(buf, sizeof(buf), p.node, (right - x - 66) / 6);
    text(x + 66, y + 4, 1, COL_TEXT, buf);
    fitText(buf, sizeof(buf), p.err, (right - x) / 6);
    text(x, y + 18, 1, COL_RED, buf);
    break;
  case CS_DOWN:
    text(x, y, 2, COL_RED, "COMFY DOWN");
    break;
  case CS_STALLED:
    text(x, y, 2, c565(C_AMBER), "STALLED");
    if (el[0])
      text(x + 92, y + 4, 1, c565(C_AMBER), el);
    fitText(buf, sizeof(buf), p.node, (right - x) / 6);
    text(x, y + 18, 1, COL_DIM, buf);
    break;
  default:
    text(x, y, 2, COL_GHOST, "COMFY --");
    break;
  }
}

void drawPcRow(int i, int y0)
{
  static const PcInfo empty;
  const PcInfo &p = brokerLost() ? empty : pcs[i]; // ブローカーが落ちていたら全部 --
  const int x = 58;

  text(4, y0 + 4, 2, COL_TEXT, PC_NAMES[i]);

  // 状態ランプ
  switch (p.status)
  {
  case PS_ONLINE:
    CIRC(46, y0 + 11, 4, c565(C_GREEN));
    break;
  case PS_OFFLINE:
    CIRC(46, y0 + 11, 4, COL_DIM);
    break;
  case PS_SLEEP:
    CIRC(46, y0 + 11, 4, c565(C_BLUE));
    break;
  case PS_LOST:
    CIRC(46, y0 + 11, 4, blinkOn() ? COL_RED : COL_GHOST);
    break;
  default:
    text(41, y0 + 8, 1, COL_DIM, "--");
    break;
  }

  // 落ちている・寝ている PC は状態と変化時刻だけ
  if (p.status == PS_OFFLINE || p.status == PS_SLEEP || p.status == PS_LOST)
  {
    char buf[24];
    const char *label = p.status == PS_OFFLINE ? "OFFLINE" : p.status == PS_SLEEP ? "SLEEP" : "LOST";
    uint16_t c = p.status == PS_OFFLINE ? COL_DIM : p.status == PS_SLEEP ? c565(C_BLUE) : (blinkOn() ? COL_RED : c565(lerpRGB(C_RED, {5, 7, 11}, 0.5f)));
    sprintf(buf, "%s %s", label, p.statusAt);
    text(x, y0 + 18, 2, c, buf);
    return;
  }

  // 1 行目: CPU・GPU・VRAM
  bool fresh = p.sysSeen && millis() - p.sysAt < SYS_STALE_MS;
  int hx = drawHwItem(x, y0 + 5, "CPU", p.cpu, p.cpuT, CPU_T_WARN, CPU_T_HOT, fresh);
  hx = drawHwItem(hx, y0 + 5, "GPU", p.gpu, p.gpuT, GPU_T_WARN, GPU_T_HOT, fresh);
  if (!fresh)
    text(hx, y0 + 5, 1, COL_DIM, "NO DATA");
  else
  {
    // VRAM は使用率のバーだけ (CPU・GPU と同じ形)
    text(hx, y0 + 5, 1, COL_DIM, "VRAM");
    FR(hx + 27, y0 + 5, 30, 7, COL_GHOST);
    if (p.vram > 0 && p.vramTot > 0)
      FR(hx + 27, y0 + 5, (int)lroundf(30 * min(p.vram / p.vramTot, 1.0f)), 7, c565(C_CYAN));
  }

  // 2 行目: ComfyUI
  drawComfy(p, x, y0 + 18);
}

// MONITOR 画面で SELECT 短押しすると出る、直近のアラート
void drawAlertHistory()
{
  text(4, 22, 1, COL_DIM, "ALERT HISTORY");
  if (alertHistN == 0)
  {
    textC(SCREEN_W / 2, 90, 2, COL_GHOST, "NO ALERTS");
    return;
  }
  for (int i = 0; i < alertHistN; i++)
  {
    const Alert &a = alertHist[i];
    int y = 34 + i * 27;
    text(4, y + 3, 1, COL_DIM, a.at);
    text(40, y, 2, a.quiet ? c565(C_AMBER) : COL_RED, a.title);
    char buf[48];
    fitText(buf, sizeof(buf), a.detail, (SCREEN_W - 40) / 6);
    text(40, y + 16, 1, COL_TEXT, buf);
  }
}

void drawMonitorLayout()
{
  drawTopBar();
  if (monitorHistory)
  {
    drawAlertHistory();
    return;
  }
  const int top = 20, rowH = (SCREEN_H - top) / PC_N;
  for (int i = 0; i < PC_N; i++)
  {
    int y0 = top + i * rowH;
    drawPcRow(i, y0);
    if (i < PC_N - 1)
      HL(4, y0 + rowH - 1, SCREEN_W - 8, COL_LINE);
  }
}

// ---- WiFi の混み具合 (2.4GHz のみ。ESP32 は 5GHz を受けられない) ----
const int WIFI_CH_N = 13;
const int CH_SPREAD = 2;                  // 各 AP は自分の ch と ±2ch に影響するとみなす
const int RSSI_STRONG = -50, RSSI_MID = -70; // 電波の強さの区切り (dBm)
const float W_STRONG = 1.0f, W_MID = 0.5f, W_WEAK = 0.2f; // 混雑スコアの重み
const uint32_t SCAN_EVERY_MS = 60000;     // WIFI 画面の表示中だけ、この間隔でスキャンする
const int AP_TOP_N = 5;

struct ApInfo
{
  char ssid[13]; // 先頭 12 文字 (ASCII 以外は ?)
  uint8_t ch;
  int8_t rssi;
};

float chScore[WIFI_CH_N + 1]; // [1]〜[13]
uint8_t chCount[WIFI_CH_N + 1];
ApInfo apTop[AP_TOP_N];
int apTopN = 0;
int apTotal = 0;
bool scanHave = false; // 一度でも結果がある
bool scanning = false;
uint32_t scanStartedAt = 0, lastScanAt = 0;
char scanAt[6] = "--:--";

// SSID を表示用に詰める。UTF-8 の多バイト文字は 1 文字 = 1 個の ? にする
void asciiSsid(char *dst, size_t maxChars, const char *src)
{
  size_t n = 0;
  for (const uint8_t *p = (const uint8_t *)src; *p && n < maxChars; p++)
  {
    if (*p >= 0x80 && *p < 0xC0)
      continue; // 多バイト文字の 2 バイト目以降
    dst[n++] = (*p >= 0x20 && *p < 0x7F) ? *p : '?';
  }
  dst[n] = 0;
  if (n == 0)
    strcpy(dst, "(hidden)");
}

void takeScanResult(int n)
{
  memset(chScore, 0, sizeof(chScore));
  memset(chCount, 0, sizeof(chCount));
  apTopN = 0;
  apTotal = 0;
  for (int i = 0; i < n; i++)
  {
    int ch = WiFi.channel(i);
    int rssi = WiFi.RSSI(i);
    if (ch < 1 || ch > WIFI_CH_N)
      continue;
    apTotal++;
    chCount[ch]++;
    float w = rssi >= RSSI_STRONG ? W_STRONG : rssi >= RSSI_MID ? W_MID : W_WEAK;
    for (int c = max(1, ch - CH_SPREAD); c <= min(WIFI_CH_N, ch + CH_SPREAD); c++)
      chScore[c] += w;

    // 強い順の上位 5 件 (挿入して並べる)
    int pos = apTopN;
    while (pos > 0 && apTop[pos - 1].rssi < rssi)
      pos--;
    if (pos >= AP_TOP_N)
      continue;
    for (int k = min(apTopN, AP_TOP_N - 1); k > pos; k--)
      apTop[k] = apTop[k - 1];
    asciiSsid(apTop[pos].ssid, 12, WiFi.SSID(i).c_str());
    apTop[pos].ch = ch;
    apTop[pos].rssi = rssi;
    apTopN = min(apTopN + 1, AP_TOP_N);
  }
}

// loop() から毎回呼ぶ。非同期スキャンなので画面は止まらない
void updateWifiScan()
{
  if (scanning)
  {
    int n = WiFi.scanComplete();
    if (n >= 0)
    {
      takeScanResult(n);
      WiFi.scanDelete(); // 結果を写したらすぐメモリを返す
      scanning = false;
      scanHave = true;
      lastScanAt = millis();
      nowHHMM(scanAt);
      Serial.printf("wifi: scan %d APs in %lums\n", n, (unsigned long)(millis() - scanStartedAt));
    }
    else if (n == WIFI_SCAN_FAILED || millis() - scanStartedAt > 15000)
    {
      WiFi.scanDelete();
      scanning = false;
      lastScanAt = millis() - SCAN_EVERY_MS + 10000; // 10 秒後にやり直す
      Serial.println("wifi: scan failed");
    }
    return;
  }
  // スキャン中は通信が 1〜2 秒途切れるので、WIFI 画面を見ているときだけ
  if (viewMode != VIEW_WIFI || WiFi.status() != WL_CONNECTED)
    return;
  if (lastScanAt && millis() - lastScanAt < SCAN_EVERY_MS)
    return;
  if (WiFi.scanNetworks(true /*async*/, true /*hidden*/, false, 120 /*ms/ch*/) == WIFI_SCAN_FAILED)
  {
    lastScanAt = millis() - SCAN_EVERY_MS + 10000;
    Serial.println("wifi: scan start failed");
    return;
  }
  scanning = true;
  scanStartedAt = millis();
}

// ---- 画面: WIFI ----
void drawWifiLayout()
{
  drawTopBar();
  char buf[24];
  bool conn = WiFi.status() == WL_CONNECTED;
  int myCh = conn ? WiFi.channel() : 0;

  // 左: ch ごとの混雑スコアの棒グラフ
  const int gx = 4, pitch = 13, barW = 10, base = 151, maxH = 104;
  text(gx, 22, 1, COL_DIM, "2.4GHz");
  if (scanning)
    strcpy(buf, "SCANNING");
  else
    sprintf(buf, "SCAN %s", scanAt);
  textR(gx + pitch * WIFI_CH_N - 3, 22, 1, scanning ? c565(C_AMBER) : COL_DIM, buf);
  if (scanHave)
  {
    float top = 2.0f; // スコアが小さくても棒が伸びすぎないよう、目盛りの上限は 2 以上
    for (int c = 1; c <= WIFI_CH_N; c++)
      top = max(top, chScore[c]);
    for (int c = 1; c <= WIFI_CH_N; c++)
    {
      int x = gx + (c - 1) * pitch;
      int h = lroundf(chScore[c] / top * maxH);
      bool mine = c == myCh;
      FR(x, base - maxH, barW, maxH, COL_GHOST);
      if (h > 0)
        FR(x, base - h, barW, h, mine ? c565(C_AMBER) : c565(C_CYAN));
      if (chCount[c])
      {
        // 棒の上にその ch の AP の数
        sprintf(buf, "%u", chCount[c]);
        textC(x + barW / 2, base - h - 9, 1, COL_TEXT, buf);
      }
    }
  }
  else
    text(gx + 20, 80, 1, COL_DIM, conn ? "SCANNING..." : netWaitLabel());
  for (int c = 1; c <= WIFI_CH_N; c++)
  {
    sprintf(buf, "%d", c);
    textC(gx + (c - 1) * pitch + barW / 2, base + 4, 1, c == myCh ? c565(C_AMBER) : COL_DIM, buf);
  }

  // 右: 自分の接続・一番空いている ch・強い AP
  const int px = 178, pr = SCREEN_W - 4;
  if (conn)
  {
    sprintf(buf, "CH%d", myCh);
    text(px, 22, 2, c565(C_AMBER), buf);
    sprintf(buf, "%ddBm", WiFi.RSSI());
    textR(pr, 22, 1, COL_TEXT, buf);
  }
  else
    text(px, 22, 2, COL_GHOST, "CH--");
  if (scanHave)
  {
    sprintf(buf, "%d APs", apTotal);
    textR(pr, 31, 1, COL_DIM, buf);

    // 重ならない 1 / 6 / 11 のうち、いちばん空いている ch
    int best = 1;
    for (int c : {6, 11})
      if (chScore[c] < chScore[best])
        best = c;
    text(px, 43, 1, COL_DIM, "BEST");
    sprintf(buf, "ch%d", best);
    text(px + 30, 40, 2, c565(C_GREEN), buf);

    HL(px, 59, pr - px, COL_LINE);
    for (int i = 0; i < apTopN; i++)
    {
      int y = 64 + i * 11;
      text(px, y, 1, COL_TEXT, apTop[i].ssid);
      sprintf(buf, "%u", apTop[i].ch);
      textR(px + 96, y, 1, apTop[i].ch == myCh ? c565(C_AMBER) : COL_DIM, buf);
      sprintf(buf, "%d", apTop[i].rssi);
      textR(pr, y, 1, apTop[i].rssi >= RSSI_STRONG ? COL_TEXT : COL_DIM, buf);
    }
  }
}

// ---- 画面: 呼び出し (どのレイアウトの上にも重ねる) ----
void drawCallOverlay()
{
  const int x = 60, y = 38, w = 200, h = 96, b = 3;
  bool on = blinkOn();
  RR(x, y, w, h, 8, on ? COL_RED : COL_LINE);
  RR(x + b, y + b, w - 2 * b, h - 2 * b, 6, COL_BG);
  textC(SCREEN_W / 2, y + 12, 6, on ? c565(C_AMBER) : c565(lerpRGB(C_AMBER, {5, 7, 11}, 0.7f)), "CALL");
  textC(SCREEN_W / 2, y + 66, 2, COL_TEXT, callTime);
}

// ---- 画面: アラート (CALL と同じ重ね表示。赤背景) ----
void drawAlertOverlay()
{
  const int x = 8, y = 34, w = 304, h = 102, b = 3;
  bool on = blinkOn();
  RR(x, y, w, h, 8, on ? COL_RED : c565(140, 20, 30));
  RR(x + b, y + b, w - 2 * b, h - 2 * b, 6, c565(70, 8, 14));
  textC(SCREEN_W / 2, y + 14, 3, COL_TEXT, alertCur.title);
  textC(SCREEN_W / 2, y + 50, 1, COL_TEXT, alertCur.detail);
  textC(SCREEN_W / 2, y + 70, 2, c565(255, 200, 200), alertCur.at);
  if (alertQueueN > 0)
  {
    // 後ろに待っている数
    char buf[8];
    sprintf(buf, "+%d", alertQueueN);
    textR(x + w - 8, y + 8, 1, c565(255, 200, 200), buf);
  }
}

// ---- 画面: WiFi の設定モード (設定ポータルの間は止まっているので、一度描くだけ) ----
void drawSetupLayout()
{
  char buf[48];
  textC(SCREEN_W / 2, 14, 3, c565(C_CYAN), "WIFI SETUP MODE");
  HL(16, 46, SCREEN_W - 32, COL_LINE);
  textC(SCREEN_W / 2, 60, 1, COL_DIM, "CONNECT YOUR PHONE TO");
  textC(SCREEN_W / 2, 74, 3, COL_TEXT, setupApName);
  textC(SCREEN_W / 2, 112, 2, COL_TEXT, "THEN OPEN 192.168.4.1");
  snprintf(buf, sizeof(buf), "NO SETUP IN %d MIN -> OFFLINE TIMER", WIFI_PORTAL_SEC / 60);
  textC(SCREEN_W / 2, 146, 1, COL_DIM, buf);
}

// ---- 画面全体 ----
void drawScene()
{
  if (setupApName)
  {
    drawSetupLayout();
    return;
  }
  if (viewMode == VIEW_DIGITAL)
    drawDigitalLayout();
  else if (viewMode == VIEW_DASH || viewMode == VIEW_DASH_END)
    drawDashLayout();
  else if (viewMode == VIEW_MONITOR)
    drawMonitorLayout();
  else if (viewMode == VIEW_WIFI)
    drawWifiLayout();
  else
    drawAnalogLayout();

  // 終了時は画面の縁を赤く点滅
  if (state == DONE && blinkOn())
  {
    const int b = 6; // 枠の太さ
    FR(0, 0, SCREEN_W, b, COL_RED);
    FR(0, SCREEN_H - b, SCREEN_W, b, COL_RED);
    FR(0, 0, b, SCREEN_H, COL_RED);
    FR(SCREEN_W - b, 0, b, SCREEN_H, COL_RED);
  }

  // 重ね表示は CALL > アラート (下位のものは上位が閉じるまで待つ)
  if (callActive)
    drawCallOverlay();
  else if (alertActive)
    drawAlertOverlay();
}

void renderFrame()
{
  for (int s = 0; s < SCREEN_H / STRIP_H; s++)
  {
    gOff = s * STRIP_H;
    cv->fillScreen(COL_BG);
    drawScene();
    cv->setOutputY(gOff);
    cv->flush();
  }
}

// ---- 操作 ----
void savePrefs()
{
  prefs.putUChar("preset", presetIdx);
  prefs.putUChar("view", viewMode);
}

void closeCall()
{
  callActive = false;
  setBuzz(0);
  Serial.println("call: closed");
}

// いまのアラートを閉じ、保留があれば次を出す
void closeAlert()
{
  Serial.printf("alert: closed %s\n", alertCur.title);
  setBuzz(0);
  if (alertQueueN > 0)
  {
    alertCur = alertQueue[0];
    memmove(alertQueue, alertQueue + 1, (alertQueueN - 1) * sizeof(Alert));
    alertQueueN--;
  }
  else
    alertActive = false;
}

void toggleBuzzer()
{
  buzzerEnabled = !buzzerEnabled;
  prefs.putBool("buzzer", buzzerEnabled);
  fbKind = buzzerEnabled ? 1 : 2;
  fbAt = millis();
  Serial.printf("buzzer: %s\n", buzzerEnabled ? "ON" : "OFF");
}

void handleButtons()
{
  BtnEvent s = btnSelect.update();
  BtnEvent g = btnStart.update();

  // SELECT と START の同時押し: 1 秒でブザーの ON/OFF。単独のイベントは出さない
  static uint32_t comboAt = 0;
  static bool comboFired = false;
  if (btnSelect.pressed && btnStart.pressed)
  {
    if (!btnSelect.suppress || !btnStart.suppress)
    {
      comboAt = millis();
      comboFired = false;
    }
    btnSelect.suppress = btnStart.suppress = true;
    s = g = EV_NONE;
    if (!comboFired && millis() - comboAt >= 1000)
    {
      comboFired = true;
      toggleBuzzer();
    }
  }

  // 呼び出し・アラートの画面はどちらかのボタンで閉じる (上に出ているものから 1 つずつ)。この押下は普段の操作に使わない
  if ((callActive || alertActive) && (s != EV_NONE || g != EV_NONE))
  {
    if (callActive)
      closeCall();
    else
      closeAlert();
    beep(2000, 40);
    return;
  }

  if (s == EV_SHORT && viewMode == VIEW_MONITOR)
  {
    // MONITOR 画面では SELECT 短押しでアラート履歴を出す・戻す (プリセット変更はほかの画面で)
    monitorHistory = !monitorHistory;
    beep(3200, 25);
  }
  else if (s == EV_SHORT && state != RUNNING)
  {
    presetIdx = (presetIdx + 1) % PRESET_N;
    resetTimer();
    savePrefs();
    beep(3200, 25);
  }
  else if (s == EV_LONG)
  {
    viewMode = (viewMode + 1) % VIEW_N;
    monitorHistory = false;
    savePrefs();
    beep(2400, 40);
  }

  if (g == EV_SHORT)
  {
    switch (state)
    {
    case READY:
    case PAUSED:
      endAt = millis() + remainMs;
      state = RUNNING;
      beep(3600, 40);
      break;
    case RUNNING:
      remainMs = remaining();
      state = PAUSED;
      beep(2000, 40);
      break;
    case DONE:
      resetTimer();
      beep(2000, 40);
      break;
    }
  }
  else if (g == EV_LONG)
  {
    resetTimer();
    beep(1600, 120);
  }
}

// ---- MQTT ----
// ペイロードを長さ付きで比べる (末尾に 0 が付いていないため)
bool payloadIs(const byte *p, unsigned int len, const char *s)
{
  size_t n = strlen(s);
  return len == n && strncmp((const char *)p, s, n) == 0;
}

// 購読するトピック。再接続のたびに全部購読し直す
const char *MQTT_SUBS[] = {MQTT_SUB_TOPIC, "info/pc/+/status", "info/pc/+/sys", "info/pc/+/comfy"};

int pcIndex(const char *name, size_t n)
{
  for (int i = 0; i < PC_N; i++)
    if (strlen(PC_NAMES[i]) == n && strncmp(PC_NAMES[i], name, n) == 0)
      return i;
  return -1;
}

ComfySt parseComfySt(const char *s)
{
  if (!strcmp(s, "off"))
    return CS_OFF;
  if (!strcmp(s, "idle"))
    return CS_IDLE;
  if (!strcmp(s, "run"))
    return CS_RUN;
  if (!strcmp(s, "error"))
    return CS_ERROR;
  if (!strcmp(s, "down"))
    return CS_DOWN;
  if (!strcmp(s, "stalled"))
    return CS_STALLED;
  return CS_NONE;
}

const char *pcStatusName(PcStatus s)
{
  static const char *N[] = {"none", "online", "offline", "sleep", "lost"};
  return N[s];
}

// アラートを積む (画面と音は loop() 側の updateAlarm()・drawScene() が出す)
void raiseAlert(const char *title, const char *detail, bool quiet, int pc = -1, uint8_t kind = 0)
{
  Alert a = {};
  a.pc = pc;
  a.kind = kind;
  strlcpy(a.title, title, sizeof(a.title));
  strlcpy(a.detail, detail, sizeof(a.detail));
  nowHHMM(a.at);
  a.quiet = quiet;
  Serial.printf("alert: %s / %s\n", a.title, a.detail);

  // 履歴 (新しいものが先頭)
  memmove(alertHist + 1, alertHist, (ALERT_HIST_N - 1) * sizeof(Alert));
  alertHist[0] = a;
  alertHistN = min(alertHistN + 1, ALERT_HIST_N);

  if (!alertActive)
  {
    alertCur = a;
    alertActive = true;
  }
  else
  {
    // 表示中なら後ろに並べる。いっぱいなら一番古い保留を捨てる
    if (alertQueueN == ALERT_QUEUE_N)
    {
      Serial.printf("alert: queue full, dropped %s\n", alertQueue[0].title);
      memmove(alertQueue, alertQueue + 1, (ALERT_QUEUE_N - 1) * sizeof(Alert));
      alertQueueN--;
    }
    alertQueue[alertQueueN++] = a;
  }
}

// 正常に戻ったので、その PC の該当するアラートを表示中・保留から取り除く (履歴には残す)
void retractAlerts(int pc, uint8_t kinds)
{
  int n = 0;
  for (int j = 0; j < alertQueueN; j++)
    if (!(alertQueue[j].pc == pc && (alertQueue[j].kind & kinds)))
      alertQueue[n++] = alertQueue[j];
  alertQueueN = n;
  if (alertActive && alertCur.pc == pc && (alertCur.kind & kinds))
  {
    Serial.printf("alert: recovered %s\n", alertCur.title);
    closeAlert();
  }
}

// info/pc/<PC>/<種類> を PC ごとの構造体に写す
void handlePcMessage(const char *topic, const byte *payload, unsigned int len)
{
  const char *name = topic + 8; // "info/pc/" の後ろ
  const char *slash = strchr(name, '/');
  if (!slash)
    return;
  int i = pcIndex(name, slash - name);
  if (i < 0)
  {
    Serial.printf("mqtt: unknown pc %s\n", topic);
    return;
  }
  PcInfo &pc = pcs[i];
  const char *kind = slash + 1;

  if (!strcmp(kind, "status"))
  {
    // 空 (retain の削除) はデータなしに戻す
    PcStatus s = payloadIs(payload, len, "online")    ? PS_ONLINE
                 : payloadIs(payload, len, "offline") ? PS_OFFLINE
                 : payloadIs(payload, len, "sleep")   ? PS_SLEEP
                 : payloadIs(payload, len, "lost")    ? PS_LOST
                                                      : PS_NONE;
    if (s != pc.status)
    {
      // 予期せぬダウン (online → lost) だけアラートにする。offline・sleep はしない
      if (pc.statusBase && pc.status == PS_ONLINE && s == PS_LOST && !(pc.alerted & AL_LOST))
      {
        char title[20];
        sprintf(title, "%s LOST", PC_NAMES[i]);
        raiseAlert(title, "UNEXPECTED DISCONNECT", false, i, AL_LOST);
        pc.alerted |= AL_LOST;
      }
      pc.status = s;
      nowHHMM(pc.statusAt);
      Serial.printf("pc: %s status %s%s\n", PC_NAMES[i], pcStatusName(s), pc.statusBase ? "" : " (base)");
    }
    if (s == PS_ONLINE)
    {
      pc.alerted &= ~AL_LOST; // 正常に戻ったので、次の lost はまた知らせる
      retractAlerts(i, AL_LOST);
    }
    pc.statusBase = true;
    return;
  }

  if (!strcmp(kind, "sys"))
  {
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, (const char *)payload, len);
    if (e)
    {
      Serial.printf("pc: %s sys bad json (%s)\n", PC_NAMES[i], e.c_str());
      return;
    }
    pc.cpu = doc["cpu"] | -1;
    pc.cpuT = doc["cpu_t"] | -1;
    pc.ram = doc["ram"] | -1;
    pc.gpu = doc["gpu"] | -1;
    pc.gpuT = doc["gpu_t"] | -1;
    pc.vram = doc["vram"] | -1.0f;
    pc.vramTot = doc["vram_tot"] | -1.0f;
    pc.sysSeen = true;
    pc.sysAt = millis();
    return;
  }

  if (!strcmp(kind, "comfy"))
  {
    ComfySt st = CS_NONE; // 空 (retain の削除) はデータなし
    if (len > 0)
    {
      JsonDocument doc;
      DeserializationError e = deserializeJson(doc, (const char *)payload, len);
      if (e)
      {
        Serial.printf("pc: %s comfy bad json (%s)\n", PC_NAMES[i], e.c_str());
        return;
      }
      st = parseComfySt(doc["st"] | "");
      pc.q = doc["q"] | -1;
      pc.step = doc["step"] | -1;
      pc.steps = doc["steps"] | -1;
      pc.el = doc["el"] | -1;
      strlcpy(pc.node, doc["node"] | "", sizeof(pc.node));
      strlcpy(pc.err, doc["err"] | "", sizeof(pc.err));
    }
    pc.comfyRecv = millis();
    if (st != pc.comfy)
    {
      // error・down・stalled に変わったらアラート (接続後の最初の値は基準値なので出さない)
      uint8_t kind = st == CS_ERROR ? AL_ERROR : st == CS_DOWN ? AL_DOWN : st == CS_STALLED ? AL_STALLED : 0;
      if (pc.comfyBase && kind && !(pc.alerted & kind))
      {
        char title[20], detail[48];
        if (kind == AL_ERROR)
        {
          sprintf(title, "%s COMFY ERROR", PC_NAMES[i]);
          snprintf(detail, sizeof(detail), "%s %s", pc.node, pc.err);
        }
        else if (kind == AL_DOWN)
        {
          sprintf(title, "%s COMFY DOWN", PC_NAMES[i]);
          strcpy(detail, "COMFYUI NOT RESPONDING");
        }
        else
        {
          sprintf(title, "%s STALLED", PC_NAMES[i]);
          char span[12] = "";
          if (pc.el >= 0)
            formatSpan(span, pc.el / 60);
          snprintf(detail, sizeof(detail), "%s %s", pc.node, span);
        }
        raiseAlert(title, detail, false, i, kind);
        pc.alerted |= kind;
      }
      pc.comfy = st;
      nowHHMM(pc.comfyAt);
      Serial.printf("pc: %s comfy st=%d node=%s err=%s%s\n", PC_NAMES[i], st, pc.node, pc.err, pc.comfyBase ? "" : " (base)");
    }
    if (st == CS_IDLE || st == CS_RUN)
    {
      pc.alerted &= ~AL_COMFY; // 正常に戻ったので、次のエラーなどはまた知らせる
      retractAlerts(i, AL_COMFY);
    }
    pc.comfyBase = true;
  }
}

// mqtt.loop() の中から呼ばれる。ここでは値やフラグを書き込むだけにし、描画や音は loop() 側で行う
void mqttCallback(char *topic, byte *payload, unsigned int len)
{
  if (!strncmp(topic, "info/pc/", 8))
  {
    handlePcMessage(topic, payload, len);
    return;
  }
  Serial.printf("mqtt: recv %s \"%.*s\"\n", topic, (int)min(len, 64u), (const char *)payload);
  if (strcmp(topic, MQTT_SUB_TOPIC) != 0)
    return;
  if (payloadIs(payload, len, "ON"))
    callReq = true;
  // OFF (送信機のボタンを離した) と TIMER (終了通知) は無視する。その他もログだけ
}

// loop() から毎回呼ぶ。待ちは connect() の中だけで、ブローカーが落ちていても 2 秒ほどで戻る
void mqttUpdate()
{
  static bool wasConnected = false;
  static uint32_t lastTry = 0;
  static uint32_t retryMs = 0; // 失敗が続いたら 5 秒から 30 秒まで間隔を延ばす (相手が落ちている間の引っかかりを減らす)

  if (!mqttEnabled())
    return;

  // 2 分以上つながらなければ、軽い音 1 回のアラートを 1 度だけ
  static bool brokerAlerted = false;
  if (!brokerAlerted && brokerLost())
  {
    brokerAlerted = true;
    char detail[48];
    snprintf(detail, sizeof(detail), "MQTT %s NOT REACHABLE", mqttHost);
    raiseAlert("BROKER LOST", detail, true, -1, AL_BROKER);
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    if (wasConnected)
    {
      wasConnected = false;
      mqttLostAt = millis();
      mqtt.disconnect(); // 状態を「切断」にしておく (MQ 表示と BROKER? の判定に使う)
      Serial.println("mqtt: disconnected (wifi lost)");
    }
    return;
  }

  if (!mqtt.connected())
  {
    if (wasConnected)
    {
      wasConnected = false;
      mqttLostAt = millis();
      retryMs = 0;
      Serial.printf("mqtt: disconnected (state %d)\n", mqtt.state());
    }
    if (retryMs && millis() - lastTry < retryMs)
      return;
    lastTry = millis();
    Serial.printf("mqtt: connecting to %s:%d ...\n", mqttHost, MQTT_PORT);
    // クライアント ID は機体ごとに別 (同じ ID の機体が 2 台あると Mosquitto が互いを切断し合う)
    const char *clientId = deviceName.c_str();
    if (!mqtt.connect(clientId))
    {
      retryMs = retryMs ? min(retryMs * 2, (uint32_t)30000) : 5000;
      Serial.printf("mqtt: connect failed (state %d), retry in %lus\n", mqtt.state(), (unsigned long)(retryMs / 1000));
      return;
    }
    wasConnected = true;
    retryMs = 5000;
    brokerAlerted = false;
    retractAlerts(-1, AL_BROKER); // つながり直したら BROKER LOST の画面は消す (履歴には残る)
    // retain で届く古い値を誤報にしないよう、接続のたびに基準値を取り直す
    for (PcInfo &pc : pcs)
      pc.statusBase = pc.comfyBase = false;
    Serial.printf("mqtt: connected as %s\n", clientId);
    // 再接続のたびに購読し直す
    for (const char *topic : MQTT_SUBS)
    {
      bool ok = mqtt.subscribe(topic);
      Serial.printf("mqtt: subscribe %s %s\n", topic, ok ? "ok" : "FAILED");
    }
  }

  mqtt.loop();
}

// 呼び出しを受けたときの処理 (loop() 側)
void updateCall()
{
  if (callReq)
  {
    callReq = false;
    callActive = true; // 表示中に再び来たら、表示時間と音を最初から
    callAt = millis();
    nowHHMM(callTime);
    if (state == RUNNING)
    {
      remainMs = remaining();
      state = PAUSED; // 再開は START ボタンで
    }
    Serial.printf("call: ON at %s\n", callTime);
  }
  if (callActive && millis() - callAt >= CALL_SHOW_MS)
    closeCall();

  // アラートは CALL が出ていないときに表に出る。警告音はそのときから数える
  if (alertActive && !callActive && !alertCur.shownAt)
    alertCur.shownAt = millis() | 1; // 0 は「まだ」の意味なので避ける
}

void setup()
{
  Serial.begin(115200);

  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BTN_START, INPUT_PULLUP);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // WiFi より先にキャンバスのメモリを確保する
  if (!cv->begin(40000000))
  {
    Serial.println("canvas begin failed!");
  }

  prefs.begin("wtimer", false);
  presetIdx = prefs.getUChar("preset", 3) % PRESET_N;
  // 以前の保存形式 (デジタル/アナログの 2 択) からも引き継ぐ
  viewMode = prefs.getUChar("view", prefs.getBool("analog", false) ? VIEW_ANALOG_CLOCK : VIEW_DIGITAL) % VIEW_N;
  pomoDay = prefs.getUInt("pDay", 0);
  pomoCount = prefs.getUShort("pCnt", 0);
  pomoMins = prefs.getUShort("pMin", 0);
  pomoStreak = prefs.getUShort("pStrk", 0);
  buzzerEnabled = prefs.getBool("buzzer", true);
  resetTimer();

  // MQTT のバッファはキャンバスの後に確保する
  if (!mqtt.setBufferSize(1024))
    Serial.println("mqtt: setBufferSize(1024) failed!");

  // WiFi: 保存済みの WiFi につなぐ。つながらなければ設定モード (SELECT を押しながら起動すると設定を消して設定モード)
  deviceName = makeDeviceName(DEVICE_BASE_NAME);
  wifiSetupAddParam("mqtt", "MQTT server (empty = no MQTT)", MQTT_HOST_DEFAULT, 40);
  renderFrame(); // つなぐ間はいつもの画面 (WIFI...) を出しておく
  setupWiFi(deviceName.c_str(), [](const char *ap) {
    setupApName = ap;
    renderFrame();
  }, BTN_SELECT, WIFI_PORTAL_SEC);
  setupApName = nullptr;
  Serial.printf("device: %s (%s)\n", deviceName.c_str(), deviceDisplayName().c_str());

  strlcpy(mqttHost, wifiSetupGet("mqtt").c_str(), sizeof(mqttHost));
  if (mqttEnabled())
  {
    IPAddress broker;
    if (broker.fromString(mqttHost))
      mqtt.setServer(broker, MQTT_PORT);
    else
      mqtt.setServer(mqttHost, MQTT_PORT); // 名前で書いたとき (mqttHost は消えない配列なので渡したままでよい)
    mqtt.setCallback(mqttCallback);
    mqtt.setSocketTimeout(2); // CONNACK などを待つ上限 (秒)
    mqttNet.setTimeout(1);    // TCP 接続を待つ上限 (秒)
    mqttLostAt = millis();    // BROKER LOST の 2 分は WiFi の設定が済んでから数える
  }
  else
    Serial.println("mqtt: disabled (no server set)");

  configTzTime("JST-9", "ntp.nict.jp", "time.google.com", "pool.ntp.org"); // オフラインでも、後でつながれば合う

  // 設定を消すために押した SELECT を、離したときの操作として拾わない
  if (digitalRead(BTN_SELECT) == LOW)
    btnSelect.suppress = true;

  Serial.printf("heap: free %u\n", ESP.getFreeHeap());
}

void loop()
{
  handleButtons();
  wifiKeepAlive();
  mqttUpdate();
  updateCall();

  if (state == RUNNING && remaining() == 0)
  {
    state = DONE;
    alarmPending = true; // 鳴らし始めは updateAlarm() が決める (CALL・アラートの表示中は待つ)
    recordPomodoro();
  }
  updateAlarm();
  updateFact();
  updateWifiScan();

  // メモリ不足による再起動がないか、1 時間ごとに残りを出す
  static uint32_t lastHeapLog = 0;
  if (millis() - lastHeapLog >= 3600000)
  {
    lastHeapLog = millis();
    Serial.printf("heap: free %u\n", ESP.getFreeHeap());
  }

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 50)
  {
    lastDraw = millis();
    renderFrame();
  }
}
