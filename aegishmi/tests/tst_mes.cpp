// Qt Test 框架
#include <QtTest>

#include <QFile>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include "core/station_config.h"
#include "service/bom_verifier.h"
#include "service/mes_station_checker.h"
#include "service/mes_uploader.h"
#include "storage/system_repository.h"
#include "storage/test_repository.h"

using namespace aegis;

namespace {

// =============================================================================
// FakeHttpServer —— 进程内极简 HTTP 服务端
//
// 只做一件事：读到一个完整 POST 请求后回 200 OK，并记录请求体。
// 用于验证 MesUploader 的"入队 → 发送 → 标记已上传"闭环，不依赖外部服务。
// =============================================================================
class FakeHttpServer : public QObject {
    Q_OBJECT

public:
    FakeHttpServer()
    {
        server_.listen(QHostAddress::LocalHost, 0);
        connect(&server_, &QTcpServer::newConnection,
                this, &FakeHttpServer::onNewConnection);
    }

    quint16 port() const { return server_.serverPort(); }
    QByteArray lastBody() const { return lastBody_; }
    QByteArray lastPath() const { return lastPath_; }
    int requests() const { return requests_; }

    // 自定义响应体（BOM 核对返回 JSON；结果上传默认回 OK）。
    void setResponseBody(const QByteArray& body) { responseBody_ = body; }

private slots:
    void onNewConnection()
    {
        while (QTcpSocket* socket = server_.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                buffer_ += socket->readAll();

                const int headerEnd = buffer_.indexOf("\r\n\r\n");
                if (headerEnd < 0)
                    return;

                int contentLength = 0;
                const QByteArray header = buffer_.left(headerEnd);
                for (const QByteArray& line : header.split('\n')) {
                    if (line.toLower().startsWith("content-length:"))
                        contentLength = line.mid(15).trimmed().toInt();
                }

                const int total = headerEnd + 4 + contentLength;
                if (buffer_.size() < total)
                    return;

                lastBody_ = buffer_.mid(headerEnd + 4, contentLength);
                lastPath_ = header.split('\n').value(0).split(' ').value(1);
                ++requests_;

                QByteArray response = "HTTP/1.1 200 OK\r\n"
                                      "Content-Type: application/json\r\n"
                                      "Content-Length: "
                    + QByteArray::number(responseBody_.size())
                    + "\r\nConnection: close\r\n\r\n" + responseBody_;
                socket->write(response);
                socket->disconnectFromHost();
                buffer_.clear();
            });

            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

private:
    QTcpServer server_;
    QByteArray buffer_; // 单请求测试够用
    QByteArray lastBody_;
    QByteArray lastPath_;
    QByteArray responseBody_ = QByteArrayLiteral("OK");
    int requests_ = 0;
};

storage::RunMeta makeMeta()
{
    storage::RunMeta meta;
    meta.sn = QStringLiteral("SN-900");
    meta.workOrder = QStringLiteral("WO-9");
    meta.lineId = QStringLiteral("L1");
    meta.stationId = QStringLiteral("ST-01");
    meta.operatorName = QStringLiteral("tester");
    meta.fixture = QStringLiteral("FX-1");
    return meta;
}

// 保存一条最小运行记录，返回 runId。
int saveRun(storage::TestRepository& repository)
{
    test::TestFlow flow;
    flow.schemaVersion = 1;
    flow.id = QStringLiteral("flow");
    flow.name = QStringLiteral("Flow");
    flow.product = QStringLiteral("PM-5V");

    test::RunResult result;
    result.verdict = test::RunVerdict::Pass;
    result.totalElapsedMs = 10;

    test::StepResult step;
    step.index = 0;
    step.id = QStringLiteral("s1");
    step.name = QStringLiteral("measure");
    step.status = test::StepStatus::Pass;
    step.measured = 5000.0;
    step.hasMeasured = true;
    result.steps = {step};

    if (!repository.saveRun(flow, result, makeMeta()))
        return 0;
    return repository.lastInsertedRunId();
}

} // namespace

// =============================================================================
// TstMes —— 测试结果上传 MES
// =============================================================================
class TstMes : public QObject {
    Q_OBJECT

private slots:
    void uploadsRunAndMarksSent();
    void uploadBusinessFailureGoesDead();
    void disabledDoesNothing();
    void bomCheckMatchAllows();
    void bomCheckMismatchLocksAndSupervisorUnlocks();
    void bomCheckUnavailableFollowsFailOpen();
    void bomCheckConfigParsed();
    void stationCheckPassAllows();
    void stationCheckRejectBlocks();
    void stationCheckUnavailableFollowsFailOpen();
};

// 正常上传：HTTP 200 后 outbox 从 PENDING 变 SENT，请求体包含 SN/工单。
void TstMes::uploadsRunAndMarksSent()
{
    QTemporaryDir dir;
    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));

    const int runId = saveRun(repository);
    QVERIFY(runId > 0);

    FakeHttpServer server;
    core::StationConfig config;
    config.mesEnabled = true;
    config.mesUrl = QStringLiteral("http://127.0.0.1:%1/api/test-results")
                        .arg(server.port());
    config.mesTimeoutMs = 3000;
    config.mesMaxAttempts = 2;
    config.mesRetryBaseMs = 200;

    storage::SystemRepository system;
    QVERIFY2(system.open(dir.filePath(QStringLiteral("system.db")), &error),
             qPrintable(error));

    service::MesUploader uploader(&repository, config, &system);
    uploader.start();
    uploader.enqueue(runId);

    QTRY_VERIFY_WITH_TIMEOUT(server.requests() >= 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        repository.uploadCountByStatus(QStringLiteral("SENT")) == 1, 5000);

    QVERIFY(server.lastBody().contains("SN-900"));
    QVERIFY(server.lastBody().contains("WO-9"));
    QVERIFY(server.lastBody().contains("measure"));

    // 上传结果必须留痕：审计动作 MES上传 / 结果 成功。
    bool audited = false;
    for (const storage::AuditEntry& entry : system.recentAudit(20)) {
        if (entry.action == QStringLiteral("MES上传")
            && entry.result == QStringLiteral("成功"))
            audited = true;
    }
    QVERIFY(audited);

    uploader.stop();
}

// HTTP 200 但响应体 ok=false 属于业务失败，不能标记为已上传。
void TstMes::uploadBusinessFailureGoesDead()
{
    QTemporaryDir dir;
    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));

    const int runId = saveRun(repository);
    QVERIFY(runId > 0);

    FakeHttpServer server;
    server.setResponseBody(QByteArrayLiteral("{\"ok\":false,\"message\":\"SN 不存在\"}"));

    core::StationConfig config;
    config.mesEnabled = true;
    config.mesUrl = QStringLiteral("http://127.0.0.1:%1/api/test-results").arg(server.port());
    config.mesTimeoutMs = 3000;
    config.mesMaxAttempts = 1;
    config.mesRetryBaseMs = 200;

    storage::SystemRepository system;
    QVERIFY2(system.open(dir.filePath(QStringLiteral("system.db")), &error),
             qPrintable(error));

    service::MesUploader uploader(&repository, config, &system);
    uploader.start();
    uploader.enqueue(runId);

    QTRY_VERIFY_WITH_TIMEOUT(
        repository.uploadCountByStatus(QStringLiteral("DEAD")) == 1, 5000);
    QCOMPARE(repository.uploadCountByStatus(QStringLiteral("SENT")), 0);

    bool audited = false;
    for (const storage::AuditEntry& entry : system.recentAudit(20)) {
        if (entry.action == QStringLiteral("MES上传")
            && entry.result == QStringLiteral("失败")
            && entry.detail.contains(QStringLiteral("SN 不存在")))
            audited = true;
    }
    QVERIFY(audited);

    uploader.stop();
}

// MES 未启用：不入队、不发送。
void TstMes::disabledDoesNothing()
{
    QTemporaryDir dir;
    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));

    const int runId = saveRun(repository);
    QVERIFY(runId > 0);

    core::StationConfig config;
    config.mesEnabled = false;

    service::MesUploader uploader(&repository, config);
    uploader.start();
    uploader.enqueue(runId);

    QCOMPARE(repository.uploadCountByStatus(QStringLiteral("PENDING")), 0);
    QCOMPARE(repository.uploadCountByStatus(QStringLiteral("SENT")), 0);
}

// 料号一致：放行，请求体带工单/产品/料号，并写审计。
void TstMes::bomCheckMatchAllows()
{
    QTemporaryDir dir;
    storage::SystemRepository system;
    QString error;
    QVERIFY2(system.open(dir.filePath(QStringLiteral("system.db")), &error),
             qPrintable(error));

    FakeHttpServer server;
    server.setResponseBody(QByteArrayLiteral("{\"match\":true,\"expected\":\"MAT-1\"}"));

    core::StationConfig config;
    config.mesEnabled = true;
    config.mesUrl = QStringLiteral("http://127.0.0.1:%1/api/test-results").arg(server.port());
    config.mesBomCheckPath = QStringLiteral("/api/bom/check");
    config.mesTimeoutMs = 3000;

    service::BomVerifier verifier(config, &system);
    bool done = false;
    service::BomCheckResult captured;
    connect(&verifier, &service::BomVerifier::verified, this,
            [&](const service::BomCheckResult& result) {
                captured = result;
                done = true;
            });

    verifier.verify(QStringLiteral("WO-9"), QStringLiteral("PM-5V"),
                    QStringLiteral("MAT-1"), QStringLiteral("tester"));
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);

    QVERIFY(captured.ok);
    QVERIFY(captured.match);
    QCOMPARE(captured.expected, QStringLiteral("MAT-1"));
    QVERIFY(!verifier.locked());
    QCOMPARE(server.lastPath(), QByteArrayLiteral("/api/bom/check"));
    QVERIFY(server.lastBody().contains("MAT-1"));
    QVERIFY(server.lastBody().contains("WO-9"));

    bool audited = false;
    for (const storage::AuditEntry& entry : system.recentAudit(20)) {
        if (entry.action == QStringLiteral("物料核对")
            && entry.target == QStringLiteral("MAT-1")
            && entry.result == QStringLiteral("成功"))
            audited = true;
    }
    QVERIFY(audited);
}

// 料号不符：锁站；班组长解锁后恢复，并各写一条审计。
void TstMes::bomCheckMismatchLocksAndSupervisorUnlocks()
{
    QTemporaryDir dir;
    storage::SystemRepository system;
    QString error;
    QVERIFY2(system.open(dir.filePath(QStringLiteral("system.db")), &error),
             qPrintable(error));

    FakeHttpServer server;
    server.setResponseBody(QByteArrayLiteral("{\"match\":false,\"expected\":\"MAT-2\"}"));

    core::StationConfig config;
    config.mesUrl = QStringLiteral("http://127.0.0.1:%1/api/test-results").arg(server.port());
    config.mesBomCheckPath = QStringLiteral("/api/bom/check");
    config.mesTimeoutMs = 3000;

    service::BomVerifier verifier(config, &system);
    bool done = false;
    service::BomCheckResult captured;
    connect(&verifier, &service::BomVerifier::verified, this,
            [&](const service::BomCheckResult& result) {
                captured = result;
                done = true;
            });

    verifier.verify(QStringLiteral("WO-9"), QStringLiteral("PM-5V"),
                    QStringLiteral("MAT-WRONG"), QStringLiteral("tester"));
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);

    QVERIFY(captured.ok);
    QVERIFY(!captured.match);
    QCOMPARE(captured.expected, QStringLiteral("MAT-2"));
    QVERIFY(verifier.locked());
    QVERIFY(verifier.lockReason().contains(QStringLiteral("MAT-WRONG")));

    verifier.supervisorUnlock(QStringLiteral("lead"));
    QVERIFY(!verifier.locked());

    bool lockedAudit = false;
    bool unlockAudit = false;
    for (const storage::AuditEntry& entry : system.recentAudit(20)) {
        if (entry.action != QStringLiteral("物料核对"))
            continue;
        if (entry.result == QStringLiteral("锁定"))
            lockedAudit = true;
        if (entry.result == QStringLiteral("解锁")
            && entry.detail.contains(QStringLiteral("lead")))
            unlockAudit = true;
    }
    QVERIFY(lockedAudit);
    QVERIFY(unlockAudit);
}

// MES 不可用：failOpen=true 放行；failOpen=false 锁站。
void TstMes::bomCheckUnavailableFollowsFailOpen()
{
    QTemporaryDir dir;
    storage::SystemRepository system;
    QString error;
    QVERIFY2(system.open(dir.filePath(QStringLiteral("system.db")), &error),
             qPrintable(error));

    // 端口 1 无服务，连接立即被拒绝，模拟 MES 断网。
    core::StationConfig config;
    config.mesBomCheckPath = QStringLiteral("http://127.0.0.1:1/api/bom/check");
    config.mesTimeoutMs = 800;
    config.mesFailOpen = true;

    {
        service::BomVerifier open(config, &system);
        bool done = false;
        service::BomCheckResult captured;
        connect(&open, &service::BomVerifier::verified, this,
                [&](const service::BomCheckResult& result) {
                    captured = result;
                    done = true;
                });
        open.verify(QStringLiteral("WO-9"), QStringLiteral("PM-5V"),
                    QStringLiteral("MAT-1"), QStringLiteral("tester"));
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY(!captured.ok);
        QVERIFY(!open.locked());
    }

    config.mesFailOpen = false;
    service::BomVerifier strict(config, &system);
    bool done = false;
    connect(&strict, &service::BomVerifier::verified, this,
            [&](const service::BomCheckResult&) { done = true; });
    strict.verify(QStringLiteral("WO-9"), QStringLiteral("PM-5V"),
                  QStringLiteral("MAT-1"), QStringLiteral("tester"));
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(strict.locked());

    bool degradeAudit = false;
    bool lockAudit = false;
    for (const storage::AuditEntry& entry : system.recentAudit(20)) {
        if (entry.action != QStringLiteral("物料核对"))
            continue;
        if (entry.result == QStringLiteral("降级放行"))
            degradeAudit = true;
        if (entry.result == QStringLiteral("锁定"))
            lockAudit = true;
    }
    QVERIFY(degradeAudit);
    QVERIFY(lockAudit);
}

// station.json：mes.bomCheckPath / mes.failOpen 解析。
void TstMes::bomCheckConfigParsed()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("station.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"stationId":"ST-9","mes":{"url":"http://mes.local/api",)"
               R"("bomCheckPath":"/api/bom/check","stationCheckPath":"/api/station/check",)"
               R"("failOpen":false}})");
    file.close();

    QString error;
    const core::StationConfig config = core::StationConfig::load(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(config.mesBomCheckPath, QStringLiteral("/api/bom/check"));
    QCOMPARE(config.mesStationCheckPath, QStringLiteral("/api/station/check"));
    QCOMPARE(config.mesFailOpen, false);
}

// 过站校验通过：请求体带 SN/工单/工位，结果 passed，写审计。
void TstMes::stationCheckPassAllows()
{
    QTemporaryDir dir;
    storage::SystemRepository system;
    QString error;
    QVERIFY2(system.open(dir.filePath(QStringLiteral("system.db")), &error),
             qPrintable(error));

    FakeHttpServer server;
    server.setResponseBody(QByteArrayLiteral("{\"ok\":true}"));

    core::StationConfig config;
    config.mesUrl = QStringLiteral("http://127.0.0.1:%1/api/test-results").arg(server.port());
    config.mesStationCheckPath = QStringLiteral("/api/station/check");
    config.mesTimeoutMs = 3000;

    service::MesStationChecker checker(config, &system);
    bool done = false;
    service::StationCheckResult captured;
    connect(&checker, &service::MesStationChecker::checked, this,
            [&](const service::StationCheckResult& result) {
                captured = result;
                done = true;
            });

    checker.check(QStringLiteral("WO-9"), QStringLiteral("SN-900"),
                  QStringLiteral("PM-5V"), QStringLiteral("tester"),
                  QStringLiteral("FX-1"));
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);

    QVERIFY(captured.ok);
    QVERIFY(captured.passed);
    QCOMPARE(captured.sn, QStringLiteral("SN-900"));
    QCOMPARE(server.lastPath(), QByteArrayLiteral("/api/station/check"));
    QVERIFY(server.lastBody().contains("SN-900"));
    QVERIFY(server.lastBody().contains("WO-9"));

    bool audited = false;
    for (const storage::AuditEntry& entry : system.recentAudit(20)) {
        if (entry.action == QStringLiteral("过站校验")
            && entry.target == QStringLiteral("SN-900")
            && entry.result == QStringLiteral("通过"))
            audited = true;
    }
    QVERIFY(audited);
}

// 过站校验被 MES 拒绝：结果 passed=false，原因透传，写审计。
void TstMes::stationCheckRejectBlocks()
{
    QTemporaryDir dir;
    storage::SystemRepository system;
    QString error;
    QVERIFY2(system.open(dir.filePath(QStringLiteral("system.db")), &error),
             qPrintable(error));

    FakeHttpServer server;
    server.setResponseBody(
        QByteArrayLiteral("{\"ok\":false,\"message\":\"工单已关闭\"}"));

    core::StationConfig config;
    config.mesUrl = QStringLiteral("http://127.0.0.1:%1/api/test-results").arg(server.port());
    config.mesStationCheckPath = QStringLiteral("/api/station/check");
    config.mesTimeoutMs = 3000;

    service::MesStationChecker checker(config, &system);
    bool done = false;
    service::StationCheckResult captured;
    connect(&checker, &service::MesStationChecker::checked, this,
            [&](const service::StationCheckResult& result) {
                captured = result;
                done = true;
            });

    checker.check(QStringLiteral("WO-9"), QStringLiteral("SN-900"),
                  QStringLiteral("PM-5V"), QStringLiteral("tester"), QString());
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);

    QVERIFY(captured.ok);
    QVERIFY(!captured.passed);
    QCOMPARE(captured.message, QStringLiteral("工单已关闭"));

    bool audited = false;
    for (const storage::AuditEntry& entry : system.recentAudit(20)) {
        if (entry.action == QStringLiteral("过站校验")
            && entry.result == QStringLiteral("拦截")
            && entry.detail.contains(QStringLiteral("工单已关闭")))
            audited = true;
    }
    QVERIFY(audited);
}

// MES 不可用：failOpen=true 写"降级放行"审计；failOpen=false 写"拦截"。
void TstMes::stationCheckUnavailableFollowsFailOpen()
{
    QTemporaryDir dir;
    storage::SystemRepository system;
    QString error;
    QVERIFY2(system.open(dir.filePath(QStringLiteral("system.db")), &error),
             qPrintable(error));

    core::StationConfig config;
    config.mesStationCheckPath = QStringLiteral("http://127.0.0.1:1/api/station/check");
    config.mesTimeoutMs = 800;
    config.mesFailOpen = true;

    {
        service::MesStationChecker open(config, &system);
        bool done = false;
        service::StationCheckResult captured;
        connect(&open, &service::MesStationChecker::checked, this,
                [&](const service::StationCheckResult& result) {
                    captured = result;
                    done = true;
                });
        open.check(QStringLiteral("WO-9"), QStringLiteral("SN-900"),
                   QStringLiteral("PM-5V"), QStringLiteral("tester"), QString());
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QVERIFY(!captured.ok);
    }

    config.mesFailOpen = false;
    service::MesStationChecker strict(config, &system);
    bool done = false;
    service::StationCheckResult captured;
    connect(&strict, &service::MesStationChecker::checked, this,
            [&](const service::StationCheckResult& result) {
                captured = result;
                done = true;
            });
    strict.check(QStringLiteral("WO-9"), QStringLiteral("SN-900"),
                 QStringLiteral("PM-5V"), QStringLiteral("tester"), QString());
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(!captured.ok);

    bool degradeAudit = false;
    bool blockAudit = false;
    for (const storage::AuditEntry& entry : system.recentAudit(20)) {
        if (entry.action != QStringLiteral("过站校验"))
            continue;
        if (entry.result == QStringLiteral("降级放行"))
            degradeAudit = true;
        if (entry.result == QStringLiteral("拦截"))
            blockAudit = true;
    }
    QVERIFY(degradeAudit);
    QVERIFY(blockAudit);
}

QTEST_GUILESS_MAIN(TstMes)

#include "tst_mes.moc"
