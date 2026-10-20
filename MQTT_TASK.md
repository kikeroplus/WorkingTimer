# WorkingTimer MQTT連携 指示書

WorkingTimer（ESP32 カウントダウンタイマー）を、既存の自作「呼び出しベルシステム」に MQTT で参加させる。
WorkingTimer は **受信（呼び出されたら反応）と送信（タイマー終了を知らせる）の両方** を行う。

---

## 0. 作業の前提・禁止事項

- 対象は `src/main.cpp` と `platformio.ini`（必要なら `include/secrets.h`）のみ。
- 呼び出しベルシステム側（ESP8266 の送信機・受信機、ラズパイの Mosquitto）は **変更しない**。
- 既存の表示・ボタン・ブザーの動作を壊さないこと。描画は 50ms 周期のまま維持する。
- `include/secrets.h` の中身（Wi-Fi のパスワード）をログや出力に表示しない。
- 大きな変更の前に、変更方針を短く説明してから着手すること。

---

## 1. 既存システムの仕様（変更不可・合わせる側）

| 項目 | 値 |
|---|---|
| ブローカー | Raspberry Pi B+ 上の Mosquitto 2.0.21 |
| アドレス | `192.168.11.32` ポート `1883`（TLS なし、認証なし） |
| トピック | `doorbell/call/device2` 〜 `doorbell/call/device5` |
| 送信機 (device1) | ボタン押下で `ON`、離すと `OFF` を publish（retain なし） |
| 受信機 device2, 3 | `ON` で呼び出しパターンを最後まで鳴らす。`OFF` は無視 |
| 受信機 device4, 5 | 単純モード。`ON` でリレーON、`OFF` でリレーOFF（現在は実機なし） |
| 全受信機共通 | `TIMER` を受けるとタイマー用パターン（長め1回×3セット）で鳴る |

送信機のボタン割り当て：A=device2、B=device3、C=device4、D=device5（C・D は配線するだけで使える）。

**過去の教訓：** Mosquitto は同じクライアント ID の接続が来ると古い方を切断する。
ID が重なると「接続→切断」が無限に繰り返されるので、WorkingTimer のクライアント ID は受信機と絶対に重ならない名前にする。

---

## 2. 現在の WorkingTimer の構成（コードを読めば分かるが要点）

- ESP32 DevKit（PSRAM なし）、PlatformIO、Arduino フレームワーク
- 液晶 ST7789 320×170、`Arduino_GFX` 1.4.7
- **RAM 節約のため画面を上下 2 回に分けて描画**している（`StripCanvas`、`renderFrame()`）。全画面キャンバスは Wi-Fi と同居できない。
- 文字は自前の `text()` / `textC()` / `textR()`（5×7 フォント、ASCII のみ）を使う。
  **ライブラリの文字描画関数は使わないこと**（帯の境目でメモリを壊して再起動の原因になった。コード内コメント参照）。
- 図形も `FR()`、`RR()`、`CIRC()` など `gOff` を考慮した下請け関数を使う。
- ボタン：`BTN_SELECT`(GPIO33)、`BTN_START`(GPIO32)。短押し・長押し(800ms)を `Button::update()` で判定。
- ブザー：GPIO25。`tone()` / `noTone()`。終了アラームは `updateAlarm()`。
- 状態：`READY / RUNNING / PAUSED / DONE`。設定は `Preferences`（名前空間 `wtimer`）に保存。
- Wi-Fi は `setup()` で `WiFi.begin()`、NTP で時刻合わせ。

---

## 3. 実装すること

### 3.1 ライブラリと設定

- `platformio.ini` の `lib_deps` に `knolleary/PubSubClient@^2.8` を追加。
- `main.cpp` 冒頭に設定値をまとめる：

```cpp
#define MQTT_HOST        "192.168.11.32"
#define MQTT_PORT        1883
#define MQTT_CLIENT_ID   "workingtimer"            // 受信機の doorbell_deviceN と重ねない
#define MQTT_SUB_TOPIC   "doorbell/call/device3"   // 決定：受信機 device3 と同じ（送信機のボタン B で両方が反応）
#define MQTT_DONE_TOPIC  "doorbell/call/device3"   // 決定：タイマー終了時は受信機 device3 を鳴らす
#define MQTT_DONE_PAYLOAD "TIMER"
```

  購読と送信が同じトピックになるが、自分の送った `TIMER` は無視するのでループしない。
- `mqtt.setBufferSize(1024)` を `connect()` より前に呼び、戻り値が false なら Serial に警告を出す。
- `setSocketTimeout()` や `WiFiClient::setTimeout()` を使い、ブローカーが落ちているときに `connect()` が長く固まらないようにする（目安 2 秒以内）。

### 3.2 接続管理（非ブロッキング必須）

- 呼び鈴の受信機コードにある `while (!connected) { ... delay(5000); }` 方式は **使わない**（画面とボタンが止まる）。
- `loop()` から毎回 `mqttUpdate()` を呼び、その中で
  - Wi-Fi 未接続なら何もしない
  - MQTT 未接続なら、前回の試行から 5 秒以上たっているときだけ `connect()` を 1 回試す
  - 接続できたら `MQTT_SUB_TOPIC` を subscribe（再接続のたびに必ず subscribe し直す）
  - 接続中は `mqtt.loop()` を呼ぶ
- 接続・切断・購読・受信・送信は Serial にログを出す。

### 3.3 受信：呼び出されたら反応する

`MQTT_SUB_TOPIC` で受け取ったメッセージの扱い：

| ペイロード | 動作 |
|---|---|
| `ON` | 呼び出し画面を表示（下記） |
| `OFF` | 無視（送信機がボタンを離したときに送ってくる） |
| `TIMER` | 無視（自分が送ったものを受けた場合のループ防止） |
| その他 | 無視して Serial にログだけ出す |

呼び出し画面（CALL 表示）：
- 現在のレイアウトの上に重ねて、画面中央に大きく `CALL` を点滅表示する。色は既存の配色（アンバーか赤）に合わせる。
  7セグ風にこだわらず、`text()` を大きいサイズ（例: 5〜6）で描いてよい。下に小さく受信時刻 `HH:MM` を出すと親切。
- ブザーで呼び出し音を鳴らす。**タイマー終了アラーム（ピピピッ 2700Hz）と聞き分けられる音**にする（例：ピンポーン風に高音→低音を数回）。
  `updateAlarm()` と同様に `millis()` ベースの非ブロッキングで実装し、終了アラームと同時に鳴らない（呼び出しを優先）。
- タイマーが `RUNNING` なら自動で `PAUSED` にする（再開はユーザーが START ボタンで行う）。
- 解除：どちらかのボタンの短押しで呼び出し画面を閉じる。**この押下は通常の操作（プリセット変更・スタートなど）として扱わない。**
- 60 秒たったら自動で閉じる（ブザーは 30 秒で止めてよい）。
- 呼び出し画面の表示中に再度 `ON` が来たら、表示時間と音をリセットする。

### 3.4 送信：タイマー終了を知らせる

> **2026-10-20 に廃止。** 本体にブザーが付いたので、タイマー終了で `TIMER` は送らない。
> 同時長押し (3.5) はブザーの ON/OFF (`buzzerEnabled`、`Preferences` のキー `buzzer`) に変更した。
> OFF のときは終了アラーム・呼び出し音・アラート音を止め、ボタンのクリック音と切り替えの合図だけ鳴らす。

- 状態が `RUNNING → DONE` に変わった瞬間に 1 回だけ、`MQTT_DONE_TOPIC` へ `TIMER` を publish する。
- **retain は付けない**（付けると受信機が再起動のたびに鳴ってしまう）。
- 送信の ON/OFF 設定（`notifyEnabled`）を持ち、OFF のときは送らない。1 分タイマーのテストで 2 階のベルが鳴るのを避けるため。
- その瞬間 MQTT が切断されていたら「送信待ち」にしておき、60 秒以内に再接続できたら送る。過ぎたら捨てる。

### 3.5 通知 ON/OFF の切り替え UI

- 操作：**SELECT と START を同時に 1 秒長押し**で `notifyEnabled` を切り替える。
  同時押しのときは、それぞれのボタンの単独の短押し・長押しイベントを発火させないこと（既存の `Button` 構造体の拡張が必要）。
- 切り替えたらビープで知らせ（ON は高音 2 回、OFF は低音 1 回など）、`Preferences` にキー `notify` で保存。起動時に読み込む。デフォルトは ON。

### 3.6 状態表示

既存の Wi-Fi アイコンの近くに小さく表示を足す（すべてのレイアウトで）：
- MQTT 接続状態：例 `MQ` の文字を接続中は明るく、未接続は `COL_GHOST`。
- 通知設定：ベルの小アイコン（丸＋台形程度）、OFF のときは斜線か暗くする。
- 既存の表示と重ならない位置を各レイアウトで確認すること。

---

## 4. 実装上の注意

- RAM：キャンバス（320×85×2 バイト）を Wi-Fi より先に確保している順序は変えない。MQTT のバッファ確保はキャンバス確保の後でよい。
- `loop()` 内に `delay()` や待ちループを入れない。
- MQTT のコールバック内では状態フラグを立てるだけにし、描画やブザー処理は `loop()` 側で行う。
- `String` の多用は避け、ペイロードは長さ付きで `strncmp` などで比較する。
- 既存の関数名・配色定数・描画下請け関数の流儀に合わせる。コメントは日本語で。

---

## 5. 動作確認手順

ラズパイに SSH（`ssh pi@raspi1`）して以下を使う。

1. 監視用ターミナル：`mosquitto_sub -h localhost -t "doorbell/#" -v`
2. 接続：WorkingTimer 起動後、Serial に接続と購読のログが出て、画面の `MQ` が点灯すること。
   ブローカーのログ `sudo tail -f /var/log/mosquitto/mosquitto.log` に切断の繰り返しが出ていないこと。
3. 呼び出し：`mosquitto_pub -h localhost -t "doorbell/call/device3" -m "ON"`
   → CALL 表示・呼び出し音・RUNNING なら一時停止。ボタン短押しで閉じ、その押下でタイマーが動かないこと。
4. 無視の確認：同じトピックに `OFF`、`TIMER` を送っても何も起きないこと。
5. 終了通知：1 分プリセットで完走 → 監視ターミナルに `doorbell/call/device3 TIMER` が 1 回だけ出て、受信機がタイマー用パターンで鳴ること。
6. 通知 OFF：同時長押しで OFF → 完走しても publish されないこと。再起動後も設定が残ること。
7. 切断耐性：ラズパイで `sudo systemctl stop mosquitto` → タイマー・表示・ボタンが固まらずに動き続けること。
   `start` に戻すと自動で再接続・再購読されること。
8. 長時間：数時間動かして再起動や再接続の繰り返しがないこと。

---

## 6. 完了条件

- 上の確認手順 2〜7 がすべて通る。
- 既存の 4 つの表示モード、プリセット選択、スタート・一時停止・リセット、終了アラームがこれまで通り動く。
- ビルド警告が増えていない。

## 7. 今回やらないこと

- 天気や PC 状態などの情報表示（将来、`info/...` トピックで受ける予定。今回の構成はそれを妨げないようにだけしておく）。
- 呼び出しベルシステム側の変更。

## 8. 決定事項（実装済み）

1. `MQTT_SUB_TOPIC` = `doorbell/call/device3`：受信機 device3 と同じ部屋にあり、送信機のボタン B で両方が同時に反応する。
2. `MQTT_DONE_TOPIC` = `doorbell/call/device3`：タイマー終了時は受信機 device3 をタイマー用パターンで鳴らす。
   WorkingTimer 自身もこの `TIMER` を受け取るが、無視する。
