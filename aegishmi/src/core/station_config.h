#pragma once // 头文件保护

#include <QDateTime>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace aegis::core {

// 一条电子作业指导书（SOP）。按产品匹配，测试前强制阅读。
struct SopDocument {
    QString product;   // 适用产品；空或 "*" 表示通用
    QString title;     // 标题
    QString version;   // 版本号
    QStringList steps; // 图文步骤（纯文本，界面按序号渲染）
    QString imagePath; // 可选配图（本地绝对路径）
};

// 班次定义：一天内的起止分钟数，支持跨零点（如 20:00 → 08:00）。
struct ShiftDefinition {
    QString name;
    int startMinute = 0;
    int endMinute = 0;
};

// =============================================================================
// StationConfig —— 测试工位配置
//
// 把"这台工位是谁、属于哪条线、用哪个治具、SN 长什么样、测试数据传到哪"
// 从代码里挪到配置，换线/换工位只改 JSON。
//
// 配置文件示例（configs/station.json）：
// {
//   "stationId": "ST-01",
//   "lineId": "L1",
//   "fixtureId": "FX-5V-01",
//   "snPattern": "^[A-Za-z0-9_-]{6,32}$",
//   "mes": { "enabled": false, "url": "http://127.0.0.1:8080/api/test-results",
//            "timeoutMs": 5000, "maxAttempts": 5, "retryBaseMs": 2000 }
// }
//
// 读取顺序：
//   1. 环境变量 AEGIS_STATION_CONFIG；
//   2. exe/configs/station.json（dist 部署形态）；
//   3. exe/../../configs/station.json（build/bin 开发形态）；
//   4. 都没有则使用内置默认值（MES 关闭）。
// =============================================================================
struct StationConfig {
    QString stationId = QStringLiteral("ST-01"); // 工位号
    QString lineId = QStringLiteral("L1");       // 线体号
    QString fixtureId;                           // 治具/夹具编号（可选）
    // SN 校验正则：默认 6~32 位字母/数字/下划线/短横线，覆盖常见条码。
    QString snPattern = QStringLiteral("^[A-Za-z0-9_-]{6,32}$");

    // ---- 首件检验（FAI）----
    bool faiEnabled = true;     // 新工单是否需要首件确认
    int faiSampleCount = 1;     // 首件需要连续测试并确认的件数

    // ---- 不良原因（测试失败时选择）----
    QStringList failReasons = {
        QStringLiteral("焊接不良"),
        QStringLiteral("元件失效"),
        QStringLiteral("参数漂移"),
        QStringLiteral("装配错误"),
        QStringLiteral("其他"),
    };

    // ---- 治具寿命与校准 ----
    int fixtureMaxRuns = 0;          // 寿命次数，0 表示不限制
    int fixtureCalibrationDays = 365; // 校准周期（天）
    QString fixtureCalibratedUntil;   // 初始校准到期日 yyyy-MM-dd（可选）
    qint64 fixtureCalibratedUntilMs = 0; // 上面的日期转毫秒（0 表示未配置）

    // 计算首次使用时应写入的校准到期时间：优先用配置日期，否则 now + 周期。
    qint64 defaultFixtureDueMs(qint64 nowMs) const;

    // ---- 电子 SOP（作业指导书）----
    bool sopEnabled = false;      // 是否启用电子 SOP
    bool sopRequireRead = true;   // 是否必须勾选"已阅读"才能开始测试
    QVector<SopDocument> sopDocuments; // 按产品配置的指导书

    // ---- 操作员资质 ----
    bool qualificationEnabled = false;
    // 产品 → 允许操作的用户名列表；产品键写 "*" 表示默认规则。
    // 某产品既没有专属条目也没有 "*" 条目时视为不限制。
    QMap<QString, QStringList> qualifiedOperators;

    // ---- 班次 ----
    QVector<ShiftDefinition> shifts;

    // ---- Andon 停线 ----
    bool andonEnabled = false;
    int andonConsecutiveFails = 3;  // 连续 FAIL 达到该值即停线
    double andonFpyThreshold = 80.0; // FPY 低于该百分比即停线（%）
    int andonMinUnits = 5;           // 产量达到该值后才按 FPY 判定

    // ---- SPC 判异联动停线 ----
    bool spcAndonEnabled = false;    // Cpk 低于阈值时自动停线
    double spcCpkThreshold = 1.0;    // Cpk 下限
    int spcMinSamples = 10;          // 样本数达到该值才判定

    // ---- 合格证标签 ----
    bool labelEnabled = false;       // PASS 后自动生成标签 PDF
    QString labelDirectory;          // 输出目录（空 = AppData/labels）
    int labelWidthMm = 60;
    int labelHeightMm = 40;

    // ---- 审核报告 ----
    QString reportCompany = QStringLiteral("AegisLab");
    QString reportTitle = QStringLiteral("出厂测试报告");
    QString reportLogoPath;               // Logo 图片路径（空 = 不显示）
    QString reportFooter = QStringLiteral(
        "AegisHMI 质量管理系统 · 本报告由系统自动生成"); // 页脚（空 = 不输出）
    QString reportAccentColor = QStringLiteral("#2c3e50"); // 标题主色
    bool reportShowSignature = true;      // 是否输出签名栏
    QString reportInspectorLabel = QStringLiteral("检验员");
    QString reportReviewerLabel = QStringLiteral("审核");

    bool mesEnabled = false;                     // 是否上传 MES
    QString mesUrl;                              // HTTP 接口地址
    int mesTimeoutMs = 5000;                     // 单次请求超时
    int mesMaxAttempts = 5;                      // 最大尝试次数
    int mesRetryBaseMs = 2000;                   // 首次重试间隔（之后指数退避）

    // ---- 工位侧物料防错（PL-08）----
    // BOM 只读核对接口：留空表示不启用；可写相对路径（如 /api/bom/check，
    // 基于 mesUrl 拼接）或完整 URL。
    QString mesBomCheckPath;
    // 过站/工单校验接口（MQ-02）：留空表示不启用；相对路径基于 mesUrl 拼接。
    QString mesStationCheckPath;
    // MES 不可用时的策略：true = 提示后放行（默认，避免停线）；
    // false = 严格模式，锁定工位。
    bool mesFailOpen = true;

    // ---- 用户插件（TP-11）----
    QStringList plugins;                    // 白名单（plugins.enabled）
    QString pluginsDir = QStringLiteral("plugins"); // 插件根目录
    int pluginTimeoutMs = 5000;             // 单次调用默认超时

    // ---- 质检复核（QA-07+）----
    // true = 关键操作复核人必须为质检员/管理员；
    // false = 保留"操作员也可复核"的历史行为。
    bool qualityApprovalOnly = false;

    // ---- 轻量版本检测（SY-05）----
    bool updateEnabled = false;              // 是否启用版本检测
    QString updateManifest;                  // version.json 路径或 http(s) 地址
    bool updateCheckOnStartup = true;        // 启动后自动检查一次

    // 读取指定配置文件；失败时返回默认配置，并通过 error 返回原因。
    static StationConfig load(const QString& path, QString* error = nullptr);
    // 按上面的搜索顺序读取"当前生效"的工位配置。
    static StationConfig loadActive();

    // ---- 辅助查询 ----
    // 找到适用于该产品的 SOP：优先精确产品，其次通用条目；找不到返回 nullptr。
    const SopDocument* sopForProduct(const QString& product) const;
    // 操作员是否有该产品的操作资质（未启用/未配置该产品时视为允许）。
    bool isOperatorQualified(const QString& product, const QString& user) const;
    // 指定时刻所属班次名称；未配置班次返回空字符串。
    QString shiftAt(const QDateTime& time) const;
};

} // namespace aegis::core
