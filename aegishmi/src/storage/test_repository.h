#pragma once // 头文件保护

#include "test/flow_types.h"
#include "test/test_result.h"

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVector>

class QSqlQuery; // 前置声明：readRunRow 只用到引用

namespace aegis::storage {

// 一条测试记录的摘要（历史列表一行）。
struct HistoryRun {
    int id = 0;
    QString flowId;
    QString flowName;
    QString product;
    QString sn;
    QString verdict;   // PASS / FAIL / ABORTED
    qint64 startedAt = 0;  // Unix 毫秒
    qint64 finishedAt = 0;
    int durationMs = 0;

    // ---- 产线追溯字段 ----
    QString workOrder;    // 工单号
    QString stationId;    // 工位号
    QString operatorName; // 操作员
    QString fixture;      // 治具编号
    bool retest = false;  // 是否复测
    QString failReason;   // 不良原因（FAIL 时填写）
    QString uploadStatus; // MES 上传状态：空/PENDING/SENT/DEAD
    QString shift;        // 班次（未配置班次时为空）
};

// 某次运行里的一个步骤明细（历史详情一行）。
struct HistoryStep {
    int index = 0;
    QString stepId;
    QString name;
    QString action;
    QString status;      // PASS / FAIL / SKIPPED / ABORTED / NOTRUN
    double measured = 0.0;
    bool hasMeasured = false;
    QString metric;
    QString unit;
    int elapsedMs = 0;
    QString message;
    // 规格限（来自流程配置，用于 SPC 与报告展示）
    bool hasUsl = false;
    bool hasLsl = false;
    double usl = 0.0;
    double lsl = 0.0;
};

// 波形摘要的一行（1 秒 × 1 通道）。
struct SummaryRow {
    QString deviceId;   // 设备 id（来自 fleet.json）
    int channel = 0;    // 通道号
    qint64 epochSec = 0; // Unix 秒
    double minValue = 0.0;
    double maxValue = 0.0;
    double avgValue = 0.0;
    int sampleCount = 0;
};

// 保存一次运行时附带的产线元信息。
struct RunMeta {
    QString sn;
    QString workOrder;
    QString lineId;
    QString stationId;
    QString operatorName;
    QString fixture;
    QString shift;        // 班次（可为空）
    bool retest = false;
};

// 生产工单。
struct WorkOrder {
    int id = 0;
    QString orderNo;
    QString product;
    int planQty = 0;
    QString status = QStringLiteral("ACTIVE"); // ACTIVE / CLOSED
    qint64 createdAt = 0;
    qint64 closedAt = 0;

    // ---- 首件检验（FAI）----
    bool faiRequired = false;
    QString faiStatus = QStringLiteral("CONFIRMED"); // PENDING / WAITING / CONFIRMED / REJECTED
    int faiTested = 0;        // 已参与首件的件数
    int faiRequiredCount = 1; // 需要连续确认的件数
    QString faiConfirmedBy;
    qint64 faiConfirmedAt = 0;

    // ---- 绑定的测试方案 ----
    QString flowPath; // 工单绑定的流程 JSON 路径（空表示不绑定）
};

// 某工单的生产统计（看板用）。
struct WorkOrderStats {
    int totalRuns = 0;     // 总测试次数（含复测）
    int passRuns = 0;
    int failRuns = 0;
    int abortedRuns = 0;
    int retestRuns = 0;    // 复测次数
    int retestPassRuns = 0; // 复测通过次数
    int units = 0;         // 去重 SN 数（实际产量）
    int firstPassUnits = 0; // 首次即通过的 SN 数
    int pendingRepairUnits = 0; // 最近一次仍为 FAIL 的 SN 数（待返修）
    double fpy = 0.0;      // 直通率 = firstPassUnits / units
    double uph = 0.0;      // 单位小时产出（按首件测试至今估算）
};

// 不良步骤排名（Pareto）。
struct FailureRank {
    QString stepName;
    QString action;
    int count = 0;
};

// 不良原因排名（Pareto）。
struct ReasonRank {
    QString reason;
    int count = 0;
};

// 工步耗时排名（瓶颈分析：找出最拖时间的步骤）。
struct StepDurationRank {
    QString stepName;
    QString action;
    int runs = 0;       // 有耗时的执行次数
    double avgMs = 0.0; // 平均耗时
    qint64 maxMs = 0;   // 最长一次
    qint64 totalMs = 0; // 累计耗时
};

// 治具使用状态。
struct FixtureStatus {
    QString fixtureId;
    int runCount = 0;
    qint64 lastUsedAt = 0;
    qint64 calibratedUntil = 0; // 0 表示未初始化/未配置
};

// SPC 可选测试项：某工单内已产生实测值、且流程里配置了规格限的步骤。
struct SpcItem {
    QString flowId;
    QString flowName;
    QString stepId;
    QString stepName;
    QString unit;
    bool hasUsl = false;
    bool hasLsl = false;
    double usl = 0.0;
    double lsl = 0.0;
    int count = 0;      // 已有样本数
};

// SPC 单个数据点（一次运行的实测值）。
struct SpcPoint {
    double value = 0.0;
    qint64 at = 0;      // 运行完成时间（Unix 毫秒）
    QString sn;
};

// Andon 停线状态（当前未解除的停线事件）。
struct AndonState {
    bool locked = false;
    QString reason;
    QString detail;
    qint64 createdAt = 0;
};

// 交接班记录。
struct HandoverRecord {
    int id = 0;
    QString orderNo;
    QString lineId;
    QString stationId;
    QString shift;
    QString operatorName;
    QString note;
    int units = 0;
    int passRuns = 0;
    int failRuns = 0;
    qint64 createdAt = 0;
};

// 日报/班次报表的一行（按日期 + 班次聚合）。
struct ProductionReportRow {
    QString date;       // yyyy-MM-dd（本地时区）
    QString shift;      // 未配置班次时显示"未配置"
    int runs = 0;
    int passRuns = 0;
    int failRuns = 0;
    int units = 0;      // 去重 SN 数
    int passUnits = 0;  // 出现 PASS 的去重 SN 数
    double yieldRate = 0.0; // passRuns / runs
};

// MES 上传队列中的一条待发送记录。
struct MesUploadItem {
    int id = 0;
    int runId = 0;
    QByteArray payload;
    int attempts = 0;
};

// =============================================================================
// TestRepository —— SQLite 存储
//
// 职责：
//   1. 建库建表（首次运行自动创建，并对旧库做加列迁移）
//   2. 保存一次运行及其步骤明细（含工单/工位/操作员/治具/复测）
//   3. 查询历史列表/步骤明细
//   4. 导出 CSV（带 UTF-8 BOM，Excel 打开不乱码）
//   5. 波形摘要：批量写入、按时间范围查询、过期清理
//   6. 生产工单与看板统计
//   7. MES 上传 outbox（断网补传）
//
// 说明：用独立的连接名，避免测试里多个实例互相干扰；
//       所有 SQL 都走预处理语句，避免拼接字符串。
// =============================================================================
class TestRepository {
public:
    TestRepository() = default;
    ~TestRepository();

    // 打开数据库并建表。失败时通过 errorMessage 返回原因。
    bool open(const QString& dbPath, QString* errorMessage = nullptr);
    bool isOpen() const { return db_.isOpen(); }

    // 保存一次运行（含所有步骤）。成功后可取 lastInsertedRunId()。
    bool saveRun(const test::TestFlow& flow, const test::RunResult& result,
                 const RunMeta& meta, QString* errorMessage = nullptr);

    int lastInsertedRunId() const { return lastRunId_; }

    // 最近的运行记录（按开始时间倒序）。
    QVector<HistoryRun> recentRuns(int limit = 200);

    // 追溯检索：keyword 匹配 SN / 工单 / 操作员 / 方案名；verdict 为空表示全部。
    QVector<HistoryRun> searchRuns(const QString& keyword, const QString& verdict,
                                   int limit = 500);

    // 按主键取一条运行记录（报告导出用）；不存在时返回空结构（id=0）。
    HistoryRun runById(int runId);

    // 指定运行的所有步骤明细（按步骤序号升序）。
    QVector<HistoryStep> stepsOfRun(int runId);

    // 导出为 CSV：一行一个步骤，前面带上整次运行的摘要信息。
    bool exportCsv(int runId, const QString& csvPath, QString* errorMessage = nullptr);

    // ---- 波形摘要 ----
    // 批量写入（调用方负责攒批；内部用单个事务）。
    bool saveSummaryBatch(const QVector<SummaryRow>& rows, QString* errorMessage = nullptr);

    // 查询某设备/通道在 [fromSec, toSec] 的秒级摘要（按时间升序，最多 maxRows 行）。
    QVector<SummaryRow> summaryTrend(const QString& deviceId, int channel,
                                     qint64 fromSec, qint64 toSec, int maxRows = 5000);

    // 删除早于 epochSec 的摘要，返回删除行数（保留策略）。
    int purgeSummariesBefore(qint64 epochSec);

    // ---- 生产工单 ----
    bool createWorkOrder(const QString& orderNo, const QString& product, int planQty,
                         bool faiRequired, int faiSampleCount,
                         QString* errorMessage = nullptr);
    // 带方案绑定的重载：flowPath 为空表示不绑定（与上面的版本等价）。
    bool createWorkOrder(const QString& orderNo, const QString& product, int planQty,
                         bool faiRequired, int faiSampleCount,
                         const QString& flowPath, QString* errorMessage = nullptr);
    QVector<WorkOrder> workOrders(int limit = 100);
    WorkOrder findWorkOrder(const QString& orderNo);
    bool closeWorkOrder(const QString& orderNo, QString* errorMessage = nullptr);

    // ---- 首件检验 ----
    // 运行结束后推进首件状态：PENDING → WAITING/REJECTED。
    bool updateFaiAfterRun(const QString& orderNo, const QString& verdict,
                           QString* errorMessage = nullptr);
    // 确认首件合格（WAITING → CONFIRMED）。
    bool confirmFai(const QString& orderNo, const QString& user,
                    QString* errorMessage = nullptr);
    // 首件不合格后重新开始（REJECTED → PENDING，计数清零）。
    bool resetFai(const QString& orderNo, QString* errorMessage = nullptr);

    // 同一工单内某个 SN 已测试次数（用于"一码一测"防呆）。
    int snTestCount(const QString& orderNo, const QString& sn);
    // 某工单内该 SN 最近一次运行（没有则返回空结构）。
    HistoryRun lastRunOfSn(const QString& orderNo, const QString& sn);
    // 某工单的运行记录（按时间倒序）。
    QVector<HistoryRun> runsOfOrder(const QString& orderNo, int limit = 200);

    // 看板统计与不良排名。
    WorkOrderStats statsOfOrder(const QString& orderNo);
    QVector<FailureRank> topFailureSteps(const QString& orderNo, int limit = 8);
    // 工步耗时 Top-N（按平均耗时倒序，用于瓶颈分析）。
    QVector<StepDurationRank> topSlowSteps(const QString& orderNo, int limit = 8);
    QVector<ReasonRank> topFailureReasons(const QString& orderNo, int limit = 8);

    // 更新某次运行的不良原因（FAIL 后由界面选择）。
    bool updateFailReason(int runId, const QString& reason);

    // ---- SPC（过程能力与控制图）----
    // 某工单内所有"有实测值 + 有规格限"的测试项（用于界面下拉框）。
    QVector<SpcItem> spcItems(const QString& orderNo);
    // 某工单某测试项的实测值（按运行时间升序，最多 limit 条）。
    QVector<SpcPoint> spcPoints(const QString& orderNo, const QString& flowId,
                                const QString& stepId, int limit = 500);
    // 该工单最近连续 FAIL 的次数（遇到 PASS/ABORTED 停止）。
    int consecutiveFailRuns(const QString& orderNo);

    // ---- Andon 停线 ----
    AndonState andonState(const QString& stationId);
    bool lockStation(const QString& stationId, const QString& reason,
                     const QString& detail);
    bool clearAndon(const QString& stationId, const QString& user);

    // ---- 交接班 ----
    bool saveHandover(const HandoverRecord& record, QString* errorMessage = nullptr);
    QVector<HandoverRecord> recentHandovers(int limit = 50);

    // ---- 日报/班次报表 ----
    QVector<ProductionReportRow> productionReport(qint64 fromMs, qint64 toMs,
                                                  const QString& workOrder = QString());

    // ---- SN 白名单（发货清单防呆）----
    // 导入清单（去重、忽略空行）；返回新增条数，空工单号返回 -1。
    int importSnWhitelist(const QString& orderNo, const QStringList& sns,
                          QString* errorMessage = nullptr);
    int snWhitelistCount(const QString& orderNo);
    bool snInWhitelist(const QString& orderNo, const QString& sn);

    // ---- 治具寿命/校准 ----
    FixtureStatus fixtureStatus(const QString& fixtureId);
    // 记录一次使用：不存在则插入，存在则 run_count+1。
    bool recordFixtureUse(const QString& fixtureId, qint64 calibratedUntilMs,
                          qint64 nowMs);
    // 复位/校准：计数清零并写入新的校准到期时间。
    bool resetFixture(const QString& fixtureId, qint64 calibratedUntilMs, qint64 nowMs);

    // ---- MES 上传 outbox ----
    bool enqueueUpload(int runId, const QByteArray& payload, qint64 nextAttemptAt,
                       QString* errorMessage = nullptr);
    QVector<MesUploadItem> dueUploads(qint64 nowMs, int limit = 1);
    bool markUploaded(int id, qint64 atMs);
    bool markUploadFailed(int id, int attempts, const QString& error, qint64 nextAttemptAt);
    bool markUploadDead(int id, int attempts, const QString& error);
    int uploadCountByStatus(const QString& status);
    bool requeueDeadUploads(qint64 nowMs);

    // 生成一次运行的 MES JSON 报文（含步骤明细）。找不到运行返回空对象。
    QByteArray buildRunPayload(int runId);

private:
    bool ensureSchema(QString* errorMessage);
    // 旧库加列：列不存在时才 ALTER TABLE（SQLite 不支持 ADD COLUMN IF NOT EXISTS）。
    bool ensureColumn(const QString& table, const QString& column,
                      const QString& definition, QString* errorMessage);
    HistoryRun readRunRow(const QSqlQuery& query, int offset) const;

    QSqlDatabase db_;
    QString connectionName_;
    int lastRunId_ = 0;
};

} // namespace aegis::storage
