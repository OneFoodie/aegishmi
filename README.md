# AegisHMI —— 工业设备自动测试与监控上位机

基于 **Qt 6 / C++17** 的单机上位机工程：多协议设备接入、10 kHz 多通道实时波形、
JSON 驱动的自动测试流程、质量闭环（SPC / FAI / Andon）、SQLite 追溯与 PDF/CSV 报告。

工程自带设备模拟器与 10 台模拟仪表联调脚本，没有真实硬件也能完整演示。

## 亮点

- **设备接入**：TCP 自定义协议 / Modbus TCP / Modbus RTU / OPC UA / SCPI 仪器；多设备、多链路并发。
- **实时监控**：自绘波形（缩放、A/B 双游标、规格限叠加）、链路健康面板、报文诊断、原始波形 CSV。
- **自动测试**：JSON 方案（命令/等待/测量/版本/脚本/插件/子流程/操作员确认），图形化编辑与单步调试。
- **质量闭环**：工单与 SN 校验、料号防错、FAI、SPC Cp/Cpk、Andon 停线、报警确认与搁置。
- **追溯与报告**：历史检索、趋势多曲线、单次/批量 PDF、日报 CSV、审计日志。
- **工业风界面**：左侧导航、深色控制台顶栏、全局状态条与报警横幅、KPI 卡片、悬停提示与快速上手。

## 快速开始（Windows + Qt 6.8 MinGW）

```powershell
cd aegishmi
cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/mingw_64
cmake --build build-mingw -j 8
```

运行 10 台模拟仪表并做连接预检（保留模拟器）：

```powershell
cd aegishmi
$env:PATH = 'D:\Qt\Tools\mingw1310_64\bin;D:\Qt\6.8.3\mingw_64\bin;' + $env:PATH
pwsh scripts/sim10_connect_test.ps1 -KeepRunning
```

另开窗口启动上位机：

```powershell
$env:AEGIS_FLEET_CONFIG = (Resolve-Path 'configs/devices/fleet_10sim.json').Path
.\build-mingw\bin\aegis_hmi.exe
```

默认账号（仅用于演示，首次运行自动创建）：

| 账号 | 口令 | 角色 |
|---|---|---|
| admin | admin123 | 管理员 |
| operator | operator123 | 操作员 |
| viewer | viewer123 | 观察员 |

## 目录

```
aegishmi/          上位机源码、模拟器、测试、配置与文档
market-gap-analysis/  市场/功能对标分析材料
杭州C++上位机岗位分析与项目设计.md  项目背景与设计说明
```

- 详细功能、目录与设计说明：[aegishmi/README.md](aegishmi/README.md)
- 界面演示步骤：[docs/08_界面演示启动步骤.md](aegishmi/docs/08_界面演示启动步骤.md)
- 需求/设计/测试文档：`aegishmi/docs/`

## 测试

```powershell
cd aegishmi
$env:PATH = 'D:\Qt\Tools\mingw1310_64\bin;D:\Qt\6.8.3\mingw_64\bin;' + $env:PATH
ctest --test-dir build-mingw --output-on-failure
```

当前 14 个 Qt Test 目标全部通过。

## 说明

- OPC UA 依赖 `third_party/open62541`，可用 `scripts/fetch_open62541.ps1` 获取；缺失时自动关闭该协议。
- 仓库不包含构建产物、第三方源码、日志与本地数据库。
- MES 地址、工位/治具编号等均为演示配置（`aegishmi/configs`），请在现场按实际环境替换。
- 默认账号与示例口令仅用于本地演示，部署前必须修改。
