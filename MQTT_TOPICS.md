# MQTT トピック仕様（WorkingTimer ⇔ PC エージェント 共通の取り決め）

この文書が唯一の正。ESP32 側（MONITOR_TASK.md）と PC 側（pc_agent/PC_AGENT_TASK.md）は必ずこれに合わせる。
仕様を変えるときは、この文書を先に直してから両方のコードを直す。

- ブローカー：`192.168.11.32:1883`（Raspberry Pi B+ の Mosquitto、TLS なし、認証なし）
- 監視対象の PC：`PC1`、`PC2`、`PC3`（それぞれ ComfyUI が動く）。トピック中の `<PC>` はこの名前。

  | PC 名 | IP | CPU | GPU（すべて NVIDIA） |
  |---|---|---|---|
  | PC1 | 192.168.55.4  | Core Ultra 5 | RTX 5060 Ti 16GB |
  | PC2 | 192.168.55.2  | Core Ultra 7 | RTX 5060 Ti 16GB |
  | PC3 | 192.168.55.3  | Core i5-13400 | RTX 5060 8GB |
- ペイロードの文字は ASCII のみ（ESP32 のフォントが ASCII のみのため）。JSON は 1 メッセージ 400 バイト以内。
- 数値が取れない項目は `-1` を入れる（キーは省略しない）。

## 1. PC の生死：`info/pc/<PC>/status`

| 値 | 意味 | 送る人 |
|---|---|---|
| `online` | エージェント稼働中 | PC エージェント（接続直後） |
| `offline` | 正常終了（シャットダウン・エージェント終了） | PC エージェント（終了直前） |
| `sleep` | スリープに入る | PC エージェント（スリープ直前。取れれば） |
| `lost` | **予期せぬ切断**（フリーズ、BSOD、停電、LAN 断） | ブローカー（LWT として自動送信） |

- すべて **retain=true、QoS 1**。
- エージェントは接続時に LWT（Will）として `lost` を登録する。キープアライブは 30 秒（約 45 秒で `lost` が出る）。

## 2. PC の負荷と温度：`info/pc/<PC>/sys`

5 秒ごと、retain=false、QoS 0。

```json
{"cpu":23,"cpu_t":55,"ram":61,"gpu":98,"gpu_t":71,"vram":10.2,"vram_tot":16.0}
```

| キー | 内容 | 単位 |
|---|---|---|
| `cpu` | CPU 使用率 | % 整数 |
| `cpu_t` | CPU 温度（取れなければ -1） | ℃ 整数 |
| `ram` | メモリ使用率 | % 整数 |
| `gpu` | GPU 使用率 | % 整数 |
| `gpu_t` | GPU 温度 | ℃ 整数 |
| `vram` / `vram_tot` | VRAM 使用量 / 総量 | GB 小数 1 桁 |

## 3. ComfyUI の状態：`info/pc/<PC>/comfy`

retain=true、QoS 1。状態が変わったとき＋実行中は 2 秒ごと＋待機中は 30 秒ごと。

```json
{"st":"run","q":3,"step":12,"steps":30,"node":"KSampler","el":45,"err":""}
```

| キー | 内容 |
|---|---|
| `st` | `off` / `idle` / `run` / `error` / `down` / `stalled`（下表） |
| `q` | 待ち行列の数（実行中を含まない）。不明なら -1 |
| `step` / `steps` | 実行中ノードのステップ進捗。取れなければ -1 |
| `node` | 実行中、またはエラーになったノードの種類名（class_type）。最大 24 文字 |
| `el` | 現在のジョブの経過秒。実行中以外は -1 |
| `err` | エラー内容（例 `OutOfMemoryError`）。最大 40 文字。エラー以外は空文字 |

| `st` | 意味 | アラート対象 |
|---|---|---|
| `off` | ComfyUI が動いていない（エージェント起動以来一度も応答していない、または待機中に普通に終了した） | しない |
| `idle` | 応答あり・実行中のジョブなし | しない |
| `run` | 実行中 | しない |
| `error` | 直近のジョブがエラーで止まった（次のジョブが始まるまで維持） | **する** |
| `down` | 応答していたのに応答しなくなり、落ちたと見られる（ジョブの実行中だった・プロセスやデスクトップ版のアプリ本体が残っている） | **する** |
| `stalled` | 実行中なのに進捗が一定時間変わらない（固まり疑い） | **する** |

ユーザーによる中断（interrupted）はエラー扱いしない（`idle` に戻す）。

`idle` / `run` に戻ると、その PC の ComfyUI アラートは画面から自動で消える（履歴には残る）。
PC の `lost` アラートも同様に、`online` に戻ると消える。

## 4. 呼び出しベル（既存）

- `doorbell/call/device2`〜`device5`、ペイロード `ON` / `OFF` / `TIMER`、retain=false。
- **WorkingTimer・InfoPanel はベルに何も送らない（受け取るだけ）。** 鳴らすのは送信機だけ。
  タイマー終了の `TIMER` は 2026-10-20 に、アラート時に送る機能（`ALERT_RING_BELL`）は 2026-10-22 に廃止した（どちらの端末にもブザーが付き、2 階のベルを鳴らす必要がなくなったため）。
- WorkingTimer は `doorbell/call/device3` を購読し（受信機 device3 と同時に反応）。
  WorkingTimer のクライアント ID は `WorkingTimer-XXXX`（XXXX は MAC の末尾 4 桁。WiFi 設定ポータルの AP 名・ホスト名と同じ。受信機の `doorbell_device3` と別で、WorkingTimer を複数台動かしても重ならない）。ブローカーのアドレスは WiFi 設定ページの「MQTT server」欄で決める（初期値 `192.168.11.32`、空欄なら MQTT を使わない）。
- InfoPanel は `doorbell/call/+`（全部）を購読し、`ON` で呼び出しを表示して鳴らし、`OFF` で閉じる。`TIMER` は記録だけ。
  InfoPanel のクライアント ID は `infopanel-XXXX`（同上。複数台でも重ならない）。
- `device4`・`device5` は空き枠。

## 5. アラートの責任分担

アラート（画面・ブザー）の判断は **すべて表示端末（WorkingTimer・InfoPanel の ESP32）側で行う**。
PC が落ちたら PC 自身は何も送れないため。PC エージェントは状態を正直に送るだけで、ベルを直接鳴らさない。

- WorkingTimer と InfoPanel は**同じ条件**でアラートを出し、それぞれ自分のブザーで鳴らす（両方で鳴ってよい。2026-10-21 に決定）。
  条件：PC の `online` → `lost`、ComfyUI の `error` / `down` / `stalled` への変化。接続後の最初の値（retain）は基準値なので出さない。正常に戻ると画面から消える。
- どちらの端末も**ベルは鳴らさない**（4 章）。
