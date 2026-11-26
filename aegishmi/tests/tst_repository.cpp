// Qt Test 框架
#include <QtTest>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "security/auth_service.h"
#include "storage/system_repository.h"
#include "storage/test_repository.h"

using namespace aegis;

namespace {

test::TestFlow makeFlow()
{
    test::TestFlow flow;
    flow.schemaVersion = 1;
    flow.id = QStringLiteral("power");
    flow.name = QStringLiteral("Power Test");
    flow.product = QStringLiteral("PM-5V");

    test::TestStep command;
    command.id = QStringLiteral("s1");
    command.name = QStringLiteral("set vout");
    command.action = test::StepAction::Command;
    command.cmd = QStringLiteral("SET_VOUT");
    command.expect.present = true;
    command.expect.ackStatus = 0;

    test::TestStep measure;
    measure.id = QStringLiteral("s2");
    measure.name = QStringLiteral("measure vout");
    measure.action = test::StepAction::Measure;
    measure.channel = 0;
    measure.windowMs = 100;
    measure.expect.present = true;
    measure.expect.metric = test::Metric::Avg;
    measure.expect.hasMin = true;
    measure.expect.min = 4750;
    measure.expect.hasMax = true;
    measure.expect.max = 5250;
    measure.expect.unit = QStringLiteral("mV");

    flow.steps = {command, measure};
    return flow;
}

test::RunResult makeResult()
{
    test::RunResult result;
    result.verdict = test::RunVerdict::Fail;
    result.totalElapsedMs = 450;

    test::StepResult first;
    first.index = 0;
    first.id = QStringLiteral("s1");
    first.name = QStringLiteral("set vout");
    first.status = test::StepStatus::Pass;
    first.elapsedMs = 40;

    test::StepResult second;
    second.index = 1;
    second.id = QStringLiteral("s2");
    second.name = QStringLiteral("measure vout");
    second.status = test::StepStatus::Fail;
    second.measured = 5300.0;
    second.hasMeasured = true;
    second.elapsedMs = 210;
    second.message = QStringLiteral("实测 5300.00 高于上限 5250.00");

    result.steps = {first, second};
    return result;
}

storage::RunMeta makeMeta(const QString& sn = QStringLiteral("SN-001"),
                          bool retest = false)
{
    storage::RunMeta meta;
    meta.sn = sn;
    meta.workOrder = QStringLiteral("WO-001");
    meta.lineId = QStringLiteral("L1");
    meta.stationId = QStringLiteral("ST-01");
    meta.operatorName = QStringLiteral("admin");
    meta.fixture = QStringLiteral("FX-01");
    meta.retest = retest;
    return meta;
}

test::RunResult makePassResult()
{
    test::RunResult result;
    result.verdict = test::RunVerdict::Pass;
    result.totalElapsedMs = 300;

    test::StepResult first;
    first.index = 0;
    first.id = QStringLiteral("s1");
    first.name = QStringLiteral("set vout");
    first.status = test::StepStatus::Pass;
    first.elapsedMs = 40;

    test::StepResult second;
    second.index = 1;
    second.id = QStringLiteral("s2");
    second.name = QStringLiteral("measure vout");
    second.status = test::StepStatus::Pass;
    second.measured = 5000.0;
    second.hasMeasured = true;
    second.elapsedMs = 200;

    result.steps = {first, second};
    return result;
}

} // namespace

// =============================================================================
// TstRepository —— SQLite 存储层
// =============================================================================
class TstRepository : public QObject {
    Q_OBJECT

private slots:
    void openCreatesSchema();
    void saveAndQueryRun();
    void exportCsvContainsRunAndSteps();
    void summaryBatchQueryAndPurge();
    void defaultUsersAuthenticateAndAudit();
    void approverRequiresSecondUser();
    void qualityRoleGatesApproval();
    void alarmLifecycle();
    void workOrderTraceability();
    void mesOutboxLifecycle();
    void qualityClosureAndFixture();
    void spcDataAndShiftTrace();
    void andonLifecycle();
    void handoverRecords();
    void traceSearchAndFlowBinding();
    void disabledUserCannotLogin();
    void reportWhitelistAndBackup();
};

void TstRepository::openCreatesSchema()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));
    QVERIFY(repository.isOpen());
    QVERIFY(repository.recentRuns().isEmpty());
}

void TstRepository::saveAndQueryRun()
{
    QTemporaryDir dir;
    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));

    QVERIFY2(repository.saveRun(makeFlow(), makeResult(), makeMeta(), &error),
             qPrintable(error));

    const int runId = repository.lastInsertedRunId();
    QVERIFY(runId > 0);

    const auto runs = repository.recentRuns();
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.at(0).flowId, QStringLiteral("power"));
    QCOMPARE(runs.at(0).flowName, QStringLiteral("Power Test"));
    QCOMPARE(runs.at(0).sn, QStringLiteral("SN-001"));
    QCOMPARE(runs.at(0).verdict, QStringLiteral("FAIL"));
    QCOMPARE(runs.at(0).durationMs, 450);

    const auto steps = repository.stepsOfRun(runId);
    QCOMPARE(steps.size(), 2);
    QCOMPARE(steps.at(0).status, QStringLiteral("PASS"));
    QCOMPARE(steps.at(0).action, QStringLiteral("command"));
    QCOMPARE(steps.at(1).status, QStringLiteral("FAIL"));
    QVERIFY(steps.at(1).hasMeasured);
    QVERIFY(qAbs(steps.at(1).measured - 5300.0) < 0.01);
    QCOMPARE(steps.at(1).unit, QStringLiteral("mV"));
    QCOMPARE(steps.at(1).metric, QStringLiteral("avg"));
}

void TstRepository::exportCsvContainsRunAndSteps()
{
    QTemporaryDir dir;
    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));
    QVERIFY2(repository.saveRun(makeFlow(), makeResult(), makeMeta(), &error),
             qPrintable(error));

    const QString csvPath = dir.filePath(QStringLiteral("report.csv"));
    QVERIFY2(repository.exportCsv(repository.lastInsertedRunId(), csvPath, &error),
             qPrintable(error));

    QFile file(csvPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray raw = file.readAll();

    // 必须以 UTF-8 BOM 开头，Excel 才能正确识别中文。
    QVERIFY(raw.startsWith("\xEF\xBB\xBF"));

    const QString text = QString::fromUtf8(raw);
    QVERIFY(text.contains(QStringLiteral("Power Test")));
    QVERIFY(text.contains(QStringLiteral("SN-001")));
    QVERIFY(text.contains(QStringLiteral("measure vout")));
    QVERIFY(text.contains(QStringLiteral("5300.00")));
    QVERIFY(text.contains(QStringLiteral("FAIL")));
}

// 摘要：批量写入 → 按时间范围查询 → 过期清理。
void TstRepository::summaryBatchQueryAndPurge()
{
    QTemporaryDir dir;
    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));

    const qint64 base = 1700000000; // 固定时间，避免依赖当前时钟
    QVector<storage::SummaryRow> rows;
    for (int i = 0; i < 10; ++i) {
        storage::SummaryRow row;
        row.deviceId = QStringLiteral("dev1");
        row.channel = 0;
        row.epochSec = base + i;
        row.minValue = 4900.0 + i;
        row.maxValue = 5100.0 + i;
        row.avgValue = 5000.0 + i;
        row.sampleCount = 10000;
        rows.append(row);
    }
    // 另一台设备/通道的数据，不应被查出来。
    storage::SummaryRow other;
    other.deviceId = QStringLiteral("dev2");
    other.channel = 1;
    other.epochSec = base + 3;
    other.sampleCount = 100;
    rows.append(other);

    QVERIFY2(repository.saveSummaryBatch(rows, &error), qPrintable(error));

    const auto trend = repository.summaryTrend(QStringLiteral("dev1"), 0, base, base + 9);
    QCOMPARE(trend.size(), 10);
    QCOMPARE(trend.first().epochSec, base);
    QCOMPARE(trend.last().epochSec, base + 9);
    QVERIFY(qAbs(trend.first().avgValue - 5000.0) < 0.01);
    QCOMPARE(trend.first().sampleCount, 10000);

    // 只查一部分时间范围。
    const auto partial = repository.summaryTrend(QStringLiteral("dev1"), 0, base + 5, base + 9);
    QCOMPARE(partial.size(), 5);

    // 清理 5 秒前的数据：dev1 的前 5 行 + dev2 那 1 行（base+3）。
    QCOMPARE(repository.purgeSummariesBefore(base + 5), 6);
    QCOMPARE(repository.summaryTrend(QStringLiteral("dev1"), 0, base, base + 9).size(), 5);
}

// 用户/登录/审计：默认账号可登录，错误口令失败，两种结果都留审计。
void TstRepository::defaultUsersAuthenticateAndAudit()
{
    QTemporaryDir dir;
    storage::SystemRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("system.db")), &error),
             qPrintable(error));

    security::AuthService auth(&repository);
    QVERIFY2(auth.seedDefaults(&error), qPrintable(error));
    QCOMPARE(repository.userCount(), 3);

    // 错误口令：失败且不切换用户。
    QVERIFY(!auth.login(QStringLiteral("admin"), QStringLiteral("wrong"), &error));
    QVERIFY(!auth.isLoggedIn());

    // 正确口令：角色为管理员，拥有全部权限。
    QVERIFY2(auth.login(QStringLiteral("admin"), QStringLiteral("admin123"), &error),
             qPrintable(error));
    QVERIFY(auth.isLoggedIn());
    QCOMPARE(auth.currentUser().name, QStringLiteral("admin"));
    QCOMPARE(auth.currentUser().role, security::Role::Admin);
    QVERIFY(auth.can(security::Permission::Operate));
    QVERIFY(auth.can(security::Permission::Administer));

    auth.audit(QStringLiteral("导出报表"), QStringLiteral("report.csv"),
               QStringLiteral("成功"));

    // 审计至少包含：失败登录、成功登录、业务操作。
    const auto entries = repository.recentAudit(20);
    QVERIFY(entries.size() >= 3);
    int failedLogins = 0;
    int successfulLogins = 0;
    for (const auto& entry : entries) {
        if (entry.action == QStringLiteral("登录") && entry.result == QStringLiteral("失败"))
            ++failedLogins;
        if (entry.action == QStringLiteral("登录") && entry.result == QStringLiteral("成功"))
            ++successfulLogins;
    }
    QCOMPARE(failedLogins, 1);
    QCOMPARE(successfulLogins, 1);

    // 观察员只有查看权限。
    auth.logout();
    QVERIFY2(auth.login(QStringLiteral("viewer"), QStringLiteral("viewer123"), &error),
             qPrintable(error));
    QCOMPARE(auth.currentUser().role, security::Role::Viewer);
    QVERIFY(!auth.can(security::Permission::Operate));
    QVERIFY(!auth.can(security::Permission::Administer));
}

// 双人复核：必须是第二个人，且具备操作权限；校验过程不切换当前用户。
void TstRepository::approverRequiresSecondUser()
{
    QTemporaryDir dir;
    storage::SystemRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("system.db")), &error),
             qPrintable(error));

    security::AuthService auth(&repository);
    QVERIFY2(auth.seedDefaults(&error), qPrintable(error));
    QVERIFY2(auth.login(QStringLiteral("admin"), QStringLiteral("admin123"), &error),
             qPrintable(error));

    // 1) 复核人 = 操作人本人 → 拒绝。
    QVERIFY(!auth.verifyApprover(QStringLiteral("admin"), QStringLiteral("admin123"),
                                 QStringLiteral("解除Andon停线"), nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("当前操作人")));

    // 2) 口令错误 / 观察员无操作权限 → 拒绝。
    QVERIFY(!auth.verifyApprover(QStringLiteral("operator"), QStringLiteral("bad"),
                                 QStringLiteral("解除Andon停线"), nullptr, &error));
    QVERIFY(!auth.verifyApprover(QStringLiteral("viewer"), QStringLiteral("viewer123"),
                                 QStringLiteral("解除Andon停线"), nullptr, &error));

    // 3) 其他操作员 → 通过，且当前用户仍是 admin（复核不改登录态）。
    security::UserInfo approver;
    QVERIFY2(auth.verifyApprover(QStringLiteral("operator"),
                                 QStringLiteral("operator123"),
                                 QStringLiteral("解除Andon停线"), &approver, &error),
             qPrintable(error));
    QCOMPARE(approver.name, QStringLiteral("operator"));
    QCOMPARE(approver.role, security::Role::Operator);
    QCOMPARE(auth.currentUser().name, QStringLiteral("admin"));

    // 4) 复核结果全部留审计。
    int approvals = 0;
    int rejected = 0;
    for (const auto& entry : repository.recentAudit(30)) {
        if (entry.action != QStringLiteral("复核授权"))
            continue;
        if (entry.result == QStringLiteral("成功"))
            ++approvals;
        else
            ++rejected;
    }
    QCOMPARE(approvals, 1);
    QCOMPARE(rejected, 3);
}

// 质检角色（QA-07+）：严格模式下仅质检员/管理员可复核，操作员被拒。
void TstRepository::qualityRoleGatesApproval()
{
    QTemporaryDir dir;
    storage::SystemRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("system.db")), &error),
             qPrintable(error));

    security::AuthService auth(&repository);
    QVERIFY2(auth.seedDefaults(&error), qPrintable(error));

    // 创建质检员账号（默认账号里没有该角色）。
    const QString salt = security::AuthService::makeSalt();
    const QString password = QStringLiteral("quality123");
    QVERIFY2(repository.insertUser(
                 QStringLiteral("quality"), salt,
                 security::AuthService::hashPassword(salt, password),
                 security::AuthService::roleText(security::Role::Quality), &error),
             qPrintable(error));

    // 权限矩阵：质检员可复核、不可管理；操作员不可复核。
    QVERIFY(security::AuthService::roleAllows(security::Role::Quality,
                                              security::Permission::Approve));
    QVERIFY(!security::AuthService::roleAllows(security::Role::Quality,
                                               security::Permission::Administer));
    QVERIFY(!security::AuthService::roleAllows(security::Role::Operator,
                                               security::Permission::Approve));

    QVERIFY2(auth.login(QStringLiteral("admin"), QStringLiteral("admin123"), &error),
             qPrintable(error));

    // 默认回退：操作员仍可复核（保持历史行为）。
    QVERIFY2(auth.verifyApprover(QStringLiteral("operator"), QStringLiteral("operator123"),
                                 QStringLiteral("解除Andon停线"), nullptr, &error),
             qPrintable(error));

    // 严格模式：操作员被拒，质检员通过。
    auth.setRequireQualityApprover(true);
    QVERIFY(!auth.verifyApprover(QStringLiteral("operator"), QStringLiteral("operator123"),
                                 QStringLiteral("解除Andon停线"), nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("质检员")));

    security::UserInfo approver;
    QVERIFY2(auth.verifyApprover(QStringLiteral("quality"), password,
                                 QStringLiteral("解除Andon停线"), &approver, &error),
             qPrintable(error));
    QCOMPARE(approver.role, security::Role::Quality);
}

// 报警：写入 → 计数 → 确认 → 恢复。
void TstRepository::alarmLifecycle()
{
    QTemporaryDir dir;
    storage::SystemRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("system.db")), &error),
             qPrintable(error));

    storage::AlarmRecord alarm;
    alarm.deviceId = QStringLiteral("dev1");
    alarm.deviceName = QStringLiteral("电源模块-1");
    alarm.channel = 0;
    alarm.channelName = QStringLiteral("CH0");
    alarm.unit = QStringLiteral("mV");
    alarm.level = QStringLiteral("HIGH");
    alarm.value = 6500.0;
    alarm.threshold = 6000.0;

    const int id = repository.saveAlarm(alarm);
    QVERIFY(id > 0);
    QCOMPARE(repository.activeAlarmCount(), 1);

    auto alarms = repository.recentAlarms(10, true);
    QCOMPARE(alarms.size(), 1);
    QCOMPARE(alarms.first().deviceId, QStringLiteral("dev1"));
    QVERIFY(!alarms.first().acknowledged);

    QVERIFY(repository.acknowledgeAlarm(id, QStringLiteral("admin"), 123456));
    alarms = repository.recentAlarms(10, false);
    QCOMPARE(alarms.size(), 1);
    QVERIFY(alarms.first().acknowledged);
    QCOMPARE(alarms.first().ackUser, QStringLiteral("admin"));

    QVERIFY(repository.clearAlarm(id, 123999));
    QCOMPARE(repository.activeAlarmCount(), 0);
    QVERIFY(repository.recentAlarms(10, true).isEmpty());
}

// 工单 + 追溯：一码一测计数、首件通过率、复测标记、不良步骤排名。
void TstRepository::workOrderTraceability()
{
    QTemporaryDir dir;
    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));

    QVERIFY2(repository.createWorkOrder(QStringLiteral("WO-001"), QStringLiteral("PM-5V"),
                                       10, false, 0, &error),
             qPrintable(error));
    QVERIFY(!repository.createWorkOrder(QStringLiteral("WO-001"), QStringLiteral("X"), 1,
                                        false, 0, &error));
    QCOMPARE(repository.workOrders().size(), 1);

    const storage::WorkOrder order = repository.findWorkOrder(QStringLiteral("WO-001"));
    QCOMPARE(order.status, QStringLiteral("ACTIVE"));
    QCOMPARE(order.planQty, 10);

    // 首件 PASS。
    QVERIFY2(repository.saveRun(makeFlow(), makePassResult(),
                                makeMeta(QStringLiteral("SN-001")), &error),
             qPrintable(error));
    QCOMPARE(repository.snTestCount(QStringLiteral("WO-001"), QStringLiteral("SN-001")), 1);

    const storage::HistoryRun last =
        repository.lastRunOfSn(QStringLiteral("WO-001"), QStringLiteral("SN-001"));
    QCOMPARE(last.verdict, QStringLiteral("PASS"));
    QCOMPARE(last.workOrder, QStringLiteral("WO-001"));
    QCOMPARE(last.stationId, QStringLiteral("ST-01"));
    QCOMPARE(last.operatorName, QStringLiteral("admin"));
    QVERIFY(!last.retest);

    storage::WorkOrderStats stats = repository.statsOfOrder(QStringLiteral("WO-001"));
    QCOMPARE(stats.totalRuns, 1);
    QCOMPARE(stats.units, 1);
    QCOMPARE(stats.firstPassUnits, 1);
    QVERIFY(qAbs(stats.fpy - 1.0) < 1e-9);

    // 同一 SN 复测 FAIL：仍算 1 个产品，直通率不变，复测次数 +1。
    QVERIFY2(repository.saveRun(makeFlow(), makeResult(),
                                makeMeta(QStringLiteral("SN-001"), true), &error),
             qPrintable(error));
    stats = repository.statsOfOrder(QStringLiteral("WO-001"));
    QCOMPARE(stats.totalRuns, 2);
    QCOMPARE(stats.retestRuns, 1);
    QCOMPARE(stats.units, 1);
    QCOMPARE(stats.firstPassUnits, 1);
    QCOMPARE(stats.failRuns, 1);

    const auto runs = repository.runsOfOrder(QStringLiteral("WO-001"));
    QCOMPARE(runs.size(), 2);
    QVERIFY(runs.first().retest); // 按时间倒序，最新一条是复测

    const auto ranks = repository.topFailureSteps(QStringLiteral("WO-001"));
    QVERIFY(!ranks.isEmpty());
    QCOMPARE(ranks.first().stepName, QStringLiteral("measure vout"));
    QCOMPARE(ranks.first().count, 1);

    // 工步耗时 Top：两次运行都成功，取平均耗时最高的步骤。
    const auto slow = repository.topSlowSteps(QStringLiteral("WO-001"));
    QVERIFY(!slow.isEmpty());
    QCOMPARE(slow.first().stepName, QStringLiteral("measure vout"));
    QCOMPARE(slow.first().runs, 2);
    QVERIFY(slow.first().avgMs > 0.0);
    QVERIFY(slow.first().maxMs >= static_cast<qint64>(slow.first().avgMs));

    QVERIFY2(repository.closeWorkOrder(QStringLiteral("WO-001"), &error), qPrintable(error));
    QCOMPARE(repository.findWorkOrder(QStringLiteral("WO-001")).status,
             QStringLiteral("CLOSED"));
}

// MES outbox：入队 → 到期取 → 失败退避 → DEAD → 重传 → SENT。
void TstRepository::mesOutboxLifecycle()
{
    QTemporaryDir dir;
    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));

    QVERIFY2(repository.saveRun(makeFlow(), makePassResult(),
                                makeMeta(QStringLiteral("SN-777")), &error),
             qPrintable(error));
    const int runId = repository.lastInsertedRunId();

    const QByteArray payload = repository.buildRunPayload(runId);
    QVERIFY(!payload.isEmpty());
    QVERIFY(payload.contains("SN-777"));
    QVERIFY(payload.contains("WO-001"));
    QVERIFY(payload.contains("measure vout"));

    const qint64 now = 1000;
    QVERIFY2(repository.enqueueUpload(runId, payload, now, &error), qPrintable(error));
    QCOMPARE(repository.uploadCountByStatus(QStringLiteral("PENDING")), 1);

    const auto due = repository.dueUploads(now, 1);
    QCOMPARE(due.size(), 1);
    QCOMPARE(due.first().runId, runId);
    const int outboxId = due.first().id;

    // 失败一次：本次到期不再取，退避时间之后才重新可见。
    QVERIFY(repository.markUploadFailed(outboxId, 1, QStringLiteral("boom"), now + 5000));
    QVERIFY(repository.dueUploads(now, 1).isEmpty());
    const auto retry = repository.dueUploads(now + 6000, 1);
    QCOMPARE(retry.size(), 1);
    QCOMPARE(retry.first().attempts, 1);

    // 达到上限转 DEAD，人工重传后回到 PENDING。
    QVERIFY(repository.markUploadDead(outboxId, 5, QStringLiteral("dead")));
    QCOMPARE(repository.uploadCountByStatus(QStringLiteral("DEAD")), 1);
    QVERIFY(repository.requeueDeadUploads(now + 10000));
    QCOMPARE(repository.uploadCountByStatus(QStringLiteral("PENDING")), 1);

    QVERIFY(repository.markUploaded(outboxId, now + 20000));
    QCOMPARE(repository.uploadCountByStatus(QStringLiteral("SENT")), 1);
    QCOMPARE(repository.uploadCountByStatus(QStringLiteral("PENDING")), 0);
}

// 质量闭环：首件状态机、不良原因、返修统计、治具计数/校准。
void TstRepository::qualityClosureAndFixture()
{
    QTemporaryDir dir;
    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));

    // ---- 首件检验：2 件都 PASS 才进入待确认，确认后放行 ----
    QVERIFY2(repository.createWorkOrder(QStringLiteral("WO-FAI"),
                                        QStringLiteral("PM-5V"), 5, true, 2, &error),
             qPrintable(error));
    storage::WorkOrder order = repository.findWorkOrder(QStringLiteral("WO-FAI"));
    QVERIFY(order.faiRequired);
    QCOMPARE(order.faiStatus, QStringLiteral("PENDING"));
    QCOMPARE(order.faiRequiredCount, 2);

    QVERIFY(repository.updateFaiAfterRun(QStringLiteral("WO-FAI"), QStringLiteral("PASS")));
    order = repository.findWorkOrder(QStringLiteral("WO-FAI"));
    QCOMPARE(order.faiStatus, QStringLiteral("PENDING"));
    QCOMPARE(order.faiTested, 1);

    QVERIFY(repository.updateFaiAfterRun(QStringLiteral("WO-FAI"), QStringLiteral("PASS")));
    order = repository.findWorkOrder(QStringLiteral("WO-FAI"));
    QCOMPARE(order.faiStatus, QStringLiteral("WAITING"));
    QCOMPARE(order.faiTested, 2);

    QVERIFY2(repository.confirmFai(QStringLiteral("WO-FAI"), QStringLiteral("admin"), &error),
             qPrintable(error));
    order = repository.findWorkOrder(QStringLiteral("WO-FAI"));
    QCOMPARE(order.faiStatus, QStringLiteral("CONFIRMED"));
    QCOMPARE(order.faiConfirmedBy, QStringLiteral("admin"));
    QVERIFY(order.faiConfirmedAt > 0);

    // ---- 不良原因 ----
    storage::RunMeta meta = makeMeta(QStringLiteral("SN-Q"));
    meta.workOrder = QStringLiteral("WO-FAI");
    meta.sn = QStringLiteral("SN-Q");
    QVERIFY2(repository.saveRun(makeFlow(), makeResult(), meta, &error), qPrintable(error));
    const int runId = repository.lastInsertedRunId();
    QVERIFY(repository.updateFailReason(runId, QStringLiteral("参数漂移")));

    const storage::HistoryRun last =
        repository.lastRunOfSn(QStringLiteral("WO-FAI"), QStringLiteral("SN-Q"));
    QCOMPARE(last.failReason, QStringLiteral("参数漂移"));

    const auto reasons = repository.topFailureReasons(QStringLiteral("WO-FAI"));
    QCOMPARE(reasons.size(), 1);
    QCOMPARE(reasons.first().reason, QStringLiteral("参数漂移"));
    QCOMPARE(reasons.first().count, 1);

    // ---- 返修闭环：最近一次 FAIL 计待返修；复测 PASS 后清零并计入复测通过 ----
    QVERIFY2(repository.createWorkOrder(QStringLiteral("WO-Q"),
                                        QStringLiteral("PM-5V"), 5, false, 0, &error),
             qPrintable(error));

    storage::RunMeta first = makeMeta(QStringLiteral("SN-R"));
    first.workOrder = QStringLiteral("WO-Q");
    QVERIFY(repository.saveRun(makeFlow(), makePassResult(), first, &error));

    storage::RunMeta retestFail = makeMeta(QStringLiteral("SN-R"), true);
    retestFail.workOrder = QStringLiteral("WO-Q");
    QVERIFY(repository.saveRun(makeFlow(), makeResult(), retestFail, &error));

    storage::WorkOrderStats stats = repository.statsOfOrder(QStringLiteral("WO-Q"));
    QCOMPARE(stats.retestRuns, 1);
    QCOMPARE(stats.pendingRepairUnits, 1);
    QCOMPARE(stats.retestPassRuns, 0);

    storage::RunMeta retestPass = makeMeta(QStringLiteral("SN-R"), true);
    retestPass.workOrder = QStringLiteral("WO-Q");
    QVERIFY(repository.saveRun(makeFlow(), makePassResult(), retestPass, &error));

    stats = repository.statsOfOrder(QStringLiteral("WO-Q"));
    QCOMPARE(stats.retestPassRuns, 1);
    QCOMPARE(stats.pendingRepairUnits, 0);
    QCOMPARE(stats.units, 1);

    // ---- 治具计数与复位 ----
    QCOMPARE(repository.fixtureStatus(QStringLiteral("FX-1")).runCount, 0);
    QVERIFY(repository.recordFixtureUse(QStringLiteral("FX-1"), 5000, 1000));
    QVERIFY(repository.recordFixtureUse(QStringLiteral("FX-1"), 5000, 2000));

    storage::FixtureStatus fixture = repository.fixtureStatus(QStringLiteral("FX-1"));
    QCOMPARE(fixture.runCount, 2);
    QCOMPARE(fixture.lastUsedAt, qint64(2000));
    QCOMPARE(fixture.calibratedUntil, qint64(5000));

    QVERIFY(repository.resetFixture(QStringLiteral("FX-1"), 9000, 3000));
    fixture = repository.fixtureStatus(QStringLiteral("FX-1"));
    QCOMPARE(fixture.runCount, 0);
    QCOMPARE(fixture.calibratedUntil, qint64(9000));
}

// SPC 数据源：测试项聚合 + 实测值序列 + 班次追溯。
void TstRepository::spcDataAndShiftTrace()
{
    QTemporaryDir dir;
    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));

    storage::RunMeta meta = makeMeta(QStringLiteral("SN-A"));
    meta.workOrder = QStringLiteral("WO-SPC");
    meta.shift = QStringLiteral("白班");
    QVERIFY(repository.saveRun(makeFlow(), makeResult(), meta, &error));

    meta = makeMeta(QStringLiteral("SN-B"));
    meta.workOrder = QStringLiteral("WO-SPC");
    meta.shift = QStringLiteral("夜班");
    QVERIFY(repository.saveRun(makeFlow(), makePassResult(), meta, &error));

    // runById 能取到完整追溯信息（含班次）。
    const storage::HistoryRun run = repository.runById(repository.lastInsertedRunId());
    QVERIFY(run.id > 0);
    QCOMPARE(run.sn, QStringLiteral("SN-B"));
    QCOMPARE(run.shift, QStringLiteral("夜班"));

    // 测试项按 flow + step 聚合，并带上流程里配置的规格限。
    const QVector<storage::SpcItem> items =
        repository.spcItems(QStringLiteral("WO-SPC"));
    QCOMPARE(items.size(), 1);
    QCOMPARE(items.at(0).flowId, QStringLiteral("power"));
    QCOMPARE(items.at(0).stepId, QStringLiteral("s2"));
    QVERIFY(items.at(0).hasLsl);
    QVERIFY(items.at(0).hasUsl);
    QCOMPARE(items.at(0).lsl, 4750.0);
    QCOMPARE(items.at(0).usl, 5250.0);
    QCOMPARE(items.at(0).count, 2);

    // 实测值按时间升序：第一次 FAIL 5300，第二次 PASS 5000。
    const QVector<storage::SpcPoint> points = repository.spcPoints(
        QStringLiteral("WO-SPC"), QStringLiteral("power"), QStringLiteral("s2"));
    QCOMPARE(points.size(), 2);
    QCOMPARE(points.at(0).value, 5300.0);
    QCOMPARE(points.at(0).sn, QStringLiteral("SN-A"));
    QCOMPARE(points.at(1).value, 5000.0);
    QCOMPARE(points.at(1).sn, QStringLiteral("SN-B"));

    // 连续 FAIL：最新是 PASS，所以为 0；再补一次 FAIL 后为 1。
    QCOMPARE(repository.consecutiveFailRuns(QStringLiteral("WO-SPC")), 0);
    meta = makeMeta(QStringLiteral("SN-C"));
    meta.workOrder = QStringLiteral("WO-SPC");
    QVERIFY(repository.saveRun(makeFlow(), makeResult(), meta, &error));
    QCOMPARE(repository.consecutiveFailRuns(QStringLiteral("WO-SPC")), 1);
}

// Andon：锁定 → 状态可查 → 解除。
void TstRepository::andonLifecycle()
{
    QTemporaryDir dir;
    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));

    QVERIFY(!repository.andonState(QStringLiteral("ST-01")).locked);

    QVERIFY(repository.lockStation(QStringLiteral("ST-01"),
                                   QStringLiteral("Andon 停线"),
                                   QStringLiteral("连续 3 件 FAIL")));
    // 重复锁定不应产生第二条未解除事件（状态仍是锁定）。
    QVERIFY(repository.lockStation(QStringLiteral("ST-01"),
                                   QStringLiteral("Andon 停线"),
                                   QStringLiteral("重复")));

    const storage::AndonState state = repository.andonState(QStringLiteral("ST-01"));
    QVERIFY(state.locked);
    QCOMPARE(state.reason, QStringLiteral("Andon 停线"));
    QCOMPARE(state.detail, QStringLiteral("连续 3 件 FAIL"));
    QVERIFY(state.createdAt > 0);

    // 别的工位不受影响。
    QVERIFY(!repository.andonState(QStringLiteral("ST-02")).locked);

    QVERIFY(repository.clearAndon(QStringLiteral("ST-01"), QStringLiteral("admin")));
    QVERIFY(!repository.andonState(QStringLiteral("ST-01")).locked);
}

// 交接班记录：写入与读取。
void TstRepository::handoverRecords()
{
    QTemporaryDir dir;
    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));

    storage::HandoverRecord record;
    record.orderNo = QStringLiteral("WO-1");
    record.lineId = QStringLiteral("L1");
    record.stationId = QStringLiteral("ST-01");
    record.shift = QStringLiteral("白班");
    record.operatorName = QStringLiteral("operator");
    record.units = 12;
    record.passRuns = 11;
    record.failRuns = 1;
    record.note = QStringLiteral("治具接触偶发不良");
    record.createdAt = 1700000000000;
    QVERIFY2(repository.saveHandover(record, &error), qPrintable(error));

    const QVector<storage::HandoverRecord> records = repository.recentHandovers();
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.at(0).orderNo, QStringLiteral("WO-1"));
    QCOMPARE(records.at(0).shift, QStringLiteral("白班"));
    QCOMPARE(records.at(0).units, 12);
    QCOMPARE(records.at(0).failRuns, 1);
    QCOMPARE(records.at(0).note, QStringLiteral("治具接触偶发不良"));

    storage::HandoverRecord invalid = record;
    invalid.operatorName.clear();
    QVERIFY(!repository.saveHandover(invalid, &error));
}

// 追溯检索 + 工单绑定方案。
void TstRepository::traceSearchAndFlowBinding()
{
    QTemporaryDir dir;
    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));

    const QString flowPath = QStringLiteral("configs/flows/power_module_basic.json");
    QVERIFY2(repository.createWorkOrder(QStringLiteral("WO-SEARCH"),
                                       QStringLiteral("PM-5V"), 10, false, 1,
                                       flowPath, &error),
             qPrintable(error));
    QCOMPARE(repository.findWorkOrder(QStringLiteral("WO-SEARCH")).flowPath, flowPath);

    // 不带方案的重载仍然可用，绑定为空。
    QVERIFY2(repository.createWorkOrder(QStringLiteral("WO-NOBIND"),
                                       QStringLiteral("PM-5V"), 5, false, 1, &error),
             qPrintable(error));
    QVERIFY(repository.findWorkOrder(QStringLiteral("WO-NOBIND")).flowPath.isEmpty());

    storage::RunMeta meta = makeMeta(QStringLiteral("SN-A"));
    meta.workOrder = QStringLiteral("WO-SEARCH");
    QVERIFY(repository.saveRun(makeFlow(), makePassResult(), meta, &error));

    meta = makeMeta(QStringLiteral("SN-B"));
    meta.workOrder = QStringLiteral("WO-SEARCH");
    QVERIFY(repository.saveRun(makeFlow(), makeResult(), meta, &error));

    // 关键字命中 SN / 工单；判定过滤可叠加。
    auto runs = repository.searchRuns(QStringLiteral("SN-A"), QString());
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.at(0).sn, QStringLiteral("SN-A"));

    QCOMPARE(repository.searchRuns(QString(), QStringLiteral("FAIL")).size(), 1);
    QCOMPARE(repository.searchRuns(QStringLiteral("WO-SEARCH"),
                                   QStringLiteral("PASS")).size(), 1);
    QCOMPARE(repository.searchRuns(QStringLiteral("WO-SEARCH"), QString()).size(), 2);
    QCOMPARE(repository.searchRuns(QStringLiteral("SN"), QString()).size(), 2);
    QVERIFY(repository.searchRuns(QStringLiteral("no-such"), QString()).isEmpty());

    // 通配符按字面处理，不会被当成 LIKE。
    QVERIFY(repository.searchRuns(QStringLiteral("%"), QString()).isEmpty());
}

// 用户管理：停用后不能登录；角色/口令可更新。
void TstRepository::disabledUserCannotLogin()
{
    QTemporaryDir dir;
    storage::SystemRepository system;
    QString error;
    QVERIFY2(system.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));

    security::AuthService auth(&system);
    QVERIFY2(auth.seedDefaults(&error), qPrintable(error));
    QCOMPARE(system.users().size(), 3);
    QCOMPARE(system.enabledAdminCount(), 1);

    QVERIFY2(system.setUserEnabled(QStringLiteral("viewer"), false, &error),
             qPrintable(error));
    QString loginError;
    QVERIFY(!auth.login(QStringLiteral("viewer"), QStringLiteral("viewer123"), &loginError));
    QVERIFY(loginError.contains(QStringLiteral("停用")));

    QVERIFY(system.setUserEnabled(QStringLiteral("viewer"), true, &error));
    QVERIFY(auth.login(QStringLiteral("viewer"), QStringLiteral("viewer123"), &loginError));
    auth.logout();

    QVERIFY(system.updateUserRole(QStringLiteral("operator"),
                                  QStringLiteral("admin"), &error));
    QCOMPARE(system.enabledAdminCount(), 2);
    QVERIFY(system.updateUserPassword(QStringLiteral("operator"),
                                      QStringLiteral("salt-x"),
                                      QStringLiteral("hash-x"), &error));

    storage::UserRow row;
    QVERIFY(system.findUser(QStringLiteral("operator"), &row));
    QCOMPARE(row.role, QStringLiteral("admin"));
    QCOMPARE(row.salt, QStringLiteral("salt-x"));
    QCOMPARE(row.hash, QStringLiteral("hash-x"));
    QVERIFY(row.enabled);
}

// 日报聚合 + SN 白名单 + 数据库备份/压缩。
void TstRepository::reportWhitelistAndBackup()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("test.db"));

    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dbPath, &error), qPrintable(error));
    QVERIFY2(repository.createWorkOrder(QStringLiteral("WO-R"),
                                       QStringLiteral("PM-5V"), 10, false, 1, &error),
             qPrintable(error));

    storage::RunMeta day = makeMeta(QStringLiteral("SN-A"));
    day.workOrder = QStringLiteral("WO-R");
    day.shift = QStringLiteral("白班");
    QVERIFY(repository.saveRun(makeFlow(), makePassResult(), day, &error));

    storage::RunMeta night = makeMeta(QStringLiteral("SN-B"));
    night.workOrder = QStringLiteral("WO-R");
    night.shift = QStringLiteral("夜班");
    QVERIFY(repository.saveRun(makeFlow(), makeResult(), night, &error));

    // 日报按班次聚合：白班 1 条 PASS，夜班 1 条 FAIL。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QVector<storage::ProductionReportRow> rows =
        repository.productionReport(0, now + 60000, QStringLiteral("WO-R"));
    QCOMPARE(rows.size(), 2);

    bool sawDay = false;
    bool sawNight = false;
    for (const storage::ProductionReportRow& row : rows) {
        QVERIFY(!row.date.isEmpty());
        if (row.shift == QStringLiteral("白班")) {
            sawDay = true;
            QCOMPARE(row.runs, 1);
            QCOMPARE(row.passRuns, 1);
            QCOMPARE(row.yieldRate, 1.0);
        } else if (row.shift == QStringLiteral("夜班")) {
            sawNight = true;
            QCOMPARE(row.runs, 1);
            QCOMPARE(row.failRuns, 1);
        }
    }
    QVERIFY(sawDay);
    QVERIFY(sawNight);

    // SN 白名单：去重、忽略空行。
    QCOMPARE(repository.importSnWhitelist(
                 QStringLiteral("WO-R"),
                 {QStringLiteral("SN-A"), QStringLiteral("SN-B"),
                  QStringLiteral("SN-A"), QString()},
                 &error),
             2);
    QCOMPARE(repository.snWhitelistCount(QStringLiteral("WO-R")), 2);
    QVERIFY(repository.snInWhitelist(QStringLiteral("WO-R"), QStringLiteral("SN-A")));
    QVERIFY(!repository.snInWhitelist(QStringLiteral("WO-R"), QStringLiteral("SN-C")));

    // 备份与压缩：同一库文件的系统连接。
    storage::SystemRepository system;
    QVERIFY2(system.open(dbPath, &error), qPrintable(error));
    const QString backupPath = dir.filePath(QStringLiteral("backup.db"));
    QVERIFY2(system.backupTo(backupPath, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(backupPath));
    QVERIFY(QFileInfo(backupPath).size() > 0);
    QVERIFY2(system.vacuum(&error), qPrintable(error));
}

QTEST_GUILESS_MAIN(TstRepository)

#include "tst_repository.moc"
