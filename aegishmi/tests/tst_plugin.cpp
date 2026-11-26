// 用户插件调用（独立进程 + JSON stdio）的隔离性测试。

#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "test/plugin_runner.h"

using namespace aegis::test;

namespace {

// 在 root 下创建插件目录与 manifest（entry 指向 plugin_stub.exe）。
bool makePlugin(const QString& root, const QString& name, const QString& mode, int timeoutMs = 5000)
{
    const QString pluginDir = QDir(root).filePath(name);
    if (!QDir().mkpath(pluginDir))
        return false;

    const QString stub =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("plugin_stub.exe"));
    if (!QFile::exists(stub))
        return false;

    QJsonObject manifest;
    manifest.insert(QStringLiteral("entry"), QJsonArray{stub, mode});
    manifest.insert(QStringLiteral("timeoutMs"), timeoutMs);

    QFile file(QDir(pluginDir).filePath(QStringLiteral("manifest.json")));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(manifest).toJson());
    return true;
}

} // namespace

class TstPlugin : public QObject {
    Q_OBJECT

private slots:
    void passReturnsMeasures();
    void failReportsMessage();
    void timeoutIsKilled();
    void crashReportsExitCode();
    void invalidJsonRejected();
    void missingManifestRejected();
    void unsafeNameRejected();
};

void TstPlugin::passReturnsMeasures()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(makePlugin(dir.filePath(QStringLiteral("plugins")), QStringLiteral("demo"),
                       QStringLiteral("pass")));

    QJsonObject input;
    input.insert(QStringLiteral("sn"), QStringLiteral("SN-1"));
    const PluginResult result = PluginRunner::run(dir.filePath(QStringLiteral("plugins")),
                                                  QStringLiteral("demo"), input, 5000);
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(result.pass);
    QCOMPARE(result.message, QStringLiteral("ok"));
    QCOMPARE(result.measures.value(QStringLiteral("value")).toDouble(), 123.5);
    QVERIFY(result.log.contains(QStringLiteral("stub log")));
}

void TstPlugin::failReportsMessage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(makePlugin(dir.filePath(QStringLiteral("plugins")), QStringLiteral("demo"),
                       QStringLiteral("fail")));

    const PluginResult result = PluginRunner::run(dir.filePath(QStringLiteral("plugins")),
                                                  QStringLiteral("demo"), QJsonObject(), 5000);
    QVERIFY(result.ok);
    QVERIFY(!result.pass);
    QVERIFY(result.message.contains(QStringLiteral("电压")));
}

// 超时：进程被杀掉，主程序只得到一个失败结果。
void TstPlugin::timeoutIsKilled()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(makePlugin(dir.filePath(QStringLiteral("plugins")), QStringLiteral("demo"),
                       QStringLiteral("timeout"), 5000));

    const PluginResult result = PluginRunner::run(dir.filePath(QStringLiteral("plugins")),
                                                  QStringLiteral("demo"), QJsonObject(), 200);
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("超时")));
}

// 崩溃：非零退出码，附带 stderr。
void TstPlugin::crashReportsExitCode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(makePlugin(dir.filePath(QStringLiteral("plugins")), QStringLiteral("demo"),
                       QStringLiteral("crash")));

    const PluginResult result = PluginRunner::run(dir.filePath(QStringLiteral("plugins")),
                                                  QStringLiteral("demo"), QJsonObject(), 5000);
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("退出码")));
}

void TstPlugin::invalidJsonRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(makePlugin(dir.filePath(QStringLiteral("plugins")), QStringLiteral("demo"),
                       QStringLiteral("badjson")));

    const PluginResult result = PluginRunner::run(dir.filePath(QStringLiteral("plugins")),
                                                  QStringLiteral("demo"), QJsonObject(), 5000);
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("JSON")));
}

void TstPlugin::missingManifestRejected()
{
    QTemporaryDir dir;
    const PluginResult result = PluginRunner::run(dir.filePath(QStringLiteral("plugins")),
                                                  QStringLiteral("ghost"), QJsonObject(), 5000);
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("manifest")));
}

void TstPlugin::unsafeNameRejected()
{
    QVERIFY(!PluginRunner::isSafeName(QStringLiteral("../evil")));
    QVERIFY(!PluginRunner::isSafeName(QStringLiteral("a/b")));
    QVERIFY(PluginRunner::isSafeName(QStringLiteral("demo_1-A")));

    const PluginResult result = PluginRunner::run(QStringLiteral("plugins"),
                                                  QStringLiteral("../evil"),
                                                  QJsonObject(), 5000);
    QVERIFY(!result.ok);
}

QTEST_GUILESS_MAIN(TstPlugin)
#include "tst_plugin.moc"
