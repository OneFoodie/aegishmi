#pragma once // 头文件保护

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

namespace aegis::test {

// =============================================================================
// 测试流程领域模型
//
// 这一层是"业务数据"，不依赖界面、不依赖通信，方便单元测试。
// FlowLoader 负责把 JSON 变成这里的结构，FlowValidator 负责校验合法性。
// =============================================================================

// 步骤动作类型。声明式流程只支持这几种，够覆盖"下发命令 → 等待 → 测量"。
enum class StepAction {
    Command, // 下发命令并等待应答
    Delay,   // 纯等待（让设备输出稳定）
    Measure, // 采集一段窗口的数据并做统计判定
    Assert,      // 预留：对已有结果做组合判断
    Script,      // 执行一段 JavaScript（QJSEngine），可组合命令/采集/判定
    ReadVersion, // 读取设备版本号（字符串）并与期望值/正则比对
    Plugin,      // 调用用户插件（独立进程 + JSON stdio），由插件返回判定结果
    SubFlow,     // TP-12a：调用另一个流程文件（相对当前方案所在目录）
    Prompt,      // TP-14：提示操作员确认（取消或超时按失败处理）
    Unknown,     // 解析失败时的兜底值，校验阶段会报错
};

// measure 步骤使用的统计量。
enum class Metric {
    Avg,         // 平均值
    Min,         // 最小值
    Max,         // 最大值
    Rms,         // 有效值
    PeakToPeak,  // 峰峰值（最大值 - 最小值）
    Unknown,     // 解析失败
};

// 判定条件。不同 action 使用不同字段：
//   command：ackStatus
//   measure：metric + min/max + unit
//   readVersion：version（精确匹配）/ versionPattern（正则匹配），二者取其一
struct ExpectSpec {
    bool present = false;      // JSON 里是否写了 expect
    int ackStatus = 0;         // command 期望的应答状态码（0 = 成功）
    Metric metric = Metric::Unknown;
    bool hasMin = false;       // 是否配置了下限
    bool hasMax = false;       // 是否配置了上限
    double min = 0.0;
    double max = 0.0;
    QString unit;              // 仅用于显示/报告，不参与判定
    QString version;           // readVersion：期望版本号（去空格后精确比对）
    QString versionPattern;    // readVersion：期望版本正则（与 version 互斥）
};

// 一个测试步骤。
struct TestStep {
    // ---- 通用字段 ----
    QString id;                // 步骤唯一标识
    QString name;              // 显示名称
    StepAction action = StepAction::Unknown;
    QJsonObject params;        // 原始参数（保留，便于将来扩展/记录）
    ExpectSpec expect;         // 判定条件
    int timeoutMs = -1;        // -1 表示使用流程默认超时
    int retries = 0;           // 失败重试次数
    QString onFail;            // 空表示继承流程配置；"abort" / "continue"
    // 流程控制（P1）：
    //   condition 为 JS 表达式，false 时跳过本步；可用 variables 与 lastValue。
    //   gotoStep 非空时，本步执行完跳转到该 id（配合 condition 可实现循环）。
    QString condition;
    QString gotoStep;

    // ---- 从 params 提取的便捷字段（避免执行时反复查 JSON） ----
    QString link;              // 目标链路 id（多链路设备；空表示主链路）
    QString cmd;               // command：命令名，如 SET_VOUT；readVersion：可选查询命令（如 *IDN?）
    int channel = -1;          // measure / command：通道号
    int valueMv = 0;           // command：数值参数（毫伏/电流等，含义由命令决定）
    int windowMs = 0;          // measure：采样窗口长度
    int delayMs = 0;           // delay：等待毫秒数
    QString script;            // script：JavaScript 源码（可用 setVout/measureAvg 等）
    QString plugin;            // plugin：插件名（plugins/<name>/manifest.json）
    QString flowRef;           // subFlow：子流程文件（相对当前方案所在目录）
    QString promptText;        // prompt：提示操作员的文本
    int promptTimeoutMs = 0;   // prompt：确认等待时长，0 表示不限时
};

// 一个完整测试流程。
struct TestFlow {
    int schemaVersion = 0;
    QString id;
    QString name;
    QString description;
    QString product;
    QString version;                       // 方案版本号（TP-03，可选）
    QString checksum;                      // 写入时生成的 SHA-256（加载时比对完整性）
    int defaultTimeoutMs = 1000;   // 单步默认超时
    QString onFailure = QStringLiteral("abort"); // abort / continue
    QMap<QString, QString> variables; // 变量表（统一按字符串存储，使用时再转换）
    QVector<TestStep> steps;

    // ---- 运行期上下文（不参与序列化）----
    // FlowLoader::loadFromFile 会填入，用于解析子流程的相对路径与循环引用检测。
    QString sourcePath; // 加载来源文件（内存加载时为空）
    QString baseDir;    // 来源文件所在目录
};

// 一条配置问题（加载错误或校验错误）。
// fieldPath 用于定位，例如 "steps[2].params.channel"。
struct FlowIssue {
    QString fieldPath;
    QString message;
};

} // namespace aegis::test
