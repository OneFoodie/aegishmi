#include "test/flow_writer.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

namespace aegis::test {

namespace {

// 校验和覆盖"除 checksum 之外的全部内容"，用紧凑 JSON 作为规范化字节序列。
QString checksumOf(const QJsonObject& object)
{
    const QByteArray canonical = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

} // namespace

QString FlowWriter::actionText(StepAction action)
{
    switch (action) {
    case StepAction::Command:
        return QStringLiteral("command");
    case StepAction::Delay:
        return QStringLiteral("delay");
    case StepAction::Measure:
        return QStringLiteral("measure");
    case StepAction::Assert:
        return QStringLiteral("assert");
    case StepAction::Script:
        return QStringLiteral("script");
    case StepAction::ReadVersion:
        return QStringLiteral("readVersion");
    case StepAction::Plugin:
        return QStringLiteral("plugin");
    case StepAction::SubFlow:
        return QStringLiteral("subFlow");
    case StepAction::Prompt:
        return QStringLiteral("prompt");
    case StepAction::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

QString FlowWriter::metricText(Metric metric)
{
    switch (metric) {
    case Metric::Avg:
        return QStringLiteral("avg");
    case Metric::Min:
        return QStringLiteral("min");
    case Metric::Max:
        return QStringLiteral("max");
    case Metric::Rms:
        return QStringLiteral("rms");
    case Metric::PeakToPeak:
        return QStringLiteral("peakToPeak");
    case Metric::Unknown:
        break;
    }
    return {};
}

QJsonObject FlowWriter::stepToJson(const TestStep& step)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), step.id);
    object.insert(QStringLiteral("name"), step.name);
    object.insert(QStringLiteral("action"), actionText(step.action));

    if (step.timeoutMs >= 0)
        object.insert(QStringLiteral("timeoutMs"), step.timeoutMs);
    if (step.retries > 0)
        object.insert(QStringLiteral("retries"), step.retries);
    if (!step.onFail.isEmpty())
        object.insert(QStringLiteral("onFail"), step.onFail);
    if (!step.link.isEmpty())
        object.insert(QStringLiteral("link"), step.link);
    if (!step.condition.isEmpty())
        object.insert(QStringLiteral("condition"), step.condition);
    if (!step.gotoStep.isEmpty())
        object.insert(QStringLiteral("goto"), step.gotoStep);

    // params 以原始对象为底，再用便捷字段覆盖已知键，避免编辑器改值后被旧值盖回去。
    QJsonObject params = step.params;
    switch (step.action) {
    case StepAction::Command:
        if (!step.cmd.isEmpty())
            params.insert(QStringLiteral("cmd"), step.cmd);
        if (step.channel >= 0)
            params.insert(QStringLiteral("channel"), step.channel);
        params.insert(QStringLiteral("valueMv"), step.valueMv);
        break;
    case StepAction::Measure:
        if (step.channel >= 0)
            params.insert(QStringLiteral("channel"), step.channel);
        if (step.windowMs > 0)
            params.insert(QStringLiteral("windowMs"), step.windowMs);
        break;
    case StepAction::Delay:
        params.insert(QStringLiteral("ms"), step.delayMs);
        break;
    case StepAction::Script:
        params.insert(QStringLiteral("script"), step.script);
        break;
    case StepAction::ReadVersion:
        if (!step.cmd.isEmpty())
            params.insert(QStringLiteral("cmd"), step.cmd);
        break;
    case StepAction::Plugin:
        if (!step.plugin.isEmpty())
            params.insert(QStringLiteral("name"), step.plugin);
        break;
    case StepAction::SubFlow:
        if (!step.flowRef.isEmpty())
            params.insert(QStringLiteral("file"), step.flowRef);
        break;
    case StepAction::Prompt:
        params.insert(QStringLiteral("text"), step.promptText);
        if (step.promptTimeoutMs > 0)
            params.insert(QStringLiteral("timeoutMs"), step.promptTimeoutMs);
        else
            params.remove(QStringLiteral("timeoutMs"));
        break;
    case StepAction::Assert:
    case StepAction::Unknown:
        break;
    }
    if (!params.isEmpty())
        object.insert(QStringLiteral("params"), params);

    if (step.expect.present) {
        QJsonObject expect;
        if (step.action == StepAction::Command) {
            expect.insert(QStringLiteral("ackStatus"), step.expect.ackStatus);
        } else if (step.action == StepAction::ReadVersion) {
            if (!step.expect.version.isEmpty())
                expect.insert(QStringLiteral("version"), step.expect.version);
            if (!step.expect.versionPattern.isEmpty())
                expect.insert(QStringLiteral("versionPattern"), step.expect.versionPattern);
        } else {
            const QString metric = metricText(step.expect.metric);
            if (!metric.isEmpty())
                expect.insert(QStringLiteral("metric"), metric);
            if (step.expect.hasMin)
                expect.insert(QStringLiteral("min"), step.expect.min);
            if (step.expect.hasMax)
                expect.insert(QStringLiteral("max"), step.expect.max);
        }
        if (!step.expect.unit.isEmpty())
            expect.insert(QStringLiteral("unit"), step.expect.unit);
        object.insert(QStringLiteral("expect"), expect);
    }

    return object;
}

QJsonObject FlowWriter::toJson(const TestFlow& flow)
{
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), flow.schemaVersion > 0 ? flow.schemaVersion : 1);
    root.insert(QStringLiteral("id"), flow.id);
    root.insert(QStringLiteral("name"), flow.name);
    if (!flow.description.isEmpty())
        root.insert(QStringLiteral("description"), flow.description);
    root.insert(QStringLiteral("product"), flow.product);
    if (!flow.version.isEmpty())
        root.insert(QStringLiteral("version"), flow.version);
    root.insert(QStringLiteral("defaultTimeoutMs"), flow.defaultTimeoutMs);
    root.insert(QStringLiteral("onFailure"),
                flow.onFailure.isEmpty() ? QStringLiteral("abort") : flow.onFailure);

    if (!flow.variables.isEmpty()) {
        QJsonObject variables;
        for (auto it = flow.variables.begin(); it != flow.variables.end(); ++it)
            variables.insert(it.key(), it.value());
        root.insert(QStringLiteral("variables"), variables);
    }

    QJsonArray steps;
    for (const TestStep& step : flow.steps)
        steps.append(stepToJson(step));
    root.insert(QStringLiteral("steps"), steps);

    // TP-03：写入前重算校验和（忽略旧的 checksum 字段）。
    root.remove(QStringLiteral("checksum"));
    root.insert(QStringLiteral("checksum"), checksumOf(root));

    return root;
}

QByteArray FlowWriter::toJsonBytes(const TestFlow& flow)
{
    return QJsonDocument(toJson(flow)).toJson(QJsonDocument::Indented);
}

bool FlowWriter::saveToFile(const TestFlow& flow, const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("无法写入文件：%1").arg(file.errorString());
        return false;
    }

    const QByteArray bytes = toJsonBytes(flow);
    if (file.write(bytes) != bytes.size()) {
        if (error)
            *error = QStringLiteral("写入不完整：%1").arg(file.errorString());
        return false;
    }
    return true;
}

} // namespace aegis::test
