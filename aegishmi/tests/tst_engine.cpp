// Qt Test 框架
#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "protocol/frame.h" // kSampleRateHz
#include "test/flow_types.h"
#include "test/test_device.h"
#include "test/test_engine.h"

using namespace aegis::test;

namespace {

// =============================================================================
// Mock 设备：不连接任何硬件，按测试需要返回成功/失败/指定采样值
// =============================================================================
class MockDevice : public ITestDevice {
public:
    bool commandOk = true;       // sendCommand 的返回值
    quint8 ack = 0;              // 应答状态码
    int failTimes = 0;           // 前 N 次命令强制失败，用于测试重试
    std::vector<std::int16_t> samples; // sampleWindow 返回的数据
    std::vector<std::int16_t> auxSamples; // aux 链路返回的数据（空则复用 samples）
    int auxSampleRate = 0;                // aux 链路采样率（0 表示与主链路一致）
    QString versionText = QStringLiteral("AegisSim,DMM,0001,1.0"); // queryText 返回值
    bool versionOk = true;
    QString versionError;
    QString lastVersionCommand;
    QString lastCommandLink;
    QString lastQueryLink;
    QString lastSampleLink;
    int commandCalls = 0;
    int sampleCalls = 0;
    int versionCalls = 0;

    bool sendCommand(DeviceCommand, int, int, int, quint8& status,
                     const QString& linkId) override
    {
        ++commandCalls;
        lastCommandLink = linkId;
        if (failTimes > 0) {
            --failTimes;
            return false;
        }
        status = ack;
        return commandOk;
    }

    bool queryText(const QString& cmd, int, QString* value, QString* errorMessage,
                   const QString& linkId) override
    {
        ++versionCalls;
        lastVersionCommand = cmd;
        lastQueryLink = linkId;
        if (!versionOk) {
            if (errorMessage)
                *errorMessage = versionError;
            return false;
        }
        if (value)
            *value = versionText;
        return true;
    }

    std::vector<std::int16_t> sampleWindow(int, int, const QString& linkId) override
    {
        ++sampleCalls;
        lastSampleLink = linkId;
        return linkId == QLatin1String("aux") && !auxSamples.empty() ? auxSamples : samples;
    }

    // 引擎会按这个速率推算"窗口内应收到多少点"，Mock 与自定义协议保持一致。
    int sampleRateHz(const QString& linkId) const override
    {
        if (linkId == QLatin1String("aux") && auxSampleRate > 0)
            return auxSampleRate;
        return aegis::protocol::kSampleRateHz;
    }
};

// =============================================================================
// Mock 时钟：sleep 不真的等待，只推进虚拟时间
// =============================================================================
class MockClock final : public ITestClock {
public:
    qint64 now = 0;
    int sleepCalls = 0;

    void sleepMs(int ms) override
    {
        ++sleepCalls;
        if (ms > 0)
            now += ms;
    }

    qint64 nowMs() override { return now; }
};

// ---- 构造测试数据的小工具 ----

TestFlow makeFlow(const QString& onFailure = QStringLiteral("abort"))
{
    TestFlow flow;
    flow.schemaVersion = 1;
    flow.id = QStringLiteral("flow");
    flow.name = QStringLiteral("flow");
    flow.defaultTimeoutMs = 1000;
    flow.onFailure = onFailure;
    return flow;
}

TestStep makeCommandStep(int retries = 0)
{
    TestStep step;
    step.id = QStringLiteral("cmd");
    step.name = QStringLiteral("set vout");
    step.action = StepAction::Command;
    step.cmd = QStringLiteral("SET_VOUT");
    step.valueMv = 5000;
    step.retries = retries;
    step.expect.present = true;
    step.expect.ackStatus = 0;
    return step;
}

TestStep makeDelayStep(int ms)
{
    TestStep step;
    step.id = QStringLiteral("delay");
    step.name = QStringLiteral("wait");
    step.action = StepAction::Delay;
    step.delayMs = ms;
    return step;
}

TestStep makeMeasureStep(Metric metric, bool hasMin, double minValue,
                         bool hasMax, double maxValue)
{
    TestStep step;
    step.id = QStringLiteral("measure");
    step.name = QStringLiteral("measure");
    step.action = StepAction::Measure;
    step.channel = 0;
    step.windowMs = 200; // 10kHz × 200ms = 2000 点
    step.expect.present = true;
    step.expect.metric = metric;
    step.expect.hasMin = hasMin;
    step.expect.min = minValue;
    step.expect.hasMax = hasMax;
    step.expect.max = maxValue;
    return step;
}

TestStep makeVersionStep(const QString& version, const QString& pattern = QString())
{
    TestStep step;
    step.id = QStringLiteral("version");
    step.name = QStringLiteral("check firmware");
    step.action = StepAction::ReadVersion;
    step.expect.present = true;
    step.expect.version = version;
    step.expect.versionPattern = pattern;
    return step;
}

std::vector<std::int16_t> constantSamples(int count, std::int16_t value)
{
    return std::vector<std::int16_t>(static_cast<std::size_t>(count), value);
}

// 子流程用例需要真实文件：按指定内容写入临时目录。
bool writeJsonFile(const QString& path, const QByteArray& content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(content) == content.size();
}

TestFlow makeSubFlowParent(const QString& baseDir, const QString& file)
{
    TestFlow flow = makeFlow();
    TestStep step;
    step.id = QStringLiteral("sub");
    step.name = QStringLiteral("sub flow");
    step.action = StepAction::SubFlow;
    step.flowRef = file;
    flow.steps = {step};
    flow.baseDir = baseDir;
    return flow;
}

TestStep makePromptStep(const QString& text, int timeoutMs = 0)
{
    TestStep step;
    step.id = QStringLiteral("confirm");
    step.name = QStringLiteral("operator confirm");
    step.action = StepAction::Prompt;
    step.promptText = text;
    step.promptTimeoutMs = timeoutMs;
    return step;
}

} // namespace

// =============================================================================
// TstEngine —— M3.2 流程执行引擎
// =============================================================================
class TstEngine : public QObject {
    Q_OBJECT

private slots:
    void allStepsPass();
    void linkRoutedToDevice();
    void commandTimeoutFails();
    void ackStatusMismatchFails();
    void retryThenSuccess();
    void measureBelowMinFails();
    void measureAboveMaxFails();
    void insufficientSamplesFails();
    void unknownCommandFails();
    void abortSkipsRemainingSteps();
    void continuePolicyKeepsRunning();
    void stopMarksRemainingAborted();
    void delayAdvancesClock();
    void rmsAndPeakToPeakMetrics();
    void scriptStepRunsCommandsAndMeasures();
    void scriptExceptionFails();
    void readVersionPasses();
    void readVersionMismatchFails();
    void readVersionRegexPasses();
    void readVersionDeviceErrorFails();
    void pluginStepRunsInSubprocess();
    void pluginNotWhitelistedFails();
    void conditionFalseSkipsStep();
    void gotoLoopRepeatsUntilConditionFails();
    void gotoMissingTargetFails();
    void subFlowRunsChildSteps();
    void subFlowFailurePropagates();
    void subFlowRecursionIsRejected();
    void promptConfirmedPasses();
    void promptCancelledFails();
    void promptWithoutHandlerFails();
    void measureStepRoutesToLink();
    void scriptMeasureOnceUsesStepLink();
};

// 全部通过：命令 → 等待 → 测量，最终 PASS，且步骤耗时/进度信号正确。
void TstEngine::allStepsPass()
{
    MockDevice device;
    device.samples = constantSamples(2000, 5000);
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makeCommandStep(), makeDelayStep(300),
                  makeMeasureStep(Metric::Avg, true, 4750, true, 5250)};

    TestEngine engine(&device, &clock);
    int startedCount = 0;
    int finishedCount = 0;
    int finishedVerdict = -1;
    connect(&engine, &TestEngine::stepStarted, this, [&](int) { ++startedCount; });
    connect(&engine, &TestEngine::stepFinished, this, [&](int, const StepResult&) {
        ++finishedCount;
    });
    connect(&engine, &TestEngine::runFinished, this, [&](const RunResult& result) {
        finishedVerdict = static_cast<int>(result.verdict);
    });

    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Pass);
    QCOMPARE(result.steps.size(), 3);
    QCOMPARE(startedCount, 3);
    QCOMPARE(finishedCount, 3);
    QCOMPARE(finishedVerdict, static_cast<int>(RunVerdict::Pass));
    QCOMPARE(device.commandCalls, 1);
    QCOMPARE(device.sampleCalls, 1);
    QVERIFY(result.steps.at(2).hasMeasured);
    QVERIFY(qAbs(result.steps.at(2).measured - 5000.0) < 0.001);
    QVERIFY(result.totalElapsedMs >= 300); // delay 至少推进了 300ms
}

// 步骤指定 link 时原样交给设备层；未指定时为空（设备层按主链路处理）。
void TstEngine::linkRoutedToDevice()
{
    MockDevice device;
    device.samples = constantSamples(2000, 5000);
    MockClock clock;

    TestFlow flow = makeFlow();
    TestStep command = makeCommandStep();
    command.link = QStringLiteral("aux");
    TestStep version = makeVersionStep(QStringLiteral("AegisSim,DMM,0001,1.0"));
    version.cmd = QStringLiteral("*IDN?");
    version.link = QStringLiteral("aux2");
    flow.steps = {command, version};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Pass);
    QCOMPARE(device.lastCommandLink, QStringLiteral("aux"));
    QCOMPARE(device.lastQueryLink, QStringLiteral("aux2"));
}

// 命令超时（设备无应答）：步骤失败，整体 FAIL。
void TstEngine::commandTimeoutFails()
{
    MockDevice device;
    device.failTimes = 1;
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makeCommandStep()};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Fail);
    QCOMPARE(result.steps.at(0).status, StepStatus::Fail);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("超时")));
}

// 应答状态不匹配：失败并记录实际状态码。
void TstEngine::ackStatusMismatchFails()
{
    MockDevice device;
    device.ack = 2; // 期望 0
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makeCommandStep()};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.steps.at(0).status, StepStatus::Fail);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("应答状态")));
}

// 重试：第一次失败、第二次成功，最终 PASS，且设备被调用两次。
void TstEngine::retryThenSuccess()
{
    MockDevice device;
    device.failTimes = 1;
    MockClock clock;

    TestFlow flow = makeFlow();
    TestStep command = makeCommandStep(1); // retries = 1
    flow.steps = {command};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Pass);
    QCOMPARE(result.steps.at(0).status, StepStatus::Pass);
    QCOMPARE(device.commandCalls, 2);
}

// 实测低于下限：失败，实测值被记录。
void TstEngine::measureBelowMinFails()
{
    MockDevice device;
    device.samples = constantSamples(2000, 4000);
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makeMeasureStep(Metric::Avg, true, 4750, true, 5250)};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Fail);
    QCOMPARE(result.steps.at(0).status, StepStatus::Fail);
    QVERIFY(result.steps.at(0).hasMeasured);
    QVERIFY(qAbs(result.steps.at(0).measured - 4000.0) < 0.001);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("低于下限")));
}

// 实测高于上限：失败。
void TstEngine::measureAboveMaxFails()
{
    MockDevice device;
    device.samples = constantSamples(2000, 6000);
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makeMeasureStep(Metric::Avg, true, 4750, true, 5250)};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.steps.at(0).status, StepStatus::Fail);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("高于上限")));
}

// 采样数据不足（例如设备掉线）：失败原因应为"数据不足"。
void TstEngine::insufficientSamplesFails()
{
    MockDevice device;
    device.samples = constantSamples(10, 5000); // 远少于 2000
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makeMeasureStep(Metric::Avg, true, 4750, true, 5250)};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.steps.at(0).status, StepStatus::Fail);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("数据不足")));
}

// 未知命令名：配置写错时应在执行阶段明确失败，而不是静默跳过。
void TstEngine::unknownCommandFails()
{
    MockDevice device;
    MockClock clock;

    TestFlow flow = makeFlow();
    TestStep step = makeCommandStep();
    step.cmd = QStringLiteral("MAGIC");
    flow.steps = {step};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.steps.at(0).status, StepStatus::Fail);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("未知命令")));
    QCOMPARE(device.commandCalls, 0); // 不应把未知命令发给设备
}

// abort 策略：第一步失败后，后续步骤标记为"跳过"，不再调用设备。
void TstEngine::abortSkipsRemainingSteps()
{
    MockDevice device;
    device.samples = constantSamples(2000, 4000); // 低于下限
    MockClock clock;

    TestFlow flow = makeFlow(QStringLiteral("abort"));
    flow.steps = {makeMeasureStep(Metric::Avg, true, 4750, true, 5250),
                  makeMeasureStep(Metric::Avg, true, 4750, true, 5250)};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Fail);
    QCOMPARE(result.steps.at(0).status, StepStatus::Fail);
    QCOMPARE(result.steps.at(1).status, StepStatus::Skipped);
    QCOMPARE(device.sampleCalls, 1); // 第二步没有执行
}

// continue 策略：第一步失败后仍然继续，最终整体仍是 FAIL。
void TstEngine::continuePolicyKeepsRunning()
{
    MockDevice device;
    device.samples = constantSamples(2000, 4000);
    MockClock clock;

    TestFlow flow = makeFlow(QStringLiteral("continue"));
    flow.steps = {makeMeasureStep(Metric::Avg, true, 4750, true, 5250),
                  makeMeasureStep(Metric::Avg, true, 4750, true, 5250)};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Fail);
    QCOMPARE(result.steps.at(0).status, StepStatus::Fail);
    QCOMPARE(result.steps.at(1).status, StepStatus::Fail); // 继续执行了
    QCOMPARE(device.sampleCalls, 2);
}

// 用户停止：第一步执行完收到停止请求，后续步骤标记中止，整体 ABORTED。
void TstEngine::stopMarksRemainingAborted()
{
    MockDevice device;
    device.samples = constantSamples(2000, 5000);
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makeMeasureStep(Metric::Avg, true, 4750, true, 5250),
                  makeMeasureStep(Metric::Avg, true, 4750, true, 5250)};

    TestEngine engine(&device, &clock);
    connect(&engine, &TestEngine::stepStarted, this, [&](int index) {
        if (index == 0)
            engine.requestStop(); // 模拟界面在第一步开始时点了"停止"
    });

    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Aborted);
    QCOMPARE(result.steps.at(0).status, StepStatus::Pass);
    QCOMPARE(result.steps.at(1).status, StepStatus::Aborted);
}

// delay 必须走时钟抽象：虚拟时间应前进，且总耗时统计正确。
void TstEngine::delayAdvancesClock()
{
    MockDevice device;
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makeDelayStep(500)};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Pass);
    QCOMPARE(clock.sleepCalls, 1);
    QCOMPARE(clock.now, qint64(500));
    QCOMPARE(result.totalElapsedMs, 500);
    QCOMPARE(result.steps.at(0).elapsedMs, 500);
}

// RMS 与峰峰值统计：交替 ±1000 的波形，RMS = 1000，峰峰值 = 2000。
void TstEngine::rmsAndPeakToPeakMetrics()
{
    MockDevice device;
    device.samples.clear();
    for (int i = 0; i < 2000; ++i)
        device.samples.push_back((i % 2 == 0) ? 1000 : -1000);
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makeMeasureStep(Metric::Rms, true, 990, true, 1010),
                  makeMeasureStep(Metric::PeakToPeak, false, 0, true, 2100)};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Pass);
    QVERIFY(qAbs(result.steps.at(0).measured - 1000.0) < 0.01);
    QVERIFY(qAbs(result.steps.at(1).measured - 2000.0) < 0.01);
}

// script 步骤：JS 里下发命令 + 采集判定，结果落成实测值，日志进入说明。
void TstEngine::scriptStepRunsCommandsAndMeasures()
{
    MockDevice device;
    device.samples = constantSamples(2000, 5000);
    MockClock clock;

    TestFlow flow = makeFlow();
    TestStep step;
    step.id = QStringLiteral("js");
    step.name = QStringLiteral("script");
    step.action = StepAction::Script;
    step.script = QStringLiteral(
        "if (!setVout(0, 5000)) throw new Error('setVout failed');\n"
        "var v = measureAvg(0, 200);\n"
        "log('avg=' + v);\n"
        "if (v < 4750 || v > 5250) throw new Error('out of range: ' + v);\n"
        "true;");
    flow.steps = {step};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Pass);
    QCOMPARE(result.steps.at(0).status, StepStatus::Pass);
    QCOMPARE(device.commandCalls, 1);
    QCOMPARE(device.sampleCalls, 1);
    QVERIFY(result.steps.at(0).hasMeasured);
    QVERIFY(qAbs(result.steps.at(0).measured - 5000.0) < 0.001);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("avg=5000")));
}

// script 抛异常：步骤失败，说明里带脚本错误信息。
void TstEngine::scriptExceptionFails()
{
    MockDevice device;
    MockClock clock;

    TestFlow flow = makeFlow();
    TestStep step;
    step.id = QStringLiteral("js");
    step.name = QStringLiteral("script");
    step.action = StepAction::Script;
    step.script = QStringLiteral("throw new Error('boom');");
    flow.steps = {step};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Fail);
    QCOMPARE(result.steps.at(0).status, StepStatus::Fail);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("boom")));
}

// readVersion：版本精确匹配 → PASS，实际版本写入说明。
void TstEngine::readVersionPasses()
{
    MockDevice device;
    device.versionText = QStringLiteral("AegisSim,DMM,0001,1.0");
    MockClock clock;

    TestFlow flow = makeFlow();
    TestStep step = makeVersionStep(QStringLiteral("AegisSim,DMM,0001,1.0"));
    step.cmd = QStringLiteral("*IDN?");
    flow.steps = {step};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Pass);
    QCOMPARE(result.steps.at(0).status, StepStatus::Pass);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("AegisSim,DMM,0001,1.0")));
    QCOMPARE(device.versionCalls, 1);
    QCOMPARE(device.lastVersionCommand, QStringLiteral("*IDN?"));
}

// readVersion：版本不一致 → FAIL，说明同时给出实际值与期望值。
void TstEngine::readVersionMismatchFails()
{
    MockDevice device;
    device.versionText = QStringLiteral("AegisSim,DMM,0001,1.0");
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makeVersionStep(QStringLiteral("AegisSim,DMM,0001,2.0"))};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Fail);
    QCOMPARE(result.steps.at(0).status, StepStatus::Fail);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("版本不匹配")));
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("AegisSim,DMM,0001,1.0")));
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("AegisSim,DMM,0001,2.0")));
}

// readVersion：正则匹配（如校验主版本号）→ PASS。
void TstEngine::readVersionRegexPasses()
{
    MockDevice device;
    device.versionText = QStringLiteral("AegisSim,DMM,0001,1.0");
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makeVersionStep(QString(), QStringLiteral(".*,1\\.0$"))};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Pass);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("版本")));
}

// readVersion：设备读取失败（协议不支持/超时）→ FAIL，透传底层错误。
void TstEngine::readVersionDeviceErrorFails()
{
    MockDevice device;
    device.versionOk = false;
    device.versionError = QStringLiteral("设备未连接");
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makeVersionStep(QStringLiteral("1.0"))};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Fail);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("设备未连接")));
}

namespace {

// 创建一个调用 plugin_stub 的插件目录（entry 用绝对路径，避免依赖 PATH）。
bool makeStubPlugin(const QString& root, const QString& name, const QString& mode)
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
    manifest.insert(QStringLiteral("timeoutMs"), 5000);

    QFile file(QDir(pluginDir).filePath(QStringLiteral("manifest.json")));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(manifest).toJson());
    return true;
}

TestStep makePluginStep(const QString& name)
{
    TestStep step;
    step.id = QStringLiteral("plugin");
    step.name = QStringLiteral("plugin check");
    step.action = StepAction::Plugin;
    step.plugin = name;
    return step;
}

} // namespace

// 插件步骤：子进程通过 JSON stdio 返回判定与实测值。
void TstEngine::pluginStepRunsInSubprocess()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(makeStubPlugin(dir.filePath(QStringLiteral("plugins")), QStringLiteral("demo"),
                           QStringLiteral("pass")));

    MockDevice device;
    MockClock clock;
    TestEngine engine(&device, &clock);

    PluginContext plugins;
    plugins.enabled = true;
    plugins.root = dir.filePath(QStringLiteral("plugins"));
    plugins.allowed = QStringList{QStringLiteral("demo")};
    engine.setPluginContext(plugins);

    QJsonObject context;
    context.insert(QStringLiteral("sn"), QStringLiteral("SN-1"));
    engine.setRunContext(context);

    TestFlow flow = makeFlow();
    flow.steps = {makePluginStep(QStringLiteral("demo"))};

    const RunResult result = engine.run(flow);
    QCOMPARE(result.verdict, RunVerdict::Pass);
    QCOMPARE(result.steps.size(), 1);
    QCOMPARE(result.steps.first().status, StepStatus::Pass);
    QVERIFY(result.steps.first().hasMeasured);
    QCOMPARE(result.steps.first().measured, 123.5);
}

// 白名单：未启用的插件直接判失败，不启动子进程。
void TstEngine::pluginNotWhitelistedFails()
{
    MockDevice device;
    MockClock clock;
    TestEngine engine(&device, &clock);

    PluginContext plugins;
    plugins.enabled = true;
    plugins.root = QStringLiteral("plugins");
    plugins.allowed = QStringList{QStringLiteral("other")};
    engine.setPluginContext(plugins);

    TestFlow flow = makeFlow();
    flow.steps = {makePluginStep(QStringLiteral("demo"))};

    const RunResult result = engine.run(flow);
    QCOMPARE(result.verdict, RunVerdict::Fail);
    QVERIFY(result.steps.first().message.contains(QStringLiteral("白名单")));
}

// 条件为 false 时跳过该步：不调用设备、不计失败，整体仍 PASS。
void TstEngine::conditionFalseSkipsStep()
{
    MockDevice device;
    MockClock clock;

    TestFlow flow = makeFlow();
    TestStep skipped = makeDelayStep(100);
    skipped.condition = QStringLiteral("false");
    flow.steps = {skipped, makeDelayStep(200)};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Pass);
    QCOMPARE(result.steps.at(0).status, StepStatus::Skipped);
    QCOMPARE(result.steps.at(0).elapsedMs, qint64(0));
    QCOMPARE(result.steps.at(1).status, StepStatus::Pass);
    QCOMPARE(clock.sleepCalls, 1); // 只有第二步真正等待
}

// goto + condition 形成循环：脚本自增 i，条件不满足时退出。
void TstEngine::gotoLoopRepeatsUntilConditionFails()
{
    MockDevice device;
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.variables.insert(QStringLiteral("i"), 0);

    TestStep inc;
    inc.id = QStringLiteral("inc");
    inc.name = QStringLiteral("inc");
    inc.action = StepAction::Script;
    inc.script = QStringLiteral("setVar('i', Number(getVar('i')) + 1); true;");

    TestStep loop = makeDelayStep(10);
    loop.id = QStringLiteral("loop");
    loop.condition = QStringLiteral("i < 3");
    loop.gotoStep = QStringLiteral("inc");

    TestStep done = makeDelayStep(20);
    done.id = QStringLiteral("done");

    flow.steps = {inc, loop, done};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Pass);
    QCOMPARE(result.steps.at(0).status, StepStatus::Pass);
    QCOMPARE(result.steps.at(1).status, StepStatus::Skipped); // 第 3 次循环条件不满足
    QCOMPARE(result.steps.at(2).status, StepStatus::Pass);
    // i 从 0 自增到 3，循环体执行 2 次（i=1、i=2），最后一步再等 1 次。
    QCOMPARE(clock.sleepCalls, 3);
}

// 跳转目标不存在：运行期兜底失败，避免静默继续或崩溃。
void TstEngine::gotoMissingTargetFails()
{
    MockDevice device;
    MockClock clock;

    TestFlow flow = makeFlow();
    TestStep step = makeDelayStep(10);
    step.gotoStep = QStringLiteral("nope");
    flow.steps = {step};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Fail);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("跳转目标不存在")));
}

// TP-12a：子流程展开执行，子步骤不刷新主表格，汇总结果回填到父步骤。
void TstEngine::subFlowRunsChildSteps()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeJsonFile(dir.filePath(QStringLiteral("child.json")), R"JSON({
      "schemaVersion": 1, "id": "child", "name": "child", "product": "PM",
      "steps": [
        {"id": "m", "name": "child measure", "action": "measure",
         "params": {"channel": 0, "windowMs": 200},
         "expect": {"metric": "avg", "min": 4750, "max": 5250}}
      ]
    })JSON"));

    MockDevice device;
    device.samples = constantSamples(2000, 5000);
    MockClock clock;

    TestFlow flow = makeSubFlowParent(dir.path(), QStringLiteral("child.json"));

    TestEngine engine(&device, &clock);
    int started = 0;
    connect(&engine, &TestEngine::stepStarted, this, [&](int) { ++started; });

    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Pass);
    QCOMPARE(result.steps.size(), 1);
    QCOMPARE(result.steps.at(0).status, StepStatus::Pass);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("子流程")));
    QVERIFY(result.steps.at(0).hasMeasured);
    QVERIFY(qAbs(result.steps.at(0).measured - 5000.0) < 0.001);
    QCOMPARE(started, 1); // 子流程内部步骤不触发主表格信号
    QCOMPARE(device.sampleCalls, 1);
}

// 子流程内步骤失败：父步骤失败，消息里带出具体子步骤与原因。
void TstEngine::subFlowFailurePropagates()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeJsonFile(dir.filePath(QStringLiteral("child.json")), R"JSON({
      "schemaVersion": 1, "id": "child", "name": "child", "product": "PM",
      "steps": [
        {"id": "m", "name": "child measure", "action": "measure",
         "params": {"channel": 0, "windowMs": 200},
         "expect": {"metric": "avg", "min": 6000, "max": 7000}}
      ]
    })JSON"));

    MockDevice device;
    device.samples = constantSamples(2000, 5000);
    MockClock clock;

    TestFlow flow = makeSubFlowParent(dir.path(), QStringLiteral("child.json"));

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Fail);
    QCOMPARE(result.steps.at(0).status, StepStatus::Fail);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("子流程")));
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("child measure")));
}

// 子流程循环引用：运行时由嵌套深度兜底，不能无限递归。
void TstEngine::subFlowRecursionIsRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeJsonFile(dir.filePath(QStringLiteral("loop.json")), R"JSON({
      "schemaVersion": 1, "id": "loop", "name": "loop", "product": "PM",
      "steps": [
        {"id": "self", "name": "self", "action": "subFlow",
         "params": {"file": "loop.json"}}
      ]
    })JSON"));

    MockDevice device;
    MockClock clock;

    TestFlow flow = makeSubFlowParent(dir.path(), QStringLiteral("loop.json"));

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Fail);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("嵌套")));
}

// TP-14：操作员确认 → 通过，提示文本与等待时长原样传给处理器。
void TstEngine::promptConfirmedPasses()
{
    MockDevice device;
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makePromptStep(QStringLiteral("请确认夹具已锁紧"), 15000)};

    TestEngine engine(&device, &clock);
    QString capturedText;
    int capturedTimeout = -1;
    int calls = 0;
    engine.setPromptHandler([&](const QString& text, int timeoutMs) {
        capturedText = text;
        capturedTimeout = timeoutMs;
        ++calls;
        return true;
    });

    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Pass);
    QCOMPARE(result.steps.at(0).status, StepStatus::Pass);
    QCOMPARE(calls, 1);
    QCOMPARE(capturedText, QStringLiteral("请确认夹具已锁紧"));
    QCOMPARE(capturedTimeout, 15000);
}

// 操作员取消（或等待超时）：步骤失败，并按流程策略影响整体结果。
void TstEngine::promptCancelledFails()
{
    MockDevice device;
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makePromptStep(QStringLiteral("请确认夹具已锁紧"))};

    TestEngine engine(&device, &clock);
    engine.setPromptHandler([](const QString&, int) { return false; });

    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Fail);
    QCOMPARE(result.steps.at(0).status, StepStatus::Fail);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("未确认")));
}

// 无人值守场景没有注入处理器：明确失败，而不是静默通过。
void TstEngine::promptWithoutHandlerFails()
{
    MockDevice device;
    MockClock clock;

    TestFlow flow = makeFlow();
    flow.steps = {makePromptStep(QStringLiteral("请确认夹具已锁紧"))};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Fail);
    QVERIFY(result.steps.at(0).message.contains(QStringLiteral("处理器")));
}

// EQ-06 收尾：measure 步骤按 link 路由，采样数据与采样率都取目标链路。
void TstEngine::measureStepRoutesToLink()
{
    MockDevice device;
    device.samples = constantSamples(2000, 4000);  // 主链路（低于下限）
    device.auxSamples = constantSamples(10, 5000); // 辅助链路：50Hz × 200ms = 10 点
    device.auxSampleRate = 50;
    MockClock clock;

    TestFlow flow = makeFlow();
    TestStep step = makeMeasureStep(Metric::Avg, true, 4750, true, 5250);
    step.link = QStringLiteral("aux");
    flow.steps = {step};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    // 若走了主链路：数据 4000 会判定低于下限；若用了主链路采样率：期望 2000 点会判数据不足。
    QCOMPARE(result.verdict, RunVerdict::Pass);
    QCOMPARE(device.lastSampleLink, QStringLiteral("aux"));
    QVERIFY(qAbs(result.steps.at(0).measured - 5000.0) < 0.001);
}

// EQ-06 收尾：脚本里的单点测量（measureOnce）同样使用步骤的链路。
void TstEngine::scriptMeasureOnceUsesStepLink()
{
    MockDevice device;
    device.samples = constantSamples(2000, 4000);
    device.auxSamples = constantSamples(4, 5000);
    device.auxSampleRate = 50;
    MockClock clock;

    TestFlow flow = makeFlow();
    TestStep step;
    step.id = QStringLiteral("js");
    step.name = QStringLiteral("script");
    step.action = StepAction::Script;
    step.link = QStringLiteral("aux");
    step.script = QStringLiteral(
        "var v = measureOnce(0);\n"
        "if (v < 4999) throw new Error('bad ' + v);\n"
        "true;");
    flow.steps = {step};

    TestEngine engine(&device, &clock);
    const RunResult result = engine.run(flow);

    QCOMPARE(result.verdict, RunVerdict::Pass);
    QCOMPARE(device.lastSampleLink, QStringLiteral("aux"));
    QVERIFY(qAbs(result.steps.at(0).measured - 5000.0) < 0.001);
}

QTEST_GUILESS_MAIN(TstEngine)
#include "tst_engine.moc"
