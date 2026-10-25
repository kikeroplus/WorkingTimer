# 指示書：WorkingTimer に Wi-Fi 設定ポータルを追加する

## 目的
現在ファームウェアに決め打ちしている Wi-Fi の SSID とパスワードを廃止し、**どの環境でもその場で Wi-Fi 設定を変えられる**ようにする。
同じファームウェアを複数台（1号機、2号機…）に書き込んで使うため、**機体ごとに名前が自動で区別される**ようにする。

- 対象：WorkingTimer（ESP32、ディスプレイ付きカウントダウンタイマー）
- 場所：`C:\Users\ilove\Desktop\ESP32\WorkingTimer`
- 方式：WiFiManager（tzapu）によるキャプティブポータル

## 作業の前に調べること（実装前に報告すること）
1. ビルド環境は Arduino IDE か PlatformIO か。
2. Wi-Fi の SSID とパスワードが決め打ちされている場所（`WiFi.begin()` の呼び出し箇所、定数、`secrets.h` など）。
3. NTP 時刻取得の処理（`configTime()` など）の場所と、Wi-Fi 接続との順序。
4. ディスプレイのライブラリと、文字を表示している関数。設定モード画面を出すのに使う。
5. MQTT を使っているか。使っている場合、**ブローカーのアドレス、クライアントID、購読トピック**がどう決まっているか。
6. 既存の Web サーバーの有無と、使っているライブラリ（`WebServer.h` か `ESPAsyncWebServer` か）。
7. 起動時に押せるボタンがあるか（GPIO0 の BOOT ボタンはケースの外から押せるか）。

## 実装内容

### 1. 共通ファイル `WiFiSetup.h` を作る
InfoPanel と LuminaCanvas でも同じファイルを使うので、プロジェクト固有のコードを入れないこと。

- `String makeDeviceName(const char* base)`
  - `ESP.getEfuseMac()` の上位16ビットを4桁の16進数にして末尾に付ける（例：`WorkingTimer-3FA2`）。
- `bool setupWiFi(const char* apName, ShowSetupScreenFn showFn, int resetPin = 0, int timeoutSec = 180)`
  - 起動時に `resetPin` が LOW（押されている）なら、`wm.resetSettings()` で保存済み設定を消す。
  - `setAPCallback` で、設定モードに入ったときに `showFn(apName)` を呼ぶ。
  - `setConfigPortalTimeout(timeoutSec)` を設定する。
  - `autoConnect(apName)` の結果を返す。
- 機体の表示名（任意）
  - WiFiManager の設定ページに `WiFiManagerParameter` で「デバイス名」欄を追加する（例：「1号機」）。
  - 入力値は `Preferences`（NVS、名前空間 `devcfg`）に保存し、次回起動時に読み出す。
  - 未入力なら `makeDeviceName()` の値を使う。

### 2. WorkingTimer 側の変更
- 決め打ちの SSID とパスワードを**完全に削除**する（コメントアウトで残さない）。
- `setup()` でディスプレイの初期化の**後**に `setupWiFi()` を呼ぶ。
  - アクセスポイント名：`makeDeviceName("WorkingTimer")`
- 設定モード画面を作る。表示内容は次の通り。
  - 「Wi-Fi設定モード」
  - アクセスポイント名（例：`WorkingTimer-3FA2`）
  - 「スマホで接続 → 192.168.4.1」
- 接続できなかった場合（タイムアウト）：**時刻表示なしでカウントダウンタイマーとしては動く**ようにする。画面の隅に「オフライン」と出す。再起動ループにはしない。
- NTP の取得は接続成功後に行う（既存の処理を流用）。

### 3. MQTT（使っている場合）
- **クライアントIDは機体ごとに必ず別にする**。`makeDeviceName("WorkingTimer")` の値を使う。同じIDの機体が2台あると、Mosquitto が互いを切断し合う。
- 購読トピックは今まで通り **device3** のままにする（複数台が同じ呼び出しに反応してよい）。カウントダウン終了時に device3 の受信機を鳴らす動作も変えない。
- ブローカーのアドレスが決め打ちなら、設定ページに「MQTTサーバー」欄を追加して NVS に保存する。空欄なら MQTT は使わない。

### 4. その他
- mDNS やホスト名を設定している場合は、`makeDeviceName()` の値を使う（`WiFi.setHostname()`）。
- ESP32 は 2.4GHz の Wi-Fi にしかつながらない。設定ページの SSID 一覧に 5GHz の SSID が出ないのは正常。
- 既存の Web サーバーが `ESPAsyncWebServer` の場合、WiFiManager が使う `WebServer.h` と `HTTP_GET` などの定義が衝突することがある。その場合は include の順番を調整し、それでも駄目なら報告すること。
- Git で管理している場合、過去のコミットにパスワードが残っていないか確認して報告すること（履歴の書き換えはしない）。

## 完了条件（実機で確認するので、手順を書いて渡すこと）
1. 書き込み直後（設定なし）に、画面に設定モードの表示が出る。
2. スマホで `WorkingTimer-XXXX` につなぐと設定ページが開き、SSID とパスワードを保存できる。
3. 再起動後、自動で接続して時刻が表示される。
4. BOOT ボタンを押しながら起動すると、設定モードに戻る。
5. Wi-Fi が無い場所で起動すると、3分後にオフラインでタイマーとして動く。
6. 2台を同時に動かしても、MQTT が切断し合わない。

## 注意
- ESP32 は実機でしか確認できない。推測でビルドを繰り返さず、変更点と確認手順をまとめて提示すること。
- 既存のタイマー機能と表示のレイアウトは変えないこと。
