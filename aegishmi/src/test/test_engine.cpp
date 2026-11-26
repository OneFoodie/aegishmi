#include "test/test_engine.h"

#include "protocol/frame.h"      // kSampleRateHz
#include "test/flow_loader.h"    // 子流程在运行期按需加载
#include "test/plugin_runner.h"  // 用户插件（独立进程 + JSON stdio）

#include <QDir>
#include <QJSEngine>
#include <QJSValue>
#include <QRegularExpression>
#include <QStringList>

#include <cmath>

namespace aegis::test {

namespace {

// 计算统计量。values 为窗口内的采样点（int16）。
double computeMetric(Metric metric, const std::vector<std::int16_t>& values)
{
    if (values.empty())
        return 0.0;

    double sum = 0.0;
    double sumSquares = 0.0;
    double minValue = values.front();
    double maxValue = values.front();

    for (const auto sample : values) {
        const double v = static_cast<double>(sample);
        sum += v;
        sumSquares += v * v;
        if (v < minValue)
            minValue = v;
        if (v > maxValue)
            maxValue = v;
    }

    const double count = static_cast<double>(values.size());
    switch (metric) {
    case Metric::Avg:
        return sum / count;
    case Metric::Min:
        return minValue;
    case Metric::Max:
        return maxValue;
    case Metric::Rms:
        return std::sqrt(sumSquares / count);
    case Metric::PeakToPeak:
        return maxValue - minValue;
    case Metric::Unknown:
        break;
    }
    return 0.0;
}

// 数字格式化：统一保留两位小数，便于界面显示与日志对比。
QString formatValue(double value)
{
    return QString::number(value, 'f', 2);
}

} // namespace

// =============================================================================
// ScriptApi —— 暴露给流程脚本（QJSEngine）的设备操作
//
// 脚本里可用：
//   setVout(channel, mV) / setIout(channel, mA)  -> 返回 bool，失败返回 false
//   measureAvg(channel, windowMs)                -> 返回该窗口平均值
//   measureOnce(channel)                         -> 单点测量（一个采样周期）
// 上述设备操作都走当前步骤的 link（空 = 主链路）。
//   sleep(ms) / log(text)
//
// 任一步操作失败都记录在 error_ 里（只保留第一条），由引擎统一判定步骤失败，
// 避免脚本忽略返回值时"静默通过"。
// =============================================================================
class ScriptApi final : public QObject {
    Q_OBJECT

public:
    ScriptApi(ITestDevice* device, ITestClock* clock, int timeoutMs,
              const QString& linkId,
              QHash<QString, QVariant>* variables = nullptr,
              QObject* parent = nullptr)
        : QObject(parent)
        , device_(device)
        , clock_(clock)
        , timeoutMs_(timeoutMs)
        , linkId_(linkId)
        , variables_(variables)
    {
    }

    Q_INVOKABLE bool setVout(int channel, int mv)
    {
        return send(DeviceCommand::SetVout, channel, mv);
    }

    Q_INVOKABLE bool setIout(int channel, int ma)
    {
        return send(DeviceCommand::SetIout, channel, ma);
    }

    Q_INVOKABLE double measureAvg(int channel, int windowMs)
    {
        const int window = qBound(10, windowMs, 10000);
        const std::vector<std::int16_t> samples =
            device_->sampleWindow(channel, window, linkId_);
        if (samples.empty()) {
            fail(QStringLiteral("measureAvg(%1)：未采集到数据").arg(channel));
            return 0.0;
        }

        // 与 measure 动作一致：按当前链路采样率判断数据是否足够。
        const int sampleRate = device_->sampleRateHz(linkId_);
        const int expected = static_cast<int>(static_cast<qint64>(window) * sampleRate / 1000);
        if (expected > 0 && static_cast<int>(samples.size()) * 10 < expected * 8) {
            fail(QStringLiteral("measureAvg(%1)：采样数据不足 %2/%3")
                     .arg(channel)
                     .arg(samples.size())
                     .arg(expected));
            return 0.0;
        }

        double sum = 0.0;
        for (const auto sample : samples)
            sum += sample;
        lastMeasured_ = sum / static_cast<double>(samples.size());
        hasMeasured_ = true;
        return lastMeasured_;
    }

    // 单点测量：等一个采样周期取一次值（辅助链路常用）。
    Q_INVOKABLE double measureOnce(int channel)
    {
        double value = 0.0;
        QString error;
        if (!device_->measureOnce(channel, timeoutMs_, &value, &error, linkId_)) {
            fail(QStringLiteral("measureOnce(%1)：%2").arg(channel).arg(error));
            return 0.0;
        }
        lastMeasured_ = value;
        hasMeasured_ = true;
        return lastMeasured_;
    }

    Q_INVOKABLE void sleep(int ms)
    {
        if (clock_ != nullptr)
            clock_->sleepMs(qBound(0, ms, 60000));
    }

    Q_INVOKABLE void log(const QString& message) { logs_.append(message); }

    // 运行期变量：脚本可以读写，用于实现循环计数、状态标志等流程控制。
    Q_INVOKABLE QVariant getVar(const QString& name) const
    {
        return variables_ != nullptr ? variables_->value(name) : QVariant();
    }

    Q_INVOKABLE void setVar(const QString& name, const QVariant& value)
    {
        if (variables_ != nullptr)
            variables_->insert(name, value);
    }

    bool ok() const { return error_.isEmpty(); }
    QString error() const { return error_; }
    bool hasMeasured() const { return hasMeasured_; }
    double lastMeasured() const { return lastMeasured_; }
    QString logsText() const { return logs_.join(QStringLiteral(" | ")); }

private:
    bool send(DeviceCommand command, int channel, int value)
    {
        quint8 status = 0xFF;
        if (!device_->sendCommand(command, channel, value, timeoutMs_, status, linkId_)) {
            fail(QStringLiteral("命令下发失败或超时"));
            return false;
        }
        if (status != 0) {
            fail(QStringLiteral("命令被拒绝，status=%1").arg(status));
            return false;
        }
        return true;
    }

    void fail(const QString& message)
    {
        if (error_.isEmpty())
            error_ = message;
    }

    ITestDevice* device_ = nullptr;
    ITestClock* clock_ = nullptr;
    int timeoutMs_ = 1000;
    QString linkId_; // 当前步骤的目标链路（空 = 主链路）
    QHash<QString, QVariant>* variables_ = nullptr;
    QString error_;
    QStringList logs_;
    bool hasMeasured_ = false;
    double lastMeasured_ = 0.0;
};

TestEngine::TestEngine(ITestDevice* device, ITestClock* clock, QObject* parent)
    : QObject(parent)
    , device_(device)
    , clock_(clock)
{
}

void TestEngine::requestStop()
{
    stopRequested_.store(true);
}

RunResult TestEngine::run(const TestFlow& flow)
{
    // 每次运行都复位停止标志，保证上一次的"停止"不会影响下一次。
    stopRequested_.store(false);

    // 运行期变量：流程 variables 的副本，脚本可写、条件表达式可读。
    runtimeVariables_.clear();
    for (auto it = flow.variables.begin(); it != flow.variables.end(); ++it)
        runtimeVariables_.insert(it.key(), it.value());

    RunResult result = runFlow(flow, 0, true);
    emit runFinished(result);
    return result;
}

RunResult TestEngine::runFlow(const TestFlow& flow, int depth, bool emitProgress)
{
    RunResult result;
    // 预先填好所有步骤的基础信息，这样即使中途停止，界面也能显示完整的表格。
    result.steps.resize(flow.steps.size());
    for (int i = 0; i < flow.steps.size(); ++i) {
        result.steps[i].index = i;
        result.steps[i].id = flow.steps.at(i).id;
        result.steps[i].name = flow.steps.at(i).name;
    }

    const qint64 runStart = clock_->nowMs();
    bool aborted = false;
    bool failed = false;

    // 跳转允许后退，必须限制总执行次数，防止配置错误导致死循环。
    constexpr int kMaxStepExecutions = 5000;
    int executions = 0;
    StepResult lastResult;

    for (int i = 0; i < flow.steps.size(); ++i) {
        // 每个步骤开始前检查一次停止请求。
        if (stopRequested_.load()) {
            aborted = true;
            break;
        }
        if (++executions > kMaxStepExecutions) {
            StepResult guard;
            guard.index = i;
            guard.id = flow.steps.at(i).id;
            guard.name = flow.steps.at(i).name;
            guard.status = StepStatus::Fail;
            guard.message = QStringLiteral("步骤执行次数超过 %1，疑似死循环")
                                .arg(kMaxStepExecutions);
            result.steps[i] = guard;
            if (emitProgress) {
                emit stepStarted(i);
                emit stepFinished(i, guard);
            }
            failed = true;
            break;
        }

        const TestStep& step = flow.steps.at(i);
        const int timeoutMs = step.timeoutMs < 0 ? flow.defaultTimeoutMs : step.timeoutMs;

        if (emitProgress)
            emit stepStarted(i);

        // 条件跳过：condition 为 false（或表达式出错）时不执行本步。
        if (!step.condition.isEmpty()) {
            QString conditionError;
            const bool conditionMet = evaluateCondition(
                step.condition, lastResult.index >= 0 ? &lastResult : nullptr,
                &conditionError);

            if (!conditionError.isEmpty()) {
                StepResult bad;
                bad.index = i;
                bad.id = step.id;
                bad.name = step.name;
                bad.status = StepStatus::Fail;
                bad.message = QStringLiteral("条件表达式错误：%1").arg(conditionError);
                result.steps[i] = bad;
                lastResult = bad;
                if (emitProgress)
                    emit stepFinished(i, bad);
                failed = true;

                const QString policy = step.onFail.isEmpty() ? flow.onFailure : step.onFail;
                if (policy == QLatin1String("abort")) {
                    for (int j = i + 1; j < flow.steps.size(); ++j)
                        result.steps[j].status = StepStatus::Skipped;
                    break;
                }
                continue;
            }

            if (!conditionMet) {
                StepResult skipped;
                skipped.index = i;
                skipped.id = step.id;
                skipped.name = step.name;
                skipped.status = StepStatus::Skipped;
                skipped.message = QStringLiteral("条件不满足，已跳过");
                result.steps[i] = skipped;
                if (emitProgress)
                    emit stepFinished(i, skipped);
                continue;
            }
        }

        // 重试循环：retries = 0 表示只执行一次。
        StepResult stepResult;
        const int attempts = step.retries + 1;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            stepResult = executeStep(step, i, timeoutMs, flow.baseDir, depth);
            if (stepResult.status == StepStatus::Pass)
                break;
            if (stopRequested_.load())
                break;

            // 失败且还有重试机会：在消息里记录重试次数，便于追溯。
            if (attempt + 1 < attempts) {
                stepResult.message = QStringLiteral("%1（第 %2 次重试）")
                                         .arg(stepResult.message)
                                         .arg(attempt + 1);
            }
        }

        result.steps[i] = stepResult;
        lastResult = stepResult;
        if (emitProgress)
            emit stepFinished(i, stepResult);

        if (stepResult.status == StepStatus::Fail) {
            failed = true;

            // 单步可以覆盖流程级策略。
            const QString policy = step.onFail.isEmpty() ? flow.onFailure : step.onFail;
            if (policy == QLatin1String("abort")) {
                // 后续步骤不再执行，标记为"跳过（因前序失败）"。
                for (int j = i + 1; j < flow.steps.size(); ++j)
                    result.steps[j].status = StepStatus::Skipped;
                break;
            }
        }

        // 跳转：goto 指向下一步之外时改变执行顺序（配合 condition 形成循环）。
        if (!step.gotoStep.isEmpty()) {
            int target = -1;
            for (int j = 0; j < flow.steps.size(); ++j) {
                if (flow.steps.at(j).id == step.gotoStep) {
                    target = j;
                    break;
                }
            }
            if (target < 0) {
                StepResult bad = stepResult;
                bad.status = StepStatus::Fail;
                bad.message = QStringLiteral("%1；跳转目标不存在：%2")
                                  .arg(stepResult.message, step.gotoStep);
                result.steps[i] = bad;
                failed = true;
                break;
            }
            i = target - 1;
        }
    }

    // 停止请求可能在最后一步之后才被设置，这里再确认一次。
    if (stopRequested_.load())
        aborted = true;

    if (aborted) {
        // 还没执行的步骤统一标记为"中止"。
        for (auto& step : result.steps) {
            if (step.status == StepStatus::NotRun)
                step.status = StepStatus::Aborted;
        }
        result.verdict = RunVerdict::Aborted;
    } else if (failed) {
        result.verdict = RunVerdict::Fail;
    } else {
        result.verdict = RunVerdict::Pass;
    }

    result.totalElapsedMs = static_cast<int>(clock_->nowMs() - runStart);
    return result;
}

StepResult TestEngine::runSingleStep(const TestFlow& flow, int index)
{
    StepResult result;
    if (index < 0 || index >= flow.steps.size()) {
        result.index = index;
        result.status = StepStatus::Fail;
        result.message = QStringLiteral("步骤序号超出范围：%1").arg(index);
        return result;
    }

    stopRequested_.store(false);

    const TestStep& step = flow.steps.at(index);
    const int timeoutMs = step.timeoutMs < 0 ? flow.defaultTimeoutMs : step.timeoutMs;

    emit stepStarted(index);

    // 与 run() 相同的重试语义：retries = 0 表示只执行一次。
    result = executeStep(step, index, timeoutMs, flow.baseDir, 0);
    const int attempts = step.retries + 1;
    for (int attempt = 1; attempt < attempts; ++attempt) {
        if (result.status == StepStatus::Pass || stopRequested_.load())
            break;
        result.message = QStringLiteral("%1（第 %2 次重试）").arg(result.message).arg(attempt);
        result = executeStep(step, index, timeoutMs, flow.baseDir, 0);
    }

    emit stepFinished(index, result);
    return result;
}

// 条件表达式求值：变量直接挂成全局（i < 3 可写），也可用 variables.i。
bool TestEngine::evaluateCondition(const QString& expression, const StepResult* last,
                                   QString* error) const
{
    QJSEngine engine;
    QJSValue variables = engine.newObject();
    for (auto it = runtimeVariables_.begin(); it != runtimeVariables_.end(); ++it) {
        const QJSValue value = engine.toScriptValue(it.value());
        variables.setProperty(it.key(), value);
        engine.globalObject().setProperty(it.key(), value);
    }
    engine.globalObject().setProperty(QStringLiteral("variables"), variables);

    if (last != nullptr && last->hasMeasured)
        engine.globalObject().setProperty(QStringLiteral("lastValue"), last->measured);
    else
        engine.globalObject().setProperty(QStringLiteral("lastValue"),
                                          QJSValue(QJSValue::NullValue));
    engine.globalObject().setProperty(
        QStringLiteral("lastOk"),
        last != nullptr && last->status == StepStatus::Pass);

    const QJSValue value = engine.evaluate(expression, QStringLiteral("flow-condition.js"));
    if (value.isError()) {
        if (error != nullptr)
            *error = value.toString();
        return false;
    }
    return value.toBool();
}

StepResult TestEngine::executeStep(const TestStep& step, int index, int timeoutMs,
                                   const QString& baseDir, int depth)
{
    StepResult result;
    result.index = index;
    result.id = step.id;
    result.name = step.name;

    const qint64 start = clock_->nowMs();

    StepStatus status = StepStatus::Pass;
    QString message;

    switch (step.action) {
    case StepAction::Command: {
        const DeviceCommand command = deviceCommandFromString(step.cmd);
        if (command == DeviceCommand::Unknown) {
            status = StepStatus::Fail;
            message = QStringLiteral("未知命令：%1").arg(step.cmd);
            break;
        }

        quint8 ackStatus = 0xFF;
        if (!device_->sendCommand(command, step.channel, step.valueMv, timeoutMs, ackStatus,
                                  step.link)) {
            status = StepStatus::Fail;
            message = QStringLiteral("命令超时或设备无应答");
            break;
        }

        if (step.expect.present
            && ackStatus != static_cast<quint8>(step.expect.ackStatus)) {
            status = StepStatus::Fail;
            message = QStringLiteral("应答状态 %1，期望 %2")
                          .arg(static_cast<int>(ackStatus))
                          .arg(step.expect.ackStatus);
        }
        break;
    }

    case StepAction::Delay:
        // 等待期间由时钟抽象决定：生产环境真的睡，测试环境瞬间推进虚拟时间。
        clock_->sleepMs(step.delayMs);
        break;

    case StepAction::Measure: {
        const std::vector<std::int16_t> samples =
            device_->sampleWindow(step.channel, step.windowMs, step.link);

        // 期望采样点数 = 窗口时长 × 当前链路采样率。允许 20% 的缺量（丢包/启动延迟）。
        const int sampleRate = device_->sampleRateHz(step.link);
        const int expected =
            static_cast<int>(static_cast<qint64>(step.windowMs) * sampleRate / 1000);

        if (samples.empty()) {
            status = StepStatus::Fail;
            message = QStringLiteral("未采集到数据");
            break;
        }
        if (expected > 0 && static_cast<int>(samples.size()) * 10 < expected * 8) {
            status = StepStatus::Fail;
            message = QStringLiteral("采样数据不足：%1/%2")
                          .arg(samples.size())
                          .arg(expected);
            break;
        }
        if (step.expect.metric == Metric::Unknown) {
            status = StepStatus::Fail;
            message = QStringLiteral("统计量未定义");
            break;
        }

        const double value = computeMetric(step.expect.metric, samples);
        result.measured = value;
        result.hasMeasured = true;

        if (step.expect.hasMin && value < step.expect.min) {
            status = StepStatus::Fail;
            message = QStringLiteral("实测 %1 低于下限 %2")
                          .arg(formatValue(value), formatValue(step.expect.min));
            break;
        }
        if (step.expect.hasMax && value > step.expect.max) {
            status = StepStatus::Fail;
            message = QStringLiteral("实测 %1 高于上限 %2")
                          .arg(formatValue(value), formatValue(step.expect.max));
        }
        break;
    }

    case StepAction::ReadVersion: {
        QString value;
        QString error;
        if (!device_->queryText(step.cmd.trimmed(), timeoutMs, &value, &error, step.link)) {
            status = StepStatus::Fail;
            message = error.isEmpty() ? QStringLiteral("读取版本失败") : error;
            break;
        }

        value = value.trimmed();
        if (value.isEmpty()) {
            status = StepStatus::Fail;
            message = QStringLiteral("设备返回的版本为空");
            break;
        }

        if (!step.expect.version.isEmpty()) {
            if (value.compare(step.expect.version, Qt::CaseSensitive) != 0) {
                status = StepStatus::Fail;
                message = QStringLiteral("版本不匹配：实际 %1，期望 %2")
                              .arg(value, step.expect.version);
                break;
            }
        } else if (!step.expect.versionPattern.isEmpty()) {
            const QRegularExpression pattern(step.expect.versionPattern);
            if (!pattern.isValid()) {
                status = StepStatus::Fail;
                message = QStringLiteral("版本正则无效：%1").arg(pattern.errorString());
                break;
            }
            if (!pattern.match(value).hasMatch()) {
                status = StepStatus::Fail;
                message = QStringLiteral("版本 %1 不匹配正则 %2")
                              .arg(value, step.expect.versionPattern);
                break;
            }
        } else {
            status = StepStatus::Fail;
            message = QStringLiteral("未配置期望版本");
            break;
        }

        // 版本号写入 message，落库后历史/PDF 报告的"说明"列可直接看到。
        message = QStringLiteral("版本：%1").arg(value);
        break;
    }

    case StepAction::Assert:
        status = StepStatus::Fail;
        message = QStringLiteral("assert 动作尚未实现");
        break;

    case StepAction::Script: {
        // 每个脚本步骤一个独立的 JS 引擎，变量/函数不会串到下一步。
        QJSEngine engine;
        ScriptApi api(device_, clock_, timeoutMs, step.link, &runtimeVariables_);

        QJSValue apiObject = engine.newQObject(&api);
        // 栈上对象不能被 JS 引擎接管所有权，显式声明由 C++ 管理。
        QJSEngine::setObjectOwnership(&api, QJSEngine::CppOwnership);
        engine.globalObject().setProperty(QStringLiteral("__aegis"), apiObject);

        // 把常用操作提升为全局函数，脚本里可直接 setVout(...)。
        engine.evaluate(QStringLiteral(
            "function setVout(channel, mv) { return __aegis.setVout(channel, mv); }\n"
            "function setIout(channel, ma) { return __aegis.setIout(channel, ma); }\n"
            "function measureAvg(channel, ms) { return __aegis.measureAvg(channel, ms); }\n"
            "function measureOnce(channel) { return __aegis.measureOnce(channel); }\n"
            "function sleep(ms) { __aegis.sleep(ms); }\n"
            "function log(text) { __aegis.log(String(text)); }\n"
            "function getVar(name) { return __aegis.getVar(String(name)); }\n"
            "function setVar(name, value) { __aegis.setVar(String(name), value); }"));
        // 变量同时挂到全局，脚本可直接写 i = i + 1。
        for (auto it = runtimeVariables_.begin(); it != runtimeVariables_.end(); ++it)
            engine.globalObject().setProperty(it.key(), engine.toScriptValue(it.value()));

        const QJSValue value = engine.evaluate(step.script,
                                               QStringLiteral("flow-script.js"));
        if (value.isError()) {
            status = StepStatus::Fail;
            message = QStringLiteral("脚本异常：%1").arg(value.toString());
        } else if (!api.ok()) {
            status = StepStatus::Fail;
            message = QStringLiteral("脚本执行失败：%1").arg(api.error());
        } else if (value.isBool() && !value.toBool()) {
            status = StepStatus::Fail;
            message = QStringLiteral("脚本返回 false");
        }

        // 脚本内调用过 measureAvg 时，把最后一次结果作为步骤实测值落库。
        if (api.hasMeasured()) {
            result.measured = api.lastMeasured();
            result.hasMeasured = true;
        }
        if (!api.logsText().isEmpty()) {
            const QString prefix = message.isEmpty() ? QString() : QStringLiteral("；");
            message += prefix + QStringLiteral("日志：") + api.logsText();
        }
        break;
    }

    case StepAction::Plugin: {
        if (!plugins_.enabled) {
            status = StepStatus::Fail;
            message = QStringLiteral("插件功能未启用（station.json plugins.enabled 为空）");
            break;
        }
        if (!plugins_.allowed.contains(step.plugin)) {
            status = StepStatus::Fail;
            message = QStringLiteral("插件未在白名单：%1").arg(step.plugin);
            break;
        }

        QJsonObject input = runContext_;
        input.insert(QStringLiteral("step"), step.id);
        input.insert(QStringLiteral("params"), step.params);

        const PluginResult plugin =
            PluginRunner::run(plugins_.root, step.plugin, input, timeoutMs);
        if (!plugin.ok) {
            status = StepStatus::Fail;
            message = QStringLiteral("插件调用失败：%1").arg(plugin.error);
            break;
        }

        if (!plugin.pass)
            status = StepStatus::Fail;
        message = plugin.message;

        if (plugin.measures.value(QStringLiteral("value")).isDouble()) {
            result.measured = plugin.measures.value(QStringLiteral("value")).toDouble();
            result.hasMeasured = true;
        }
        if (!plugin.log.isEmpty()) {
            const QString prefix = message.isEmpty() ? QString() : QStringLiteral("；");
            message += prefix + QStringLiteral("日志：") + plugin.log;
        }
        break;
    }

    case StepAction::SubFlow:
        // 子流程需要自己计算耗时，直接返回。
        return executeSubFlow(step, index, timeoutMs, baseDir, depth);

    case StepAction::Prompt: {
        if (step.promptText.trimmed().isEmpty()) {
            status = StepStatus::Fail;
            message = QStringLiteral("未配置操作员确认的提示文本");
            break;
        }
        if (!promptHandler_) {
            status = StepStatus::Fail;
            message = QStringLiteral("未配置操作员确认处理器");
            break;
        }
        if (!promptHandler_(step.promptText, step.promptTimeoutMs)) {
            status = StepStatus::Fail;
            message = QStringLiteral("操作员未确认（取消或等待超时）");
        }
        break;
    }

    case StepAction::Unknown:
        status = StepStatus::Fail;
        message = QStringLiteral("未知的动作类型");
        break;
    }

    result.status = status;
    result.message = message;
    result.elapsedMs = static_cast<int>(clock_->nowMs() - start);
    return result;
}

// TP-12a：加载并执行子流程。子步骤不刷新主表格，只把汇总结果回填到当前步骤。
StepResult TestEngine::executeSubFlow(const TestStep& step, int index, int timeoutMs,
                                      const QString& baseDir, int depth)
{
    Q_UNUSED(timeoutMs); // 子流程内每一步使用自己的超时

    StepResult result;
    result.index = index;
    result.id = step.id;
    result.name = step.name;

    constexpr int kMaxSubFlowDepth = 8;
    const qint64 start = clock_->nowMs();

    auto fail = [&result](const QString& text) {
        result.status = StepStatus::Fail;
        result.message = text;
    };

    if (step.flowRef.trimmed().isEmpty()) {
        fail(QStringLiteral("子流程文件未配置"));
    } else if (depth >= kMaxSubFlowDepth) {
        fail(QStringLiteral("子流程嵌套层数超过 %1，疑似循环引用").arg(kMaxSubFlowDepth));
    } else {
        const QString path = QDir(baseDir).filePath(step.flowRef);
        const FlowLoader::LoadResult loaded = FlowLoader::loadFromFile(path);
        if (!loaded.ok()) {
            const FlowIssue& first = loaded.errors.first();
            fail(QStringLiteral("子流程加载失败（%1）：%2")
                     .arg(first.fieldPath, first.message));
        } else {
            const RunResult sub = runFlow(loaded.flow, depth + 1, false);
            switch (sub.verdict) {
            case RunVerdict::Pass:
                result.status = StepStatus::Pass;
                result.message = QStringLiteral("子流程 %1 通过（%2 步）")
                                     .arg(step.flowRef)
                                     .arg(loaded.flow.steps.size());
                break;
            case RunVerdict::Fail: {
                QString detail = QStringLiteral("未知步骤失败");
                for (const StepResult& child : sub.steps) {
                    if (child.status == StepStatus::Fail) {
                        detail = QStringLiteral("%1：%2").arg(child.name, child.message);
                        break;
                    }
                }
                fail(QStringLiteral("子流程 %1 失败（%2）").arg(step.flowRef, detail));
                break;
            }
            case RunVerdict::Aborted:
                result.status = StepStatus::Aborted;
                result.message = QStringLiteral("子流程 %1 被中止").arg(step.flowRef);
                break;
            }

            // 透传最后一个实测值，便于报告展示。
            for (const StepResult& child : sub.steps) {
                if (child.hasMeasured) {
                    result.measured = child.measured;
                    result.hasMeasured = true;
                }
            }
        }
    }

    result.elapsedMs = static_cast<int>(clock_->nowMs() - start);
    return result;
}

} // namespace aegis::test

// Q_OBJECT 写在 .cpp 里的 ScriptApi 需要包含 moc 生成代码。
#include "test_engine.moc"
