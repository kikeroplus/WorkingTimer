# PC エージェント 指示書（Windows 側・Python）

PC1〜PC3 の各 PC（構成は ../MQTT_TOPICS.md の表）で常駐し、PC の負荷・温度と ComfyUI の状態を MQTT で WorkingTimer に送るプログラム。
同じプログラムを 3 台に置き、設定ファイルの PC 名だけ変えて動かす。

## 0. 前提

- トピックとペイロードは **../MQTT_TOPICS.md が唯一の正**。必ず最初に読むこと。
- アラートの判断は ESP32 側の仕事。このプログラムは **状態を正直に送るだけ**（ベルを直接鳴らさない）。
- 置き場所：このフォルダ（`WorkingTimer/pc_agent/`）。各 PC へはフォルダごとコピーして使う。
- Python 3.11 以上、Windows 11。

## 1. 構成

```
pc_agent/
  agent.py            本体
  config.example.toml 設定の見本（PC 名、ブローカー、ComfyUI の URL、各種間隔）
  requirements.txt
  README.md           インストール・自動起動・動作確認の手順（日本語）
```

`config.toml`（各 PC で見本からコピーして作る）の項目：

```toml
pc_name   = "PC1"
broker    = "192.168.11.32"
port      = 1883
comfy_url = "http://127.0.0.1:8188"
lhm_url   = "http://127.0.0.1:8085/data.json"   # LibreHardwareMonitor を使わないなら空文字
sys_interval_sec   = 5
stall_minutes      = 15     # 進捗が変わらない時間がこれを超えたら stalled
max_job_minutes    = 120    # 進捗が取れない場合、1 ジョブの実行時間がこれを超えたら stalled
```

ライブラリ：`paho-mqtt`（2.x。`CallbackAPIVersion.VERSION2` を使う）、`psutil`、`nvidia-ml-py`（pynvml）、
`websocket-client`、`requests`、`pywin32`（電源イベント用）。

## 2. MQTT

- クライアント ID：`pcagent_<pc_name>`（他の機器と重ねない。重なると接続と切断を繰り返す）。
- **接続前に** Will（LWT）を設定：`info/pc/<PC>/status`、ペイロード `lost`、QoS 1、retain=true。
- キープアライブ 30 秒。
- 接続できたら `online` を retain で送る。切断したら自動で再接続（paho の `reconnect_delay_set` など）し、再接続のたびに `online` を送り直す。
- 正常終了時（Ctrl+C、終了処理、Windows のシャットダウン・ログオフ）は `offline` を retain で送ってから切断する。
- スリープに入る直前に `sleep` を送り、復帰したら `online` を送る。
- ブローカーに届かない間もプログラムは落ちずに動き続け、ログに残すだけにする。

### Windows の電源イベント（重要）

`pythonw` で動かすとコンソールがないので、シャットダウンやスリープを Ctrl+C のような方法では受け取れない。
pywin32 で **非表示のウィンドウ** を作り、次のメッセージを受ける：

- `WM_QUERYENDSESSION` / `WM_ENDSESSION` → `offline` を送る
- `WM_POWERBROADCAST` の `PBT_APMSUSPEND` → `sleep` を送る、`PBT_APMRESUMEAUTOMATIC` → 再接続して `online`

ここがうまく動かない場合でも、送れなかった分は LWT で `lost` になるだけで致命的ではない。
まず基本機能を完成させ、電源イベントは後から足してもよい（その場合は README に未対応と書く）。

## 3. PC の負荷と温度（`info/pc/<PC>/sys`、5 秒ごと）

- CPU 使用率・メモリ使用率：psutil。
- GPU 使用率・GPU 温度・VRAM：pynvml（NVIDIA）。3 台とも NVIDIA（RTX 5060 Ti 16GB ×2、RTX 5060 8GB ×1）なので pynvml だけでよい。
- CPU 温度：Windows では psutil で取れない。`lhm_url` が設定されていれば LibreHardwareMonitor の
  Remote Web Server（`data.json`）から CPU パッケージ温度を探して使う。取れなければ -1。
  PC1・PC2 は Core Ultra なので、LibreHardwareMonitor は **新しい版** を使う（古い版だと温度が出ないことがある）。
  `data.json` 内のセンサー名は CPU の世代で違う可能性があるので、名前の決め打ちではなく
  「CPU のハードウェア配下の Temperature で、名前に Package を含むもの → なければ最大値」の順で探す。
- どれかが取れなくても他の値は送る（取れない項目は -1）。

## 4. ComfyUI の監視（`info/pc/<PC>/comfy`）

### 4.1 状態の判定

| 状態 | 判定方法 |
|---|---|
| `off` | エージェント起動以来、`GET /system_stats` が一度も成功していない |
| `down` | 一度は応答していたのに、`/system_stats` が 3 回続けて失敗（5 秒間隔） |
| `idle` | 応答あり、`GET /queue` の実行中が 0 件 |
| `run` | `/queue` の実行中が 1 件以上 |
| `error` | 直近に終わったジョブの結果がエラー。**次のジョブが始まるまで `error` を維持**する |
| `stalled` | `run` のまま、ステップ進捗が `stall_minutes` 変わらない（進捗が取れない場合は経過が `max_job_minutes` 超え） |

ユーザーの中断（interrupted）は `idle` に戻す。`down` から応答が戻ったら `idle`/`run` に戻す。

### 4.2 情報の取り方（2 つを組み合わせる）

**A. ポーリング（必須・確実な方）**
- 2 秒ごとに `GET /queue`：`queue_running` と `queue_pending` の件数。実行中ジョブの `prompt_id` と
  ノードの一覧（ノード ID → `class_type` の対応表）もここから取る。
- 実行中だったジョブが `queue_running` から消えたら、`GET /history/<prompt_id>` を読み、
  `status.status_str` が `error` なら `error` にする。`messages` の中の `execution_error` から
  `node_type`（→ `node`）と `exception_type`（→ `err`、例 `OutOfMemoryError`）を取る。

**B. WebSocket（あれば進捗が細かくなる方）**
- `ws://<host>:8188/ws?clientId=<uuid>` に接続し、`progress`（value/max → step/steps）、`executing`（実行中ノード ID）、
  `execution_error`、`execution_interrupted`、`execution_success` を受ける。
- **注意：** ComfyUI は進捗系のメッセージを「そのジョブを投げたクライアント」にだけ送ることがある。
  ブラウザから投げたジョブの進捗がこのエージェントに届かない可能性があるので、**実機で確認すること**。
  届かない場合は B を進捗の補助と割り切り、`step`/`steps` は -1、状態は A で判定する。
- WebSocket が切れても A だけで動き続けるようにし、30 秒ごとに再接続を試みる。

### 4.3 送信タイミング

- 状態（`st`）が変わったら即送信。
- `run` の間は 2 秒ごと、それ以外は 30 秒ごとに送る。
- retain=true、QoS 1。

## 5. 常駐と自動起動

- `pythonw agent.py` で、ウィンドウなしで動かす。
- ログは `pc_agent/logs/agent.log` にローテーション付きで出力（1MB×3 ファイル程度）。
- 二重起動を防ぐ（名前付きミューテックスなど）。二重起動すると同じクライアント ID で接続と切断を繰り返すため。
- README に、タスクスケジューラで「ログオン時に起動」させる手順を書く（`pythonw.exe` のフルパス、作業フォルダの指定、
  「バッテリーで止めない」設定など）。可能なら登録用の `.bat` か PowerShell スクリプトも用意する。
- ComfyUI を他の PC から見る必要はない（エージェントは同じ PC の `127.0.0.1` を見る）。

## 6. 動作確認

ラズパイで `mosquitto_sub -h localhost -t "info/#" -v` を開いておく。

1. エージェントを起動すると `info/pc/PC1/status online` が出て、`sys` が 5 秒ごとに流れる。
2. ComfyUI を起動していない状態で `comfy` が `off` になる。起動すると `idle` になる。
3. 生成を実行すると `run` になり、`q`・`el`（取れれば `step`/`steps`・`node`）が更新される。
4. わざとエラーを起こす（存在しないモデル名を指定するなど）と `error` になり、`node` と `err` が入る。次の生成で `run` に戻る。
5. 実行中に中断すると `idle` に戻る（`error` にならない）。
6. ComfyUI のプロセスを終了すると約 15 秒で `down` になる。
7. エージェントのプロセスをタスクマネージャーで強制終了すると、約 45 秒後に `status lost` がブローカーから出る。
8. エージェントを普通に終了すると `offline` が出る。シャットダウンでも `offline` が出る（電源イベント対応時）。
9. スリープで `sleep`、復帰で `online` が出る（電源イベント対応時）。
10. ブローカーを止めても落ちず、戻すと自動で再接続して `online` を送り直す。

## 7. 完了条件

- 6 の 1〜7 と 10 がすべて通る（8・9 は電源イベント対応ができた場合）。
- 1 台で確認できたら、README の手順どおりに残りの PC に入れて、3 台分のデータが流れる。
