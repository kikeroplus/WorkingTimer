# WorkingTimer PC エージェント 手順書

PC の負荷・温度と ComfyUI の状態を、MQTT で WorkingTimer (ESP32) に送る常駐プログラムです。
PC1〜PC3 に同じものを入れ、`config.toml` だけ PC ごとに変えて動かします。

久しぶりに作業するときは、上から順に読めば済むように書いてあります。

- [1. 全体の形](#1-全体の形)
- [2. 新しい PC に入れる](#2-新しい-pc-に入れる) (初めての PC、入れ直し)
- [3. 普段の操作](#3-普段の操作) (止める・動かす・ログを見る)
- [4. 更新する](#4-更新する) (GitHub の変更を取り込む)
- [5. うまくいかないとき](#5-うまくいかないとき)
- [6. PC ごとのメモ](#6-pc-ごとのメモ)
- [7. 仕組みの参考](#7-仕組みの参考)

---

## 1. 全体の形

```
各 PC (PC1 / PC2 / PC3)                          ラズパイ (192.168.11.32)          WorkingTimer
┌───────────────────────────────────┐          ┌──────────────┐          ┌────────────┐
│ ComfyUI (:8000 か :8188)           │◀─見る──┐ │              │          │            │
│ LibreHardwareMonitor (:8085)       │◀─見る──┤ │  Mosquitto   │──送る──▶│ MONITOR 画面 │
│ エージェント (pythonw agent.py)     │────────┴▶│  :1883       │          │ アラート     │
└───────────────────────────────────┘   送る    └──────────────┘          └────────────┘
```

- エージェントは **自分の PC の** ComfyUI と LibreHardwareMonitor だけを見ます。
- アラートを出すかどうかは WorkingTimer が決めます。エージェントは状態をそのまま送るだけです。
- 送る内容とトピックは [../MQTT_TOPICS.md](../MQTT_TOPICS.md) が唯一の正です。

### ファイルの置き場所

どの PC も **`C:\Tools\WorkingTimer`** に GitHub から clone して、その中の `pc_agent` から動かします。

```
C:\Tools\WorkingTimer\           ← git clone したもの (3 台とも同じ場所)
  pc_agent\
    agent.py                     本体
    config.example.toml          設定の見本
    config.toml                  ★ その PC だけで作る設定 (Git に入らない)
    requirements.txt             使うライブラリ
    register_task.ps1            自動起動の登録・解除
    logs\agent.log               ★ ログ (Git に入らない)
```

- **デスクトップやドキュメントには置かない**：OneDrive で同期されていることが多く、ログを書いている最中にファイルをつかまれることがあります。
- **開発している PC でも、開発用のフォルダとは別に clone したものを動かす**：書きかけのコードが動いてしまわないように。
- **登録した後はフォルダを動かさない**：自動起動はフルパスで登録されます。動かしたら登録し直します。

---

## 2. 新しい PC に入れる

PowerShell で、上から順にやります。全部で 10〜15 分くらいです。

### 2-1. Python と Git があるか確かめる

```powershell
py --version     # Python 3.11 以上ならよい
git --version
```

ないときは入れます。
- Python：https://www.python.org/downloads/windows/ (インストール画面で「py launcher」にチェック)
- Git：https://git-scm.com/download/win

### 2-2. clone する

```powershell
mkdir C:\Tools -Force
cd C:\Tools
git clone https://github.com/kikeroplus/WorkingTimer.git
cd WorkingTimer\pc_agent
```

リポジトリが非公開なら、初めての PC ではブラウザが開いて GitHub へのログインを求められます。一度ログインすれば次からは聞かれません。

### 2-3. ライブラリを入れる

```powershell
py -m pip install -r requirements.txt
```

### 2-4. 設定ファイルを作る

```powershell
copy config.example.toml config.toml
notepad config.toml
```

書き換えるのは次の 2 か所です。

**`pc_name`** … その PC の名前。

| PC 名 | IP | CPU | GPU |
|---|---|---|---|
| PC1 | 192.168.55.4  | Core Ultra 5 | RTX 5060 Ti 16GB |
| PC2 | 192.168.55.2  | Core Ultra 7 | RTX 5060 Ti 16GB |
| PC3 | 192.168.55.3  | Core i5-13400 | RTX 5060 8GB |

同じ名前を 2 台に付けないこと (同じクライアント ID になり、接続と切断を繰り返します)。
その PC の IP は `ipconfig` の「IPv4 アドレス」で分かります。

**`comfy_url`** … 見本のまま `"auto"` でかまいません。
その PC で 8000〜8009 と 8188 を待ち受けているアドレスを調べて順に試し、ComfyUI が応答した所を使います。
ComfyUI を LAN 向け (例 `192.168.55.3:8000`) に待ち受けさせていて `127.0.0.1` では届かない場合も、そのアドレスで見つけます。

| ComfyUI の種類 | ポート |
|---|---|
| デスクトップ版 (Documents\ComfyUI に入っているもの) | 8000 (使われていると 8001、8002 … へずれる) |
| 普通の ComfyUI (portable 版、git で入れたもの) | 8188 |

ComfyUI が応答しなくなったら探し直すので、起動し直してポートがずれても追いかけます。
ポートを決めたいときは `"http://192.168.55.3:8000"` のように書きます (そのときはその URL だけを見て、ずれても追いかけません)。

`broker`・`port`・`lhm_url` などはそのままでかまいません。

### 2-5. CPU 温度のために LibreHardwareMonitor を入れる (任意)

Windows では CPU 温度を普通には取れないので、LibreHardwareMonitor の Web サーバーから読みます。
入れない場合は `config.toml` の `lhm_url = ""` にします (CPU 温度が `--` になるだけで、ほかは動きます)。

1. https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases から **新しい版** を入れます。
   PC1・PC2 は Core Ultra なので、古い版だと温度が出ないことがあります。
2. **管理者として** 起動し、`Options` → `Remote Web Server` → `Run` にチェック (ポートは 8085 のまま)。
3. `Options` で `Start Minimized`・`Minimize To Tray`・`Run On Windows Startup` にチェック (ログオン時に勝手に動く)。
4. ブラウザで http://127.0.0.1:8085/data.json が開ければ準備完了です。

### 2-6. 手で動かして確かめる

```powershell
py agent.py
```

ログが画面に出ます。次の 2 つを確かめたら **Ctrl+C** で止めます (`offline` を送ってから終わります)。

- 「MQTT つながった」と出る。
- WorkingTimer の MONITOR 画面に、その PC の状態ランプ (緑) と CPU・GPU の値が出る。
  ComfyUI を起動していれば `IDLE`、していなければ `COMFY OFF`。

### 2-7. 自動起動を登録する

ログオンしたら、ウィンドウなし (`pythonw`) で自動で動くようにします。

```powershell
powershell -ExecutionPolicy Bypass -File register_task.ps1
Start-ScheduledTask -TaskName "WorkingTimer PC Agent"     # 再起動せずに今すぐ動かす
```

- 「アクセスが拒否されました」と出たら、PowerShell を **管理者として** 開いてやり直します。
- 登録される内容：ログオン時に起動、バッテリーでも止めない、実行時間の制限なし、落ちたら 1 分後に 3 回まで起動し直す、二重起動しない。
- スタートアップフォルダは使いません (タスクのほうが、落ちたときの起動し直しなどを設定できるため)。

これで終わりです。[6. PC ごとのメモ](#6-pc-ごとのメモ) に、その PC のことを書き足しておくと次が楽です。

---

## 3. 普段の操作

`C:\Tools\WorkingTimer\pc_agent` で PowerShell を開いて使います。

| やりたいこと | コマンド |
|---|---|
| 止める | `py agent.py --stop` (`offline` を送ってから終わる) |
| 動かす | `Start-ScheduledTask -TaskName "WorkingTimer PC Agent"` |
| 画面付きで動かす (様子を見たいとき) | 止めてから `py agent.py` (Ctrl+C で終わる) |
| ログを見る | `notepad logs\agent.log` (1MB ごとに 3 世代まで残る) |
| 動いているか確かめる | `Get-ScheduledTask -TaskName "WorkingTimer PC Agent"` の State が Running |
| 自動起動をやめる | `powershell -ExecutionPolicy Bypass -File register_task.ps1 -Unregister` |

二重に起動しようとすると、後から起動したほうが「もう動いているので終了する」とログに書いて終わります (害はありません)。

---

## 4. 更新する

GitHub のプログラムが新しくなったら、各 PC で次をやります。

```powershell
cd C:\Tools\WorkingTimer
git pull
py pc_agent\agent.py --stop
Start-ScheduledTask -TaskName "WorkingTimer PC Agent"
```

- `config.toml` と `logs\` は Git に入っていないので、pull しても PC ごとの設定は残ります。
- `requirements.txt` が変わったときは `py -m pip install -r pc_agent\requirements.txt` もやります。
- `config.example.toml` に新しい項目が増えたときは、必要なら `config.toml` にも書き写します (書かなければ既定値で動きます)。
- 2026-10-17 より前に作った `config.toml` は `comfy_url = "http://127.0.0.1:8188"` になっています。デスクトップ版の ComfyUI なら `"auto"` に直します。
- 2026-10-18 から `auto` が LAN アドレスで待ち受けている ComfyUI も見つけるようになりました。`"http://192.168.11.x:8000"` のように直接書いている PC も `"auto"` に戻せます (ポートがずれても追いかけるようになります)。

---

## 5. うまくいかないとき

まず `logs\agent.log` を見ます。

| 症状 | 見るところ・直し方 |
|---|---|
| MONITOR 画面にその PC が出ない (`--` のまま) | エージェントが動いているか ([3. 普段の操作](#3-普段の操作))。ログに「MQTT つながった」があるか。`pc_name` が PC1〜PC3 のどれかになっているか (それ以外の名前は WorkingTimer が無視する) |
| ログに「MQTT につながらない」が続く | ラズパイが動いているか。`config.toml` の `broker` が `192.168.11.32` か |
| つながったり切れたりを 1 秒おきに繰り返し、WorkingTimer で何度も LOST が出る | **2 台に同じ `pc_name` を付けている** (2026-10-17 に PC1・PC2 で実際に起きた。見本の `PC1` のまま動かしていたと見られる)。同じクライアント ID を Mosquitto が取り合わせ、切られるたびに `lost` が出る。各 PC の `config.toml` を見直し、直した PC で `py agent.py --stop` → `Start-ScheduledTask` する。ログに「接続してすぐ切れるのを繰り返している」と警告が出る。ラズパイの `/var/log/mosquitto/mosquitto.log` で、同じ `pcagent_PCx` が 2 つの IP から交互に来ていれば確定 |
| 起動してすぐ終わる・ログに「pc_name が空」 | `config.toml` の `pc_name` を書いていない |
| ComfyUI を起動しているのに `COMFY OFF` | `config.toml` の `comfy_url` が古い見本の `"http://127.0.0.1:8188"` のままになっていないか (デスクトップ版は 8000〜)。`"auto"` にして動かし直す。ログに「ComfyUI を http://…:8000 で見つけた」と出れば OK。auto でも見つからないときは、ブラウザで ComfyUI を開いたときのポートを見て、8000〜8009・8188 以外なら URL を直接書く |
| CPU 温度が `--` | LibreHardwareMonitor が **管理者で** 動いているか。http://127.0.0.1:8085/data.json が開けるか |
| `step`/`steps` (進捗バー) が出ない | ブラウザから投げたジョブは、ComfyUI が進捗をエージェントに送らないことがある。状態 (RUN など) は正しく出るので問題なし |
| シャットダウンのたびに WorkingTimer で `LOST` アラート | シャットダウンが速くて `offline` が間に合っていない。害はない (次に起動すれば `online` に戻る) |
| 自動起動の登録で「アクセスが拒否されました」 | PowerShell を管理者として開いて `register_task.ps1` をやり直す |
| `py` が見つからない | Python を入れ直し、「py launcher」にチェックを入れる |

ラズパイで実際に流れているものを見るとき：

```bash
ssh pi@raspi1
mosquitto_sub -h localhost -t "info/#" -v
```

---

## 6. PC ごとのメモ

入れたら書き足していきます。

| PC | 入れた日 | ComfyUI (comfy_url) | LibreHardwareMonitor | 自動起動 | メモ |
|---|---|---|---|---|---|
| PC1 | | | | | |
| PC2 | | | | | |
| PC3 | | デスクトップ版 → `:8000` (8001〜にずれることあり) | | | 開発に使っている PC。開発フォルダ (デスクトップ) とは別に `C:\Tools` へ clone して動かす |

---

## 7. 仕組みの参考

### 送るもの

| トピック | 中身 | いつ |
|---|---|---|
| `info/pc/<PC>/status` | `online` / `offline` / `sleep` / `lost` | 状態が変わったとき (retain) |
| `info/pc/<PC>/sys` | CPU・メモリ・GPU の使用率と温度、VRAM | 5 秒ごと |
| `info/pc/<PC>/comfy` | ComfyUI の状態 (下表) | 変わったとき、実行中は 2 秒ごと、それ以外は 30 秒ごと (retain) |

### PC の状態 (status)

見えないウィンドウを作って Windows からの知らせを受けています。

| 出来事 | 送るもの |
|---|---|
| 起動してつながった | `online` |
| 普通に終了・Ctrl+C・`--stop` | `offline` |
| シャットダウン・ログオフ | `offline` |
| スリープに入る | `sleep` を送って MQTT を切断 (つないだまま眠ると 45 秒後に `lost` になるため) |
| スリープから戻る | つなぎ直して `online` |
| フリーズ・強制終了・停電・LAN 断 | (送れないので) ブローカーが `lost` を出す → WorkingTimer でアラート |

Modern Standby (S0 スリープ) の PC では、スリープの知らせが来ないことがあります。

### ComfyUI の状態 (comfy の `st`)

2 秒ごとに `/queue` を見て、実行中のジョブが終わったら `/history` で結果を確かめます。

| `st` | いつ | WorkingTimer のアラート |
|---|---|---|
| `off` | ComfyUI が動いていない (エージェントの起動以来一度も応答していない、または普通に終了した) | なし |
| `idle` | 応答あり、実行中のジョブなし | なし |
| `run` | 実行中 | なし |
| `error` | 直近のジョブがエラーで終わった (次のジョブが始まるまで続く) | あり |
| `down` | 応答していたのに 3 回続けて (約 15 秒) 応答せず、落ちたと見られる (下の表) | あり |
| `stalled` | 実行中なのに進捗が `stall_minutes` 分変わらない (進捗が取れないときは 1 ジョブが `max_job_minutes` 分を超えた) | あり |

ユーザーが中断したジョブはエラーにせず `idle` に戻します。

応答しなくなったときに、落ちたのか普通に終了したのかを次のように見分けます。

| 応答しなくなったときの様子 | 送る `st` |
|---|---|
| ジョブを実行中だった | `down` |
| 待機中だが、待ち受けていたプロセスは残っている (固まった) | `down` |
| 待機中だが、デスクトップ版のアプリ本体 (ComfyUI.exe) は残っている (サーバーだけ落ちた) | `down` |
| 待機中で、プロセスもアプリ本体も消えた | `off` (普通に終了したとみなす) |

待機中に ComfyUI がまるごと落ちた場合は普通の終了と見分けられず、`off` になります。
生成中に ComfyUI を閉じたときや、デスクトップ版でサーバーを再起動して 15 秒以上かかったときは `down` になりますが、応答が戻ればアラートは自動で消えます。

進捗 (`step`/`steps`) と実行中のノード名は WebSocket から受け取ります。届かない場合も状態の判定は `/queue` と `/history` だけで動きます。

### もっと詳しく

- 仕様：[PC_AGENT_TASK.md](PC_AGENT_TASK.md)、[../MQTT_TOPICS.md](../MQTT_TOPICS.md)
- 動作確認の手順：[PC_AGENT_TASK.md の「6. 動作確認」](PC_AGENT_TASK.md)
