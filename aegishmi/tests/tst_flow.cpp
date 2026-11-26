// Qt Test 框架
#include <QtTest>

#include "test/flow_catalog.h"
#include "test/flow_loader.h"
#include "test/flow_validator.h"
#include "test/flow_writer.h"

#include <QFile>
#include <QTemporaryDir>

using namespace aegis::test;

namespace {

// 一份合法配置，作为"正向用例"的基准。
const char* kValidFlow = R"JSON(
{
  "schemaVersion": 1,
  "id": "demo",
  "name": "Demo flow",
  "product": "PM-5V",
  "defaultTimeoutMs": 1000,
  "onFailure": "abort",
  "variables": { "vout": 5000 },
  "steps": [
    {
      "id": "s1", "name": "set vout", "action": "command",
      "params": { "cmd": "SET_VOUT", "valueMv": "${vout}" },
      "expect": { "ackStatus": 0 }
    },
    {
      "id": "s2", "name": "wait stable", "action": "delay",
      "params": { "ms": 300 }
    },
    {
      "id": "s3", "name": "measure ${vout}", "action": "measure",
      "params": { "channel": 0, "windowMs": 200 },
      "expect": { "metric": "avg", "min": 4750, "max": 5250, "unit": "mV" }
    }
  ]
}
)JSON";

// 加载 + 校验：加载失败直接返回加载错误，否则返回校验结果。
QVector<FlowIssue> check(const QByteArray& json)
{
    const auto loaded = FlowLoader::loadFromJson(json);
    if (!loaded.ok())
        return loaded.errors;
    return FlowValidator::validate(loaded.flow);
}

bool hasPath(const QVector<FlowIssue>& issues, const QString& path)
{
    for (const auto& issue : issues) {
        if (issue.fieldPath == path)
            return true;
    }
    return false;
}

// 写入临时 JSON 文件，供子流程用例构造目录结构。
bool writeFile(const QString& path, const QByteArray& content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(content) == content.size();
}

} // namespace

// =============================================================================
// TstFlow —— M3.1 流程配置加载与校验
// =============================================================================
class TstFlow : public QObject {
    Q_OBJECT

private slots:
    void validFlowPassesValidation();
    void variableSubstitutionApplies();
    void malformedJsonIsReported();
    void rootMustBeObject();
    void duplicateStepIdIsRejected();
    void unknownActionIsRejected();
    void measureChannelOutOfRange();
    void minMustBeLessThanMax();
    void undefinedVariableIsReported();
    void timeoutOutOfRange();
    void emptyStepsIsRejected();
    void flowWithoutMeasureIsRejected();
    void delayRequiresPositiveMs();
    void measureRequiresExpect();
    void scriptStepIsValid();
    void scriptStepRequiresBody();
    void shippedExampleConfigIsValid();
    void flowCatalogFindsShippedExamples();
    void writerRoundtripPreservesFlow();
    void stepLinkIsParsedAndWritten();
    void versionAndChecksumAreWrittenAndVerified();
    void flowControlFieldsRoundTrip();
    void gotoMissingTargetIsRejected();
    void readVersionStepIsValid();
    void readVersionRequiresExpect();
    void readVersionRejectsAmbiguousExpect();
    void subFlowIsLoadedValidatedAndWritten();
    void subFlowMissingFileIsRejected();
    void subFlowRecursionIsRejected();
    void promptStepRoundTrip();
    void promptStepRequiresText();
};

// 合法配置：加载无错误，校验无问题，字段解析正确。
void TstFlow::validFlowPassesValidation()
{
    const auto loaded = FlowLoader::loadFromJson(QByteArray(kValidFlow));
    QVERIFY2(loaded.ok(), "valid flow should load without errors");

    QCOMPARE(loaded.flow.schemaVersion, 1);
    QCOMPARE(loaded.flow.id, QStringLiteral("demo"));
    QCOMPARE(loaded.flow.product, QStringLiteral("PM-5V"));
    QCOMPARE(loaded.flow.steps.size(), 3);

    QVERIFY(FlowValidator::validate(loaded.flow).isEmpty());
}

// 变量替换：数字占位符要能变成整数，文本中的占位符也要替换。
void TstFlow::variableSubstitutionApplies()
{
    const auto loaded = FlowLoader::loadFromJson(QByteArray(kValidFlow));
    QVERIFY(loaded.ok());

    // params.valueMv 原来是 "${vout}"，替换后应能按整数读取
    QCOMPARE(loaded.flow.steps.at(0).valueMv, 5000);
    // 步骤名里的占位符也应替换
    QCOMPARE(loaded.flow.steps.at(2).name, QStringLiteral("measure 5000"));
}

// JSON 语法错误：应给出解析错误，而不是崩溃或静默通过。
void TstFlow::malformedJsonIsReported()
{
    const auto loaded = FlowLoader::loadFromJson(QByteArray("{ not-json "));
    QVERIFY(!loaded.ok());
    QCOMPARE(loaded.errors.first().fieldPath, QStringLiteral("$"));
}

// 根节点必须是对象。
void TstFlow::rootMustBeObject()
{
    const auto loaded = FlowLoader::loadFromJson(QByteArray("[]"));
    QVERIFY(!loaded.ok());
    QCOMPARE(loaded.errors.first().fieldPath, QStringLiteral("$"));
}

// 步骤 id 重复：定位到第二个步骤。
void TstFlow::duplicateStepIdIsRejected()
{
    const auto issues = check(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "a", "name": "a", "action": "delay", "params": {"ms": 10}},
        {"id": "a", "name": "b", "action": "delay", "params": {"ms": 10}},
        {"id": "m", "name": "m", "action": "measure",
         "params": {"channel": 0, "windowMs": 100},
         "expect": {"metric": "avg", "min": 0, "max": 1}}
      ]
    })JSON");
    QVERIFY(hasPath(issues, QStringLiteral("steps[1].id")));
}

// 未知动作类型：必须在加载后校验出来。
void TstFlow::unknownActionIsRejected()
{
    const auto issues = check(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "a", "name": "a", "action": "magic", "params": {}},
        {"id": "m", "name": "m", "action": "measure",
         "params": {"channel": 0, "windowMs": 100},
         "expect": {"metric": "avg", "min": 0, "max": 1}}
      ]
    })JSON");
    QVERIFY(hasPath(issues, QStringLiteral("steps[0].action")));
}

// 通道号越界（支持的最大通道数是 64）。
void TstFlow::measureChannelOutOfRange()
{
    const auto issues = check(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "m", "name": "m", "action": "measure",
         "params": {"channel": 64, "windowMs": 100},
         "expect": {"metric": "avg", "min": 0, "max": 1}}
      ]
    })JSON");
    QVERIFY(hasPath(issues, QStringLiteral("steps[0].params.channel")));
}

// min >= max 属于配置错误。
void TstFlow::minMustBeLessThanMax()
{
    const auto issues = check(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "m", "name": "m", "action": "measure",
         "params": {"channel": 0, "windowMs": 100},
         "expect": {"metric": "avg", "min": 10, "max": 5}}
      ]
    })JSON");
    QVERIFY(hasPath(issues, QStringLiteral("steps[0].expect")));
}

// 引用了不存在的变量：加载阶段报错，路径要能定位到具体字段。
void TstFlow::undefinedVariableIsReported()
{
    const auto loaded = FlowLoader::loadFromJson(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "variables": {},
      "steps": [
        {"id": "a", "name": "${missing}", "action": "delay", "params": {"ms": 10}},
        {"id": "m", "name": "m", "action": "measure",
         "params": {"channel": 0, "windowMs": 100},
         "expect": {"metric": "avg", "min": 0, "max": 1}}
      ]
    })JSON");
    QVERIFY(!loaded.ok());
    QVERIFY(hasPath(loaded.errors, QStringLiteral("$.steps[0].name")));
}

// 超时值越界。
void TstFlow::timeoutOutOfRange()
{
    const auto issues = check(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "m", "name": "m", "action": "measure", "timeoutMs": 5,
         "params": {"channel": 0, "windowMs": 100},
         "expect": {"metric": "avg", "min": 0, "max": 1}}
      ]
    })JSON");
    QVERIFY(hasPath(issues, QStringLiteral("steps[0].timeoutMs")));
}

// 空步骤列表。
void TstFlow::emptyStepsIsRejected()
{
    const auto issues = check(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f", "steps": []
    })JSON");
    QVERIFY(hasPath(issues, QStringLiteral("steps")));
}

// 只有 command/delay、没有任何 measure：无法形成判定，应拒绝。
void TstFlow::flowWithoutMeasureIsRejected()
{
    const auto issues = check(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "a", "name": "a", "action": "delay", "params": {"ms": 10}}
      ]
    })JSON");
    QVERIFY(hasPath(issues, QStringLiteral("steps")));
}

// delay 的等待时长必须为正。
void TstFlow::delayRequiresPositiveMs()
{
    const auto issues = check(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "a", "name": "a", "action": "delay", "params": {"ms": 0}},
        {"id": "m", "name": "m", "action": "measure",
         "params": {"channel": 0, "windowMs": 100},
         "expect": {"metric": "avg", "min": 0, "max": 1}}
      ]
    })JSON");
    QVERIFY(hasPath(issues, QStringLiteral("steps[0].params.ms")));
}

// measure 没有 expect：无法判定，必须报错。
void TstFlow::measureRequiresExpect()
{
    const auto issues = check(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "m", "name": "m", "action": "measure",
         "params": {"channel": 0, "windowMs": 100}}
      ]
    })JSON");
    QVERIFY(hasPath(issues, QStringLiteral("steps[0].expect")));
}

// script 步骤可以独立承担判定：即使没有 measure 也应通过校验。
void TstFlow::scriptStepIsValid()
{
    const auto loaded = FlowLoader::loadFromJson(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "js", "name": "script check", "action": "script",
         "params": {"script": "if (measureAvg(0, 200) < 0) throw new Error('bad');"}}
      ]
    })JSON");
    QVERIFY2(loaded.ok(), "script flow should load");

    const auto issues = FlowValidator::validate(loaded.flow);
    QVERIFY2(issues.isEmpty(),
             qPrintable(issues.isEmpty() ? QStringLiteral("ok")
                                         : issues.first().fieldPath + QStringLiteral(": ")
                                               + issues.first().message));
    QCOMPARE(loaded.flow.steps.at(0).action, StepAction::Script);
    QVERIFY(!loaded.flow.steps.at(0).script.isEmpty());
}

// script 内容为空：必须报错定位到 params.script。
void TstFlow::scriptStepRequiresBody()
{
    const auto issues = check(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "js", "name": "script check", "action": "script", "params": {}}
      ]
    })JSON");
    QVERIFY(hasPath(issues, QStringLiteral("steps[0].params.script")));
}

// 仓库里随项目提供的示例配置必须始终合法，避免文档与实现脱节。
void TstFlow::shippedExampleConfigIsValid()
{
    const QString path =
        QStringLiteral(AEGIS_SOURCE_DIR "/configs/flows/power_module_basic.json");
    const auto loaded = FlowLoader::loadFromFile(path);
    QVERIFY2(loaded.ok(),
             qPrintable(loaded.errors.isEmpty()
                            ? QStringLiteral("load failed")
                            : loaded.errors.first().fieldPath + QStringLiteral(": ")
                                  + loaded.errors.first().message));
    QCOMPARE(loaded.flow.id, QStringLiteral("power_module_basic"));
    QVERIFY(loaded.flow.steps.size() >= 3);

    const auto issues = FlowValidator::validate(loaded.flow);
    QVERIFY2(issues.isEmpty(),
             qPrintable(issues.isEmpty()
                            ? QStringLiteral("ok")
                            : issues.first().fieldPath + QStringLiteral(": ")
                                  + issues.first().message));
}

// 方案库：扫描仓库自带示例，非法 JSON 标记为无效但不影响其它条目。
void TstFlow::flowCatalogFindsShippedExamples()
{
    const QString dir = QStringLiteral(AEGIS_SOURCE_DIR "/configs/flows");
    const QVector<FlowEntry> entries = FlowCatalog::scan(dir);
    QVERIFY(!entries.isEmpty());

    bool found = false;
    for (const FlowEntry& entry : entries) {
        QVERIFY(entry.valid);
        if (entry.id == QLatin1String("power_module_basic")) {
            found = true;
            QCOMPARE(entry.name, QStringLiteral("电源模块出厂测试"));
            QCOMPARE(entry.product, QStringLiteral("PM-5V-10A"));
            QCOMPARE(entry.steps, 6);
        }
    }
    QVERIFY(found);

    // 不存在的目录返回空列表，不抛异常。
    QVERIFY(FlowCatalog::scan(dir + QStringLiteral("/not-exists")).isEmpty());

    // 非法 JSON：describe 标记无效并给出原因。
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString badPath = temp.filePath(QStringLiteral("bad.json"));
    QFile bad(badPath);
    QVERIFY(bad.open(QIODevice::WriteOnly));
    bad.write("{ not json");
    bad.close();

    const FlowEntry invalid = FlowCatalog::describe(badPath);
    QVERIFY(!invalid.valid);
    QVERIFY(!invalid.error.isEmpty());
}

// 编辑器保存 → 重新加载：图形化编辑器依赖 FlowWriter，必须保证"写出去还能读回来"。
void TstFlow::writerRoundtripPreservesFlow()
{
    const QString sourcePath =
        QStringLiteral(AEGIS_SOURCE_DIR "/configs/flows/power_module_basic.json");
    const auto loaded = FlowLoader::loadFromFile(sourcePath);
    QVERIFY2(loaded.ok(), "shipped example should load");

    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString outPath = temp.filePath(QStringLiteral("roundtrip.json"));

    QString error;
    QVERIFY2(FlowWriter::saveToFile(loaded.flow, outPath, &error), qPrintable(error));

    const auto reloaded = FlowLoader::loadFromFile(outPath);
    QVERIFY2(reloaded.ok(),
             qPrintable(reloaded.errors.isEmpty()
                            ? QStringLiteral("reload failed")
                            : reloaded.errors.first().fieldPath + QStringLiteral(": ")
                                  + reloaded.errors.first().message));

    const TestFlow& before = loaded.flow;
    const TestFlow& after = reloaded.flow;
    QCOMPARE(after.schemaVersion, before.schemaVersion);
    QCOMPARE(after.id, before.id);
    QCOMPARE(after.name, before.name);
    QCOMPARE(after.product, before.product);
    QCOMPARE(after.defaultTimeoutMs, before.defaultTimeoutMs);
    QCOMPARE(after.onFailure, before.onFailure);
    QCOMPARE(after.variables, before.variables);
    QCOMPARE(after.steps.size(), before.steps.size());

    for (int i = 0; i < before.steps.size(); ++i) {
        const TestStep& expected = before.steps.at(i);
        const TestStep& actual = after.steps.at(i);
        QCOMPARE(actual.id, expected.id);
        QCOMPARE(actual.name, expected.name);
        QCOMPARE(actual.action, expected.action);
        QCOMPARE(actual.timeoutMs, expected.timeoutMs);
        QCOMPARE(actual.retries, expected.retries);
        QCOMPARE(actual.onFail, expected.onFail);
        QCOMPARE(actual.cmd, expected.cmd);
        QCOMPARE(actual.link, expected.link);
        QCOMPARE(actual.channel, expected.channel);
        QCOMPARE(actual.valueMv, expected.valueMv);
        QCOMPARE(actual.windowMs, expected.windowMs);
        QCOMPARE(actual.delayMs, expected.delayMs);
        QCOMPARE(actual.script, expected.script);
        QCOMPARE(actual.expect.present, expected.expect.present);
        QCOMPARE(actual.expect.ackStatus, expected.expect.ackStatus);
        QCOMPARE(actual.expect.metric, expected.expect.metric);
        QCOMPARE(actual.expect.hasMin, expected.expect.hasMin);
        QCOMPARE(actual.expect.hasMax, expected.expect.hasMax);
        QCOMPARE(actual.expect.min, expected.expect.min);
        QCOMPARE(actual.expect.max, expected.expect.max);
        QCOMPARE(actual.expect.unit, expected.expect.unit);
        QCOMPARE(actual.expect.version, expected.expect.version);
        QCOMPARE(actual.expect.versionPattern, expected.expect.versionPattern);
    }

    // 写出的内容仍然是合法配置。
    QVERIFY(FlowValidator::validate(after).isEmpty());
}

// EQ-06 多链路：步骤级 link 支持顶层字段与 params.link 两种写法，保存后仍能读回。
void TstFlow::stepLinkIsParsedAndWritten()
{
    const auto loaded = FlowLoader::loadFromJson(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "c", "name": "aux command", "action": "command", "link": "aux",
         "params": {"cmd": "SET_VOUT", "channel": 0, "valueMv": 1000}},
        {"id": "m", "name": "aux measure", "action": "measure",
         "params": {"link": "aux2", "channel": 0, "windowMs": 100},
         "expect": {"metric": "avg", "min": 0, "max": 1}}
      ]
    })JSON");
    QVERIFY2(loaded.ok(), "flow with link should load");
    QVERIFY(FlowValidator::validate(loaded.flow).isEmpty());
    QCOMPARE(loaded.flow.steps.at(0).link, QStringLiteral("aux"));
    QCOMPARE(loaded.flow.steps.at(1).link, QStringLiteral("aux2"));

    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString outPath = temp.filePath(QStringLiteral("link.json"));
    QString error;
    QVERIFY2(FlowWriter::saveToFile(loaded.flow, outPath, &error), qPrintable(error));

    const auto reloaded = FlowLoader::loadFromFile(outPath);
    QVERIFY2(reloaded.ok(),
             qPrintable(reloaded.errors.isEmpty() ? QStringLiteral("reload failed")
                                                 : reloaded.errors.first().message));
    QCOMPARE(reloaded.flow.steps.at(0).link, QStringLiteral("aux"));
    QCOMPARE(reloaded.flow.steps.at(1).link, QStringLiteral("aux2"));
}

// TP-03：保存时写入版本与校验和；加载时校验一致；被手工修改后给出告警。
void TstFlow::versionAndChecksumAreWrittenAndVerified()
{
    const auto loaded = FlowLoader::loadFromJson(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f", "product": "PM",
      "version": "1.2.0",
      "steps": [
        {"id": "m", "name": "measure", "action": "measure",
         "params": {"channel": 0, "windowMs": 100},
         "expect": {"metric": "avg", "min": 0, "max": 1}}
      ]
    })JSON");
    QVERIFY2(loaded.ok(), "flow with version should load");
    QCOMPARE(loaded.flow.version, QStringLiteral("1.2.0"));

    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString outPath = temp.filePath(QStringLiteral("versioned.json"));
    QString error;
    QVERIFY2(FlowWriter::saveToFile(loaded.flow, outPath, &error), qPrintable(error));

    const auto reloaded = FlowLoader::loadFromFile(outPath);
    QVERIFY2(reloaded.ok(), "saved flow should reload");
    QCOMPARE(reloaded.flow.version, QStringLiteral("1.2.0"));
    QVERIFY(!reloaded.flow.checksum.isEmpty());
    QVERIFY2(reloaded.warnings.isEmpty(), "checksum should match after save");

    // 手工改动内容但不更新校验和 → 只告警，不阻断加载。
    QFile file(outPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QByteArray content = file.readAll();
    file.close();
    content.replace("\"1.2.0\"", "\"9.9.9\"");
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(content);
    file.close();

    const auto tampered = FlowLoader::loadFromFile(outPath);
    QVERIFY(tampered.ok());
    QCOMPARE(tampered.flow.version, QStringLiteral("9.9.9"));
    bool checksumWarning = false;
    for (const FlowIssue& issue : tampered.warnings) {
        if (issue.fieldPath == QStringLiteral("checksum"))
            checksumWarning = true;
    }
    QVERIFY(checksumWarning);
}

// P1 流程控制：condition/goto 能加载、写回，并通过校验。
void TstFlow::flowControlFieldsRoundTrip()
{
    const auto loaded = FlowLoader::loadFromJson(R"JSON({
      "schemaVersion": 1, "id": "loop", "name": "loop", "product": "PM",
      "variables": {"i": 0},
      "steps": [
        {"id": "inc", "name": "inc", "action": "script",
         "params": {"script": "setVar('i', Number(getVar('i')) + 1); true;"}},
        {"id": "body", "name": "body", "action": "delay",
         "condition": "i < 3", "goto": "inc", "params": {"ms": 10}},
        {"id": "measure", "name": "m", "action": "measure",
         "params": {"channel": 0, "windowMs": 100},
         "expect": {"metric": "avg", "min": 0, "max": 1}}
      ]
    })JSON");
    QVERIFY2(loaded.ok(), "flow control fields should parse");
    QCOMPARE(loaded.flow.steps.at(1).condition, QStringLiteral("i < 3"));
    QCOMPARE(loaded.flow.steps.at(1).gotoStep, QStringLiteral("inc"));
    QVERIFY2(FlowValidator::validate(loaded.flow).isEmpty(), "loop flow should validate");

    const QJsonObject json = FlowWriter::toJson(loaded.flow);
    const QJsonObject body =
        json.value(QStringLiteral("steps")).toArray().at(1).toObject();
    QCOMPARE(body.value(QStringLiteral("condition")).toString(), QStringLiteral("i < 3"));
    QCOMPARE(body.value(QStringLiteral("goto")).toString(), QStringLiteral("inc"));
}

// 跳转目标必须存在，否则校验期就报错。
void TstFlow::gotoMissingTargetIsRejected()
{
    const auto loaded = FlowLoader::loadFromJson(R"JSON({
      "schemaVersion": 1, "id": "bad", "name": "bad", "product": "PM",
      "steps": [
        {"id": "m", "name": "m", "action": "measure", "goto": "nope",
         "params": {"channel": 0, "windowMs": 100},
         "expect": {"metric": "avg", "min": 0, "max": 1}}
      ]
    })JSON");
    QVERIFY2(loaded.ok(), "loader should not reject goto target (validator does)");
    QVERIFY(hasPath(FlowValidator::validate(loaded.flow),
                    QStringLiteral("steps[0].goto")));
}

// readVersion：精确版本与正则两种写法都要能加载、校验并写回。
void TstFlow::readVersionStepIsValid()
{
    const auto loaded = FlowLoader::loadFromJson(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "v1", "name": "check fw", "action": "readVersion",
         "params": {"cmd": "*IDN?"},
         "expect": {"version": "AegisSim,DMM,0001,1.0"}},
        {"id": "v2", "name": "check fw by pattern", "action": "readVersion",
         "expect": {"versionPattern": ".*,1\\.0$"}}
      ]
    })JSON");
    QVERIFY2(loaded.ok(), "readVersion flow should load");
    QVERIFY2(FlowValidator::validate(loaded.flow).isEmpty(), "readVersion flow should validate");

    const TestStep& first = loaded.flow.steps.at(0);
    QCOMPARE(first.action, StepAction::ReadVersion);
    QCOMPARE(first.cmd, QStringLiteral("*IDN?"));
    QCOMPARE(first.expect.version, QStringLiteral("AegisSim,DMM,0001,1.0"));
    QVERIFY(first.expect.versionPattern.isEmpty());
    QVERIFY(loaded.flow.steps.at(1).expect.versionPattern.startsWith(QStringLiteral(".*")));

    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString outPath = temp.filePath(QStringLiteral("version.json"));
    QString error;
    QVERIFY2(FlowWriter::saveToFile(loaded.flow, outPath, &error), qPrintable(error));

    const auto reloaded = FlowLoader::loadFromFile(outPath);
    QVERIFY2(reloaded.ok(), "written readVersion flow should reload");
    QCOMPARE(reloaded.flow.steps.at(0).expect.version,
             QStringLiteral("AegisSim,DMM,0001,1.0"));
    QCOMPARE(reloaded.flow.steps.at(1).expect.versionPattern,
             QStringLiteral(".*,1\\.0$"));
}

// readVersion 没有 expect：报错定位到 expect，不能被当成"无判定"通过。
void TstFlow::readVersionRequiresExpect()
{
    const auto issues = check(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "v1", "name": "check fw", "action": "readVersion", "params": {}}
      ]
    })JSON");
    QVERIFY(hasPath(issues, QStringLiteral("steps[0].expect")));
}

// version 与 versionPattern 同时配置，或都不配置：均报错。
void TstFlow::readVersionRejectsAmbiguousExpect()
{
    const auto both = check(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "v1", "name": "check fw", "action": "readVersion",
         "expect": {"version": "1.0", "versionPattern": "1\\..*"}}
      ]
    })JSON");
    QVERIFY(hasPath(both, QStringLiteral("steps[0].expect")));

    const auto neither = check(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "v1", "name": "check fw", "action": "readVersion", "expect": {}}
      ]
    })JSON");
    QVERIFY(hasPath(neither, QStringLiteral("steps[0].expect")));
}

// TP-12a：子流程按"相对当前方案目录"解析；加载、校验、写回都要保留引用。
void TstFlow::subFlowIsLoadedValidatedAndWritten()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    const QString childPath = temp.filePath(QStringLiteral("child.json"));
    QVERIFY(writeFile(childPath, R"JSON({
      "schemaVersion": 1, "id": "child", "name": "child", "product": "PM",
      "steps": [
        {"id": "m", "name": "measure", "action": "measure",
         "params": {"channel": 0, "windowMs": 100},
         "expect": {"metric": "avg", "min": 0, "max": 1}}
      ]
    })JSON"));

    const QString parentPath = temp.filePath(QStringLiteral("parent.json"));
    QVERIFY(writeFile(parentPath, R"JSON({
      "schemaVersion": 1, "id": "parent", "name": "parent", "product": "PM",
      "steps": [
        {"id": "pre", "name": "precheck", "action": "subFlow",
         "params": {"file": "child.json"}}
      ]
    })JSON"));

    const auto loaded = FlowLoader::loadFromFile(parentPath);
    QVERIFY2(loaded.ok(), "parent flow should load");
    QCOMPARE(loaded.flow.steps.at(0).action, StepAction::SubFlow);
    QCOMPARE(loaded.flow.steps.at(0).flowRef, QStringLiteral("child.json"));
    QVERIFY(!loaded.flow.baseDir.isEmpty());
    QVERIFY2(FlowValidator::validate(loaded.flow).isEmpty(),
             "subFlow parent should validate");

    const QString outPath = temp.filePath(QStringLiteral("parent_out.json"));
    QString error;
    QVERIFY2(FlowWriter::saveToFile(loaded.flow, outPath, &error), qPrintable(error));

    const auto reloaded = FlowLoader::loadFromFile(outPath);
    QVERIFY(reloaded.ok());
    QCOMPARE(reloaded.flow.steps.at(0).action, StepAction::SubFlow);
    QCOMPARE(reloaded.flow.steps.at(0).flowRef, QStringLiteral("child.json"));
    QVERIFY2(FlowValidator::validate(reloaded.flow).isEmpty(),
             "written subFlow parent should validate");
}

// 子流程文件不存在：校验期定位到 params.file，而不是等到运行才失败。
void TstFlow::subFlowMissingFileIsRejected()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString parentPath = temp.filePath(QStringLiteral("parent.json"));
    QVERIFY(writeFile(parentPath, R"JSON({
      "schemaVersion": 1, "id": "parent", "name": "parent", "product": "PM",
      "steps": [
        {"id": "pre", "name": "precheck", "action": "subFlow",
         "params": {"file": "not-exists.json"}}
      ]
    })JSON"));

    const auto loaded = FlowLoader::loadFromFile(parentPath);
    QVERIFY2(loaded.ok(), "loader should not check file existence");
    QVERIFY(hasPath(FlowValidator::validate(loaded.flow),
                    QStringLiteral("steps[0].params.file")));
}

// 子流程循环引用：校验期直接报错，避免运行时无限递归。
void TstFlow::subFlowRecursionIsRejected()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString aPath = temp.filePath(QStringLiteral("a.json"));
    const QString bPath = temp.filePath(QStringLiteral("b.json"));
    QVERIFY(writeFile(aPath, R"JSON({
      "schemaVersion": 1, "id": "a", "name": "a", "product": "PM",
      "steps": [{"id": "toB", "name": "to b", "action": "subFlow",
                 "params": {"file": "b.json"}}]
    })JSON"));
    QVERIFY(writeFile(bPath, R"JSON({
      "schemaVersion": 1, "id": "b", "name": "b", "product": "PM",
      "steps": [{"id": "toA", "name": "to a", "action": "subFlow",
                 "params": {"file": "a.json"}}]
    })JSON"));

    const auto loaded = FlowLoader::loadFromFile(aPath);
    QVERIFY(loaded.ok());
    QVERIFY(hasPath(FlowValidator::validate(loaded.flow),
                    QStringLiteral("steps[0].params.file")));
}

// TP-14：操作员确认步骤能加载/写回，提示文本与等待时长不能丢。
void TstFlow::promptStepRoundTrip()
{
    const auto loaded = FlowLoader::loadFromJson(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f", "product": "PM",
      "steps": [
        {"id": "confirm", "name": "confirm fixture", "action": "prompt",
         "params": {"text": "请确认夹具已锁紧", "timeoutMs": 30000}},
        {"id": "m", "name": "measure", "action": "measure",
         "params": {"channel": 0, "windowMs": 100},
         "expect": {"metric": "avg", "min": 0, "max": 1}}
      ]
    })JSON");
    QVERIFY2(loaded.ok(), "prompt flow should load");
    QCOMPARE(loaded.flow.steps.at(0).action, StepAction::Prompt);
    QCOMPARE(loaded.flow.steps.at(0).promptText, QStringLiteral("请确认夹具已锁紧"));
    QCOMPARE(loaded.flow.steps.at(0).promptTimeoutMs, 30000);
    QVERIFY2(FlowValidator::validate(loaded.flow).isEmpty(),
             "prompt flow should validate");

    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString outPath = temp.filePath(QStringLiteral("prompt.json"));
    QString error;
    QVERIFY2(FlowWriter::saveToFile(loaded.flow, outPath, &error), qPrintable(error));

    const auto reloaded = FlowLoader::loadFromFile(outPath);
    QVERIFY(reloaded.ok());
    QCOMPARE(reloaded.flow.steps.at(0).promptText, QStringLiteral("请确认夹具已锁紧"));
    QCOMPARE(reloaded.flow.steps.at(0).promptTimeoutMs, 30000);
}

// 没有提示文本的确认步骤无法执行，校验必须发现。
void TstFlow::promptStepRequiresText()
{
    const auto issues = check(R"JSON({
      "schemaVersion": 1, "id": "f", "name": "f",
      "steps": [
        {"id": "confirm", "name": "confirm", "action": "prompt", "params": {}}
      ]
    })JSON");
    QVERIFY(hasPath(issues, QStringLiteral("steps[0].params.text")));
}

QTEST_GUILESS_MAIN(TstFlow)
#include "tst_flow.moc"
