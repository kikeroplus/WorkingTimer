#pragma once
// WiFi 設定ポータル (WiFiManager) の共通部品。WorkingTimer・InfoPanel・LuminaCanvas で同じファイルを使う。
// プロジェクト固有のコードは入れないこと。
//
// 使い方:
//   String name = makeDeviceName("WorkingTimer");     // 例 WorkingTimer-3FA2 (AP 名・ホスト名・MQTT クライアント ID に使う)
//   wifiSetupAddParam("mqtt", "MQTT server", "192.168.11.32", 40); // 設定ページに欄を足す (setupWiFi より前に)
//   bool ok = setupWiFi(name.c_str(), showSetupScreen, BUTTON_PIN);  // つながらなければ設定モード (既定 180 秒)
//   String host = wifiSetupGet("mqtt");               // 保存された値 (未保存なら既定値)
//   loop() で wifiKeepAlive() を呼ぶ                  // 切れていたら 30 秒ごとにつなぎ直す
//
// 設定ページの値は Preferences (NVS) の名前空間 "devcfg" に保存する。WiFi の SSID とパスワードは WiFiManager (ESP-IDF) が保存する。

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <esp_wifi.h>

typedef void (*ShowSetupScreenFn)(const char *apName);

namespace wifisetup_detail
{
static const int MAX_PARAMS = 6;
struct Param
{
  const char *key, *label, *defVal;
  int maxLen;
  WiFiManagerParameter *wm;
};
static Param params[MAX_PARAMS];
static int paramN = 0;
static const char *apName = "";
static ShowSetupScreenFn showFn = nullptr;
static bool offline = false;     // 設定モードが時間切れで終わり、まだ一度もつながっていない
static uint32_t lastRetry = 0;

inline void saveParams()
{
  Preferences p;
  p.begin("devcfg", false);
  for (int i = 0; i < paramN; i++)
  {
    p.putString(params[i].key, params[i].wm->getValue());
    Serial.printf("wifisetup: saved %s = \"%s\"\n", params[i].key, params[i].wm->getValue());
  }
  p.end();
}

// SSID が保存されているか (WiFi.begin() を引数なしで呼んでよいか)
inline bool hasSavedSsid()
{
  wifi_config_t conf;
  return esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0];
}
} // namespace wifisetup_detail

// base の後ろに MAC の末尾 4 桁 (efuse MAC の上位 16 ビット) を付ける。例 "WorkingTimer-3FA2"
inline String makeDeviceName(const char *base)
{
  char buf[40];
  snprintf(buf, sizeof(buf), "%s-%04X", base, (uint16_t)(ESP.getEfuseMac() >> 32));
  return String(buf);
}

// 設定ページに文字の欄を足す。setupWiFi() より前に呼ぶ。key は NVS のキー (15 文字まで) と欄の id を兼ねる
inline void wifiSetupAddParam(const char *key, const char *label, const char *defVal, int maxLen)
{
  using namespace wifisetup_detail;
  if (paramN < MAX_PARAMS)
    params[paramN++] = {key, label, defVal, maxLen, nullptr};
}

// 保存された値。一度も保存していなければ既定値 (空欄で保存したら空文字)
inline String wifiSetupGet(const char *key, const char *defVal = "")
{
  using namespace wifisetup_detail;
  for (int i = 0; i < paramN; i++)
    if (!strcmp(params[i].key, key))
      defVal = params[i].defVal;
  Preferences p;
  p.begin("devcfg", true);
  String v = p.isKey(key) ? p.getString(key, defVal) : String(defVal);
  p.end();
  return v;
}

// 機体の表示名 (設定ページの「デバイス名」)。未入力なら makeDeviceName() の値
inline String deviceDisplayName()
{
  String v = wifiSetupGet("name");
  return v.length() ? v : String(wifisetup_detail::apName);
}

// 保存済みの WiFi につなぐ。つながらなければ AP apName を立てて設定モードにし、showFn(apName) を呼ぶ。
// 起動時に resetPin が LOW (押されている) なら保存済みの WiFi 設定を消してから始める。
// timeoutSec で設定モードを終えて false を返す (その後も wifiKeepAlive() がつなぎ直しを続ける)
inline bool setupWiFi(const char *apName, ShowSetupScreenFn showFn, int resetPin = 0, int timeoutSec = 180)
{
  using namespace wifisetup_detail;
  wifisetup_detail::apName = apName;
  wifisetup_detail::showFn = showFn;

  static WiFiManager wm;
  WiFi.mode(WIFI_STA);

  pinMode(resetPin, INPUT_PULLUP);
  delay(20);
  if (digitalRead(resetPin) == LOW)
  {
    Serial.println("wifisetup: reset button held, erasing saved WiFi settings");
    wm.resetSettings();
  }

  // デバイス名の欄は常に先頭に置く
  static WiFiManagerParameter nameParam("name", "Device name (e.g. No.1)", "", 32);
  static bool added = false;
  if (!added)
  {
    added = true;
    memmove(params + 1, params, sizeof(Param) * min(paramN, MAX_PARAMS - 1));
    params[0] = {"name", "Device name", "", 32, &nameParam};
    paramN = min(paramN + 1, MAX_PARAMS);
    nameParam.setValue(wifiSetupGet("name").c_str(), 32);
    wm.addParameter(&nameParam);
    for (int i = 1; i < paramN; i++)
    {
      params[i].wm = new WiFiManagerParameter(params[i].key, params[i].label, wifiSetupGet(params[i].key).c_str(), params[i].maxLen);
      wm.addParameter(params[i].wm);
    }
  }

  wm.setHostname(apName);
  wm.setSaveParamsCallback(saveParams);
  wm.setAPCallback([](WiFiManager *) {
    Serial.printf("wifisetup: config portal started, AP %s\n", wifisetup_detail::apName);
    if (wifisetup_detail::showFn)
      wifisetup_detail::showFn(wifisetup_detail::apName);
  });
  wm.setConnectTimeout(20);             // 保存済みの WiFi につなぐのを待つ上限 (秒)
  wm.setConfigPortalTimeout(timeoutSec); // 設定モードの上限 (秒)

  bool ok = wm.autoConnect(apName);
  if (ok)
  {
    Serial.printf("wifisetup: connected to %s, IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  }
  else
  {
    // 時間切れ: AP を止めて STA に戻し、保存済みの WiFi があれば裏でつなぎ直す
    Serial.println("wifisetup: config portal timed out, running offline");
    offline = true;
    WiFi.mode(WIFI_STA);
    if (hasSavedSsid())
      WiFi.begin();
    lastRetry = millis();
  }
  WiFi.setAutoReconnect(true);
  return ok;
}

// 設定モードが時間切れで終わり、まだつながっていないか (画面の「オフライン」表示に使う)
inline bool wifiSetupOffline() { return wifisetup_detail::offline; }

// loop() から毎回呼ぶ。切れたままなら 30 秒ごとに保存済みの WiFi へつなぎ直す
inline void wifiKeepAlive()
{
  using namespace wifisetup_detail;
  if (WiFi.status() == WL_CONNECTED)
  {
    if (offline)
      Serial.printf("wifisetup: connected to %s, IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    offline = false;
    lastRetry = millis();
    return;
  }
  if (millis() - lastRetry < 30000)
    return;
  lastRetry = millis();
  if (hasSavedSsid())
  {
    Serial.println("wifisetup: not connected, retrying");
    WiFi.begin();
  }
}
