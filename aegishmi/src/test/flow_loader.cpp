#include "test/flow_loader.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <cmath>

namespace aegis::test {

namespace {

// ---- 小工具：把 JSON 值安全地转成文本/整数/浮点 ----
// 支持"数字被写成字符串"的情况，因为变量替换后数字会变成字符串（如 "${voutNominalMv}"）。

QString valueToText(const QJsonValue& value)
{
    if (value.isString())
        return value.toString();
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) {
        const double d = value.toDouble();
        // 整数去掉小数部分，避免 5000 变成 "5000.0"
        if (std::abs(d) < 1e15 && std::floor(d) == d)
            return QString::number(static_cast<qint64>(d));
        return QString::number(d);
    }
    return {};
}

int toInt(const QJsonValue& value, int defaultValue)
{
    if (value.isDouble())
        return value.toInt(defaultValue);
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().trimmed().toInt(&ok);
        return ok ? parsed : defaultValue;
    }
    return defaultValue;
}

double toDouble(const QJsonValue& value, double defaultValue)
{
    if (value.isDouble())
        return value.toDouble(defaultValue);
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().trimmed().toDouble(&ok);
        return ok ? parsed : defaultValue;
    }
    return defaultValue;
}

StepAction parseAction(const QString& text)
{
    if (text == QLatin1String("command"))
        return StepAction::Command;
    if (text == QLatin1String("delay"))
        return StepAction::Delay;
    if (text == QLatin1String("measure"))
        return StepAction::Measure;
    if (text == QLatin1String("assert"))
        return StepAction::Assert;
    if (text == QLatin1String("script"))
        return StepAction::Script;
    if (text == QLatin1String("readVersion"))
        return StepAction::ReadVersion;
    if (text == QLatin1String("plugin"))
        return StepAction::Plugin;
    if (text == QLatin1String("subFlow"))
        return StepAction::SubFlow;
    if (text == QLatin1String("prompt"))
        return StepAction::Prompt;
    return StepAction::Unknown; // 交给校验器报错，这里不直接失败
}

Metric parseMetric(const QString& text)
{
    if (text == QLatin1String("avg"))
        return Metric::Avg;
    if (text == QLatin1String("min"))
        return Metric::Min;
    if (text == QLatin1String("max"))
        return Metric::Max;
    if (text == QLatin1String("rms"))
        return Metric::Rms;
    if (text == QLatin1String("peakToPeak"))
        return Metric::PeakToPeak;
    return Metric::Unknown;
}

// =============================================================================
// 变量替换
//
// 规则：字符串里出现 ${name} 就替换成 variables 里的值。
// 变量不存在时记录一条错误（带上字段路径），并删除占位符，避免污染后续解析。
// =============================================================================
void substituteText(QString& text, const QMap<QString, QString>& variables,
                    const QString& path, QVector<FlowIssue>& errors)
{
    int pos = 0;
    while (true) {
        const int start = text.indexOf(QStringLiteral("${"), pos);
        if (start < 0)
            break;

        const int end = text.indexOf(QLatin1Char('}'), start + 2);
        if (end < 0) {
            errors.append({path, QStringLiteral("变量占位符缺少右花括号: %1").arg(text)});
            break;
        }

        const QString name = text.mid(start + 2, end - start - 2).trimmed();
        if (!variables.contains(name)) {
            errors.append({path, QStringLiteral("未定义的变量: %1").arg(name)});
            text.remove(start, end - start + 1);
            pos = start; // 占位符已删除，从同一位置继续找下一个
        } else {
            const QString replacement = variables.value(name);
            text.replace(start, end - start + 1, replacement);
            pos = start + replacement.size();
        }
    }
}

// 递归遍历 JSON，对所有字符串值做变量替换。
void substituteValue(QJsonValue& value, const QMap<QString, QString>& variables,
                     const QString& path, QVector<FlowIssue>& errors)
{
    if (value.isString()) {
        QString text = value.toString();
        substituteText(text, variables, path, errors);
        value = QJsonValue(text);
    } else if (value.isObject()) {
        QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            QJsonValue child = it.value();
            substituteValue(child, variables, path + QLatin1Char('.') + it.key(), errors);
            it.value() = child;
        }
        value = object;
    } else if (value.isArray()) {
        QJsonArray array = value.toArray();
        for (int i = 0; i < array.size(); ++i) {
            QJsonValue child = array.at(i);
            substituteValue(child, variables,
                            path + QStringLiteral("[%1]").arg(i), errors);
            array.replace(i, child);
        }
        value = array;
    }
    // 其他类型（数字/布尔/null）不需要替换
}

// 解析 expect 段。
ExpectSpec parseExpect(const QJsonValue& value, const QString& path,
                       QVector<FlowIssue>& errors)
{
    ExpectSpec expect;
    expect.present = true;

    if (!value.isObject()) {
        errors.append({path, QStringLiteral("expect 必须是对象")});
        return expect;
    }

    const QJsonObject object = value.toObject();
    if (object.contains(QStringLiteral("ackStatus")))
        expect.ackStatus = toInt(object.value(QStringLiteral("ackStatus")), 0);
    if (object.contains(QStringLiteral("metric")))
        expect.metric = parseMetric(object.value(QStringLiteral("metric")).toString());
    if (object.contains(QStringLiteral("min"))) {
        expect.hasMin = true;
        expect.min = toDouble(object.value(QStringLiteral("min")), 0.0);
    }
    if (object.contains(QStringLiteral("max"))) {
        expect.hasMax = true;
        expect.max = toDouble(object.value(QStringLiteral("max")), 0.0);
    }
    if (object.contains(QStringLiteral("unit")))
        expect.unit = object.value(QStringLiteral("unit")).toString();
    if (object.contains(QStringLiteral("version")))
        expect.version = object.value(QStringLiteral("version")).toString().trimmed();
    if (object.contains(QStringLiteral("versionPattern")))
        expect.versionPattern =
            object.value(QStringLiteral("versionPattern")).toString().trimmed();

    return expect;
}

// 解析单个步骤：字段缺失时保持默认值，合法性由校验器判断。
TestStep parseStep(const QJsonObject& object, const QString& path,
                   QVector<FlowIssue>& errors)
{
    TestStep step;
    step.id = object.value(QStringLiteral("id")).toString();
    step.name = object.value(QStringLiteral("name")).toString();
    step.action = parseAction(object.value(QStringLiteral("action")).toString());
    step.params = object.value(QStringLiteral("params")).toObject();

    if (object.contains(QStringLiteral("timeoutMs")))
        step.timeoutMs = toInt(object.value(QStringLiteral("timeoutMs")), -1);
    if (object.contains(QStringLiteral("retries")))
        step.retries = toInt(object.value(QStringLiteral("retries")), 0);
    if (object.contains(QStringLiteral("onFail")))
        step.onFail = object.value(QStringLiteral("onFail")).toString();

    // 便捷字段：从 params 里预取，避免执行阶段反复查 JSON
    step.link = object.value(QStringLiteral("link")).toString();
    if (step.link.isEmpty())
        step.link = step.params.value(QStringLiteral("link")).toString();

    // 流程控制（P1）：条件跳过 + 跳转。
    step.condition = object.value(QStringLiteral("condition")).toString().trimmed();
    step.gotoStep = object.value(QStringLiteral("goto")).toString().trimmed();
    step.cmd = step.params.value(QStringLiteral("cmd")).toString();
    step.channel = step.params.contains(QStringLiteral("channel"))
                       ? toInt(step.params.value(QStringLiteral("channel")), -1)
                       : -1;
    step.windowMs = toInt(step.params.value(QStringLiteral("windowMs")), 0);
    step.delayMs = toInt(step.params.value(QStringLiteral("ms")), 0);
    step.valueMv = step.params.contains(QStringLiteral("valueMv"))
                       ? toInt(step.params.value(QStringLiteral("valueMv")), 0)
                       : toInt(step.params.value(QStringLiteral("value")), 0);
    step.script = step.params.value(QStringLiteral("script")).toString();
    step.plugin = step.params.value(QStringLiteral("name")).toString().trimmed();
    if (step.plugin.isEmpty())
        step.plugin = step.params.value(QStringLiteral("plugin")).toString().trimmed();

    // TP-12a：子流程文件；TP-14：操作员确认文本与等待时长。
    step.flowRef = step.params.value(QStringLiteral("file")).toString().trimmed();
    if (step.flowRef.isEmpty())
        step.flowRef = step.params.value(QStringLiteral("flow")).toString().trimmed();
    step.promptText = step.params.value(QStringLiteral("text")).toString();
    if (step.promptText.isEmpty())
        step.promptText = step.params.value(QStringLiteral("message")).toString();
    step.promptTimeoutMs = toInt(step.params.value(QStringLiteral("timeoutMs")), 0);

    if (object.contains(QStringLiteral("expect")))
        step.expect = parseExpect(object.value(QStringLiteral("expect")),
                                  path + QStringLiteral(".expect"), errors);

    return step;
}

} // namespace

FlowLoader::LoadResult FlowLoader::loadFromJson(const QByteArray& json)
{
    LoadResult result;

    // ---- 1) JSON 语法解析 ----
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.errors.append({QStringLiteral("$"),
                              QStringLiteral("JSON 解析失败：%1（偏移 %2）")
                                  .arg(parseError.errorString())
                                  .arg(parseError.offset)});
        return result;
    }
    if (!document.isObject()) {
        result.errors.append({QStringLiteral("$"), QStringLiteral("配置根节点必须是对象")});
        return result;
    }

    QJsonObject root = document.object();

    // ---- 1.5) TP-03：校验和比对（针对原始内容计算，必须早于变量替换） ----
    const QString storedChecksum =
        root.value(QStringLiteral("checksum")).toString().trimmed();
    if (!storedChecksum.isEmpty()) {
        QJsonObject withoutChecksum = root;
        withoutChecksum.remove(QStringLiteral("checksum"));
        const QByteArray canonical =
            QJsonDocument(withoutChecksum).toJson(QJsonDocument::Compact);
        const QString actual = QString::fromLatin1(
            QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
        result.flow.checksum = storedChecksum;
        if (actual.compare(storedChecksum, Qt::CaseInsensitive) != 0) {
            result.warnings.append({QStringLiteral("checksum"),
                                    QStringLiteral("方案内容与记录校验和不一致，可能已被修改")});
        }
    }

    // ---- 2) 读取变量表 ----
    const QJsonValue variablesValue = root.value(QStringLiteral("variables"));
    if (!variablesValue.isUndefined()) {
        if (!variablesValue.isObject()) {
            result.errors.append({QStringLiteral("variables"),
                                  QStringLiteral("variables 必须是对象")});
        } else {
            const QJsonObject variablesObject = variablesValue.toObject();
            for (auto it = variablesObject.begin(); it != variablesObject.end(); ++it) {
                const QJsonValue v = it.value();
                if (v.isString() || v.isDouble() || v.isBool())
                    result.flow.variables.insert(it.key(), valueToText(v));
                else
                    result.errors.append({QStringLiteral("variables.") + it.key(),
                                          QStringLiteral("变量值必须是字符串、数字或布尔")});
            }
        }
    }

    // ---- 3) 变量替换（对整棵 JSON 树做一遍） ----
    QJsonValue rootValue(root);
    substituteValue(rootValue, result.flow.variables, QStringLiteral("$"), result.errors);
    root = rootValue.toObject();

    // ---- 4) 组装顶层字段 ----
    TestFlow& flow = result.flow;
    flow.schemaVersion = toInt(root.value(QStringLiteral("schemaVersion")), 0);
    flow.id = root.value(QStringLiteral("id")).toString();
    flow.name = root.value(QStringLiteral("name")).toString();
    flow.description = root.value(QStringLiteral("description")).toString();
    flow.product = root.value(QStringLiteral("product")).toString();
    flow.version = root.value(QStringLiteral("version")).toString().trimmed();
    flow.defaultTimeoutMs = toInt(root.value(QStringLiteral("defaultTimeoutMs")), 1000);

    const QString onFailure = root.value(QStringLiteral("onFailure")).toString();
    flow.onFailure = onFailure.isEmpty() ? QStringLiteral("abort") : onFailure;

    // ---- 5) 组装步骤 ----
    const QJsonValue stepsValue = root.value(QStringLiteral("steps"));
    if (!stepsValue.isArray()) {
        result.errors.append({QStringLiteral("steps"), QStringLiteral("steps 必须是数组")});
        return result;
    }

    const QJsonArray stepsArray = stepsValue.toArray();
    for (int i = 0; i < stepsArray.size(); ++i) {
        const QString path = QStringLiteral("steps[%1]").arg(i);
        if (!stepsArray.at(i).isObject()) {
            result.errors.append({path, QStringLiteral("步骤必须是对象")});
            continue;
        }
        flow.steps.append(parseStep(stepsArray.at(i).toObject(), path, result.errors));
    }

    return result;
}

FlowLoader::LoadResult FlowLoader::loadFromFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        LoadResult result;
        result.errors.append({path, QStringLiteral("无法打开配置文件：%1").arg(file.errorString())});
        return result;
    }
    LoadResult result = loadFromJson(file.readAll());
    // 记录来源位置：校验/执行子流程时需要据此解析相对路径。
    const QFileInfo info(path);
    result.flow.sourcePath = info.absoluteFilePath();
    result.flow.baseDir = info.absolutePath();
    return result;
}

} // namespace aegis::test
