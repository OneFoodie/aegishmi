#include "service/update_checker.h"

#include "core/station_config.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QVector>

namespace aegis::service {

namespace {

// 网络清单请求超时：内网环境只需要很短的时间。
constexpr int kManifestTimeoutMs = 5000;

// 把 "v1.2.3-beta" 拆成 [1, 2, 3]；无法解析的段按 0 处理。
QVector<int> versionParts(const QString& version)
{
    QString text = version.trimmed();
    if (text.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        text.remove(0, 1);

    // 预发布/构建后缀不参与比较（"1.2.3-rc1" 与 "1.2.3" 视为同一版本）。
    int cut = text.indexOf(QLatin1Char('-'));
    if (cut < 0)
        cut = text.indexOf(QLatin1Char('+'));
    if (cut >= 0)
        text.truncate(cut);

    QVector<int> parts;
    const QStringList segments = text.split(QLatin1Char('.'));
    for (const QString& segment : segments) {
        int value = 0;
        int index = 0;
        while (index < segment.size() && segment.at(index).isDigit()) {
            value = value * 10 + (segment.at(index).unicode() - '0');
            ++index;
        }
        parts.append(value);
    }
    return parts;
}

// 把相对清单路径解析到 exe 目录，便于现场把 version.json 放在程序旁边。
QString resolveLocalPath(const QString& manifest)
{
    const QFileInfo info(manifest);
    if (info.isAbsolute())
        return manifest;
    return QDir(QCoreApplication::applicationDirPath()).filePath(manifest);
}

} // namespace

int compareVersions(const QString& a, const QString& b)
{
    const QVector<int> left = versionParts(a);
    const QVector<int> right = versionParts(b);
    const int count = qMax(left.size(), right.size());
    for (int i = 0; i < count; ++i) {
        const int l = i < left.size() ? left.at(i) : 0;
        const int r = i < right.size() ? right.at(i) : 0;
        if (l != r)
            return l < r ? -1 : 1;
    }
    return 0;
}

bool parseVersionManifest(const QByteArray& data, VersionInfo* info, QString* error)
{
    if (info == nullptr) {
        if (error != nullptr)
            *error = QStringLiteral("输出参数为空");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (document.isNull() || !document.isObject()) {
        if (error != nullptr)
            *error = QStringLiteral("不是有效的 JSON 对象（%1）").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = document.object();
    VersionInfo parsed;
    parsed.version = root.value(QStringLiteral("version")).toString().trimmed();
    if (parsed.version.isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("缺少 version 字段");
        return false;
    }
    parsed.notes = root.value(QStringLiteral("notes")).toString().trimmed();
    parsed.url = root.value(QStringLiteral("url")).toString().trimmed();
    parsed.sha256 =
        root.value(QStringLiteral("sha256")).toString().trimmed().toLower();
    parsed.minVersion = root.value(QStringLiteral("minVersion")).toString().trimmed();

    *info = parsed;
    return true;
}

QString fileSha256(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return QString();
    return QString::fromLatin1(hash.result().toHex());
}

UpdateChecker::UpdateChecker(const core::StationConfig& config, QObject* parent)
    : QObject(parent)
    , enabled_(config.updateEnabled && !config.updateManifest.isEmpty())
    , manifest_(config.updateManifest)
{
    const QString version = QCoreApplication::applicationVersion().trimmed();
    currentVersion_ = version.isEmpty() ? QStringLiteral("0.0.0") : version;
}

bool UpdateChecker::readManifestData(QByteArray* data, QString* error) const
{
    const QUrl url(manifest_);
    const QString scheme = url.scheme().toLower();
    const bool networkScheme = scheme == QLatin1String("http")
        || scheme == QLatin1String("https");

    // 本地路径 / file:// ：直接读文件（内网共享目录同样适用）。
    // 注意：Windows 盘符（C:/...）会被 QUrl 当成 scheme，需先按绝对路径识别。
    if (scheme == QLatin1String("file")
        || (!networkScheme && (scheme.isEmpty() || QFileInfo(manifest_).isAbsolute()))) {
        const QString path = scheme == QLatin1String("file") ? url.toLocalFile()
                                                             : resolveLocalPath(manifest_);
        QFile file(path);
        if (!file.exists()) {
            if (error != nullptr)
                *error = QStringLiteral("清单不存在：%1").arg(path);
            return false;
        }
        if (!file.open(QIODevice::ReadOnly)) {
            if (error != nullptr)
                *error = QStringLiteral("无法读取清单：%1").arg(path);
            return false;
        }
        *data = file.readAll();
        return true;
    }

    // 只允许 http/https，避免被配置成任意协议。
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        if (error != nullptr)
            *error = QStringLiteral("不支持的清单协议：%1").arg(scheme);
        return false;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = manager.get(request);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(kManifestTimeoutMs);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        if (error != nullptr)
            *error = QStringLiteral("检查更新超时（%1 ms）").arg(kManifestTimeoutMs);
        return false;
    }
    if (reply->error() != QNetworkReply::NoError) {
        if (error != nullptr)
            *error = reply->errorString();
        reply->deleteLater();
        return false;
    }

    *data = reply->readAll();
    reply->deleteLater();
    return true;
}

bool UpdateChecker::loadManifest(VersionInfo* info, QString* error)
{
    if (!enabled_) {
        if (error != nullptr)
            *error = QStringLiteral("未配置更新源");
        return false;
    }

    QByteArray data;
    QString reason;
    if (!readManifestData(&data, &reason)) {
        if (error != nullptr)
            *error = reason;
        return false;
    }
    return parseVersionManifest(data, info, error);
}

void UpdateChecker::check()
{
    if (!enabled_) {
        status_ = Status::Skipped;
        emit checked(Status::Skipped, VersionInfo(),
                     QStringLiteral("未配置更新源，跳过版本检测"));
        return;
    }

    QByteArray data;
    QString error;
    if (!readManifestData(&data, &error)) {
        // 离线 / 清单缺失按静默跳过处理，避免影响现场正常开机。
        status_ = Status::Skipped;
        emit logMessage(QStringLiteral("版本检测跳过：%1").arg(error));
        emit checked(Status::Skipped, VersionInfo(), error);
        return;
    }

    VersionInfo info;
    if (!parseVersionManifest(data, &info, &error)) {
        status_ = Status::Invalid;
        const QString message = QStringLiteral("版本清单格式错误：%1").arg(error);
        emit logMessage(message);
        emit checked(Status::Invalid, info, message);
        return;
    }

    if (compareVersions(info.version, currentVersion_) > 0) {
        status_ = Status::UpdateAvailable;
        const QString message = QStringLiteral("发现新版本 %1（当前 %2）")
                                    .arg(info.version, currentVersion_);
        emit logMessage(message);
        emit checked(Status::UpdateAvailable, info, message);
        return;
    }

    status_ = Status::UpToDate;
    const QString message =
        QStringLiteral("当前已是最新版本 %1").arg(currentVersion_);
    emit logMessage(message);
    emit checked(Status::UpToDate, info, message);
}

bool UpdateChecker::verifyPackage(const QString& path, const QString& expectedSha256,
                                  QString* error)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        if (error != nullptr)
            *error = QStringLiteral("升级包不存在：%1").arg(path);
        return false;
    }

    const QString expected = expectedSha256.trimmed();
    if (expected.isEmpty())
        return true; // 清单未提供摘要：只提示，不阻断。

    const QString actual = fileSha256(path);
    if (actual.isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("无法读取升级包：%1").arg(path);
        return false;
    }
    if (actual.compare(expected, Qt::CaseInsensitive) != 0) {
        if (error != nullptr)
            *error = QStringLiteral("SHA-256 不匹配（期望 %1，实际 %2）")
                         .arg(expected, actual);
        return false;
    }
    return true;
}

} // namespace aegis::service
