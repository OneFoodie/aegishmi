// 轻量版本检测（SY-05）：版本比较、清单解析、SHA-256、离线跳过
#include <QtTest>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "core/station_config.h"
#include "service/update_checker.h"

using namespace aegis;

namespace {

// 写一个 version.json 到临时目录，返回绝对路径。
QString writeManifest(const QTemporaryDir& dir, const QString& version,
                      const QString& sha256 = QString())
{
    QJsonObject object;
    object.insert(QStringLiteral("version"), version);
    object.insert(QStringLiteral("notes"), QStringLiteral("修复若干问题"));
    object.insert(QStringLiteral("url"), QStringLiteral("upgrade/aegis-0.2.0.zip"));
    if (!sha256.isEmpty())
        object.insert(QStringLiteral("sha256"), sha256);
    object.insert(QStringLiteral("minVersion"), QStringLiteral("0.1.0"));

    const QString path = dir.filePath(QStringLiteral("version.json"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    file.write(QJsonDocument(object).toJson());
    return path;
}

core::StationConfig updateConfig(const QString& manifest)
{
    core::StationConfig config;
    config.updateEnabled = true;
    config.updateManifest = manifest;
    return config;
}

} // namespace

class TstUpdate : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void versionComparison();
    void manifestParse();
    void invalidManifestRejected();
    void localManifestDetectsUpdate();
    void upToDateWhenSameVersion();
    void missingManifestSkipsSilently();
    void disabledCheckerSkips();
    void sha256Verification();
};

void TstUpdate::initTestCase()
{
    qRegisterMetaType<service::VersionInfo>("aegis::service::VersionInfo");
    qRegisterMetaType<service::UpdateChecker::Status>("aegis::service::UpdateChecker::Status");
}

// 版本比较：数字段逐段比较，v 前缀与预发布后缀不影响结果。
void TstUpdate::versionComparison()
{
    QVERIFY(service::compareVersions(QStringLiteral("1.2.10"), QStringLiteral("1.2.9")) > 0);
    QVERIFY(service::compareVersions(QStringLiteral("1.2.0"), QStringLiteral("1.2")) == 0);
    QVERIFY(service::compareVersions(QStringLiteral("v0.2.0"),
                                     QStringLiteral("0.1.9")) > 0);
    QVERIFY(service::compareVersions(QStringLiteral("0.1.0"),
                                     QStringLiteral("0.1.0-rc1")) == 0);
    QVERIFY(service::compareVersions(QStringLiteral("1.9"), QStringLiteral("1.10")) < 0);
}

// 清单解析：必需 version，其余字段可选。
void TstUpdate::manifestParse()
{
    const QByteArray data = R"({
        "version": "0.2.0",
        "notes": "新增插件",
        "url": "http://intranet/upgrade/aegis-0.2.0.zip",
        "sha256": "ABCDEF",
        "minVersion": "0.1.0"
    })";

    service::VersionInfo info;
    QString error;
    QVERIFY2(service::parseVersionManifest(data, &info, &error), qPrintable(error));
    QCOMPARE(info.version, QStringLiteral("0.2.0"));
    QCOMPARE(info.notes, QStringLiteral("新增插件"));
    QCOMPARE(info.url, QStringLiteral("http://intranet/upgrade/aegis-0.2.0.zip"));
    QCOMPARE(info.sha256, QStringLiteral("abcdef")); // 统一小写，便于比较
    QCOMPARE(info.minVersion, QStringLiteral("0.1.0"));
}

// 非 JSON / 缺 version 都要被拒绝。
void TstUpdate::invalidManifestRejected()
{
    service::VersionInfo info;
    QString error;
    QVERIFY(!service::parseVersionManifest(QByteArray("not json"), &info, &error));
    QVERIFY(!error.isEmpty());

    QVERIFY(!service::parseVersionManifest(QByteArray(R"({"notes":"x"})"), &info, &error));
    QVERIFY(error.contains(QStringLiteral("version")));
}

// 内网目录中的 version.json 被检测到，并给出新版本信息。
void TstUpdate::localManifestDetectsUpdate()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeManifest(dir, QStringLiteral("0.2.0"));
    QVERIFY(!path.isEmpty());

    service::UpdateChecker checker(updateConfig(path));
    checker.setCurrentVersion(QStringLiteral("0.1.0"));
    QVERIFY(checker.enabled());

    QSignalSpy spy(&checker, &service::UpdateChecker::checked);
    checker.check();

    QCOMPARE(spy.count(), 1);
    const auto status =
        spy.first().at(0).value<service::UpdateChecker::Status>();
    QCOMPARE(status, service::UpdateChecker::Status::UpdateAvailable);
    const auto info = spy.first().at(1).value<service::VersionInfo>();
    QCOMPARE(info.version, QStringLiteral("0.2.0"));
    QCOMPARE(checker.status(), service::UpdateChecker::Status::UpdateAvailable);
}

// 版本相同：判定为已是最新，不提示升级。
void TstUpdate::upToDateWhenSameVersion()
{
    QTemporaryDir dir;
    const QString path = writeManifest(dir, QStringLiteral("0.1.0"));

    service::UpdateChecker checker(updateConfig(path));
    checker.setCurrentVersion(QStringLiteral("0.1.0"));

    QSignalSpy spy(&checker, &service::UpdateChecker::checked);
    checker.check();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).value<service::UpdateChecker::Status>(),
             service::UpdateChecker::Status::UpToDate);
}

// 清单不存在（离线/未部署）：静默跳过，不弹窗、不阻断开机。
void TstUpdate::missingManifestSkipsSilently()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("missing.json"));

    service::UpdateChecker checker(updateConfig(path));
    QSignalSpy spy(&checker, &service::UpdateChecker::checked);
    checker.check();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).value<service::UpdateChecker::Status>(),
             service::UpdateChecker::Status::Skipped);
}

// 未配置清单：直接跳过。
void TstUpdate::disabledCheckerSkips()
{
    core::StationConfig config; // update 段默认关闭
    service::UpdateChecker checker(config);
    QVERIFY(!checker.enabled());

    QSignalSpy spy(&checker, &service::UpdateChecker::checked);
    checker.check();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).value<service::UpdateChecker::Status>(),
             service::UpdateChecker::Status::Skipped);
}

// SHA-256：匹配放行、不匹配拒绝、无摘要时跳过校验。
void TstUpdate::sha256Verification()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("aegis-0.2.0.zip"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("upgrade-package");
    file.close();

    const QString actual = service::fileSha256(path);
    QCOMPARE(actual.size(), 64);

    QString error;
    QVERIFY2(service::UpdateChecker::verifyPackage(path, actual, &error),
             qPrintable(error));
    QVERIFY2(service::UpdateChecker::verifyPackage(path, actual.toUpper(), &error),
             qPrintable(error)); // 大小写不敏感

    QVERIFY(!service::UpdateChecker::verifyPackage(
        path, QStringLiteral("0000000000000000000000000000000000000000000000000000000000000000"),
        &error));
    QVERIFY(error.contains(QStringLiteral("SHA-256")));

    QVERIFY(service::UpdateChecker::verifyPackage(path, QString(), &error));

    QVERIFY(!service::UpdateChecker::verifyPackage(
        dir.filePath(QStringLiteral("missing.zip")), actual, &error));
}

QTEST_MAIN(TstUpdate)
#include "tst_update.moc"
