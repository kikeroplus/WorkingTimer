# WorkingTimer PC エージェント

PC の負荷・温度と ComfyUI の状態を MQTT で WorkingTimer (ESP32) に送る常駐プログラムです。
PC1〜PC3 の各 PC に同じものを入れ、`config.toml` の PC 名だけ変えて動かします。

- 送る内容とトピックは [../MQTT_TOPICS.md](../MQTT_TOPICS.md) のとおりです。
- アラート (画面・音) を出すかどうかは WorkingTimer が決めます。エージェントは状態をそのまま送るだけです。

## 入れ方 (1 台ごと)

1. **Python 3.11 以上** と **Git** を入れます (Python は python.org の Windows 版。インストール時に「py launcher」を入れておく)。
2. リポジトリを、OneDrive で同期されない決まった場所に clone します。3 台とも同じ場所にそろえます。

   ```powershell
   mkdir C:\Tools
   cd C:\Tools
   git clone https://github.com/kikeroplus/WorkingTimer.git
   cd WorkingTimer\pc_agent
   ```

   デスクトップやドキュメントは OneDrive で同期されていることが多く、ログの書き込み中にファイルをつかまれることがあるので避けます。
   開発している PC でも、開発用のフォルダとは別に clone したものを動かします (書きかけのコードが動かないように)。

3. ライブラリを入れます。

   ```powershell
   py -m pip install -r requirements.txt
   ```

4. `config.example.toml` を `config.toml` という名前でコピーし、`pc_name` をその PC の名前にします。

   | PC 名 | IP |
   |---|---|
   | PC1 | 192.168.11.31 |
   | PC2 | 192.168.11.30 |
   | PC3 | 192.168.11.14 |

   同じ名前を 2 台に付けないでください (同じクライアント ID になり、接続と切断を繰り返します)。

5. CPU 温度も送るなら、**LibreHardwareMonitor** を入れます (下記)。使わないなら `lhm_url = ""` にします。
6. まず画面付きで動かして確かめます。

   ```powershell
   py agent.py
   ```

   ログに「MQTT つながった」と出て、ラズパイの `mosquitto_sub -h localhost -t "info/#" -v` に
   `info/pc/PC1/status online` と、5 秒ごとの `sys` が流れれば OK です。Ctrl+C で止めます (`offline` を送ってから終わります)。

7. 自動起動を登録します (下記)。

## CPU 温度 (LibreHardwareMonitor)

Windows では CPU 温度を普通には取れないので、LibreHardwareMonitor の Web サーバーから読みます。

1. https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases から **新しい版** を入れます。
   PC1・PC2 は Core Ultra なので、古い版だと温度が出ないことがあります。
2. LibreHardwareMonitor を **管理者として** 起動し、`Options` → `Remote Web Server` → `Run` にチェックを入れます (ポートは 8085 のまま)。
3. `Options` で `Start Minimized`・`Minimize To Tray`・`Run On Windows Startup` を入れておくと、ログオン時に勝手に動きます。
4. ブラウザで http://127.0.0.1:8085/data.json が開ければ準備完了です。

温度は「CPU の Temperatures の中で名前に Package を含むもの → なければいちばん高いもの」を送ります。
読めないときは `cpu_t` が -1 になり、WorkingTimer では `--` と表示されます。

## 自動起動 (タスクスケジューラ)

ログオンしたらウィンドウなし (`pythonw`) で動くように登録します。

```powershell
powershell -ExecutionPolicy Bypass -File register_task.ps1
Start-ScheduledTask -TaskName "WorkingTimer PC Agent"   # 今すぐ動かす
```

- 「アクセスが拒否されました」と出たら、PowerShell を **管理者として** 開いてやり直します。
- 登録される内容：ログオン時に起動、バッテリーでも止めない、実行時間の制限なし、落ちたら 1 分後に 3 回まで再起動、二重起動しない。
- 解除するとき：`powershell -ExecutionPolicy Bypass -File register_task.ps1 -Unregister`
- 手で登録するときは、タスクスケジューラで「操作」を `pythonw.exe` のフルパス (例 `C:\Users\<名前>\AppData\Local\Programs\Python\Python311\pythonw.exe`)、
  引数を `agent.py`、「開始」をこのフォルダにし、「条件」タブの「AC 電源の場合のみ」を外します。

## 更新する

```powershell
cd C:\Tools\WorkingTimer
git pull
py pc_agent\agent.py --stop
Start-ScheduledTask -TaskName "WorkingTimer PC Agent"
```

- `config.toml` と `logs\` は Git に入れていないので、pull しても PC ごとの設定は残ります。
- `requirements.txt` が変わったときは `py -m pip install -r pc_agent\requirements.txt` もやります。
- タスクはフルパスで登録しているので、フォルダを動かしたら `register_task.ps1` で登録し直します。

## 止める・様子を見る

- 止める：`py agent.py --stop` (動いているエージェントに終了を伝え、`offline` を送ってから終わります)。
- ログ：`logs\agent.log` (1MB ごとに 3 世代まで残します)。
- 二重に起動しようとすると、後から起動したほうが「もう動いているので終了する」とログに書いて終わります。

## 電源イベント

見えないウィンドウを作って Windows からの知らせを受けています。

| 出来事 | 送るもの |
|---|---|
| 普通に終了・Ctrl+C・`--stop` | `offline` |
| シャットダウン・ログオフ | `offline` |
| スリープに入る | `sleep` を送って MQTT を切断 (つないだまま眠ると 45 秒後に `lost` になるため) |
| スリープから戻る | つなぎ直して `online` |
| フリーズ・強制終了・停電・LAN 断 | (送れないので) ブローカーが `lost` を出す |

シャットダウンが速すぎて `offline` が間に合わなかったときは、`lost` になります (WorkingTimer でアラートが出ます)。
Modern Standby (S0 スリープ) の PC では、スリープの知らせが来ないことがあります。

## ComfyUI の状態

2 秒ごとに `/queue` を見て、実行中のジョブが終わったら `/history` で結果を確かめます。

| 送る `st` | いつ |
|---|---|
| `off` | エージェントの起動以来、ComfyUI が一度も応答していない |
| `idle` | 応答あり、実行中のジョブなし |
| `run` | 実行中 |
| `error` | 直近のジョブがエラーで終わった (次のジョブが始まるまで続く) |
| `down` | 応答していたのに 3 回続けて (約 15 秒) 応答しない |
| `stalled` | 実行中なのに進捗が `stall_minutes` 分変わらない (進捗が取れないときは 1 ジョブが `max_job_minutes` 分を超えた) |

ユーザーが中断したジョブはエラーにせず `idle` に戻します。

進捗 (`step`/`steps`) と実行中のノード名は WebSocket から受け取ります。ComfyUI は進捗を「ジョブを投げたクライアント」にしか
送らないことがあるので、ブラウザから投げたジョブでは `step` が -1 のままになるかもしれません。その場合も状態の判定は
`/queue` と `/history` だけで動きます (stalled は `max_job_minutes` で判断)。

## 動作確認

ラズパイで `mosquitto_sub -h localhost -t "info/#" -v` を開いておき、PC_AGENT_TASK.md の「6. 動作確認」を順に試します。

## ファイル

| ファイル | 内容 |
|---|---|
| `agent.py` | 本体 |
| `config.example.toml` | 設定の見本 (`config.toml` にコピーして使う。`config.toml` は Git に入れない) |
| `requirements.txt` | 使うライブラリ |
| `register_task.ps1` | タスクスケジューラへの登録・解除 |
| `logs/` | ログ (Git に入れない) |
