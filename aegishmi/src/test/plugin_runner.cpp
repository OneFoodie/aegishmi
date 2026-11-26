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
constexpr int kSupportedApiVersion = 1;

QString pluginDirectory(const QString& root, const QString& name)
{
    return QDir(root).filePath(name);
}

// 读取并解析 manifest.json 对象；名称安全性与 JSON 语法在这里统一校验。
bool loadManifestObject(const QString& pluginsRoot, const QString& name,
                        QJsonObject* object, QString* error)
{
    auto fail = [error](const QString& text) {
        if (error != nullptr)
            *error = text;
        return false;
    };

    if (!PluginRunner::isSafeName(name))
        return fail(QStringLiteral("非法插件名：%1").arg(name));

    const QString manifestPath = QDir(pluginDirectory(PluginRunner::resolveRoot(pluginsRoot), name))
                                     .filePath(QStringLiteral("manifest.json"));
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("缺少 manifest.json：%1").arg(manifestPath));

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail(QStringLiteral("manifest.json 解析失败：%1").arg(parseError.errorString()));
    if (!document.isObject())
        return fail(QStringLiteral("manifest.json 根节点必须是对象"));

    *object = document.object();
    return true;
}

// 校验清单核心字段：entry 非空 + API 版本受支持。
bool validateManifest(const QJsonObject& object, QString* error)
{
    const int apiVersion = object.value(QStringLiteral("apiVersion")).toInt(kSupportedApiVersion);
    if (apiVersion <= 0 || apiVersion > kSupportedApiVersion) {
        if (error != nullptr) {
            *error = QStringLiteral("插件 API 版本 %1 不受支持（当前支持 %2）")
                         .arg(apiVersion)
                         .arg(kSupportedApiVersion);
        }
        return false;
    }

    const QJsonArray entry = object.value(QStringLiteral("entry")).toArray();
    if (entry.isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("manifest.json 缺少 entry 数组");
        return false;
    }
    for (const QJsonValue& value : entry) {
        if (value.toString().isEmpty()) {
            if (error != nullptr)
                *error = QStringLiteral("manifest.json entry 只能是非空字符串数组");
            return false;
        }
    }
    return true;
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
    QJsonObject object;
    if (!loadManifestObject(pluginsRoot, name, &object, error))
        return false;
    if (!validateManifest(object, error))
        return false;

    QStringList parsed;
    const QJsonArray entry = object.value(QStringLiteral("entry")).toArray();
    for (const QJsonValue& value : entry)
        parsed.append(value.toString());

    if (command != nullptr)
        *command = parsed;
    if (timeoutMs != nullptr) {
        const int ms = object.value(QStringLiteral("timeoutMs")).toInt(kDefaultTimeoutMs);
        *timeoutMs = std::clamp(ms, 100, kMaxTimeoutMs);
    }
    return true;
}

bool PluginRunner::readInfo(const QString& pluginsRoot, const QString& name,
                            PluginInfo* info, QString* error)
{
    PluginInfo parsed;
    parsed.name = name;

    QJsonObject object;
    QString reason;
    if (!loadManifestObject(pluginsRoot, name, &object, &reason)) {
        parsed.error = reason;
        if (info != nullptr)
            *info = parsed;
        if (error != nullptr)
            *error = reason;
        return false;
    }

    parsed.apiVersion = object.value(QStringLiteral("apiVersion")).toInt(kSupportedApiVersion);
    parsed.version = object.value(QStringLiteral("version")).toString(QStringLiteral("1.0.0"));
    parsed.description = object.value(QStringLiteral("description")).toString();
    parsed.author = object.value(QStringLiteral("author")).toString();
    const int ms = object.value(QStringLiteral("timeoutMs")).toInt(kDefaultTimeoutMs);
    parsed.timeoutMs = std::clamp(ms, 100, kMaxTimeoutMs);

    const QJsonArray capabilities = object.value(QStringLiteral("capabilities")).toArray();
    for (const QJsonValue& value : capabilities) {
        const QString capability = value.toString().trimmed();
        if (!capability.isEmpty())
            parsed.capabilities.append(capability);
    }

    parsed.valid = validateManifest(object, &reason);
    parsed.error = parsed.valid ? QString() : reason;
    if (info != nullptr)
        *info = parsed;
    if (!parsed.valid && error != nullptr)
        *error = reason;
    return parsed.valid;
}

QVector<PluginInfo> PluginRunner::listPlugins(const QString& pluginsRoot)
{
    QVector<PluginInfo> result;
    const QDir root(resolveRoot(pluginsRoot));
    if (!root.exists())
        return result;

    const QFileInfoList entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                     QDir::Name);
    for (const QFileInfo& entry : entries) {
        if (!QFileInfo::exists(QDir(entry.absoluteFilePath())
                                   .filePath(QStringLiteral("manifest.json")))) {
            continue; // 非插件目录（脚本/文档等）直接跳过
        }
        PluginInfo info;
        readInfo(pluginsRoot, entry.fileName(), &info, nullptr);
        result.append(info);
    }
    return result;
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
