#include "core/station_config.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTime>

#include <algorithm>

namespace aegis::core {

namespace {

// "08:30" → 510；非法格式返回 -1。
int parseClockMinute(const QString& text)
{
    const QStringList parts = text.trimmed().split(QLatin1Char(':'));
    if (parts.size() != 2)
        return -1;
    bool okHour = false;
    bool okMinute = false;
    const int hour = parts.at(0).toInt(&okHour);
    const int minute = parts.at(1).toInt(&okMinute);
    if (!okHour || !okMinute || hour < 0 || hour > 23 || minute < 0 || minute > 59)
        return -1;
    return hour * 60 + minute;
}

} // namespace

StationConfig StationConfig::load(const QString& path, QString* error)
{
    StationConfig config;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr)
            *error = QStringLiteral("无法打开工位配置：%1").arg(file.errorString());
        return config;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr)
            *error = QStringLiteral("工位配置 JSON 解析失败：%1").arg(parseError.errorString());
        return config;
    }

    const QJsonObject root = document.object();
    const QString stationId = root.value(QStringLiteral("stationId")).toString().trimmed();
    if (!stationId.isEmpty())
        config.stationId = stationId;

    const QString lineId = root.value(QStringLiteral("lineId")).toString().trimmed();
    if (!lineId.isEmpty())
        config.lineId = lineId;

    config.fixtureId = root.value(QStringLiteral("fixtureId")).toString().trimmed();

    const QString snPattern = root.value(QStringLiteral("snPattern")).toString();
    if (!snPattern.isEmpty())
        config.snPattern = snPattern;

    // ---- 首件检验 ----
    const QJsonObject fai = root.value(QStringLiteral("fai")).toObject();
    if (!fai.isEmpty()) {
        config.faiEnabled = fai.value(QStringLiteral("enabled")).toBool(true);
        config.faiSampleCount = std::clamp(
            fai.value(QStringLiteral("sampleCount")).toInt(1), 1, 100);
    }

    // ---- 不良原因 ----
    const QJsonArray reasons = root.value(QStringLiteral("failReasons")).toArray();
    if (!reasons.isEmpty()) {
        QStringList list;
        for (const QJsonValue& value : reasons) {
            const QString reason = value.toString().trimmed();
            if (!reason.isEmpty())
                list.append(reason);
        }
        if (!list.isEmpty())
            config.failReasons = list;
    }

    // ---- 治具寿命/校准 ----
    const QJsonObject fixture = root.value(QStringLiteral("fixture")).toObject();
    if (!fixture.isEmpty()) {
        config.fixtureMaxRuns = std::clamp(
            fixture.value(QStringLiteral("maxRuns")).toInt(0), 0, 100000000);
        config.fixtureCalibrationDays = std::clamp(
            fixture.value(QStringLiteral("calibrationDays")).toInt(365), 1, 3650);
        config.fixtureCalibratedUntil =
            fixture.value(QStringLiteral("calibratedUntil")).toString().trimmed();
    }
    if (!config.fixtureCalibratedUntil.isEmpty()) {
        const QDate date = QDate::fromString(config.fixtureCalibratedUntil,
                                             QStringLiteral("yyyy-MM-dd"));
        if (date.isValid()) {
            config.fixtureCalibratedUntilMs =
                QDateTime(date, QTime(23, 59, 59)).toMSecsSinceEpoch();
        }
    }

    // ---- 电子 SOP ----
    const QJsonObject sop = root.value(QStringLiteral("sop")).toObject();
    if (!sop.isEmpty()) {
        config.sopEnabled = sop.value(QStringLiteral("enabled")).toBool(false);
        config.sopRequireRead = sop.value(QStringLiteral("requireRead")).toBool(true);
        const QJsonArray documents = sop.value(QStringLiteral("documents")).toArray();
        for (const QJsonValue& value : documents) {
            const QJsonObject doc = value.toObject();
            if (doc.isEmpty())
                continue;
            SopDocument item;
            item.product = doc.value(QStringLiteral("product")).toString().trimmed();
            item.title = doc.value(QStringLiteral("title")).toString().trimmed();
            item.version = doc.value(QStringLiteral("version")).toString().trimmed();
            item.imagePath = doc.value(QStringLiteral("image")).toString().trimmed();
            const QJsonArray steps = doc.value(QStringLiteral("steps")).toArray();
            for (const QJsonValue& step : steps) {
                const QString text = step.toString().trimmed();
                if (!text.isEmpty())
                    item.steps.append(text);
            }
            if (!item.title.isEmpty() || !item.steps.isEmpty())
                config.sopDocuments.append(item);
        }
    }

    // ---- 操作员资质 ----
    const QJsonObject qualification =
        root.value(QStringLiteral("qualification")).toObject();
    if (!qualification.isEmpty()) {
        config.qualificationEnabled =
            qualification.value(QStringLiteral("enabled")).toBool(false);
        const QJsonObject products =
            qualification.value(QStringLiteral("products")).toObject();
        for (auto it = products.begin(); it != products.end(); ++it) {
            QStringList users;
            for (const QJsonValue& value : it.value().toArray()) {
                const QString user = value.toString().trimmed();
                if (!user.isEmpty())
                    users.append(user);
            }
            config.qualifiedOperators.insert(it.key().trimmed(), users);
        }
    }

    // ---- 班次 ----
    for (const QJsonValue& value : root.value(QStringLiteral("shifts")).toArray()) {
        const QJsonObject shift = value.toObject();
        if (shift.isEmpty())
            continue;
        ShiftDefinition item;
        item.name = shift.value(QStringLiteral("name")).toString().trimmed();
        item.startMinute = parseClockMinute(
            shift.value(QStringLiteral("start")).toString());
        item.endMinute = parseClockMinute(
            shift.value(QStringLiteral("end")).toString());
        if (!item.name.isEmpty() && item.startMinute >= 0 && item.endMinute >= 0)
            config.shifts.append(item);
    }

    // ---- Andon 停线 ----
    const QJsonObject andon = root.value(QStringLiteral("andon")).toObject();
    if (!andon.isEmpty()) {
        config.andonEnabled = andon.value(QStringLiteral("enabled")).toBool(false);
        config.andonConsecutiveFails = std::clamp(
            andon.value(QStringLiteral("consecutiveFails")).toInt(3), 1, 100);
        config.andonFpyThreshold = std::clamp(
            andon.value(QStringLiteral("fpyThreshold")).toDouble(80.0), 0.0, 100.0);
        config.andonMinUnits = std::clamp(
            andon.value(QStringLiteral("minUnits")).toInt(5), 1, 100000);
    }

    // ---- SPC 判异联动停线 ----
    const QJsonObject spc = root.value(QStringLiteral("spc")).toObject();
    if (!spc.isEmpty()) {
        config.spcAndonEnabled = spc.value(QStringLiteral("andonOnCpk")).toBool(false);
        config.spcCpkThreshold = std::clamp(
            spc.value(QStringLiteral("cpkThreshold")).toDouble(1.0), 0.0, 10.0);
        config.spcMinSamples = std::clamp(
            spc.value(QStringLiteral("minSamples")).toInt(10), 2, 10000);
    }

    // ---- 合格证标签 ----
    const QJsonObject label = root.value(QStringLiteral("label")).toObject();
    if (!label.isEmpty()) {
        config.labelEnabled = label.value(QStringLiteral("enabled")).toBool(false);
        config.labelDirectory = label.value(QStringLiteral("directory")).toString().trimmed();
        config.labelWidthMm = std::clamp(
            label.value(QStringLiteral("widthMm")).toInt(60), 20, 200);
        config.labelHeightMm = std::clamp(
            label.value(QStringLiteral("heightMm")).toInt(40), 15, 200);
    }

    // ---- 审核报告 ----
    const QJsonObject report = root.value(QStringLiteral("report")).toObject();
    if (!report.isEmpty()) {
        const QString company = report.value(QStringLiteral("company")).toString().trimmed();
        const QString title = report.value(QStringLiteral("title")).toString().trimmed();
        if (!company.isEmpty())
            config.reportCompany = company;
        if (!title.isEmpty())
            config.reportTitle = title;
        config.reportLogoPath = report.value(QStringLiteral("logo")).toString().trimmed();
        config.reportFooter = report.value(QStringLiteral("footer"))
                                  .toString(config.reportFooter)
                                  .trimmed();
        config.reportAccentColor = report.value(QStringLiteral("accentColor"))
                                       .toString(config.reportAccentColor)
                                       .trimmed();
        config.reportShowSignature = report.value(QStringLiteral("signature"))
                                         .toBool(config.reportShowSignature);
        const QString inspector = report.value(QStringLiteral("inspectorLabel"))
                                      .toString(config.reportInspectorLabel)
                                      .trimmed();
        const QString reviewer = report.value(QStringLiteral("reviewerLabel"))
                                     .toString(config.reportReviewerLabel)
                                     .trimmed();
        if (!inspector.isEmpty())
            config.reportInspectorLabel = inspector;
        if (!reviewer.isEmpty())
            config.reportReviewerLabel = reviewer;
    }

    const QJsonObject mes = root.value(QStringLiteral("mes")).toObject();
    config.mesEnabled = mes.value(QStringLiteral("enabled")).toBool(false);
    config.mesUrl = mes.value(QStringLiteral("url")).toString().trimmed();
    config.mesTimeoutMs = std::clamp(mes.value(QStringLiteral("timeoutMs")).toInt(5000),
                                     500, 60000);
    config.mesMaxAttempts = std::clamp(mes.value(QStringLiteral("maxAttempts")).toInt(5),
                                       1, 20);
    config.mesRetryBaseMs = std::clamp(mes.value(QStringLiteral("retryBaseMs")).toInt(2000),
                                       200, 60000);
    config.mesBomCheckPath = mes.value(QStringLiteral("bomCheckPath")).toString().trimmed();
    config.mesStationCheckPath =
        mes.value(QStringLiteral("stationCheckPath")).toString().trimmed();
    config.mesFailOpen = mes.value(QStringLiteral("failOpen")).toBool(true);

    // ---- 用户插件（TP-11）----
    const QJsonObject plugins = root.value(QStringLiteral("plugins")).toObject();
    if (!plugins.isEmpty()) {
        const QString dir = plugins.value(QStringLiteral("dir")).toString().trimmed();
        if (!dir.isEmpty())
            config.pluginsDir = dir;
        config.pluginTimeoutMs = std::clamp(
            plugins.value(QStringLiteral("timeoutMs")).toInt(5000), 100, 60000);
        QStringList allowed;
        for (const QJsonValue& value : plugins.value(QStringLiteral("enabled")).toArray()) {
            const QString name = value.toString().trimmed();
            if (!name.isEmpty())
                allowed.append(name);
        }
        config.plugins = allowed;
    }

    // ---- 质检复核（QA-07+）----
    config.qualityApprovalOnly =
        root.value(QStringLiteral("qualityApprovalOnly")).toBool(false);

    // ---- 轻量版本检测（SY-05）----
    const QJsonObject update = root.value(QStringLiteral("update")).toObject();
    if (!update.isEmpty()) {
        config.updateEnabled = update.value(QStringLiteral("enabled")).toBool(false);
        config.updateManifest =
            update.value(QStringLiteral("manifest")).toString().trimmed();
        config.updateCheckOnStartup =
            update.value(QStringLiteral("checkOnStartup")).toBool(true);
    }

    // 开着 MES 但没给地址等于没开：避免每笔都进入失败队列。
    if (config.mesEnabled && config.mesUrl.isEmpty()) {
        config.mesEnabled = false;
        if (error != nullptr)
            *error = QStringLiteral("MES 已启用但未配置 url，已自动关闭上传");
    }

    return config;
}

StationConfig StationConfig::loadActive()
{
    QStringList candidates;

    const QString fromEnv = qEnvironmentVariable("AEGIS_STATION_CONFIG");
    if (!fromEnv.isEmpty())
        candidates.append(fromEnv);

    const QString appDir = QCoreApplication::applicationDirPath();
    candidates.append(appDir + QStringLiteral("/configs/station.json"));
    candidates.append(appDir + QStringLiteral("/../../configs/station.json"));

    for (const QString& path : candidates) {
        if (QFileInfo::exists(path))
            return load(path);
    }

    return StationConfig();
}

qint64 StationConfig::defaultFixtureDueMs(qint64 nowMs) const
{
    if (fixtureCalibratedUntilMs > 0)
        return fixtureCalibratedUntilMs;
    if (fixtureCalibrationDays > 0) {
        return nowMs
             + static_cast<qint64>(fixtureCalibrationDays) * 24 * 3600 * 1000;
    }
    return 0; // 不校验校准期
}

const SopDocument* StationConfig::sopForProduct(const QString& product) const
{
    if (!sopEnabled)
        return nullptr;

    const auto matches = [&product](const SopDocument& doc) {
        return doc.product.isEmpty() || doc.product == QLatin1String("*")
            || doc.product.compare(product, Qt::CaseInsensitive) == 0;
    };

    // 先找精确匹配，再退到通用条目。
    for (const SopDocument& doc : sopDocuments) {
        if (!doc.product.isEmpty() && doc.product != QLatin1String("*")
            && matches(doc)) {
            return &doc;
        }
    }
    for (const SopDocument& doc : sopDocuments) {
        if (doc.product.isEmpty() || doc.product == QLatin1String("*"))
            return &doc;
    }
    return nullptr;
}

bool StationConfig::isOperatorQualified(const QString& product,
                                        const QString& user) const
{
    if (!qualificationEnabled)
        return true;

    QStringList allowed;
    if (qualifiedOperators.contains(product))
        allowed = qualifiedOperators.value(product);
    else if (qualifiedOperators.contains(QStringLiteral("*")))
        allowed = qualifiedOperators.value(QStringLiteral("*"));
    else
        return true; // 该产品未配置资质要求

    if (allowed.isEmpty())
        return true;
    if (allowed.contains(QStringLiteral("*")))
        return true;
    for (const QString& name : allowed) {
        if (name.compare(user, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QString StationConfig::shiftAt(const QDateTime& time) const
{
    if (shifts.isEmpty())
        return QString();

    const int minute = time.time().hour() * 60 + time.time().minute();
    for (const ShiftDefinition& shift : shifts) {
        if (shift.startMinute <= shift.endMinute) {
            if (minute >= shift.startMinute && minute < shift.endMinute)
                return shift.name;
        } else {
            // 跨零点：20:00 ~ 08:00。
            if (minute >= shift.startMinute || minute < shift.endMinute)
                return shift.name;
        }
    }
    return QString();
}

} // namespace aegis::core
