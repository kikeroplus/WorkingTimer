# WorkingTimer PC エージェントを、タスクスケジューラに「ログオン時に起動」で登録する。
#
#   登録:  powershell -ExecutionPolicy Bypass -File register_task.ps1
#   解除:  powershell -ExecutionPolicy Bypass -File register_task.ps1 -Unregister
#
# 「アクセスが拒否されました」と出たら、PowerShell を管理者として開いてやり直す。

param([switch]$Unregister)
$ErrorActionPreference = "Stop"
$taskName = "WorkingTimer PC Agent"

if ($Unregister) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    Write-Host "タスク「$taskName」を解除しました。動いているエージェントは python agent.py --stop で止めてください。"
    exit 0
}

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not (Test-Path (Join-Path $here "config.toml"))) {
    throw "config.toml がありません。config.example.toml をコピーして pc_name を書き換えてから登録してください。"
}

# pythonw.exe のフルパスを探す (py ランチャー → PATH の python の順)
$python = $null
try { $python = (& py -3 -c "import sys; print(sys.executable)") 2>$null } catch {}
if (-not $python) { $python = (Get-Command python.exe -ErrorAction Stop).Source }
$pythonw = Join-Path (Split-Path -Parent $python) "pythonw.exe"
if (-not (Test-Path $pythonw)) { throw "pythonw.exe が見つかりません: $pythonw" }

$action = New-ScheduledTaskAction -Execute $pythonw -Argument "`"$here\agent.py`"" -WorkingDirectory $here
$trigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
    -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Force | Out-Null
Write-Host "タスク「$taskName」を登録しました。"
Write-Host "  実行: $pythonw `"$here\agent.py`""
Write-Host "  今すぐ動かすには: Start-ScheduledTask -TaskName `"$taskName`""
