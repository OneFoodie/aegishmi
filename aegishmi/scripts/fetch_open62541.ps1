# =============================================================================
# fetch_open62541.ps1 —— 下载并解压 OPC UA 协议栈源码（open62541 v1.4.6）
#
# 用法（在 aegishmi 目录下）：
#   powershell -ExecutionPolicy Bypass -File scripts\fetch_open62541.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\fetch_open62541.ps1 -Zip D:\downloads\open62541.zip
#
# 说明：
#   - CMake 只探测 third_party/open62541/CMakeLists.txt 是否存在；
#     不存在时自动关闭 OPC UA 功能，其余功能不受影响。
#   - 内网/国内网络访问 GitHub 较慢时，可用 -Zip 指定事先下载好的源码压缩包。
# =============================================================================
param(
    [string]$Version = "v1.4.6",
    [string]$Zip = ""
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot          # aegishmi 目录
$thirdParty = Join-Path $root "third_party"
$target = Join-Path $thirdParty "open62541"

if (Test-Path (Join-Path $target "CMakeLists.txt")) {
    Write-Host "[skip] third_party/open62541 已存在" -ForegroundColor Yellow
    exit 0
}

New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null

$tempZip = $Zip
if ([string]::IsNullOrWhiteSpace($tempZip)) {
    $tempZip = Join-Path $env:TEMP "open62541-$Version.zip"
    $url = "https://github.com/open62541/open62541/archive/refs/tags/$Version.zip"
    Write-Host "[info] 下载 $url"
    # TLS12：老版本 PowerShell 默认不启用，GitHub 需要
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile $tempZip -UseBasicParsing
}

Write-Host "[info] 解压到 $target"
$extractDir = Join-Path $env:TEMP ("open62541-extract-" + [Guid]::NewGuid().ToString("N"))
Expand-Archive -Path $tempZip -DestinationPath $extractDir -Force

# 压缩包内是一层 open62541-x.y.z 目录，把它整体重命名为 third_party/open62541
$inner = Get-ChildItem -Path $extractDir -Directory | Select-Object -First 1
if ($null -eq $inner) {
    throw "压缩包结构异常：没有找到顶层目录"
}
Move-Item -Path $inner.FullName -Destination $target
Remove-Item -Recurse -Force $extractDir

Write-Host "[ok] open62541 已就绪：$target" -ForegroundColor Green
Write-Host "     重新执行 scripts\build.bat 即可启用 OPC UA 支持。" -ForegroundColor Green
