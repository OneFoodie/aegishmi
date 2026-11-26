#include "storage/test_repository.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace aegis::storage {

namespace {

// ---- 枚举转文本：数据库里存可读字符串，便于直接查库和导出 ----

QString verdictText(test::RunVerdict verdict)
{
    switch (verdict) {
    case test::RunVerdict::Pass:
        return QStringLiteral("PASS");
    case test::RunVerdict::Fail:
        return QStringLiteral("FAIL");
    case test::RunVerdict::Aborted:
        return QStringLiteral("ABORTED");
    }
    return QStringLiteral("UNKNOWN");
}

QString statusText(test::StepStatus status)
{
    switch (status) {
    case test::StepStatus::Pass:
        return QStringLiteral("PASS");
    case test::StepStatus::Fail:
        return QStringLiteral("FAIL");
    case test::StepStatus::Skipped:
        return QStringLiteral("SKIPPED");
    case test::StepStatus::Aborted:
        return QStringLiteral("ABORTED");
    case test::StepStatus::NotRun:
        return QStringLiteral("NOTRUN");
    }
    return QStringLiteral("UNKNOWN");
}

QString actionText(test::StepAction action)
{
    switch (action) {
    case test::StepAction::Command:
        return QStringLiteral("command");
    case test::StepAction::Delay:
        return QStringLiteral("delay");
    case test::StepAction::Measure:
        return QStringLiteral("measure");
    case test::StepAction::Assert:
        return QStringLiteral("assert");
    case test::StepAction::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

QString metricText(test::Metric metric)
{
    switch (metric) {
    case test::Metric::Avg:
        return QStringLiteral("avg");
    case test::Metric::Min:
        return QStringLiteral("min");
    case test::Metric::Max:
        return QStringLiteral("max");
    case test::Metric::Rms:
        return QStringLiteral("rms");
    case test::Metric::PeakToPeak:
        return QStringLiteral("peakToPeak");
    case test::Metric::Unknown:
        break;
    }
    return QString();
}

// CSV 字段转义：含逗号/引号/换行时用双引号包裹，内部引号翻倍。
QString csvField(const QString& text)
{
    if (!text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char('"')))
        return text;

    QString escaped = text;
    escaped.replace(QLatin1String("\""), QLatin1String("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString formatTime(qint64 milliseconds)
{
    if (milliseconds <= 0)
        return QString();
    return QDateTime::fromMSecsSinceEpoch(milliseconds).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

} // namespace

TestRepository::~TestRepository()
{
    // 先关闭数据库，再移除连接，避免 Qt 报告 "connection still in use" 警告。
    if (db_.isValid()) {
        db_.close();
        db_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName_);
    }
}

bool TestRepository::open(const QString& dbPath, QString* errorMessage)
{
    // 如果已经打开过，先清理旧连接。
    if (db_.isValid()) {
        db_.close();
        db_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName_);
    }

    // 每个实例一个连接名，避免多实例（例如单元测试）互相覆盖。
    connectionName_ = QStringLiteral("aegis_repo_%1")
                          .arg(reinterpret_cast<quintptr>(this));

    db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    db_.setDatabaseName(dbPath);

    if (!db_.open()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("打开数据库失败：%1").arg(db_.lastError().text());
        return false;
    }

    // WAL 模式：写入更快、读写不互相阻塞。
    QSqlQuery pragma(db_);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    // 主线程（历史查询）与摘要写线程会同时访问数据库，遇到锁时等一会儿再报错。
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=3000"));

    return ensureSchema(errorMessage);
}

bool TestRepository::ensureSchema(QString* errorMessage)
{
    QSqlQuery query(db_);

    const QString runTable = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS test_run ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " flow_id TEXT NOT NULL,"
        " flow_name TEXT NOT NULL,"
        " product TEXT,"
        " sn TEXT,"
        " verdict TEXT NOT NULL,"
        " started_at INTEGER NOT NULL,"
        " finished_at INTEGER,"
        " duration_ms INTEGER NOT NULL DEFAULT 0,"
        " work_order TEXT,"
        " line_id TEXT,"
        " station_id TEXT,"
        " operator TEXT,"
        " fixture TEXT,"
        " retest INTEGER NOT NULL DEFAULT 0,"
        " fail_reason TEXT,"
        " shift TEXT)");

    if (!query.exec(runTable)) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("建表 test_run 失败：%1").arg(query.lastError().text());
        return false;
    }

    // 旧库兼容：逐列检查，缺哪列补哪列。
    const struct {
        const char* name;
        const char* definition;
    } runColumns[] = {
        {"work_order", "TEXT"},
        {"line_id", "TEXT"},
        {"station_id", "TEXT"},
        {"operator", "TEXT"},
        {"fixture", "TEXT"},
        {"retest", "INTEGER NOT NULL DEFAULT 0"},
        {"fail_reason", "TEXT"},
        {"shift", "TEXT"},
    };
    for (const auto& column : runColumns) {
        if (!ensureColumn(QStringLiteral("test_run"),
                          QString::fromLatin1(column.name),
                          QString::fromLatin1(column.definition), errorMessage)) {
            return false;
        }
    }

    // 工单/SN 组合查询是防呆高频路径，单独建索引。
    if (!query.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_run_order_sn"
            " ON test_run(work_order, sn)"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("创建工单/SN 索引失败：%1")
                                .arg(query.lastError().text());
        return false;
    }

    const QString stepTable = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS test_step ("
        " run_id INTEGER NOT NULL,"
        " step_index INTEGER NOT NULL,"
        " step_id TEXT NOT NULL,"
        " name TEXT NOT NULL,"
        " action TEXT NOT NULL,"
        " status TEXT NOT NULL,"
        " measured REAL,"
        " has_measured INTEGER NOT NULL DEFAULT 0,"
        " metric TEXT,"
        " unit TEXT,"
        " elapsed_ms INTEGER,"
        " message TEXT,"
        " min_limit REAL,"
        " max_limit REAL,"
        " PRIMARY KEY (run_id, step_index))");

    if (!query.exec(stepTable)) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("建表 test_step 失败：%1").arg(query.lastError().text());
        return false;
    }

    // 旧库兼容：SPC 需要的规格限列。
    const struct {
        const char* name;
        const char* definition;
    } stepColumns[] = {
        {"min_limit", "REAL"},
        {"max_limit", "REAL"},
    };
    for (const auto& column : stepColumns) {
        if (!ensureColumn(QStringLiteral("test_step"),
                          QString::fromLatin1(column.name),
                          QString::fromLatin1(column.definition), errorMessage)) {
            return false;
        }
    }

    // 波形摘要：设备 × 通道 × 秒 一行，只存 min/max/avg/count。
    const QString summaryTable = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS sample_summary ("
        " device_id TEXT NOT NULL,"
        " channel INTEGER NOT NULL,"
        " epoch_sec INTEGER NOT NULL,"
        " min_value REAL NOT NULL,"
        " max_value REAL NOT NULL,"
        " avg_value REAL NOT NULL,"
        " sample_count INTEGER NOT NULL,"
        " PRIMARY KEY (device_id, channel, epoch_sec))");

    if (!query.exec(summaryTable)) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("建表 sample_summary 失败：%1")
                                .arg(query.lastError().text());
        return false;
    }

    // 趋势查询按时间范围过滤，时间索引必不可少。
    if (!query.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_summary_time"
            " ON sample_summary(epoch_sec)"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("创建摘要时间索引失败：%1")
                                .arg(query.lastError().text());
        return false;
    }

    // 生产工单。
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS work_order ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " order_no TEXT NOT NULL UNIQUE,"
            " product TEXT NOT NULL,"
            " plan_qty INTEGER NOT NULL DEFAULT 0,"
            " status TEXT NOT NULL DEFAULT 'ACTIVE',"
            " created_at INTEGER NOT NULL,"
            " closed_at INTEGER,"
            " fai_required INTEGER NOT NULL DEFAULT 0,"
            " fai_status TEXT NOT NULL DEFAULT 'CONFIRMED',"
            " fai_tested INTEGER NOT NULL DEFAULT 0,"
            " fai_required_count INTEGER NOT NULL DEFAULT 1,"
            " fai_confirmed_by TEXT,"
            " fai_confirmed_at INTEGER,"
            " flow_path TEXT)"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("建表 work_order 失败：%1")
                                .arg(query.lastError().text());
        return false;
    }

    // 上一批建的 work_order 没有首件字段，这里做加列迁移。
    const struct {
        const char* name;
        const char* definition;
    } orderColumns[] = {
        {"fai_required", "INTEGER NOT NULL DEFAULT 0"},
        {"fai_status", "TEXT NOT NULL DEFAULT 'CONFIRMED'"},
        {"fai_tested", "INTEGER NOT NULL DEFAULT 0"},
        {"fai_required_count", "INTEGER NOT NULL DEFAULT 1"},
        {"fai_confirmed_by", "TEXT"},
        {"fai_confirmed_at", "INTEGER"},
        {"flow_path", "TEXT"},
    };
    for (const auto& column : orderColumns) {
        if (!ensureColumn(QStringLiteral("work_order"),
                          QString::fromLatin1(column.name),
                          QString::fromLatin1(column.definition), errorMessage)) {
            return false;
        }
    }

    // 治具使用次数与校准到期时间。
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS fixture_usage ("
            " fixture_id TEXT PRIMARY KEY,"
            " run_count INTEGER NOT NULL DEFAULT 0,"
            " last_used_at INTEGER,"
            " calibrated_until INTEGER)"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("建表 fixture_usage 失败：%1")
                                .arg(query.lastError().text());
        return false;
    }

    // MES 上传 outbox：一次运行最多一条；断网/重启后按 next_attempt_at 补传。
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS mes_outbox ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " run_id INTEGER NOT NULL UNIQUE,"
            " payload TEXT NOT NULL,"
            " status TEXT NOT NULL DEFAULT 'PENDING',"
            " attempts INTEGER NOT NULL DEFAULT 0,"
            " next_attempt_at INTEGER NOT NULL,"
            " last_error TEXT,"
            " created_at INTEGER NOT NULL,"
            " uploaded_at INTEGER)"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("建表 mes_outbox 失败：%1")
                                .arg(query.lastError().text());
        return false;
    }

    if (!query.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_outbox_due"
            " ON mes_outbox(status, next_attempt_at)"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("创建上传队列索引失败：%1")
                                .arg(query.lastError().text());
        return false;
    }

    // Andon 停线事件：一次停线一条记录，cleared_at 为空表示仍在停线。
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS andon_event ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " station_id TEXT NOT NULL,"
            " reason TEXT NOT NULL,"
            " detail TEXT,"
            " created_at INTEGER NOT NULL,"
            " cleared_at INTEGER,"
            " cleared_by TEXT)"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("建表 andon_event 失败：%1")
                                .arg(query.lastError().text());
        return false;
    }

    // 交接班记录：保存交班时的产量/良率快照与备注，便于追溯。
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS shift_handover ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " order_no TEXT,"
            " line_id TEXT,"
            " station_id TEXT,"
            " shift TEXT,"
            " operator TEXT NOT NULL,"
            " units INTEGER NOT NULL DEFAULT 0,"
            " pass_runs INTEGER NOT NULL DEFAULT 0,"
            " fail_runs INTEGER NOT NULL DEFAULT 0,"
            " note TEXT,"
            " created_at INTEGER NOT NULL)"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("建表 shift_handover 失败：%1")
                                .arg(query.lastError().text());
        return false;
    }

    // SN 白名单：按工单导入发货清单，测试时校验 SN 是否在清单内。
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS sn_whitelist ("
            " work_order TEXT NOT NULL,"
            " sn TEXT NOT NULL,"
            " PRIMARY KEY (work_order, sn))"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("建表 sn_whitelist 失败：%1")
                                .arg(query.lastError().text());
        return false;
    }

    return true;
}

// SQLite 的 ALTER TABLE 不支持 "ADD COLUMN IF NOT EXISTS"，这里先查 PRAGMA。
bool TestRepository::ensureColumn(const QString& table, const QString& column,
                                 const QString& definition, QString* errorMessage)
{
    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("读取表结构失败（%1）：%2")
                                .arg(table, query.lastError().text());
        return false;
    }

    while (query.next()) {
        if (query.value(1).toString() == column)
            return true; // 已存在
    }

    QSqlQuery alter(db_);
    if (!alter.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3")
                        .arg(table, column, definition))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("为 %1 增加列 %2 失败：%3")
                                .arg(table, column, alter.lastError().text());
        return false;
    }
    return true;
}

bool TestRepository::saveRun(const test::TestFlow& flow, const test::RunResult& result,
                             const RunMeta& meta, QString* errorMessage)
{
    if (!db_.isOpen()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开");
        return false;
    }

    const qint64 finishedAt = QDateTime::currentMSecsSinceEpoch();
    const qint64 startedAt = finishedAt - result.totalElapsedMs;

    // 事务：一次运行的所有步骤要么全写入，要么全不写。
    db_.transaction();

    QSqlQuery insertRun(db_);
    insertRun.prepare(QStringLiteral(
        "INSERT INTO test_run(flow_id, flow_name, product, sn, verdict,"
        " started_at, finished_at, duration_ms,"
        " work_order, line_id, station_id, operator, fixture, retest, shift)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    insertRun.addBindValue(flow.id);
    insertRun.addBindValue(flow.name);
    insertRun.addBindValue(flow.product);
    insertRun.addBindValue(meta.sn);
    insertRun.addBindValue(verdictText(result.verdict));
    insertRun.addBindValue(startedAt);
    insertRun.addBindValue(finishedAt);
    insertRun.addBindValue(result.totalElapsedMs);
    insertRun.addBindValue(meta.workOrder);
    insertRun.addBindValue(meta.lineId);
    insertRun.addBindValue(meta.stationId);
    insertRun.addBindValue(meta.operatorName);
    insertRun.addBindValue(meta.fixture);
    insertRun.addBindValue(meta.retest ? 1 : 0);
    insertRun.addBindValue(meta.shift);

    if (!insertRun.exec()) {
        db_.rollback();
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("写入运行记录失败：%1").arg(insertRun.lastError().text());
        return false;
    }

    const int runId = insertRun.lastInsertId().toInt();

    QSqlQuery insertStep(db_);
    insertStep.prepare(QStringLiteral(
        "INSERT INTO test_step(run_id, step_index, step_id, name, action, status,"
        " measured, has_measured, metric, unit, elapsed_ms, message,"
        " min_limit, max_limit)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));

    for (const auto& step : result.steps) {
        insertStep.addBindValue(runId);
        insertStep.addBindValue(step.index);
        insertStep.addBindValue(step.id);
        insertStep.addBindValue(step.name);
        insertStep.addBindValue(actionText(step.index < flow.steps.size()
                                               ? flow.steps.at(step.index).action
                                               : test::StepAction::Unknown));
        insertStep.addBindValue(statusText(step.status));
        insertStep.addBindValue(step.hasMeasured ? QVariant(step.measured) : QVariant());
        insertStep.addBindValue(step.hasMeasured ? 1 : 0);
        insertStep.addBindValue(step.index < flow.steps.size()
                                    ? metricText(flow.steps.at(step.index).expect.metric)
                                    : QString());
        insertStep.addBindValue(step.index < flow.steps.size()
                                    ? flow.steps.at(step.index).expect.unit
                                    : QString());
        insertStep.addBindValue(step.elapsedMs);
        insertStep.addBindValue(step.message);
        // 规格限直接取自流程配置：测试项名 + 上下限是可追溯的"配方"，必须随结果落库。
        const test::TestStep& spec = step.index < flow.steps.size()
                                         ? flow.steps.at(step.index)
                                         : test::TestStep{};
        insertStep.addBindValue(spec.expect.hasMin ? QVariant(spec.expect.min)
                                                   : QVariant());
        insertStep.addBindValue(spec.expect.hasMax ? QVariant(spec.expect.max)
                                                   : QVariant());

        if (!insertStep.exec()) {
            db_.rollback();
            if (errorMessage != nullptr)
                *errorMessage = QStringLiteral("写入步骤记录失败：%1").arg(insertStep.lastError().text());
            return false;
        }
    }

    if (!db_.commit()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("提交事务失败：%1").arg(db_.lastError().text());
        return false;
    }

    lastRunId_ = runId;
    return true;
}

// 统一的字段读取：SELECT 顺序与这里必须保持一致。
HistoryRun TestRepository::readRunRow(const QSqlQuery& query, int offset) const
{
    HistoryRun run;
    run.id = query.value(offset + 0).toInt();
    run.flowId = query.value(offset + 1).toString();
    run.flowName = query.value(offset + 2).toString();
    run.product = query.value(offset + 3).toString();
    run.sn = query.value(offset + 4).toString();
    run.verdict = query.value(offset + 5).toString();
    run.startedAt = query.value(offset + 6).toLongLong();
    run.finishedAt = query.value(offset + 7).toLongLong();
    run.durationMs = query.value(offset + 8).toInt();
    run.workOrder = query.value(offset + 9).toString();
    run.stationId = query.value(offset + 10).toString();
    run.operatorName = query.value(offset + 11).toString();
    run.fixture = query.value(offset + 12).toString();
    run.retest = query.value(offset + 13).toInt() != 0;
    run.failReason = query.value(offset + 14).toString();
    run.uploadStatus = query.value(offset + 15).toString();
    run.shift = query.value(offset + 16).toString();
    return run;
}

QVector<HistoryRun> TestRepository::recentRuns(int limit)
{
    QVector<HistoryRun> runs;
    if (!db_.isOpen())
        return runs;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT r.id, r.flow_id, r.flow_name, r.product, r.sn, r.verdict, r.started_at,"
        " r.finished_at, r.duration_ms, r.work_order, r.station_id, r.operator,"
        " r.fixture, r.retest, r.fail_reason, o.status, r.shift"
        " FROM test_run r LEFT JOIN mes_outbox o ON o.run_id = r.id"
        " ORDER BY r.id DESC LIMIT ?"));
    query.addBindValue(limit);

    if (!query.exec())
        return runs;

    while (query.next())
        runs.append(readRunRow(query, 0));
    return runs;
}

// 追溯检索：关键字匹配 SN/工单/操作员/方案名，判定可选。
QVector<HistoryRun> TestRepository::searchRuns(const QString& keyword,
                                               const QString& verdict, int limit)
{
    QVector<HistoryRun> runs;
    if (!db_.isOpen())
        return runs;

    QString key = keyword.trimmed();
    QString wantedVerdict = verdict.trimmed().toUpper();
    // 空关键字必须绑定成非 NULL 的空串：NULL = '' 在 SQLite 里结果是 NULL（不成立）。
    if (key.isNull())
        key = QStringLiteral("");
    if (wantedVerdict.isNull())
        wantedVerdict = QStringLiteral("");

    QSqlQuery query(db_);
    // 用 instr(lower(...)) 而不是 LIKE：避免用户输入的 %/_ 被当成通配符。
    // 统一用位置占位符（SQLite 驱动对命名占位符的重复绑定支持不一致）。
    query.prepare(QStringLiteral(
        "SELECT r.id, r.flow_id, r.flow_name, r.product, r.sn, r.verdict, r.started_at,"
        " r.finished_at, r.duration_ms, r.work_order, r.station_id, r.operator,"
        " r.fixture, r.retest, r.fail_reason, o.status, r.shift"
        " FROM test_run r LEFT JOIN mes_outbox o ON o.run_id = r.id"
        " WHERE (? = ''"
        "        OR instr(lower(COALESCE(r.sn, '')), lower(?)) > 0"
        "        OR instr(lower(COALESCE(r.work_order, '')), lower(?)) > 0"
        "        OR instr(lower(COALESCE(r.operator, '')), lower(?)) > 0"
        "        OR instr(lower(r.flow_name), lower(?)) > 0)"
        "   AND (? = '' OR r.verdict = ?)"
        " ORDER BY r.id DESC LIMIT ?"));
    for (int i = 0; i < 5; ++i)
        query.addBindValue(key);
    query.addBindValue(wantedVerdict);
    query.addBindValue(wantedVerdict);
    query.addBindValue(qMax(1, limit));

    if (!query.exec())
        return runs;

    while (query.next())
        runs.append(readRunRow(query, 0));
    return runs;
}

HistoryRun TestRepository::runById(int runId)
{
    if (!db_.isOpen() || runId <= 0)
        return {};

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT r.id, r.flow_id, r.flow_name, r.product, r.sn, r.verdict, r.started_at,"
        " r.finished_at, r.duration_ms, r.work_order, r.station_id, r.operator,"
        " r.fixture, r.retest, r.fail_reason, o.status, r.shift"
        " FROM test_run r LEFT JOIN mes_outbox o ON o.run_id = r.id"
        " WHERE r.id = ?"));
    query.addBindValue(runId);
    if (!query.exec() || !query.next())
        return {};
    return readRunRow(query, 0);
}

QVector<HistoryStep> TestRepository::stepsOfRun(int runId)
{
    QVector<HistoryStep> steps;
    if (!db_.isOpen())
        return steps;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT step_index, step_id, name, action, status, measured, has_measured,"
        " metric, unit, elapsed_ms, message, min_limit, max_limit FROM test_step"
        " WHERE run_id = ? ORDER BY step_index ASC"));
    query.addBindValue(runId);

    if (!query.exec())
        return steps;

    while (query.next()) {
        HistoryStep step;
        step.index = query.value(0).toInt();
        step.stepId = query.value(1).toString();
        step.name = query.value(2).toString();
        step.action = query.value(3).toString();
        step.status = query.value(4).toString();
        step.hasMeasured = query.value(6).toInt() != 0;
        step.measured = step.hasMeasured ? query.value(5).toDouble() : 0.0;
        step.metric = query.value(7).toString();
        step.unit = query.value(8).toString();
        step.elapsedMs = query.value(9).toInt();
        step.message = query.value(10).toString();
        step.hasLsl = !query.value(11).isNull();
        step.lsl = step.hasLsl ? query.value(11).toDouble() : 0.0;
        step.hasUsl = !query.value(12).isNull();
        step.usl = step.hasUsl ? query.value(12).toDouble() : 0.0;
        steps.append(step);
    }
    return steps;
}

bool TestRepository::exportCsv(int runId, const QString& csvPath, QString* errorMessage)
{
    if (!db_.isOpen()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开");
        return false;
    }

    QSqlQuery runQuery(db_);
    runQuery.prepare(QStringLiteral(
        "SELECT flow_name, product, sn, verdict, started_at, duration_ms,"
        " work_order, station_id, operator, fixture, retest, fail_reason"
        " FROM test_run WHERE id = ?"));
    runQuery.addBindValue(runId);
    if (!runQuery.exec() || !runQuery.next()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("找不到运行记录 #%1").arg(runId);
        return false;
    }

    const QString flowName = runQuery.value(0).toString();
    const QString product = runQuery.value(1).toString();
    const QString sn = runQuery.value(2).toString();
    const QString verdict = runQuery.value(3).toString();
    const QString startedAt = formatTime(runQuery.value(4).toLongLong());
    const int durationMs = runQuery.value(5).toInt();
    const QString workOrder = runQuery.value(6).toString();
    const QString stationId = runQuery.value(7).toString();
    const QString operatorName = runQuery.value(8).toString();
    const QString fixture = runQuery.value(9).toString();
    const QString retest = runQuery.value(10).toInt() != 0 ? QStringLiteral("是")
                                                           : QStringLiteral("否");
    const QString failReason = runQuery.value(11).toString();

    const QVector<HistoryStep> steps = stepsOfRun(runId);

    QFile file(csvPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("无法写入文件：%1").arg(file.errorString());
        return false;
    }

    // UTF-8 BOM：让 Excel 正确识别中文。
    file.write("\xEF\xBB\xBF");

    auto writeLine = [&file](const QStringList& fields) {
        QStringList escaped;
        escaped.reserve(fields.size());
        for (const auto& field : fields)
            escaped << csvField(field);
        file.write((escaped.join(QLatin1Char(',')) + QLatin1String("\r\n")).toUtf8());
    };

    writeLine({QStringLiteral("工单"), QStringLiteral("工位"), QStringLiteral("操作员"),
               QStringLiteral("治具"), QStringLiteral("复测"), QStringLiteral("不良原因"),
               QStringLiteral("方案"), QStringLiteral("产品"), QStringLiteral("SN"),
               QStringLiteral("总判定"), QStringLiteral("开始时间"),
               QStringLiteral("总耗时(ms)"), QStringLiteral("序号"), QStringLiteral("步骤"),
               QStringLiteral("动作"), QStringLiteral("状态"), QStringLiteral("实测值"),
               QStringLiteral("单位"), QStringLiteral("步骤耗时(ms)"), QStringLiteral("说明")});

    for (const auto& step : steps) {
        writeLine({workOrder,
                   stationId,
                   operatorName,
                   fixture,
                   retest,
                   failReason,
                   flowName,
                   product,
                   sn,
                   verdict,
                   startedAt,
                   QString::number(durationMs),
                   QString::number(step.index + 1),
                   step.name,
                   step.action,
                   step.status,
                   step.hasMeasured ? QString::number(step.measured, 'f', 2) : QString(),
                   step.unit,
                   QString::number(step.elapsedMs),
                   step.message});
    }

    file.close();
    return true;
}

// =============================================================================
// 波形摘要：批量写入 / 趋势查询 / 过期清理
// =============================================================================
bool TestRepository::saveSummaryBatch(const QVector<SummaryRow>& rows, QString* errorMessage)
{
    if (rows.isEmpty())
        return true;
    if (!db_.isOpen()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开");
        return false;
    }

    // 单事务写入：几百行一次提交，避免每行一次 fsync。
    if (!db_.transaction()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("开启事务失败：%1").arg(db_.lastError().text());
        return false;
    }

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO sample_summary"
        "(device_id, channel, epoch_sec, min_value, max_value, avg_value, sample_count)"
        " VALUES(?, ?, ?, ?, ?, ?, ?)"));

    for (const SummaryRow& row : rows) {
        query.addBindValue(row.deviceId);
        query.addBindValue(row.channel);
        query.addBindValue(row.epochSec);
        query.addBindValue(row.minValue);
        query.addBindValue(row.maxValue);
        query.addBindValue(row.avgValue);
        query.addBindValue(row.sampleCount);

        if (!query.exec()) {
            if (errorMessage != nullptr)
                *errorMessage = QStringLiteral("写入摘要失败：%1").arg(query.lastError().text());
            db_.rollback();
            return false;
        }
    }

    if (!db_.commit()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("提交摘要事务失败：%1").arg(db_.lastError().text());
        return false;
    }
    return true;
}

QVector<SummaryRow> TestRepository::summaryTrend(const QString& deviceId, int channel,
                                                 qint64 fromSec, qint64 toSec, int maxRows)
{
    QVector<SummaryRow> rows;
    if (!db_.isOpen() || deviceId.isEmpty() || channel < 0)
        return rows;

    QSqlQuery query(db_);
    // 时间升序；用 LIMIT 防御超大时间范围（60 分钟 = 3600 行）。
    query.prepare(QStringLiteral(
        "SELECT device_id, channel, epoch_sec, min_value, max_value, avg_value, sample_count"
        " FROM sample_summary"
        " WHERE device_id = ? AND channel = ? AND epoch_sec BETWEEN ? AND ?"
        " ORDER BY epoch_sec ASC LIMIT ?"));
    query.addBindValue(deviceId);
    query.addBindValue(channel);
    query.addBindValue(fromSec);
    query.addBindValue(toSec);
    query.addBindValue(maxRows);

    if (!query.exec())
        return rows;

    while (query.next()) {
        SummaryRow row;
        row.deviceId = query.value(0).toString();
        row.channel = query.value(1).toInt();
        row.epochSec = query.value(2).toLongLong();
        row.minValue = query.value(3).toDouble();
        row.maxValue = query.value(4).toDouble();
        row.avgValue = query.value(5).toDouble();
        row.sampleCount = query.value(6).toInt();
        rows.append(row);
    }
    return rows;
}

int TestRepository::purgeSummariesBefore(qint64 epochSec)
{
    if (!db_.isOpen())
        return 0;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral("DELETE FROM sample_summary WHERE epoch_sec < ?"));
    query.addBindValue(epochSec);
    if (!query.exec())
        return 0;
    return query.numRowsAffected();
}

// =============================================================================
// 生产工单
// =============================================================================
bool TestRepository::createWorkOrder(const QString& orderNo, const QString& product,
                                     int planQty, bool faiRequired, int faiSampleCount,
                                     QString* errorMessage)
{
    return createWorkOrder(orderNo, product, planQty, faiRequired, faiSampleCount,
                           QString(), errorMessage);
}

bool TestRepository::createWorkOrder(const QString& orderNo, const QString& product,
                                     int planQty, bool faiRequired, int faiSampleCount,
                                     const QString& flowPath, QString* errorMessage)
{
    if (!db_.isOpen()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开");
        return false;
    }

    const QString number = orderNo.trimmed();
    const QString productName = product.trimmed();
    if (number.isEmpty() || productName.isEmpty()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("工单号与产品不能为空");
        return false;
    }
    if (!findWorkOrder(number).orderNo.isEmpty()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("工单号已存在：%1").arg(number);
        return false;
    }

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT INTO work_order(order_no, product, plan_qty, status, created_at,"
        " fai_required, fai_status, fai_tested, fai_required_count, flow_path)"
        " VALUES(?, ?, ?, 'ACTIVE', ?, ?, ?, 0, ?, ?)"));
    query.addBindValue(number);
    query.addBindValue(productName);
    query.addBindValue(qMax(0, planQty));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(faiRequired ? 1 : 0);
    query.addBindValue(faiRequired ? QStringLiteral("PENDING")
                                   : QStringLiteral("CONFIRMED"));
    query.addBindValue(qMax(1, faiSampleCount));
    query.addBindValue(flowPath.trimmed());

    if (!query.exec()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("创建工单失败：%1").arg(query.lastError().text());
        return false;
    }
    return true;
}

QVector<WorkOrder> TestRepository::workOrders(int limit)
{
    QVector<WorkOrder> orders;
    if (!db_.isOpen())
        return orders;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT id, order_no, product, plan_qty, status, created_at, closed_at,"
        " fai_required, fai_status, fai_tested, fai_required_count,"
        " fai_confirmed_by, fai_confirmed_at, flow_path"
        " FROM work_order ORDER BY created_at DESC LIMIT ?"));
    query.addBindValue(limit);
    if (!query.exec())
        return orders;

    while (query.next()) {
        WorkOrder order;
        order.id = query.value(0).toInt();
        order.orderNo = query.value(1).toString();
        order.product = query.value(2).toString();
        order.planQty = query.value(3).toInt();
        order.status = query.value(4).toString();
        order.createdAt = query.value(5).toLongLong();
        order.closedAt = query.value(6).isNull() ? 0 : query.value(6).toLongLong();
        order.faiRequired = query.value(7).toInt() != 0;
        order.faiStatus = query.value(8).toString();
        order.faiTested = query.value(9).toInt();
        order.faiRequiredCount = query.value(10).toInt();
        order.faiConfirmedBy = query.value(11).toString();
        order.faiConfirmedAt = query.value(12).isNull() ? 0 : query.value(12).toLongLong();
        order.flowPath = query.value(13).toString();
        orders.append(order);
    }
    return orders;
}

WorkOrder TestRepository::findWorkOrder(const QString& orderNo)
{
    WorkOrder order;
    if (!db_.isOpen() || orderNo.trimmed().isEmpty())
        return order;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT id, order_no, product, plan_qty, status, created_at, closed_at,"
        " fai_required, fai_status, fai_tested, fai_required_count,"
        " fai_confirmed_by, fai_confirmed_at, flow_path"
        " FROM work_order WHERE order_no = ?"));
    query.addBindValue(orderNo.trimmed());
    if (!query.exec() || !query.next())
        return order;

    order.id = query.value(0).toInt();
    order.orderNo = query.value(1).toString();
    order.product = query.value(2).toString();
    order.planQty = query.value(3).toInt();
    order.status = query.value(4).toString();
    order.createdAt = query.value(5).toLongLong();
    order.closedAt = query.value(6).isNull() ? 0 : query.value(6).toLongLong();
    order.faiRequired = query.value(7).toInt() != 0;
    order.faiStatus = query.value(8).toString();
    order.faiTested = query.value(9).toInt();
    order.faiRequiredCount = query.value(10).toInt();
    order.faiConfirmedBy = query.value(11).toString();
    order.faiConfirmedAt = query.value(12).isNull() ? 0 : query.value(12).toLongLong();
    order.flowPath = query.value(13).toString();
    return order;
}

bool TestRepository::closeWorkOrder(const QString& orderNo, QString* errorMessage)
{
    const WorkOrder order = findWorkOrder(orderNo);
    if (order.orderNo.isEmpty()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("工单不存在：%1").arg(orderNo);
        return false;
    }
    if (order.status == QLatin1String("CLOSED"))
        return true;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "UPDATE work_order SET status = 'CLOSED', closed_at = ? WHERE order_no = ?"));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(order.orderNo);
    if (!query.exec()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("关闭工单失败：%1").arg(query.lastError().text());
        return false;
    }
    return true;
}

// =============================================================================
// 首件检验（FAI）
// =============================================================================
bool TestRepository::updateFaiAfterRun(const QString& orderNo, const QString& verdict,
                                       QString* errorMessage)
{
    const WorkOrder order = findWorkOrder(orderNo);
    if (order.orderNo.isEmpty()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("工单不存在：%1").arg(orderNo);
        return false;
    }
    if (!order.faiRequired || order.faiStatus == QLatin1String("CONFIRMED"))
        return true;
    // WAITING / REJECTED 表示生产已锁定，不应再推进。
    if (order.faiStatus != QLatin1String("PENDING"))
        return true;

    const int tested = order.faiTested + 1;
    QString status = order.faiStatus;
    if (verdict != QLatin1String("PASS"))
        status = QStringLiteral("REJECTED");
    else if (tested >= qMax(1, order.faiRequiredCount))
        status = QStringLiteral("WAITING");

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "UPDATE work_order SET fai_tested = ?, fai_status = ? WHERE order_no = ?"));
    query.addBindValue(tested);
    query.addBindValue(status);
    query.addBindValue(order.orderNo);
    if (!query.exec()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("更新首件状态失败：%1").arg(query.lastError().text());
        return false;
    }
    return true;
}

bool TestRepository::confirmFai(const QString& orderNo, const QString& user,
                                QString* errorMessage)
{
    const WorkOrder order = findWorkOrder(orderNo);
    if (order.orderNo.isEmpty()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("工单不存在：%1").arg(orderNo);
        return false;
    }
    if (order.faiStatus != QLatin1String("WAITING")) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("当前不是待确认状态（%1）").arg(order.faiStatus);
        return false;
    }

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "UPDATE work_order SET fai_status = 'CONFIRMED', fai_confirmed_by = ?,"
        " fai_confirmed_at = ? WHERE order_no = ?"));
    query.addBindValue(user);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(order.orderNo);
    if (!query.exec()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("确认首件失败：%1").arg(query.lastError().text());
        return false;
    }
    return true;
}

bool TestRepository::resetFai(const QString& orderNo, QString* errorMessage)
{
    const WorkOrder order = findWorkOrder(orderNo);
    if (order.orderNo.isEmpty()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("工单不存在：%1").arg(orderNo);
        return false;
    }

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "UPDATE work_order SET fai_status = 'PENDING', fai_tested = 0,"
        " fai_confirmed_by = NULL, fai_confirmed_at = NULL WHERE order_no = ?"));
    query.addBindValue(order.orderNo);
    if (!query.exec()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("重置首件失败：%1").arg(query.lastError().text());
        return false;
    }
    return true;
}

// =============================================================================
// 不良原因
// =============================================================================
bool TestRepository::updateFailReason(int runId, const QString& reason)
{
    if (!db_.isOpen() || runId <= 0)
        return false;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral("UPDATE test_run SET fail_reason = ? WHERE id = ?"));
    query.addBindValue(reason.left(200));
    query.addBindValue(runId);
    return query.exec();
}

QVector<ReasonRank> TestRepository::topFailureReasons(const QString& orderNo, int limit)
{
    QVector<ReasonRank> ranks;
    if (!db_.isOpen() || orderNo.trimmed().isEmpty())
        return ranks;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT fail_reason, COUNT(*) AS failures FROM test_run"
        " WHERE work_order = ? AND verdict = 'FAIL' AND fail_reason <> ''"
        " GROUP BY fail_reason ORDER BY failures DESC LIMIT ?"));
    query.addBindValue(orderNo.trimmed());
    query.addBindValue(limit);
    if (!query.exec())
        return ranks;

    while (query.next()) {
        ReasonRank rank;
        rank.reason = query.value(0).toString();
        rank.count = query.value(1).toInt();
        ranks.append(rank);
    }
    return ranks;
}

// =============================================================================
// SPC：测试项列表与实测值序列
// =============================================================================
QVector<SpcItem> TestRepository::spcItems(const QString& orderNo)
{
    QVector<SpcItem> items;
    if (!db_.isOpen() || orderNo.trimmed().isEmpty())
        return items;

    QSqlQuery query(db_);
    // 只统计"有实测值 + 流程配置了规格限"的步骤；同一 flow+step 聚合成一个测试项。
    query.prepare(QStringLiteral(
        "SELECT r.flow_id, MAX(r.flow_name), s.step_id, MAX(s.name), MAX(s.unit),"
        " MAX(s.min_limit), MAX(s.max_limit), COUNT(*)"
        " FROM test_step s JOIN test_run r ON r.id = s.run_id"
        " WHERE r.work_order = ? AND s.has_measured = 1"
        "   AND (s.min_limit IS NOT NULL OR s.max_limit IS NOT NULL)"
        " GROUP BY r.flow_id, s.step_id"
        " ORDER BY MAX(r.id) DESC"));
    query.addBindValue(orderNo.trimmed());
    if (!query.exec())
        return items;

    while (query.next()) {
        SpcItem item;
        item.flowId = query.value(0).toString();
        item.flowName = query.value(1).toString();
        item.stepId = query.value(2).toString();
        item.stepName = query.value(3).toString();
        item.unit = query.value(4).toString();
        item.hasLsl = !query.value(5).isNull();
        item.lsl = item.hasLsl ? query.value(5).toDouble() : 0.0;
        item.hasUsl = !query.value(6).isNull();
        item.usl = item.hasUsl ? query.value(6).toDouble() : 0.0;
        item.count = query.value(7).toInt();
        items.append(item);
    }
    return items;
}

QVector<SpcPoint> TestRepository::spcPoints(const QString& orderNo,
                                            const QString& flowId,
                                            const QString& stepId, int limit)
{
    QVector<SpcPoint> points;
    if (!db_.isOpen() || orderNo.trimmed().isEmpty() || stepId.trimmed().isEmpty())
        return points;

    QSqlQuery query(db_);
    // 先按时间倒序取最近 limit 条，再反转成时间升序（控制图从左到右）。
    query.prepare(QStringLiteral(
        "SELECT s.measured, r.finished_at, r.sn"
        " FROM test_step s JOIN test_run r ON r.id = s.run_id"
        " WHERE r.work_order = ? AND r.flow_id = ? AND s.step_id = ?"
        "   AND s.has_measured = 1"
        " ORDER BY r.id DESC LIMIT ?"));
    query.addBindValue(orderNo.trimmed());
    query.addBindValue(flowId);
    query.addBindValue(stepId.trimmed());
    query.addBindValue(qMax(1, limit));
    if (!query.exec())
        return points;

    while (query.next()) {
        SpcPoint point;
        point.value = query.value(0).toDouble();
        point.at = query.value(1).toLongLong();
        point.sn = query.value(2).toString();
        points.append(point);
    }
    std::reverse(points.begin(), points.end());
    return points;
}

int TestRepository::consecutiveFailRuns(const QString& orderNo)
{
    if (!db_.isOpen() || orderNo.trimmed().isEmpty())
        return 0;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT verdict FROM test_run WHERE work_order = ?"
        " ORDER BY id DESC LIMIT 200"));
    query.addBindValue(orderNo.trimmed());
    if (!query.exec())
        return 0;

    int streak = 0;
    while (query.next()) {
        const QString verdict = query.value(0).toString();
        if (verdict == QLatin1String("FAIL")) {
            ++streak;
            continue;
        }
        // PASS 或 ABORTED（人工停止）都不算质量不良，连续中断。
        break;
    }
    return streak;
}

// =============================================================================
// Andon 停线
// =============================================================================
AndonState TestRepository::andonState(const QString& stationId)
{
    AndonState state;
    if (!db_.isOpen() || stationId.trimmed().isEmpty())
        return state;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT reason, detail, created_at FROM andon_event"
        " WHERE station_id = ? AND cleared_at IS NULL"
        " ORDER BY id DESC LIMIT 1"));
    query.addBindValue(stationId.trimmed());
    if (!query.exec() || !query.next())
        return state;

    state.locked = true;
    state.reason = query.value(0).toString();
    state.detail = query.value(1).toString();
    state.createdAt = query.value(2).toLongLong();
    return state;
}

bool TestRepository::lockStation(const QString& stationId, const QString& reason,
                                 const QString& detail)
{
    if (!db_.isOpen() || stationId.trimmed().isEmpty())
        return false;
    // 已处于停线状态时不重复插入，避免看板出现多条相同事件。
    if (andonState(stationId).locked)
        return true;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT INTO andon_event(station_id, reason, detail, created_at)"
        " VALUES(?, ?, ?, ?)"));
    query.addBindValue(stationId.trimmed());
    query.addBindValue(reason.left(200));
    query.addBindValue(detail);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    return query.exec();
}

bool TestRepository::clearAndon(const QString& stationId, const QString& user)
{
    if (!db_.isOpen() || stationId.trimmed().isEmpty())
        return false;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "UPDATE andon_event SET cleared_at = ?, cleared_by = ?"
        " WHERE station_id = ? AND cleared_at IS NULL"));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(user.left(100));
    query.addBindValue(stationId.trimmed());
    return query.exec();
}

// =============================================================================
// 交接班
// =============================================================================
bool TestRepository::saveHandover(const HandoverRecord& record, QString* errorMessage)
{
    if (!db_.isOpen()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开");
        return false;
    }
    if (record.operatorName.trimmed().isEmpty()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("交接人不能为空");
        return false;
    }

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT INTO shift_handover(order_no, line_id, station_id, shift, operator,"
        " units, pass_runs, fail_runs, note, created_at)"
        " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(record.orderNo);
    query.addBindValue(record.lineId);
    query.addBindValue(record.stationId);
    query.addBindValue(record.shift);
    query.addBindValue(record.operatorName.trimmed());
    query.addBindValue(record.units);
    query.addBindValue(record.passRuns);
    query.addBindValue(record.failRuns);
    query.addBindValue(record.note.left(1000));
    query.addBindValue(record.createdAt > 0 ? record.createdAt
                                            : QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("写入交接班记录失败：%1")
                                .arg(query.lastError().text());
        return false;
    }
    return true;
}

QVector<HandoverRecord> TestRepository::recentHandovers(int limit)
{
    QVector<HandoverRecord> records;
    if (!db_.isOpen())
        return records;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT id, order_no, line_id, station_id, shift, operator, units,"
        " pass_runs, fail_runs, note, created_at FROM shift_handover"
        " ORDER BY id DESC LIMIT ?"));
    query.addBindValue(qMax(1, limit));
    if (!query.exec())
        return records;

    while (query.next()) {
        HandoverRecord record;
        record.id = query.value(0).toInt();
        record.orderNo = query.value(1).toString();
        record.lineId = query.value(2).toString();
        record.stationId = query.value(3).toString();
        record.shift = query.value(4).toString();
        record.operatorName = query.value(5).toString();
        record.units = query.value(6).toInt();
        record.passRuns = query.value(7).toInt();
        record.failRuns = query.value(8).toInt();
        record.note = query.value(9).toString();
        record.createdAt = query.value(10).toLongLong();
        records.append(record);
    }
    return records;
}

// =============================================================================
// 日报 / 班次报表
// =============================================================================
QVector<ProductionReportRow> TestRepository::productionReport(qint64 fromMs,
                                                              qint64 toMs,
                                                              const QString& workOrder)
{
    QVector<ProductionReportRow> rows;
    if (!db_.isOpen())
        return rows;

    const QString orderNo = workOrder.trimmed();
    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT date(r.started_at / 1000, 'unixepoch', 'localtime') AS day,"
        " COALESCE(NULLIF(r.shift, ''), '未配置') AS shift_name,"
        " COUNT(*),"
        " COALESCE(SUM(CASE WHEN r.verdict = 'PASS' THEN 1 ELSE 0 END), 0),"
        " COALESCE(SUM(CASE WHEN r.verdict = 'FAIL' THEN 1 ELSE 0 END), 0),"
        " COUNT(DISTINCT r.sn),"
        " COUNT(DISTINCT CASE WHEN r.verdict = 'PASS' THEN r.sn END)"
        " FROM test_run r"
        " WHERE r.started_at BETWEEN ? AND ?"
        "   AND (? = '' OR r.work_order = ?)"
        " GROUP BY day, shift_name"
        " ORDER BY day DESC, shift_name ASC"));
    query.addBindValue(fromMs);
    query.addBindValue(toMs);
    query.addBindValue(orderNo.isNull() ? QStringLiteral("") : orderNo);
    query.addBindValue(orderNo.isNull() ? QStringLiteral("") : orderNo);
    if (!query.exec())
        return rows;

    while (query.next()) {
        ProductionReportRow row;
        row.date = query.value(0).toString();
        row.shift = query.value(1).toString();
        row.runs = query.value(2).toInt();
        row.passRuns = query.value(3).toInt();
        row.failRuns = query.value(4).toInt();
        row.units = query.value(5).toInt();
        row.passUnits = query.value(6).toInt();
        row.yieldRate = row.runs > 0
                            ? static_cast<double>(row.passRuns) / row.runs
                            : 0.0;
        rows.append(row);
    }
    return rows;
}

// =============================================================================
// SN 白名单（发货清单）
// =============================================================================
int TestRepository::importSnWhitelist(const QString& orderNo,
                                      const QStringList& sns,
                                      QString* errorMessage)
{
    const QString order = orderNo.trimmed();
    if (!db_.isOpen() || order.isEmpty()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开或工单号为空");
        return -1;
    }

    if (!db_.transaction()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("开启事务失败：%1").arg(db_.lastError().text());
        return -1;
    }

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO sn_whitelist(work_order, sn) VALUES(?, ?)"));

    int added = 0;
    for (const QString& raw : sns) {
        const QString sn = raw.trimmed();
        if (sn.isEmpty())
            continue;
        query.bindValue(0, order);
        query.bindValue(1, sn);
        if (!query.exec()) {
            db_.rollback();
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("导入 SN 清单失败：%1")
                                    .arg(query.lastError().text());
            }
            return -1;
        }
        added += qMax(0, query.numRowsAffected());
    }

    if (!db_.commit()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("提交事务失败：%1").arg(db_.lastError().text());
        return -1;
    }
    return added;
}

int TestRepository::snWhitelistCount(const QString& orderNo)
{
    if (!db_.isOpen() || orderNo.trimmed().isEmpty())
        return 0;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM sn_whitelist WHERE work_order = ?"));
    query.addBindValue(orderNo.trimmed());
    if (!query.exec() || !query.next())
        return 0;
    return query.value(0).toInt();
}

bool TestRepository::snInWhitelist(const QString& orderNo, const QString& sn)
{
    if (!db_.isOpen() || orderNo.trimmed().isEmpty() || sn.trimmed().isEmpty())
        return false;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT 1 FROM sn_whitelist WHERE work_order = ? AND sn = ? LIMIT 1"));
    query.addBindValue(orderNo.trimmed());
    query.addBindValue(sn.trimmed());
    return query.exec() && query.next();
}

// =============================================================================
// 治具寿命与校准
// =============================================================================
FixtureStatus TestRepository::fixtureStatus(const QString& fixtureId)
{
    FixtureStatus status;
    status.fixtureId = fixtureId;
    if (!db_.isOpen() || fixtureId.trimmed().isEmpty())
        return status;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT run_count, last_used_at, calibrated_until FROM fixture_usage"
        " WHERE fixture_id = ?"));
    query.addBindValue(fixtureId.trimmed());
    if (!query.exec() || !query.next())
        return status;

    status.runCount = query.value(0).toInt();
    status.lastUsedAt = query.value(1).isNull() ? 0 : query.value(1).toLongLong();
    status.calibratedUntil = query.value(2).isNull() ? 0 : query.value(2).toLongLong();
    return status;
}

bool TestRepository::recordFixtureUse(const QString& fixtureId, qint64 calibratedUntilMs,
                                      qint64 nowMs)
{
    if (!db_.isOpen() || fixtureId.trimmed().isEmpty())
        return false;

    const QString id = fixtureId.trimmed();
    const FixtureStatus current = fixtureStatus(id);

    QSqlQuery query(db_);
    if (current.runCount == 0 && current.lastUsedAt == 0 && current.calibratedUntil == 0) {
        query.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO fixture_usage(fixture_id, run_count, last_used_at,"
            " calibrated_until) VALUES(?, 1, ?, ?)"));
        query.addBindValue(id);
        query.addBindValue(nowMs);
        query.addBindValue(calibratedUntilMs);
    } else {
        query.prepare(QStringLiteral(
            "UPDATE fixture_usage SET run_count = run_count + 1, last_used_at = ?,"
            " calibrated_until = CASE WHEN calibrated_until IS NULL OR calibrated_until = 0"
            " THEN ? ELSE calibrated_until END WHERE fixture_id = ?"));
        query.addBindValue(nowMs);
        query.addBindValue(calibratedUntilMs);
        query.addBindValue(id);
    }
    return query.exec();
}

bool TestRepository::resetFixture(const QString& fixtureId, qint64 calibratedUntilMs,
                                  qint64 nowMs)
{
    if (!db_.isOpen() || fixtureId.trimmed().isEmpty())
        return false;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO fixture_usage(fixture_id, run_count, last_used_at,"
        " calibrated_until) VALUES(?, 0, ?, ?)"));
    query.addBindValue(fixtureId.trimmed());
    query.addBindValue(nowMs);
    query.addBindValue(calibratedUntilMs);
    return query.exec();
}

int TestRepository::snTestCount(const QString& orderNo, const QString& sn)
{
    if (!db_.isOpen() || orderNo.trimmed().isEmpty() || sn.trimmed().isEmpty())
        return 0;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM test_run WHERE work_order = ? AND sn = ?"));
    query.addBindValue(orderNo.trimmed());
    query.addBindValue(sn.trimmed());
    if (!query.exec() || !query.next())
        return 0;
    return query.value(0).toInt();
}

HistoryRun TestRepository::lastRunOfSn(const QString& orderNo, const QString& sn)
{
    if (!db_.isOpen() || orderNo.trimmed().isEmpty() || sn.trimmed().isEmpty())
        return {};

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT r.id, r.flow_id, r.flow_name, r.product, r.sn, r.verdict, r.started_at,"
        " r.finished_at, r.duration_ms, r.work_order, r.station_id, r.operator,"
        " r.fixture, r.retest, r.fail_reason, o.status, r.shift"
        " FROM test_run r LEFT JOIN mes_outbox o ON o.run_id = r.id"
        " WHERE r.work_order = ? AND r.sn = ?"
        " ORDER BY r.id DESC LIMIT 1"));
    query.addBindValue(orderNo.trimmed());
    query.addBindValue(sn.trimmed());
    if (!query.exec() || !query.next())
        return {};
    return readRunRow(query, 0);
}

QVector<HistoryRun> TestRepository::runsOfOrder(const QString& orderNo, int limit)
{
    QVector<HistoryRun> runs;
    if (!db_.isOpen() || orderNo.trimmed().isEmpty())
        return runs;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT r.id, r.flow_id, r.flow_name, r.product, r.sn, r.verdict, r.started_at,"
        " r.finished_at, r.duration_ms, r.work_order, r.station_id, r.operator,"
        " r.fixture, r.retest, r.fail_reason, o.status, r.shift"
        " FROM test_run r LEFT JOIN mes_outbox o ON o.run_id = r.id"
        " WHERE r.work_order = ? ORDER BY r.id DESC LIMIT ?"));
    query.addBindValue(orderNo.trimmed());
    query.addBindValue(limit);
    if (!query.exec())
        return runs;

    while (query.next())
        runs.append(readRunRow(query, 0));
    return runs;
}

WorkOrderStats TestRepository::statsOfOrder(const QString& orderNo)
{
    WorkOrderStats stats;
    if (!db_.isOpen() || orderNo.trimmed().isEmpty())
        return stats;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT COUNT(*),"
        " COALESCE(SUM(CASE WHEN verdict='PASS' THEN 1 ELSE 0 END), 0),"
        " COALESCE(SUM(CASE WHEN verdict='FAIL' THEN 1 ELSE 0 END), 0),"
        " COALESCE(SUM(CASE WHEN verdict='ABORTED' THEN 1 ELSE 0 END), 0),"
        " COALESCE(SUM(CASE WHEN retest=1 THEN 1 ELSE 0 END), 0),"
        " COALESCE(SUM(CASE WHEN retest=1 AND verdict='PASS' THEN 1 ELSE 0 END), 0),"
        " COALESCE(MIN(started_at), 0)"
        " FROM test_run WHERE work_order = ?"));
    query.addBindValue(orderNo.trimmed());
    if (query.exec() && query.next()) {
        stats.totalRuns = query.value(0).toInt();
        stats.passRuns = query.value(1).toInt();
        stats.failRuns = query.value(2).toInt();
        stats.abortedRuns = query.value(3).toInt();
        stats.retestRuns = query.value(4).toInt();
        stats.retestPassRuns = query.value(5).toInt();
    }

    // 去重 SN：每个 SN 取最早一次运行的判定，作为"首次通过"依据。
    // 用 id（自增、单调）而不是 started_at：后者按耗时回推，密集测试时可能乱序。
    QSqlQuery unitQuery(db_);
    unitQuery.prepare(QStringLiteral(
        "SELECT COUNT(*),"
        " COALESCE(SUM(CASE WHEN first_verdict='PASS' THEN 1 ELSE 0 END), 0)"
        " FROM ("
        "   SELECT r.sn,"
        "     (SELECT r2.verdict FROM test_run r2"
        "       WHERE r2.work_order = r.work_order AND r2.sn = r.sn"
        "       ORDER BY r2.id ASC LIMIT 1) AS first_verdict"
        "   FROM test_run r"
        "   WHERE r.work_order = ? AND r.sn <> ''"
        "   GROUP BY r.sn)"));
    unitQuery.addBindValue(orderNo.trimmed());
    if (unitQuery.exec() && unitQuery.next()) {
        stats.units = unitQuery.value(0).toInt();
        stats.firstPassUnits = unitQuery.value(1).toInt();
    }

    stats.fpy = stats.units > 0
                   ? static_cast<double>(stats.firstPassUnits) / stats.units
                   : 0.0;

    // 待返修：每个 SN 看最近一次运行，仍非 PASS 的算待返修。
    QSqlQuery repairQuery(db_);
    repairQuery.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM ("
        "  SELECT r.sn,"
        "    (SELECT r2.verdict FROM test_run r2"
        "      WHERE r2.work_order = r.work_order AND r2.sn = r.sn"
        "      ORDER BY r2.id DESC LIMIT 1) AS last_verdict"
        "  FROM test_run r WHERE r.work_order = ? AND r.sn <> ''"
        "  GROUP BY r.sn) WHERE last_verdict <> 'PASS'"));
    repairQuery.addBindValue(orderNo.trimmed());
    if (repairQuery.exec() && repairQuery.next())
        stats.pendingRepairUnits = repairQuery.value(0).toInt();

    // UPH：从该工单首件测试到现在的平均产出；不足 1 分钟按 1 分钟估算。
    const qint64 earliest = query.value(6).toLongLong();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (earliest > 0 && now > earliest && stats.units > 0) {
        const double hours = qMax((now - earliest) / 3600000.0, 1.0 / 60.0);
        stats.uph = stats.units / hours;
    }

    return stats;
}

QVector<FailureRank> TestRepository::topFailureSteps(const QString& orderNo, int limit)
{
    QVector<FailureRank> ranks;
    if (!db_.isOpen() || orderNo.trimmed().isEmpty())
        return ranks;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT s.name, s.action, COUNT(*) AS failures"
        " FROM test_step s JOIN test_run r ON r.id = s.run_id"
        " WHERE r.work_order = ? AND s.status = 'FAIL'"
        " GROUP BY s.name, s.action ORDER BY failures DESC LIMIT ?"));
    query.addBindValue(orderNo.trimmed());
    query.addBindValue(limit);
    if (!query.exec())
        return ranks;

    while (query.next()) {
        FailureRank rank;
        rank.stepName = query.value(0).toString();
        rank.action = query.value(1).toString();
        rank.count = query.value(2).toInt();
        ranks.append(rank);
    }
    return ranks;
}

QVector<StepDurationRank> TestRepository::topSlowSteps(const QString& orderNo, int limit)
{
    QVector<StepDurationRank> ranks;
    if (!db_.isOpen() || orderNo.trimmed().isEmpty())
        return ranks;

    QSqlQuery query(db_);
    // 只统计确实记录到耗时的步骤（elapsed_ms > 0），避免旧数据/跳过步骤干扰。
    query.prepare(QStringLiteral(
        "SELECT s.name, s.action, COUNT(*) AS runs,"
        " AVG(s.elapsed_ms) AS avg_ms, MAX(s.elapsed_ms) AS max_ms,"
        " SUM(s.elapsed_ms) AS total_ms"
        " FROM test_step s JOIN test_run r ON r.id = s.run_id"
        " WHERE r.work_order = ? AND s.elapsed_ms > 0"
        " GROUP BY s.name, s.action ORDER BY avg_ms DESC LIMIT ?"));
    query.addBindValue(orderNo.trimmed());
    query.addBindValue(limit);
    if (!query.exec())
        return ranks;

    while (query.next()) {
        StepDurationRank rank;
        rank.stepName = query.value(0).toString();
        rank.action = query.value(1).toString();
        rank.runs = query.value(2).toInt();
        rank.avgMs = query.value(3).toDouble();
        rank.maxMs = query.value(4).toLongLong();
        rank.totalMs = query.value(5).toLongLong();
        ranks.append(rank);
    }
    return ranks;
}

// =============================================================================
// MES 上传 outbox
// =============================================================================
bool TestRepository::enqueueUpload(int runId, const QByteArray& payload,
                                   qint64 nextAttemptAt, QString* errorMessage)
{
    if (!db_.isOpen() || runId <= 0) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("运行记录无效或数据库未打开");
        return false;
    }

    QSqlQuery query(db_);
    // run_id 唯一：重复入队直接忽略，保证一笔运行只上传一次。
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO mes_outbox(run_id, payload, status, attempts,"
        " next_attempt_at, created_at) VALUES(?, ?, 'PENDING', 0, ?, ?)"));
    query.addBindValue(runId);
    query.addBindValue(QString::fromUtf8(payload));
    query.addBindValue(nextAttemptAt);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());

    if (!query.exec()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("写入上传队列失败：%1").arg(query.lastError().text());
        return false;
    }
    return true;
}

QVector<MesUploadItem> TestRepository::dueUploads(qint64 nowMs, int limit)
{
    QVector<MesUploadItem> items;
    if (!db_.isOpen())
        return items;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT id, run_id, payload, attempts FROM mes_outbox"
        " WHERE status = 'PENDING' AND next_attempt_at <= ?"
        " ORDER BY next_attempt_at ASC, id ASC LIMIT ?"));
    query.addBindValue(nowMs);
    query.addBindValue(limit);
    if (!query.exec())
        return items;

    while (query.next()) {
        MesUploadItem item;
        item.id = query.value(0).toInt();
        item.runId = query.value(1).toInt();
        item.payload = query.value(2).toString().toUtf8();
        item.attempts = query.value(3).toInt();
        items.append(item);
    }
    return items;
}

bool TestRepository::markUploaded(int id, qint64 atMs)
{
    if (!db_.isOpen())
        return false;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "UPDATE mes_outbox SET status='SENT', uploaded_at=?, last_error=NULL WHERE id=?"));
    query.addBindValue(atMs);
    query.addBindValue(id);
    return query.exec();
}

bool TestRepository::markUploadFailed(int id, int attempts, const QString& error,
                                      qint64 nextAttemptAt)
{
    if (!db_.isOpen())
        return false;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "UPDATE mes_outbox SET status='PENDING', attempts=?, last_error=?,"
        " next_attempt_at=? WHERE id=?"));
    query.addBindValue(attempts);
    query.addBindValue(error.left(500));
    query.addBindValue(nextAttemptAt);
    query.addBindValue(id);
    return query.exec();
}

bool TestRepository::markUploadDead(int id, int attempts, const QString& error)
{
    if (!db_.isOpen())
        return false;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "UPDATE mes_outbox SET status='DEAD', attempts=?, last_error=? WHERE id=?"));
    query.addBindValue(attempts);
    query.addBindValue(error.left(500));
    query.addBindValue(id);
    return query.exec();
}

int TestRepository::uploadCountByStatus(const QString& status)
{
    if (!db_.isOpen())
        return 0;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM mes_outbox WHERE status = ?"));
    query.addBindValue(status);
    if (!query.exec() || !query.next())
        return 0;
    return query.value(0).toInt();
}

bool TestRepository::requeueDeadUploads(qint64 nowMs)
{
    if (!db_.isOpen())
        return false;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "UPDATE mes_outbox SET status='PENDING', attempts=0, next_attempt_at=?,"
        " last_error=NULL WHERE status='DEAD'"));
    query.addBindValue(nowMs);
    return query.exec();
}

QByteArray TestRepository::buildRunPayload(int runId)
{
    if (!db_.isOpen() || runId <= 0)
        return {};

    QSqlQuery runQuery(db_);
    runQuery.prepare(QStringLiteral(
        "SELECT flow_name, product, sn, verdict, started_at, finished_at, duration_ms,"
        " work_order, line_id, station_id, operator, fixture, retest, fail_reason"
        " FROM test_run WHERE id = ?"));
    runQuery.addBindValue(runId);
    if (!runQuery.exec() || !runQuery.next())
        return {};

    QJsonObject root;
    root.insert(QStringLiteral("runId"), runId);
    root.insert(QStringLiteral("flowName"), runQuery.value(0).toString());
    root.insert(QStringLiteral("product"), runQuery.value(1).toString());
    root.insert(QStringLiteral("sn"), runQuery.value(2).toString());
    root.insert(QStringLiteral("verdict"), runQuery.value(3).toString());
    root.insert(QStringLiteral("startedAt"), runQuery.value(4).toLongLong());
    root.insert(QStringLiteral("finishedAt"), runQuery.value(5).toLongLong());
    root.insert(QStringLiteral("durationMs"), runQuery.value(6).toInt());
    root.insert(QStringLiteral("workOrder"), runQuery.value(7).toString());
    root.insert(QStringLiteral("lineId"), runQuery.value(8).toString());
    root.insert(QStringLiteral("stationId"), runQuery.value(9).toString());
    root.insert(QStringLiteral("operator"), runQuery.value(10).toString());
    root.insert(QStringLiteral("fixture"), runQuery.value(11).toString());
    root.insert(QStringLiteral("retest"), runQuery.value(12).toInt() != 0);
    root.insert(QStringLiteral("failReason"), runQuery.value(13).toString());

    QSqlQuery stepQuery(db_);
    stepQuery.prepare(QStringLiteral(
        "SELECT step_index, step_id, name, action, status, measured, has_measured,"
        " metric, unit, elapsed_ms, message FROM test_step"
        " WHERE run_id = ? ORDER BY step_index ASC"));
    stepQuery.addBindValue(runId);

    QJsonArray steps;
    if (stepQuery.exec()) {
        while (stepQuery.next()) {
            QJsonObject step;
            step.insert(QStringLiteral("index"), stepQuery.value(0).toInt() + 1);
            step.insert(QStringLiteral("id"), stepQuery.value(1).toString());
            step.insert(QStringLiteral("name"), stepQuery.value(2).toString());
            step.insert(QStringLiteral("action"), stepQuery.value(3).toString());
            step.insert(QStringLiteral("status"), stepQuery.value(4).toString());
            if (stepQuery.value(6).toInt() != 0)
                step.insert(QStringLiteral("measured"), stepQuery.value(5).toDouble());
            step.insert(QStringLiteral("metric"), stepQuery.value(7).toString());
            step.insert(QStringLiteral("unit"), stepQuery.value(8).toString());
            step.insert(QStringLiteral("elapsedMs"), stepQuery.value(9).toInt());
            step.insert(QStringLiteral("message"), stepQuery.value(10).toString());
            steps.append(step);
        }
    }
    root.insert(QStringLiteral("steps"), steps);

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace aegis::storage
