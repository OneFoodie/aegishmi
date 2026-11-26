# =============================================================================
# sim10_connect_test.ps1 —— 启动 10 台模拟仪表并做一次连接测试
#
# 用法（在任意目录执行）：
#   pwsh scripts\sim10_connect_test.ps1
#
# 流程：
#   1. 按 configs/devices/fleet_10sim.json 的端口启动 10 个 aegis_sim 进程
#      （只开自定义协议端口，Modbus/OPC UA/SCPI 置 0，避免端口冲突）
#   2. 依次等待端口可连接
#   3. 运行 aegis_linkcheck：连接全部设备/链路，检查是否在线且收到数据
#   4. 无论测试结果如何都会关闭模拟器进程
# =============================================================================
param(
    [string]$BuildDir = "",
    [int]$TimeoutMs = 20000,
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    foreach ($candidate in @("$root\build-mingw\bin", "$root\build\bin", "$root\dist")) {
        if (Test-Path (Join-Path $candidate 'aegis_sim.exe')) {
            $BuildDir = $candidate
            break
        }
    }
}

$simExe = Join-Path $BuildDir 'aegis_sim.exe'
$checkExe = Join-Path $BuildDir 'aegis_linkcheck.exe'
$fleetPath = Join-Path $root 'configs\devices\fleet_10sim.json'
$simConfig = Join-Path $root 'configs\devices\active.json'

foreach ($file in @($simExe, $checkExe, $fleetPath, $simConfig)) {
    if (-not (Test-Path $file)) {
        Write-Error "缺少文件：$file（先执行 cmake --build build-mingw）"
        exit 2
    }
}

# 端口是否已可连接（模拟器启动完成的判据）
function Test-LocalPort([int]$Port) {
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $task = $client.ConnectAsync('127.0.0.1', $Port)
        return $task.Wait(300) -and $client.Connected
    }
    catch {
        return $false
    }
    finally {
        $client.Dispose()
    }
}

$fleet = Get-Content $fleetPath -Raw | ConvertFrom-Json
$devices = @($fleet.devices)
Write-Host "启动 $($devices.Count) 台模拟仪表..." -ForegroundColor Cyan

$processes = @()
foreach ($device in $devices) {
    $arguments = @(
        '--port', "$($device.port)",
        '--modbus-port', '0',
        '--opcua-port', '0',
        '--scpi-port', '0',
        '--config', $simConfig
    )
    $processes += Start-Process -FilePath $simExe -ArgumentList $arguments `
        -PassThru -WindowStyle Hidden
}

$exitCode = 1
try {
    # 等待全部端口就绪（最多 15 秒）
    $deadline = (Get-Date).AddSeconds(15)
    $pending = @($devices | ForEach-Object { [int]$_.port })
    while ($pending.Count -gt 0 -and (Get-Date) -lt $deadline) {
        $pending = @($pending | Where-Object { -not (Test-LocalPort $_) })
        if ($pending.Count -gt 0) { Start-Sleep -Milliseconds 200 }
    }
    if ($pending.Count -gt 0) {
        Write-Warning "以下端口未就绪：$($pending -join ', ')"
    }

    Write-Host "运行连接测试..." -ForegroundColor Cyan
    & $checkExe --fleet $fleetPath --timeout $TimeoutMs
    $exitCode = $LASTEXITCODE
}
finally {
    if ($KeepRunning) {
        $ids = @($processes | Where-Object { $_ -and -not $_.HasExited } | ForEach-Object { $_.Id })
        Write-Host "模拟仪表保持运行（PID：$($ids -join ', ')）。" -ForegroundColor Yellow
        Write-Host "停止方式：Get-Process aegis_sim | Stop-Process -Force" -ForegroundColor DarkGray
    }
    else {
        Write-Host "关闭模拟仪表进程..." -ForegroundColor DarkGray
        foreach ($process in $processes) {
            if ($process -and -not $process.HasExited) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            }
        }
    }
}

if ($exitCode -eq 0) {
    Write-Host "连接测试通过：$($devices.Count)/$($devices.Count) 台模拟仪表在线且有数据。" -ForegroundColor Green
}
else {
    Write-Host "连接测试失败（退出码 $exitCode）。" -ForegroundColor Red
}
exit $exitCode
