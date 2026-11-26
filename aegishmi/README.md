# AegisHMI —— 工业设备自动测试与监控上位机

基于 Qt 6 / C++17 的上位机项目：连接设备（TCP 自定义协议 / Modbus TCP / Modbus RTU / OPC UA / SCPI 仪器）、
10 kHz 多通道波形显示、JSON 配置的自动测试流程（支持图形化编辑与单步调试）、SQLite 历史记录与 CSV 导出。

配套设备模拟器（`aegis_sim`）可在没有真实硬件时完整演示链路，同时对外提供四种协议服务。

---

## 1. 功能概览

| 模块 | 能力 | 状态 |
|---|---|---|
| 设备接入 | TCP 自定义协议、Modbus TCP、Modbus RTU（串口主站）、OPC UA、SCPI 仪器（SCPI-RAW over TCP，5025）；多设备并发（fleet 配置）；单设备多链路（主/辅链路独立采集与测量） | 已实现 |
| 协议解析 | 帧头/长度/序号/类型/负载/CRC16，粘包分包状态机 | 已实现 |
| 数据采集 | 分块无锁环形缓冲（Chunk）、10 kHz × 6~64 通道（配置驱动） | 已实现 |
| 波形增强 | 滚轮缩放（1~10 秒）、悬停读数、A/B 双游标 ΔT/ΔV、通道上下限虚线叠加 | 已实现 |
| 原始波形导出 | 捕获最近 20 秒全通道原始采样并导出 CSV | 已实现 |
| 通信可靠性 | 断线自动重连（1/2/4/8/16 秒退避，封顶 16 秒）、HEX 报文诊断开关 | 已实现 |
| 通道校准 | 按通道 gain/offset 线性补偿 + 金样日常点检 | 已实现 |
| 实时显示 | 自绘波形（900 点/通道、20 fps、cosmetic 画笔）、数值面板、日志 | 已实现 |
| 命令交互 | SET_VOUT / SET_IOUT / READ_MEASURE / RESET，Ack 应答 + 超时 | 已实现 |
| 自动测试 | JSON 流程、变量替换、command/delay/measure/readVersion/script/plugin/subFlow/prompt、条件与 goto、上下限与版本判定、重试/中止；图形化流程编辑器与单步调试 | 已实现 |
| 界面框架 | 工业风左侧导航 + 深色控制台顶栏 + 页面标题区；全局状态条与报警横幅；链路健康面板、设备 Faceplate、KPI 卡片、首次启动快速上手 | 已实现 |
| 生产防呆 | 工单绑定、SN 扫码校验、一码一测、复测确认、工位/治具/操作员追溯 | 已实现 |
| 测试方案库 | 扫描 `configs/flows`，工单绑定方案并自动加载；方案产品与工单不一致时拦截 | 已实现 |
| 用户管理 | 管理员新建用户、改角色、重置口令、启用/停用；保护最后一个管理员 | 已实现 |
| 追溯检索 | 历史页按 SN/工单/操作员/方案 + 判定过滤；审计日志可导出 CSV | 已实现 |
| 质量闭环 | 首件检验 FAI 门禁、不良原因、返修/复测统计、治具寿命与校准拦截 | 已实现 |
| 生产看板 | 产量/进度、PASS/FAIL、FPY 直通率、UPH、复测通过、待返修、不良步骤/原因 Pareto | 已实现 |
| 生产报表 | 按日期+班次的日报 CSV（运行数/良率）、数据库备份与压缩 | 已实现 |
| SN 清单 | 按工单导入发货清单；清单非空时只允许清单内 SN 测试 | 已实现 |
| 过程能力 SPC | 按测试项上下限算 Cp/Cpk、I-MR 控制图、判异规则（越限/同侧/趋势/2σ） | 已实现 |
| 电子 SOP | 按产品/工单展示图文作业指导书，未勾选"已阅读"不能开始测试 | 已实现 |
| 审核报告 | 单次/批量导出 PDF：判定、追溯信息、步骤明细与规格限、签名栏；模板可配置 Logo/页脚/主色/签名栏文案 | 已实现 |
| 班组管理 | 产品-操作员资质白名单、班次判定、交接班记录（产量/良率快照 + 备注） | 已实现 |
| Andon 停线 | 连续 N 件 FAIL 或 FPY 低于阈值自动锁定工位，管理员解除后恢复 | 已实现 |
| SPC 联动停线 | 测试项 Cpk 低于阈值且样本足够时自动 Andon（含测试项名称与 Cpk） | 已实现 |
| 合格证标签 | PASS 后按 SN/工单/产品自动生成小尺寸标签 PDF，供标签打印机取用 | 已实现 |
| MES 对接 | 测试结果 HTTP JSON 上传，本地 outbox 断网补传、指数退避、人工重传 | 已实现 |
| 用户权限 | 登录鉴权（SHA256+盐），观察员/操作员/管理员三级权限；关键操作需第二人复核（4-eyes） | 已实现 |
| 操作审计 | 登录、连接、测试、工单、报警确认、报表导出等操作留痕，管理员可查 | 已实现 |
| 阈值报警 | 通道高/低阈值 + 回差，报警事件落库、确认、设备列表 ⚠ 标记 | 已实现 |
| 数据存储 | SQLite（WAL）：运行记录、秒级波形摘要（min/max/avg）、趋势查询、CSV/PDF 导出 | 已实现 |
| 工程化 | CMake、14 个测试目标、windeployqt 打包、Inno Setup 脚本、Docker/CI | 已实现 |
| 性能基准 | `aegis_bench` 解码/环形缓冲基准 + 性能报告 | 已实现 |

明确不做：Web 端、云平台。

---

## 2. 目录结构

```
aegishmi/
├─ CMakeLists.txt              # 构建入口（可选启用 open62541）
├─ src/
│  ├─ protocol/                # 帧格式、编解码状态机、CRC16、Modbus 编解码
│  ├─ core/                    # 分块环形缓冲、设备通道配置、工位配置（station.json）
│  ├─ quality/                 # SPC 过程能力计算（Cp/Cpk、I-MR 控制图、判异规则）
│  ├─ infra/                   # TCP/串口传输、Modbus TCP/RTU 主站、OPC UA 客户端、SCPI 仪器客户端
│  ├─ service/                 # DeviceManager、DeviceService、AlarmMonitor、MesUploader
│  ├─ security/                # 登录鉴权（AuthService）
│  ├─ test/                    # 流程加载/校验/写回、执行引擎（含 script 动作）、设备抽象
│  ├─ storage/                 # SQLite 仓储（运行/摘要/用户/审计/报警/工单/outbox）、异步写入
│  ├─ ui/                      # 生产看板、监控、自动测试（含流程编辑器）、历史、趋势、报警、审计、登录框
│  └─ app/main.cpp             # 上位机入口
├─ simulator/main.cpp          # 设备模拟器（9527 / 5020 / 4840 / 5025）
├─ tests/                      # 14 个 QTest 测试目标
├─ tools/bench.cpp             # 性能基准
├─ configs/flows/*.json        # 示例测试方案
├─ configs/devices/*.json      # 通道配置（active.json）+ 设备清单（fleet.json）
├─ configs/station.json        # 工位/线体/治具/SN 规则/MES 参数
├─ scripts/                    # build / deploy / 下载 open62541 / Inno Setup
├─ docker/Dockerfile           # Linux 构建与测试镜像
├─ third_party/                # open62541（可选，缺失时自动关闭 OPC UA）
└─ docs/                       # 设计、需求、详细设计、测试与性能报告
```

---

## 3. 构建

### 3.1 Windows（推荐，已验证）

环境：Qt 6.8.3 + CMake ≥ 3.21 + Ninja；编译器可用 MinGW 13 或 MSVC 2022。

```bat
cd aegishmi
scripts\build.bat Release
```

脚本会自动检测 `cl.exe`：存在则用 MSVC（输出到 `build/`），否则用 MinGW（输出到 `build-mingw/`）。
产物：`build-mingw\bin\aegis_hmi.exe`、`aegis_sim.exe`、`tst_*.exe`、`aegis_bench.exe`。

### 3.2 启用 OPC UA

OPC UA 依赖 open62541，源码放在 `third_party/open62541`：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\fetch_open62541.ps1
```

也可以完全跳过：CMake 找不到源码时会关闭 OPC UA，其余功能不受影响（OPC UA 连接会提示未启用）。
open62541 在 amalgamation 模式下生成动态库 `libopen62541.dll`，`deploy.bat` 会一并复制。

### 3.3 Linux / Docker

```bash
docker build -f docker/Dockerfile -t aegishmi:latest .
```

Ubuntu 24.04 + 系统 Qt 6（apt），构建完成后自动执行一轮单元测试。
仓库根目录的 `.github/workflows/ci.yml` 提供 Windows（MSVC）与 Linux（GCC）双平台 CI。

### 3.4 打包

```bat
scripts\deploy.bat
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" scripts\installer.iss
```

`deploy.bat` 用 windeployqt 生成 `dist/`（含 Qt DLL、插件、MinGW 运行库、open62541 DLL、configs）；
安装包输出到 `installer\AegisHMI-0.1.0-setup.exe`。

---

## 4. 运行与演示

```bat
dist\aegis_sim.exe      :: 设备模拟器：9527（自定义协议）、5020（Modbus TCP）、4840（OPC UA）、5025（SCPI）
dist\aegis_hmi.exe      :: 上位机
```

上位机操作：

0. 启动后先登录（默认账号见下表），登录成功才进入主界面；
1. "生产"页新建/选择工单（工单号 + 产品 + 计划数量 + 可选绑定测试方案），这是开始测试的前提；
2. "监控"页选择协议与地址 → 点"连接"（自定义协议默认 `127.0.0.1:9527`）；
3. "监控"页观察各通道波形、统计信息（帧数/CRC 错误/丢块）；
   可用滚轮缩放、鼠标游标读数；"捕获波形…"导出最近 20 秒原始采样；
   "日常点检"按金样值检查通道；"报文诊断（HEX）"查看收发报文；
4. "自动测试"页加载 `configs/flows/power_module_basic.json` → 扫码/输入 SN 回车 →
   首次会弹出产品 SOP，勾选"已阅读"后开始；逐步判定（重复 SN 弹复测确认；FAIL 后选不良原因）；
5. "生产"页查看产量/计划进度、PASS/FAIL、FPY、UPH、复测通过/待返修、不良 Pareto 与 MES 状态；
   首件待确认时点"首件确认"放行；首件不合格点"重新首件"；治具到期点"治具复位/校准"；
   触发 Andon 时顶部出现红色横幅，管理员点"解除停线"恢复；PASS 且启用标签时自动生成合格证 PDF；
   可"导入 SN 清单…"做发货清单防呆，或"导出日报"按日期/班次导出产量与良率；
6. "历史"页查看运行记录（含工单/复测/上传状态），可按 SN/工单/操作员/方案与判定检索，
   可导出 CSV、单次 PDF 或批量 PDF；
7. "趋势"页选设备/通道/时间范围，查看秒级 min-max 包络与 avg 曲线；
8. "SPC"页选工单与测试项，查看 Cp/Cpk、控制图与判异结果；
9. "班组"页查看当前班次/操作员资质，并记录交接班；
10. "报警"页查看阈值报警、确认报警；设备列表出现 ⚠ 表示该设备有未恢复报警；
11. 管理员还会看到"审计"（可导出 CSV）与"用户"页，用于追溯操作和管理账号。

默认账号（首次运行自动创建，口令只存盐 + SHA256 摘要）：

| 账号 | 口令 | 角色 | 权限 |
|---|---|---|---|
| admin | admin123 | 管理员 | 全部（含审计页） |
| operator | operator123 | 操作员 | 连接、测试、确认报警、导出报表 |
| viewer | viewer123 | 观察员 | 只读查看波形/历史/趋势/报警 |

模拟器端口与约定：

| 协议 | 端口 | 数据约定 |
|---|---|---|
| 自定义帧协议 | 9527 | 10 kHz × N 通道（N 由设备配置决定），DATA 帧带时间戳与序号 |
| Modbus TCP | 5020（502 需管理员权限） | FC03/04 读寄存器 0~5；FC06 写 0=电压(mV)、1=电流(mA) |
| Modbus RTU | 串口（需硬件/虚拟串口对） | 站号 + FC04/FC06 + CRC16，轮询周期 50 ms |
| OPC UA | 4840 | 订阅 `ns=1;s=Channel0` ~ `ChannelN-1`（Double）；节点 0/1 可写（设定值），其余只读 |
| SCPI 仪器 | 5025 | SCPI-RAW over TCP（按行）：`*IDN?`、`MEAS:VOLT:DC?`、`MEAS:CURR:DC?`、`SOUR:VOLT x`、`SOUR:CURR x`、`*RST` |

无 GUI 时可用 `build-mingw\bin\aegis_opcua_probe.exe opc.tcp://127.0.0.1:4840 [0 7000]`
验证订阅与写设定值。

各协议对应的采样率：自定义协议 10 kHz、Modbus TCP 50 Hz、Modbus RTU 20 Hz、OPC UA 10 Hz、
SCPI 仪器 1/poll 周期（默认 100 ms → 10 Hz，可通过 `scpiPollMs` 调整）
（测试引擎按当前协议的采样率判断"窗口内数据是否足够"，不会误判）。

运行日志：界面日志面板保留最近 500 行，同时按天写入
`%APPDATA%\AegisLab\AegisHMI\logs\app-yyyyMMdd.log`（保留 7 天），便于现场排障。

### 4.1 通道配置（6 → 32/64 通道）

通道数、采样率、通道名称都由 `configs/devices/active.json` 决定，**模拟器与上位机必须用同一份配置**：

```json
{
  "schemaVersion": 1,
  "channelCount": 32,
  "sampleRateHz": 10000,
  "channels": [ { "name": "CH0", "unit": "mV" }, ... ]
}
```

- 默认 `active.json` 为 6 通道；
- 演示 32 通道：`configs/devices/lab_32ch.json`，用环境变量切换：

```bat
set AEGIS_DEVICE_CONFIG=%CD%\configs\devices\lab_32ch.json
aegis_sim.exe
set AEGIS_DEVICE_CONFIG=%CD%\configs\devices\lab_32ch.json
aegis_hmi.exe
```

实现要点：DATA 帧上限 64 KB；每帧 100 点 × 32 通道约 6.4 KB；上位机按"数据块"入环
（256 槽 × 512 点 × N 通道 × 2 B，32 通道约 8 MB），界面按像素桶做 min/max 抽稀。

### 4.2 多设备并发

设备清单在 `configs/devices/fleet.json`：

```json
{
  "devices": [
    { "id": "dev1", "name": "电源模块-1", "protocol": "custom",
      "host": "127.0.0.1", "port": 9527, "config": "active.json" },
    { "id": "dev2", "name": "采集卡-2（32 通道）", "protocol": "custom",
      "host": "127.0.0.1", "port": 9528, "config": "lab_32ch.json" },
    { "id": "dmm1", "name": "台式万用表", "protocol": "scpi",
      "host": "127.0.0.1", "port": 5025,
      "scpiQuery": "MEAS:VOLT:DC?", "scpiPollMs": 100, "scpiScale": 1000 }
  ]
}
```

`protocol` 支持 `custom` / `modbus-tcp` / `modbus-rtu` / `opcua` / `scpi`。
SCPI 设备默认每 100 ms 发一次 `MEAS:VOLT:DC?`，把返回的电压乘以 `scpiScale`（默认 1，
填 1000 表示 V→mV）后写入 CH0，供波形与判定使用。

- 每台设备有独立的 `DeviceService`（通信线程 + 分块缓冲），互不阻塞；
- 主界面左侧"设备列表"显示连接状态，选中哪台就显示哪台的波形/统计；
- "自动测试"页跟随选中设备；测试执行期间锁定设备切换；
- `config` 相对 fleet.json 所在目录解析，换机器/打包不用改路径；
- `AEGIS_FLEET_CONFIG` 可指定其他清单；找不到清单时退化为 1 台默认设备（9527）。

一键演示（两个模拟器进程 + 上位机）：

```bat
scripts\demo_multi_device.bat
```

模拟器也支持手工指定端口/通道配置：

```bat
aegis_sim.exe --port 9528 --modbus-port 5021 --opcua-port 4841 --config configs\devices\lab_32ch.json
```

### 4.3 存储分层与趋势（P3）

原始波形只保留在内存（约 2.5 秒），数据库只存**每秒摘要**：

```
sample_summary(device_id, channel, epoch_sec, min_value, max_value, avg_value, sample_count)
```

- 聚合在 `DeviceService` 内完成（收包路径顺手累计，不额外遍历）；
- 写入由 `SummaryWriter` 独立线程完成：500ms 或 500 行攒批，单事务提交；
- 保留策略：启动时删除 7 天前的摘要（`purgeSummariesBefore`）；
- "趋势"页读取该表，60 分钟也只有 3600 行/通道，绘制 min-max 包络 + avg 曲线。

实测（2 设备：6 通道 + 32 通道，运行 10 秒）：

| 指标 | 结果 |
|---|---|
| 摘要行数 | 380 行（38 通道 × 10 秒） |
| CH0 电压 | min 4980 / max 5020 / avg 4999.5 mV，10000 点/秒（源：5000 ± 20mV 纹波） |
| CH1 电流 | min 995 / max 1005 / avg 999.5 mA |

### 4.4 用户权限与操作审计

- `app_user` 表存 `username / salt / hash / role`；口令 = 1 万轮 SHA256(salt + password)，不存明文；
- 角色权限点：观察员 `View`，操作员 `View+Operate`，管理员 `View+Operate+Administer`；
- "切换用户"按钮会先登出（写审计）再弹出登录框；测试执行期间禁止切换；
- 4-eyes 复核：解除停线、重新首件、治具校准会弹出复核框，需另一个操作员/管理员
  输入账号口令授权；复核人不能是当前操作人，观察员无复核权限，通过/拒绝均写审计；
- `audit_log` 只追加，记录时间/用户/动作/对象/结果/详情，涵盖：登录成功/失败、登出、
  连接/断开设备、开始/停止/结束测试、确认报警、导出 CSV；
- 审计页仅管理员可见，显示最近 500 条。

### 4.5 阈值报警

通道配置可选增加 `high / low / deadband`（`configs/devices/active.json`）：

```json
{ "name": "CH0", "unit": "mV", "high": 6000, "low": 4000, "deadband": 50 }
```

- 判定使用每秒摘要的 `avg`，1 秒一次；
- 进入报警：`avg > high` 或 `avg < low`；
- 恢复条件带回差：高报警需回落到 `high - deadband`，低报警需回升到 `low + deadband`，避免阈值抖动刷屏；
- 每次"进入 → 恢复"在 `alarm_event` 表里是一条完整记录，可确认、可查历史；
- 程序重启时，上次遗留的未恢复记录会先标记为已恢复（若条件仍越限，下一秒重新产生报警）；
- 设备列表出现 `⚠` 且页签显示 `报警 (n)`（n = 未恢复数量）。

### 4.6 脚本步骤（script）

流程步骤支持 `"action": "script"`，用 Qt QJSEngine 执行 JavaScript，适合"下发命令 + 采集 + 自定义判定"的组合场景：

```json
{
  "id": "s7",
  "name": "脚本：上电自检",
  "action": "script",
  "params": {
    "script": "if (!setVout(0, 5000)) throw new Error('设置电压失败');\nsleep(300);\nvar v = measureAvg(0, 200);\nlog('avg=' + v);\nif (v < 4750 || v > 5250) throw new Error('超差: ' + v);"
  }
}
```

脚本内可用函数：

| 函数 | 说明 |
|---|---|
| `setVout(channel, mV)` / `setIout(channel, mA)` | 下发设定值，返回 bool |
| `measureAvg(channel, windowMs)` | 采集窗口平均值（窗口 10~10000ms） |
| `sleep(ms)` | 等待（0~60000ms） |
| `log(text)` | 写入步骤说明，执行后显示在"说明"列 |

判定规则：脚本抛异常、返回 `false` 或任一设备操作失败，步骤即失败；
脚本里最后一次 `measureAvg` 的结果会作为步骤实测值落库。

---

### 4.6.1 图形化编辑与单步调试

- "自动测试"页点"编辑方案…"：左侧步骤表格（添加/删除/上移/下移），右侧属性面板
  （动作、命令/通道/数值/采样窗口/延时/脚本、超时/重试/失败处理、判定条件），顶部为流程级字段；
- "校验"即时列出问题并定位到字段路径；"保存"回写当前方案文件，"另存为…"生成新方案；
- "单步调试"每次只执行下一步（含该步重试），结果显示在摘要区，不写数据库、不影响正式统计，
  跑完最后一步后回到第一步，方便边改边验；
- 编辑器与运行时共用同一份 JSON：保存走 `FlowWriter`，随后按正常加载流程重新读取+校验，
  `tst_flow` 中的 roundtrip 用例保证"写出去还能原样读回来"。

### 4.6.2 固件版本校验（readVersion）

流程步骤新增 `readVersion`，用于"读设备版本 → 与工单/BOM 要求比对"：

- `params.cmd` 可省略：SCPI 默认发 `*IDN?`，自定义 TCP 默认发 `ReadVersion` 命令（Info 帧回文本）；
- `expect.version` 精确匹配，`expect.versionPattern` 正则匹配，二者只能配置一个；
- 实际版本写入步骤说明，历史记录与 PDF 报告的"说明"列可见；
- 查询超时、设备不支持、返回为空均判 FAIL，失败策略沿用 `retries` / `onFail`；
- 各协议取版本：自定义协议 Info 帧文本、SCPI 查询（默认 `*IDN?`）、
  Modbus TCP/RTU 读保持寄存器 `0x0100` 起 12 个（24 字节 ASCII，NUL 截断）、
  OPC UA 读字符串节点（默认 `ns=1;s=FirmwareVersion`，`params.cmd` 填 `ns=` 开头可改用其他节点）。

```json
{
  "id": "fw", "name": "校验固件版本", "action": "readVersion",
  "params": { "cmd": "*IDN?" },
  "expect": { "versionPattern": ".*,1\\.0$" }
}
```

### 4.7 工单与 SN 防呆

"生产"页维护工单（`work_order` 表）：工单号、产品、计划数量、状态。当前工单保存在
`QSettings`，重启后自动恢复；已关闭工单不再用于新测试。

开始测试前依次检查：

1. 必须已选择 ACTIVE 工单；
2. 首件状态允许（PENDING 或 CONFIRMED），WAITING/REJECTED 时锁定；
3. 治具未过校准期、未达寿命上限；
4. 工位未处于 Andon 停线状态；
5. 当前操作员具备该产品的操作资质；
6. 已阅读并确认该产品的电子 SOP（启用时）；
7. 已加载测试方案，且方案产品与工单产品一致；
8. SN 必填且匹配 `configs/station.json` 的 `snPattern`；
9. 同一工单内同一 SN 重复测试会弹确认框，确认后该次记录标记 `retest=1`。

扫码枪以回车结尾时，SN 输入框回车直接触发开始（无需鼠标）。
每次运行落库的追溯字段：`work_order / line_id / station_id / operator / fixture / retest`。

工位配置示例（`configs/station.json`）：

```json
{
  "stationId": "ST-01",
  "lineId": "L1",
  "fixtureId": "FX-5V-01",
  "snPattern": "^[A-Za-z0-9_-]{6,32}$",
  "fai": { "enabled": true, "sampleCount": 1 },
  "failReasons": ["焊接不良", "元件失效", "参数漂移", "装配错误", "其他"],
  "fixture": { "maxRuns": 5000, "calibrationDays": 365, "calibratedUntil": "" },
  "mes": { "enabled": false, "url": "http://127.0.0.1:8080/api/test-results",
           "timeoutMs": 5000, "maxAttempts": 5, "retryBaseMs": 2000 }
}
```

### 4.8 生产看板与统计口径

"生产"页展示当前工单的：

| 指标 | 口径 |
|---|---|
| 产量 / 进度 | 去重 SN 数；进度 = 产量 / 计划数量 |
| 总测试 / PASS / FAIL | 运行次数（含复测） |
| FPY 直通率 | 首次即 PASS 的 SN 数 / 去重 SN 数 |
| UPH | 去重 SN 数 /（首件测试至今的小时数），不足 1 分钟按 1 分钟估算 |
| 复测通过 | `retest=1 且 PASS` 的运行次数（返修后通过） |
| 待返修 | 最近一次运行仍非 PASS 的去重 SN 数 |
| 不良步骤 Pareto | `test_step` 中 FAIL 步骤按次数倒序 |
| 不良原因 Pareto | FAIL 运行按所选不良原因分组计数 |
| MES | 待上传（PENDING）/ 失败（DEAD）数量 |

看板在切换到该页、测试落库、MES 状态变化时刷新；统计全部由 SQLite 聚合查询完成，
不额外占用采集线程。

### 4.9 MES 上传（HTTP JSON）

流程：测试落库 → 生成报文 → 写入 `mes_outbox`（PENDING）→ `MesUploader` 异步 POST →
成功标记 SENT；失败按 `retryBaseMs × 2^(n-1)` 退避重试（上限 60 秒），达到
`maxAttempts` 标记 DEAD，可在看板"重传失败项"人工恢复。程序重启后继续补传 PENDING。

报文示例（节选）：

```json
{
  "runId": 42,
  "workOrder": "WO-20261125-001",
  "lineId": "L1",
  "stationId": "ST-01",
  "sn": "SN-0001",
  "verdict": "PASS",
  "operator": "operator",
  "fixture": "FX-5V-01",
  "retest": false,
  "startedAt": 1795620000000,
  "steps": [ { "index": 1, "name": "测量输出电压", "status": "PASS",
               "measured": 5000.0, "unit": "mV" } ]
}
```

默认 `mes.enabled=false`（不打扰本地演示）；对接时把 `url` 指向 MES 网关并置为 `true`。

### 4.10 质量闭环与工装

**首件检验（FAI）**：新建工单时按 `fai.enabled` 生成首件要求，状态机：

```
PENDING（待首件） --连续 N 件 PASS--> WAITING（待确认） --主管确认--> CONFIRMED（放行）
        |                                   |
        +--任一 FAIL--> REJECTED（不合格）<--+
                              |
                        "重新首件" --> PENDING
```

- 首件件数取工单创建时的 `fai.sampleCount`（默认 1），之后改配置不影响已有工单；
- WAITING / REJECTED 期间"自动测试"拒绝开始；确认/重新首件都写审计；
- 老工单/`fai.enabled=false` 的工单直接视为 CONFIRMED，不影响历史数据。

**不良原因**：FAIL 落库后弹窗选择 `failReasons`（可取消），写入 `test_run.fail_reason`；
历史列表、CSV、MES 报文都带该字段，看板按原因做 Pareto。

**返修/复测**：复测记录沿用 `retest=1`；看板"待返修"= 最近一次仍非 PASS 的 SN，
"复测通过"= 复测且 PASS 的次数，不需要单独的返修工单表。

**治具寿命/校准**：

- `fixture_usage` 表按治具累计使用次数与校准到期时间；
- 每次运行落库后计数 +1；超过 `fixture.maxRuns` 或当前时间晚于校准到期日时拒绝开始测试；
- 看板显示"已用 N/M、校准至日期、正常/寿命已到/校准已过期"；
- "治具复位/校准"清零计数并把校准期顺延 `calibrationDays` 天，写审计。

### 4.11 SPC（过程能力与控制图）

"SPC"页按工单选择测试项（measure 步骤），计算过程能力并绘制单值-移动极差（I-MR）控制图：

| 指标 | 口径 |
|---|---|
| Cp | (USL - LSL) / (6σ)，仅双侧规格 |
| Cpk | min(USL - mean, mean - LSL) / (3σ)，单侧规格也支持 |
| 等级 | A+ ≥1.67，A ≥1.33，B ≥1.0，C ≥0.67，D <0.67 |
| σ | 样本标准差（n-1） |
| 控制限 | CL = mean，UCL/LCL = mean ± 2.66 × MR̄，MR 图 UCL = 3.267 × MR̄ |

判异规则（任一命中即列出位置）：

1. 单点越出控制限；
2. 连续 7 点在中心线同一侧；
3. 连续 6 点单调上升/下降；
4. 连续 3 点中至少 2 点超出 2σ（同侧）。

规格限来自流程 JSON 的 `expect.min/max`，落库时写入 `test_step.min_limit/max_limit`，
因此换配方后历史数据的限值仍然是当时的配方，不会被新配置改写。

### 4.12 电子 SOP（作业指导书）

`configs/station.json` 的 `sop` 按产品配置图文步骤：

```json
"sop": {
  "enabled": true,
  "requireRead": true,
  "documents": [
    { "product": "PM-5V-10A", "title": "电源模块出厂测试作业指导书",
      "version": "A.1", "steps": ["确认治具清洁", "扫描 SN", "..."],
      "image": "D:/sop/pm5v.png" }
  ]
}
```

- 产品精确匹配优先，其次 `product` 为空或 `"*"` 的通用条目；
- 点击"开始测试"时弹出 SOP 阅读框；`requireRead=true` 时必须勾选"已阅读"才能继续；
- 同一工单 + 产品 + 版本只需确认一次，确认动作写入审计；
- `product` 不匹配任何条目时直接放行。

### 4.13 PDF 审核报告

"历史"页支持单次导出与批量导出：

- 单次：判定、产品/方案、SN、工单/线体/工位/治具、操作员/班次、复测标记、耗时；
- 步骤表：动作、状态、实测值、规格限、耗时、说明；
- 首次页含报告编号、公司/标题/Logo/页脚/主色（`station.json` 的 `report` 段）、签名栏；
- 签名栏文案可配置（默认"检验员 / 审核"），也可关闭签名栏只保留生成时间；
- 批量：首个汇总页（报告数、PASS/FAIL/ABORTED）+ 每次运行一页；
- 实现用 `QPdfWriter + QTextDocument::print`（Qt6::Gui，无需 PrintSupport 模块）。

报告模板配置示例（`configs/station.json`）：

```json
"report": {
  "company": "AegisLab",
  "title": "出厂测试报告",
  "logo": "configs/logo.png",
  "footer": "AegisHMI 质量管理系统 · 本报告由系统自动生成",
  "accentColor": "#2c3e50",
  "signature": true,
  "inspectorLabel": "检验员",
  "reviewerLabel": "审核"
}
```

`logo` 为空或文件不存在时自动跳过；`accentColor` 非法时回退默认色。

### 4.14 操作员资质、班次与交接班

```json
"qualification": {
  "enabled": true,
  "products": { "PM-5V-10A": ["admin", "operator"], "*": ["admin", "operator"] }
},
"shifts": [
  { "name": "白班", "start": "08:00", "end": "20:00" },
  { "name": "夜班", "start": "20:00", "end": "08:00" }
]
```

- 开始测试前按"工单产品 + 当前用户"校验资质，未授权直接拒绝并提示；
- 产品没有专属条目时退回 `"*"`，两者都没有视为不限制；列表里写 `"*"` 表示所有人；
- 班次支持跨零点；每次运行把班次写入 `test_run.shift`，看板顶部实时显示当前班次；
- "班组"页可记录交接班：当前工单累计产量/PASS/FAIL 快照 + 备注，写入 `shift_handover`，
  只读角色不可操作，交接动作写审计。

### 4.15 Andon 停线

```json
"andon": { "enabled": true, "consecutiveFails": 3, "fpyThreshold": 80, "minUnits": 5 }
```

- 每次测试落库后评估：最近连续 FAIL 达到 `consecutiveFails`，或产量 ≥ `minUnits`
  且 FPY 低于 `fpyThreshold`（%）时，写入 `andon_event` 锁定工位；
- 停线期间"自动测试"拒绝开始，看板顶部显示红色横幅与触发原因；
- 只有管理员能点"解除停线"（写审计）；其它角色只能看到横幅；
- 人工停止（ABORTED）不算质量不良，会中断连续 FAIL 计数。

### 4.16 测试方案库与工单绑定

- 程序启动/新建工单时扫描方案目录（`AEGIS_FLOW_DIR` → `exe/configs/flows` → `exe/../../configs/flows`），
  列出方案名、产品、步骤数；加载或校验失败的 JSON 不会出现在下拉框；
- 新建工单时可选择绑定方案，`work_order.flow_path` 保存绝对路径；
- 切换工单时"自动测试"页自动加载绑定方案，并在左侧显示方案文件名；
- 开始测试前校验"方案产品 == 工单产品"，不一致直接拒绝，避免换线后跑错程序；
- 仍保留"加载方案…"按钮，允许临时加载其它方案（产品不一致时同样会被拦截）。

### 4.17 用户管理

管理员可见"用户"页：

| 操作 | 说明 |
|---|---|
| 新建用户 | 用户名（3~32 位字母/数字/_.-）+ 初始口令（≥6 位）+ 角色 |
| 重置口令 | 随机盐 + 1 万轮 SHA256，旧口令立即失效 |
| 修改角色 | 管理员 / 操作员 / 观察员 |
| 启用/停用 | 停用后无法登录（登录记录写审计"账号已停用"） |

保护规则：不能停用/降级自己；系统必须保留至少一个启用状态的管理员。所有变更写审计。

### 4.18 追溯检索与审计导出

- "历史"页新增检索条件：关键字（SN / 工单号 / 操作员 / 方案名）+ 判定（PASS/FAIL/ABORTED）；
- 关键字按字面匹配（`%`、`_` 不作为通配符），最多返回 500 条；
- 检索结果可直接用于"批量 PDF"，CSV/PDF 导出仍以选中运行为准；
- "审计"页支持导出最近 500 条审计为 CSV（UTF-8 BOM），导出动作本身也写审计。

### 4.19 合格证标签（PASS 自动生成）

```json
"label": { "enabled": true, "directory": "", "widthMm": 60, "heightMm": 40 }
```

- 判定 PASS 且落库成功后，按 SN + 时间戳生成 `<SN>_<yyyyMMdd_HHmmss>.pdf`；
- 默认目录 `%APPDATA%/AegisLab/AegisHMI/labels`，可用 `directory` 指向共享目录；
- 内容：公司、SN（大号）、产品、工单、时间、检验员/班次、PASS；
- 生成成功/失败都写审计，结果摘要区显示文件名或失败原因；
- 标签尺寸由 `widthMm/heightMm` 决定，可直接送到标签打印机（打印 PDF 即可）。

### 4.20 SPC 联动停线

```json
"spc": { "andonOnCpk": true, "cpkThreshold": 1.0, "minSamples": 10 }
```

- 每次运行落库后，对流程里"有实测值 + 有规格限"的 measure 步骤计算 Cpk；
- 样本数达到 `minSamples` 且 Cpk 低于 `cpkThreshold` 时，复用 Andon 事件锁定工位；
- 停线原因包含测试项名称、Cpk、阈值与样本数，便于工艺直接定位；
- 与"连续 FAIL / FPY"规则共用 `andon_event` 与解除流程。

### 4.21 波形缩放、游标与限值叠加

- 鼠标滚轮在波形上缩放时间窗（1~10 秒），松手后继续按新窗口滚动；
- 鼠标移动显示竖向游标：时刻 + 前 8 个通道的读数（右上角信息框）；
- 双游标测量：左键在波形上定位 A、右键定位 B，左上角显示 ΔT 与前 8 通道 `A / B / Δ`，双击清除
  （右键不会弹菜单）；断线或换设备时游标自动清除；
- 通道配置了 `high/low` 时，波形上叠加对应通道颜色的虚线（H/L），现场不用翻配置；
- 绘制仍是"每像素 min/max 抽稀"，缩放不增加绘制点数，10kHz 下保持 20fps。

### 4.22 原始波形捕获与导出

"监控"页点"捕获波形…"开始累积最近 20 秒原始采样（全部通道），再次点击保存：

- CSV 头：`time_s, CH0(mV), CH1(mA), ...`，UTF-8 BOM，Excel 直接打开；
- 捕获期间不影响波形显示（复用 onTick 已消费的数据块，不额外读环）；
- 达到 20 秒自动停止，按钮变为"保存波形…"；
- 切换设备/断线会清空未保存的捕获，避免不同设备数据混在一起。

### 4.23 断线自动重连与报文诊断

- 连接意外断开（拔网线、模拟器重启）时按 1/2/4/8/16 秒退避自动重连（封顶 16 秒）；
- 手动点"断开"或重新"连接"会取消排队的重连，不会出现"用户已放弃、后台还在连"；
- 日志显示"连接已断开，N 秒后自动重连（第 K 次）"与"正在自动重连…"；
- "报文诊断（HEX）"开关打开后，日志显示收/发报文前 96 字节的十六进制，
  数据帧高频时按条打印会较多，仅建议排障时临时开启。

### 4.24 通道校准与日常点检

通道配置支持线性校准与金样点检：

```json
{ "name": "CH0", "unit": "mV",
  "gain": 1.05, "offset": -12,
  "check": 5000, "checkTolerance": 50 }
```

- 校准值 `value = raw × gain + offset` 在收包路径统一应用，
  显示、SPC、测试判定、导出用的都是校准后的值；
- "日常点检"按钮对配置了 `check` 的通道各采集 500ms 平均值，
  与 `check ± checkTolerance` 比较，弹出逐通道结果并写审计；
- 未配置 `check` 的通道不参与点检（提示如何配置）。

### 4.25 日报与数据库维护

- "生产"页"导出日报"：按日期 + 班次聚合当天运行数/PASS/FAIL/产量/良率，导出 CSV；
  有当前工单时只统计该工单，无工单时统计全部；
- "审计"页"备份数据库…"：`VACUUM INTO` 生成一致副本（含 WAL 已提交数据），默认放到
  `%APPDATA%/AegisLab/AegisHMI/backups`；
- "压缩数据库"：`VACUUM` 回收删除记录空间，操作期间短暂锁库；
- 备份/压缩都写审计；摘要表仍按 7 天自动清理。

### 4.26 SN 清单（发货清单防呆）

- "生产"页"导入 SN 清单…"支持 CSV/TXT，一行一个 SN（CSV 取第一列），自动去重；
- 导入后工单信息显示"SN清单：N 条"；
- 清单非空时，"自动测试"只接受清单内的 SN，否则提示"不在本工单发货清单内"；
- 清单独立于运行记录，不影响历史数据；导入动作写审计（文件、条数）。

---

## 5. 测试

```bat
ctest --test-dir build-mingw --output-on-failure
```

| 测试目标 | 覆盖内容 |
|---|---|
| tst_core | CRC16、帧编解码、粘包/分包、非法帧、环形缓冲 |
| tst_flow | JSON 加载、字段校验、变量替换、异常配置、script 步骤校验、方案库扫描与非法 JSON、编辑器写回 roundtrip |
| tst_engine | 通过/失败/超时/重试/中止、Mock 设备、script 脚本执行 |
| tst_deviceservice | 进程内假设备 + 假 Modbus 服务端、命令应答、Modbus 写寄存器、多设备并发 |
| tst_repository | SQLite 保存/查询/步骤/CSV、摘要/趋势/清理、用户登录/审计、报警、工单/FPY 统计、outbox、FAI/不良原因/治具、SPC 数据源、Andon、交接班、追溯检索、方案绑定、用户停用/角色/口令 |
| tst_modbus | MBAP/PDU、RTU CRC、异常码、半包 |
| tst_opcua | 进程内 UA 服务端 + 客户端读节点、错误端点 |
| tst_scpi | 进程内假仪器（QTcpServer）：查询应答、只写确认、无响应超时、周期轮询产出测量值 |
| tst_mes | 进程内 HTTP 假服务端：入队 → POST → SENT，未启用时不入队 |
| tst_spc | Cp/Cpk、I-MR 控制限、越限/同侧/趋势判异、单点与零波动边界 |
| tst_report | 工位配置解析（SOP/资质/班次/Andon/SPC/标签/报告）+ 单次/批量 PDF + 合格证标签 |

性能基准（可重复运行）：

```bat
build-mingw\bin\aegis_bench.exe
```

本机（MinGW 13 / Release）实测：单帧解码约 0.01 ms（约 58~64 M 采样点/秒），
环形缓冲 push+pop 约 1.2~1.3 ns/次。详见 [测试方案与性能报告](docs/04_测试方案与性能报告.md)。

---

## 6. 协议与命令

自定义帧格式：

```
AA 55 | len(u16 LE) | seq(u16 LE) | type(u8) | payload | CRC16(u16 LE)
```

type：`0x01` DATA、`0x02` INFO、`0x10` PING、`0x11` SET_RATE、`0x20` COMMAND、`0x81` ACK。
DATA 负载 = u32 时间戳(ms) + N 通道 int16 交叠采样；COMMAND/ACK 均 4 字节。

| 命令 | cmdId | 语义 | 应答 status |
|---|---|---|---|
| SET_VOUT | 0x01 | 设置输出电压（mV） | 0 成功 / 1 越界 |
| SET_IOUT | 0x02 | 设置输出电流（mA） | 0 成功 / 1 越界 |
| READ_MEASURE | 0x03 | 读指定通道测量值 | 0 成功 |
| RESET | 0x04 | 恢复默认设定 | 0 成功 |

Modbus 模式下 SET_VOUT/SET_IOUT 映射为 FC06 写寄存器 0/1；
OPC UA 模式下映射为写节点 0/1（Double，mV/mA）；
SCPI 模式下 SET_VOUT → `SOUR:VOLT <V>`、SET_IOUT → `SOUR:CURR <A>`、RESET → `*RST`，
测量值来自周期查询（默认 `MEAS:VOLT:DC?`）的结果乘 `scpiScale` 后写入 CH0。

---

## 7. 已知限制与未验证项

- **Modbus RTU 运行期未验证**：本机没有串口/虚拟串口对，仅通过编解码单测与代码评审验证；
  有硬件时用 `connectModbusRtu("COM3", 9600, 1)` 即可。
- **OPC UA 写入限制**：仅支持 Double 标量节点（0/1）；如需写 Float/Int 或有符号类型，需要扩展 `OpcUaClient::writeValue` 的类型分支。
- **Linux / Docker / CI 脚本未在本机实际执行**（本机为 Windows），需推送到 GitHub 或在 Linux 上验证。
- **脚本步骤**：同步执行，单个 `sleep` 上限 60 秒；不开放文件/网络访问，不提供脚本调试器；
  复杂逻辑建议拆分到多个步骤，便于定位失败位置。
- **报警阈值**：依据每秒摘要的 avg 判定，短于 1 秒的瞬态尖峰可能不报警；需要捕捉毛刺时应加 `measure` 步骤或降低判定粒度（当前未提供）。
- **工单/防呆范围**：一码一测只覆盖"本工位 + 本工单"；不做跨工位/跨线体在线防重，也不做物料/上料防错（属于 MES 范畴）。
- **首件检验**：确认权限落在"操作员/管理员"，未接入独立质检角色；重新首件已要求双人复核。
- **治具管理**：按使用次数与日期判断，不接入真实校准仪器/电子标签。
- **MES 对接**：仅 HTTP JSON POST，不含鉴权/签名/重放保护，建议经 MES 网关接入；不做 MES 下发工单、配方或停线指令。
- **生产看板**：统计基于本机 SQLite，多工位汇总需由 MES/中央数据库完成。
- **SPC**：按每件一个测量值做单值-移动极差（I-MR）图，不做子组抽样（X̄-R）与自动调限；
  数据量按最近 500 个样本统计，超大批量建议由 MES 侧做长期趋势分析。
- **电子 SOP**：内容来自 `station.json`，配图使用本地文件路径；不支持在线编辑器与签核流程。
- **Andon**：只锁定本工位并提示，不联动声光柱、不向 MES 下发停线指令；解除权限仅管理员，且需第二人复核。
- **交接班**：记录的是当前工单累计快照与备注，不做班次自动结算与工时统计。
- **PDF 报告**：使用系统字体渲染中文（Microsoft YaHei），Linux 上需确认已安装中文字体。
- **合格证标签**：生成的是 PDF，实际打印由系统打印或标签软件完成；不直接驱动 ZPL/TSPL 指令。
- **SPC 联动**：Cpk 基于最近 100 个样本估计，样本较少时波动较大；已用 `minSamples` 避免刚开工误停线。
- **MFC 旧版客户端未实现**：需要 Visual Studio 的 MFC 工作负载，属于可选项，不在当前交付范围。
- **演示视频未录制**：演示步骤见 `docs/测试流程与界面设计.md` 与下文"2 分钟演示脚本"。

2 分钟演示脚本：启动模拟器 → 上位机登录 → 生产页新建工单 → 连接设备 →
加载"电源模块出厂测试" → 扫码 SN 回车（首次勾选 SOP 已阅读）→ 步骤逐行变绿/变红 →
生产页查看 FPY/UPH → 历史页导出 CSV / PDF → SPC 页查看 Cp/Cpk 与控制图。

---

## 8. 文档索引

| 文档 | 内容 |
|---|---|
| [需求规格说明](docs/01_需求规格说明.md) | 角色、场景、功能/非功能需求、验收标准 |
| [概要设计](docs/02_概要设计.md) | 分层架构、线程模型、关键决策与权衡 |
| [详细设计](docs/03_详细设计.md) | 数据结构、状态机、接口、时序、存储表结构 |
| [测试方案与性能报告](docs/04_测试方案与性能报告.md) | 测试策略、覆盖矩阵、实测数据 |
| [设备协议接入说明](docs/05_设备协议接入说明.md) | 自定义帧 / Modbus / OPC UA 的报文与映射 |
| [功能对标与开发计划](docs/06_功能对标与开发计划.md) | 市场功能全景、P0/P1/P2 排期与完成状态 |
| [待补全功能开发计划](docs/07_待补全功能开发计划.md) | 剩余功能（版本校验/多链路/多语言/自动更新/GPIB）的方案、批次与验收 |
| [测试流程与界面设计](docs/测试流程与界面设计.md) | JSON 流程格式、界面线框、里程碑 |
| [岗位分析与项目设计](../杭州C++上位机岗位分析与项目设计.md) | 岗位采集结论、能力模型与项目定位 |
