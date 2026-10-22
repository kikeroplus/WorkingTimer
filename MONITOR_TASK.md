# WorkingTimer モニター画面・アラート 指示書（ESP32 側）

WorkingTimer に「PC と ComfyUI の監視画面」「Wi-Fi の混み具合画面」を追加し、
PC の予期せぬダウンと ComfyUI のエラー停止をアラートで知らせる。

## 0. 前提

- **MQTT_TASK.md の実装が終わっていること**（MQTT 接続管理、呼び出し CALL 表示、通知設定）。この指示書はその上に積む。
- トピックとペイロードは **MQTT_TOPICS.md が唯一の正**。食い違いがあればそちらに合わせ、勝手に仕様を変えない。
- PC 側の送信プログラムは別作業（pc_agent/PC_AGENT_TASK.md）。PC 側が未完成でも、`mosquitto_pub` で値を流せば ESP32 側を完成させられるように作る。
- MQTT_TASK.md の「0. 前提・禁止事項」と「4. 実装上の注意」はこの作業にもそのまま適用する
  （描画は自前の `text()` と `FR()` などの下請け関数を使う、ライブラリの文字描画は使わない、`delay()` 禁止、RAM に注意、など）。

---

## 1. 画面構成

既存の表示モード（長押し SELECT で切り替え）に 2 枚追加する：

```
DIGITAL → (既存) → ... → MONITOR → WIFI → DIGITAL ...
```

- 表示中の画面でもタイマー操作（START 短押しでスタート・一時停止など）は既存どおり効くこと。
- 選んだ表示モードは既存どおり Preferences に保存される。

### 1.1 共通の上部バー（MONITOR / WIFI 両方、高さ 16px 程度）

タイマーは控えめに、時計を主役にする：

```
 18:23   ▶ 12:34 (25m)             [MQ] [bell] [wifi]
```

- 左：現在時刻 `HH:MM`（文字サイズ 2）。
- 中央：タイマーの小表示。状態ごとに
  - READY：`READY 25m`（暗め）
  - RUNNING：`▶ 12:34`、色は既存の残り割合の配色（シアン → アンバー → 赤）
  - PAUSED：`|| 12:34` を点滅
  - DONE：`DONE` を赤で点滅（終了アラームは既存どおり鳴る）
- 右：MQTT_TASK.md で作った `MQ`・ベル・Wi-Fi のアイコン。

### 1.2 MONITOR 画面（PC1〜PC3 の 3 段）

上部バーの下を 3 段に分け（1 段あたり約 50px）、1 台ずつ表示する。目安：

```
PC1 ●  CPU[████░░] 55C  GPU[██████] 71C  VRAM 10.2/16
       RUN  [██████████░░░░░░] 12/30  KSampler  Q3  0:45
PC2 ●  CPU[█░░░░░] 41C  GPU[░░░░░░] 38C  VRAM  1.1/8
       IDLE
PC3 ×  LOST 18:02
```

- 1 行目：PC 名、状態ランプ、CPU 使用率の小バー＋温度、GPU 使用率の小バー＋温度、VRAM 使用量/総量。
  - 状態ランプ：online＝緑、offline＝灰、sleep＝青、lost＝赤点滅、データなし＝灰の `--`。
  - 温度は閾値で色を変える（例：CPU 80℃・GPU 80℃ 以上でアンバー、90℃ 以上で赤）。閾値は定数にまとめる。
  - `-1` の項目は `--` 表示。
- 2 行目：ComfyUI の状態。
  - `run`：進捗バー＋`step/steps`＋ノード名＋待ち数＋経過時間。`step` が -1 ならバーの代わりに経過時間だけ。
  - `idle`：`IDLE`（待ちがあれば `Q3`）。
  - `off`：`COMFY OFF`（暗め）。
  - `error`：赤で `ERROR <node> <err>`。
  - `down`：赤で `COMFY DOWN`。
  - `stalled`：アンバーで `STALLED <node> <経過>`。
- PC が `offline` / `sleep` / `lost` のときは 2 行構成をやめ、`OFFLINE 17:40` のように状態と変化時刻だけを出す。
- PC の名前一覧は定数配列 `{"PC1","PC2","PC3"}` で持つ。後で台数を変えやすくする。

文字数の目安：文字サイズ 1 で 1 文字 6px、1 行約 53 文字。入りきらなければ項目を削ってよいが、
**状態ランプ・GPU 温度・ComfyUI の状態** は必ず残す。

### 1.3 WIFI 画面（2.4GHz の混み具合）

- ESP32 は 2.4GHz のみ対応なので、5GHz は対象外（画面の隅に `2.4GHz` と明記）。
- 横軸 ch1〜13 の棒グラフ：
  - 棒の高さ＝そのチャンネルの「混雑スコア」。各 AP は自分の ch と ±2ch に影響するとみなし、
    電波の強さで重み付けして足し合わせる（例：-50dBm 以上=1.0、-70dBm 以上=0.5、それ以下=0.2。定数にする）。
  - 棒の上に AP の数を小さく表示。
  - **自分が接続している ch** は色を変えて強調。
- グラフの下（または右）に：
  - 接続中の ch・自分の RSSI（dBm）・見えている AP 総数
  - 1 / 6 / 11 の中で一番空いている ch（`BEST ch6`）
  - 電波の強い AP 上位 5 件（SSID 先頭 12 文字、ch、RSSI）。ASCII 以外の文字は `?` に置き換える。
- スキャン：
  - `WiFi.scanNetworks(true /*async*/, true /*hidden*/, false, 120 /*ms/ch*/)` の非同期スキャンを使い、
    `WiFi.scanComplete()` を毎ループ確認する。**同期スキャンで画面を止めない。**
  - WIFI 画面の表示中は 60 秒ごと、それ以外では行わない（スキャン中は通信が 1〜2 秒途切れるため）。
  - 結果を内部の配列（ch ごとのスコアと上位 5 件）に写したら、すぐ `WiFi.scanDelete()` でメモリを解放する。
  - 最終スキャン時刻を `SCAN 18:20` として表示。

---

## 2. MQTT の受信

- 購読：`info/pc/+/status`、`info/pc/+/sys`、`info/pc/+/comfy`（MQTT_TASK.md の購読に追加。再接続時も全部購読し直す）。
- JSON の解析は **ArduinoJson 7**（`bblanchon/ArduinoJson@^7`）を使う。`JsonDocument` は関数ローカルで使い捨てにし、グローバルで抱えない。
- コールバックでは PC ごとの構造体に値を書き込み、「受信時刻（`millis()`）」と「変化した時刻（NTP の `HH:MM`）」を記録するだけ。描画やブザーは `loop()` 側で行う。
- 知らない PC 名は無視して Serial にログだけ出す。

### データの鮮度

- `sys` が 20 秒以上来ていない PC は、ハード情報を `--` で暗く表示（`NO DATA`）。これ自体はアラートにしない。
- MQTT が 2 分以上切断されたままなら、全 PC を `--` にし、上部に `BROKER?` を表示。アラート（軽い音 1 回）を 1 度だけ出す。つながり直したらそのアラートの画面は自動で消す（履歴には残す。2026-10-20）。

---

## 3. アラート

### 3.1 アラートにする出来事

| 出来事 | 判定 | 表示例 |
|---|---|---|
| PC の予期せぬダウン | status が `online` → `lost` に変わった | `PC2 LOST` |
| ComfyUI のエラー停止 | comfy の st が `error` に変わった | `PC1 COMFY ERROR` ＋ `KSampler OutOfMemoryError` |
| ComfyUI のクラッシュ | st が `down` に変わった | `PC3 COMFY DOWN` |
| ComfyUI の固まり | st が `stalled` に変わった | `PC1 STALLED KSampler 15m` |
| ブローカー断 | MQTT 切断 2 分以上 | `BROKER LOST` |

**アラートにしないもの：** `offline`（正常終了）、`sleep`、ユーザー中断、`off`（ComfyUI 未起動）。

### 3.2 起動直後に誤報を出さない（重要）

status と comfy は retain なので、WorkingTimer の起動直後や MQTT 再接続直後に「古い `lost` や `error`」が届く。
PubSubClient のコールバックでは retain フラグが分からないため、次のルールで誤報を防ぐ：

- **各 PC・各トピックとも、接続後に最初に届いた値は「基準値」として記録するだけで、アラートにしない。**
  表示（`LOST` など）には反映する。
- 2 通目以降で、値が **変化した** ときだけアラートの判定をする。
- 同じ PC・同じ種類のアラートは、いったん正常（`online`、`idle`、`run`）に戻るまで繰り返し出さない。

### 3.3 アラートの出し方

- 画面：CALL 表示と同じ重ね表示の仕組みを使い、赤背景（または赤枠）で 2 行（大きく `PC2 LOST`、小さく詳細と時刻）。
- 音：**呼び出し音・タイマー終了アラームと聞き分けられる警告音**（例：低めの 2 音を交互に「ブー・ボー」）。
  30 秒で鳴り止み、画面は解除されるまで残す。
- 解除：どちらかのボタンの短押し。この押下は通常操作として扱わない（MQTT_TASK.md の CALL と同じ）。
- 優先順位：**CALL ＞ アラート ＞ タイマー終了**。表示中に上位のものが来たら差し替え、下位のものは上位が閉じた後に出す（最大 4 件まで保留）。
- 履歴：直近 5 件（時刻・PC・内容）を保持し、MONITOR 画面で SELECT を短押しすると履歴表示に切り替わる（もう一度押すと戻る）。
  ※ MONITOR 画面での SELECT 短押しがプリセット変更と衝突する場合は、既存の操作体系を見て最も自然な割り当てを提案してから実装すること。
- ~~ベル連携（任意）：定数 `ALERT_RING_BELL` が true のとき、アラート発生時に `MQTT_DONE_TOPIC` へ `TIMER` を publish して受信機を鳴らす。~~
  **2026-10-22 に廃止**（コードも削除）。本体のブザーで足りるため、ベルへは何も送らない（MQTT_TOPICS.md 4・5 章）。

---

## 4. 動作確認（PC エージェントなしでできる）

ラズパイで以下を実行して確認する。

```bash
# PC1 を正常稼働に
mosquitto_pub -h localhost -r -t info/pc/PC1/status -m online
mosquitto_pub -h localhost    -t info/pc/PC1/sys   -m '{"cpu":23,"cpu_t":55,"ram":61,"gpu":98,"gpu_t":71,"vram":10.2,"vram_tot":16.0}'
mosquitto_pub -h localhost -r -t info/pc/PC1/comfy -m '{"st":"run","q":3,"step":12,"steps":30,"node":"KSampler","el":45,"err":""}'

# エラー停止 → アラートが出ること
mosquitto_pub -h localhost -r -t info/pc/PC1/comfy -m '{"st":"error","q":0,"step":-1,"steps":-1,"node":"KSampler","el":-1,"err":"OutOfMemoryError"}'

# 予期せぬダウン → アラートが出ること
mosquitto_pub -h localhost -r -t info/pc/PC1/status -m lost
```

1. 上記で表示とアラートが仕様どおりになる。
2. **WorkingTimer を再起動しても、retain に残った `lost` / `error` でアラートが再発しない**（表示には出る）。
3. `offline` を送ってもアラートにならない。
4. `sys` を止めて 20 秒後に `NO DATA` になる。
5. `sudo systemctl stop mosquitto` で 2 分後に `BROKER LOST` が 1 回だけ出る。戻すと自動で復帰する。
6. WIFI 画面で 60 秒ごとにグラフが更新され、スキャン中も時計とタイマーが止まらない。
7. CALL・アラート・タイマー終了が重なったとき、優先順位どおりに出る。
8. 全画面でタイマー操作が今までどおりできる。

確認が終わったら、テストで残した retain を消しておく：
`mosquitto_pub -h localhost -r -t info/pc/PC1/status -n`（`comfy` も同様）

## 5. 完了条件

- 4 の確認がすべて通る。既存の表示モードとタイマー機能が壊れていない。
- 数時間動かして、メモリ不足による再起動がない（起動時と 1 時間ごとに `ESP.getFreeHeap()` を Serial に出して確認）。

## 6. 着手前にユーザーに確認すること

- MONITOR 画面の表示項目の優先度（入りきらないとき何を削るか）。
- ~~アラート時にベルも鳴らすか（`ALERT_RING_BELL`）。~~ → 鳴らさない（2026-10-22 に機能ごと廃止）。
