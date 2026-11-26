#include "test/plugin_runner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>

namespace aegis::test {

namespace {
constexpr int kDefaultTimeoutMs = 5000;
constexpr int kMaxTimeoutMs = 60000;

QString pluginDirectory(const QString& root, const QString& name)
{
    return QDir(root).filePath(name);
}

} // namespace

bool PluginRunner::isSafeName(const QString& name)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]+$"));
    return pattern.match(name).hasMatch();
}

QString PluginRunner::resolveRoot(const QString& pluginsRoot)
{
    QDir dir(pluginsRoot);
    if (dir.isRelative())
        dir = QDir(QCoreApplication::applicationDirPath()).filePath(pluginsRoot);
    return dir.absolutePath();
}

bool PluginRunner::readManifest(const QString& pluginsRoot, const QString& name,
                                QStringList* command, int* timeoutMs, QString* error)
{
    auto fail = [error](const QString& text) {
        if (error != nullptr)
            *error = text;
        return false;
    };

    if (!isSafeName(name))
        return fail(QStringLiteral("非法插件名：%1").arg(name));

    const QString manifestPath =
        QDir(pluginDirectory(resolveRoot(pluginsRoot), name)).filePath(QStringLiteral("manifest.json"));
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("缺少 manifest.json：%1").arg(manifestPath));

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail(QStringLiteral("manifest.json 解析失败：%1").arg(parseError.errorString()));
    if (!document.isObject())
        return fail(QStringLiteral("manifest.json 根节点必须是对象"));

    const QJsonObject object = document.object();
    const QJsonArray entry = object.value(QStringLiteral("entry")).toArray();
    if (entry.isEmpty())
        return fail(QStringLiteral("manifest.json 缺少 entry 数组"));

    QStringList parsed;
    for (const QJsonValue& value : entry) {
        const QString part = value.toString();
        if (part.isEmpty())
            return fail(QStringLiteral("manifest.json entry 只能是非空字符串数组"));
        parsed.append(part);
    }

    if (command != nullptr)
        *command = parsed;
    if (timeoutMs != nullptr) {
        const int ms = object.value(QStringLiteral("timeoutMs")).toInt(kDefaultTimeoutMs);
        *timeoutMs = std::clamp(ms, 100, kMaxTimeoutMs);
    }
    return true;
}

PluginResult PluginRunner::run(const QString& pluginsRoot, const QString& name,
                               const QJsonObject& input, int timeoutMs)
{
    PluginResult result;

    QStringList command;
    int manifestTimeoutMs = kDefaultTimeoutMs;
    QString error;
    if (!readManifest(pluginsRoot, name, &command, &manifestTimeoutMs, &error)) {
        result.error = error;
        return result;
    }

    int effectiveTimeoutMs = timeoutMs > 0 ? timeoutMs : manifestTimeoutMs;
    effectiveTimeoutMs = std::min(effectiveTimeoutMs, manifestTimeoutMs);
    effectiveTimeoutMs = std::clamp(effectiveTimeoutMs, 100, kMaxTimeoutMs);

    const QString directory = pluginDirectory(resolveRoot(pluginsRoot), name);

    QProcess process;
    process.setWorkingDirectory(directory);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(command.first(), command.mid(1),
                  QIODevice::ReadWrite | QIODevice::Text);
    if (!process.waitForStarted(effectiveTimeoutMs)) {
        result.error = QStringLiteral("插件启动失败：%1").arg(process.errorString());
        return result;
    }

    QByteArray payload = QJsonDocument(input).toJson(QJsonDocument::Compact);
    payload.append('\n');
    process.write(payload);
    process.closeWriteChannel();

    if (!process.waitForFinished(effectiveTimeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        result.error = QStringLiteral("插件执行超时（%1 ms）").arg(effectiveTimeoutMs);
        return result;
    }

    result.log = QString::fromUtf8(process.readAllStandardError()).trimmed();

    if (process.exitStatus() != QProcess::NormalExit) {
        result.error = QStringLiteral("插件异常终止");
        return result;
    }
    if (process.exitCode() != 0) {
        result.error = QStringLiteral("插件退出码 %1").arg(process.exitCode());
        if (!result.log.isEmpty())
            result.error += QStringLiteral("：%1").arg(result.log);
        return result;
    }

    const QByteArray stdoutData = process.readAllStandardOutput().trimmed();
    if (stdoutData.isEmpty()) {
        result.error = QStringLiteral("插件没有输出结果");
        return result;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(stdoutData, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = QStringLiteral("插件输出不是 JSON 对象：%1")
                           .arg(QString::fromUtf8(stdoutData.left(120)));
        return result;
    }

    const QJsonObject object = document.object();
    result.ok = true;
    result.pass = object.value(QStringLiteral("pass")).toBool(false);
    result.message = object.value(QStringLiteral("message")).toString();
    result.measures = object.value(QStringLiteral("measures")).toObject();
    return result;
}

} // namespace aegis::test
