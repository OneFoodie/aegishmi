#include "test/flow_validator.h"

#include "protocol/frame.h"      // kMaxChannels
#include "test/flow_loader.h"    // 子流程校验需要加载被引用文件
#include "test/plugin_runner.h"  // 插件名/清单校验

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace aegis::test {

namespace {
constexpr int kMinTimeoutMs = 10;
constexpr int kMaxTimeoutMs = 60000;
constexpr int kMinWindowMs = 10;
constexpr int kMaxWindowMs = 10000;
constexpr int kMaxRetries = 10;
constexpr int kMaxScriptLength = 20000; // 脚本源码上限，避免误贴大段文件
constexpr int kMaxSubFlowDepth = 8;     // 子流程嵌套上限（同时兜底循环引用）
} // namespace

QVector<FlowIssue> FlowValidator::validate(const TestFlow& flow,
                                           const QStringList& allowedPlugins,
                                           const QString& pluginsRoot)
{
    QSet<QString> visited;
    if (!flow.sourcePath.isEmpty())
        visited.insert(QFileInfo(flow.sourcePath).canonicalFilePath());
    return validateInternal(flow, allowedPlugins, pluginsRoot, 0, visited);
}

QVector<FlowIssue> FlowValidator::validateInternal(const TestFlow& flow,
                                                   const QStringList& allowedPlugins,
                                                   const QString& pluginsRoot,
                                                   int depth,
                                                   QSet<QString>& visited)
{
    QVector<FlowIssue> issues;
    // 小工具：追加一条问题，减少重复代码。
    auto add = [&issues](const QString& path, const QString& message) {
        issues.append({path, message});
    };

    // ---- 顶层校验 ----
    if (flow.schemaVersion != 1)
        add(QStringLiteral("schemaVersion"),
            QStringLiteral("仅支持 schemaVersion = 1，当前为 %1").arg(flow.schemaVersion));

    if (flow.id.trimmed().isEmpty())
        add(QStringLiteral("id"), QStringLiteral("id 不能为空"));

    if (flow.name.trimmed().isEmpty())
        add(QStringLiteral("name"), QStringLiteral("name 不能为空"));

    if (flow.defaultTimeoutMs < kMinTimeoutMs || flow.defaultTimeoutMs > kMaxTimeoutMs)
        add(QStringLiteral("defaultTimeoutMs"),
            QStringLiteral("默认超时必须在 %1~%2 ms 之间").arg(kMinTimeoutMs).arg(kMaxTimeoutMs));

    if (flow.onFailure != QLatin1String("abort") && flow.onFailure != QLatin1String("continue"))
        add(QStringLiteral("onFailure"), QStringLiteral("onFailure 只允许 abort 或 continue"));

    if (flow.steps.isEmpty()) {
        add(QStringLiteral("steps"), QStringLiteral("至少需要 1 个步骤"));
        return issues; // 没有步骤就没必要继续校验
    }

    // ---- 逐步校验 ----
    QSet<QString> seenIds;
    bool hasCheck = false; // measure / readVersion / script：至少有一种判定手段

    for (int i = 0; i < flow.steps.size(); ++i) {
        const TestStep& step = flow.steps.at(i);
        const QString base = QStringLiteral("steps[%1]").arg(i);

        // 通用字段
        if (step.id.trimmed().isEmpty()) {
            add(base + QStringLiteral(".id"), QStringLiteral("步骤 id 不能为空"));
        } else if (seenIds.contains(step.id)) {
            add(base + QStringLiteral(".id"),
                QStringLiteral("步骤 id 重复：%1").arg(step.id));
        } else {
            seenIds.insert(step.id);
        }

        if (step.name.trimmed().isEmpty())
            add(base + QStringLiteral(".name"), QStringLiteral("步骤 name 不能为空"));

        const int timeout = step.timeoutMs < 0 ? flow.defaultTimeoutMs : step.timeoutMs;
        if (timeout < kMinTimeoutMs || timeout > kMaxTimeoutMs)
            add(base + QStringLiteral(".timeoutMs"),
                QStringLiteral("超时必须在 %1~%2 ms 之间").arg(kMinTimeoutMs).arg(kMaxTimeoutMs));

        if (step.retries < 0 || step.retries > kMaxRetries)
            add(base + QStringLiteral(".retries"),
                QStringLiteral("重试次数必须在 0~%1 之间").arg(kMaxRetries));

        if (!step.onFail.isEmpty() && step.onFail != QLatin1String("abort")
            && step.onFail != QLatin1String("continue")) {
            add(base + QStringLiteral(".onFail"),
                QStringLiteral("onFail 只允许 abort 或 continue"));
        }

        // 按动作类型校验
        switch (step.action) {
        case StepAction::Command:
            if (step.cmd.trimmed().isEmpty())
                add(base + QStringLiteral(".params.cmd"),
                    QStringLiteral("command 步骤必须指定 cmd"));
            if (step.expect.present && step.expect.ackStatus < 0)
                add(base + QStringLiteral(".expect.ackStatus"),
                    QStringLiteral("ackStatus 不能为负数"));
            if (step.expect.present && step.expect.metric != Metric::Unknown)
                add(base + QStringLiteral(".expect.metric"),
                    QStringLiteral("command 步骤不应配置 metric"));
            break;

        case StepAction::Delay:
            if (step.delayMs < 1 || step.delayMs > kMaxTimeoutMs)
                add(base + QStringLiteral(".params.ms"),
                    QStringLiteral("delay 的 ms 必须在 1~%1 之间").arg(kMaxTimeoutMs));
            if (step.expect.present)
                add(base + QStringLiteral(".expect"),
                    QStringLiteral("delay 步骤不支持 expect"));
            break;

        case StepAction::Measure:
            hasCheck = true;
            if (step.channel < 0
                || step.channel >= static_cast<int>(protocol::kMaxChannels)) {
                add(base + QStringLiteral(".params.channel"),
                    QStringLiteral("通道号必须在 0~%1 之间")
                        .arg(static_cast<int>(protocol::kMaxChannels) - 1));
            }
            if (step.windowMs < kMinWindowMs || step.windowMs > kMaxWindowMs)
                add(base + QStringLiteral(".params.windowMs"),
                    QStringLiteral("采样窗口必须在 %1~%2 ms 之间")
                        .arg(kMinWindowMs)
                        .arg(kMaxWindowMs));

            if (!step.expect.present) {
                add(base + QStringLiteral(".expect"),
                    QStringLiteral("measure 步骤必须配置 expect"));
            } else {
                if (step.expect.metric == Metric::Unknown) {
                    add(base + QStringLiteral(".expect.metric"),
                        QStringLiteral("metric 必须是 avg/min/max/rms/peakToPeak 之一"));
                }
                if (!step.expect.hasMin && !step.expect.hasMax) {
                    add(base + QStringLiteral(".expect"),
                        QStringLiteral("至少配置 min 或 max"));
                }
                if (step.expect.hasMin && step.expect.hasMax
                    && step.expect.min >= step.expect.max) {
                    add(base + QStringLiteral(".expect"),
                        QStringLiteral("min 必须小于 max"));
                }
            }
            break;

        case StepAction::Assert:
            // 设计里保留、当前未实现，明确报错避免误用。
            add(base + QStringLiteral(".action"),
                QStringLiteral("assert 动作尚未实现"));
            break;

        case StepAction::Script:
            hasCheck = true;
            if (step.script.trimmed().isEmpty())
                add(base + QStringLiteral(".params.script"),
                    QStringLiteral("script 步骤必须提供脚本内容"));
            else if (step.script.size() > kMaxScriptLength)
                add(base + QStringLiteral(".params.script"),
                    QStringLiteral("脚本长度不能超过 %1 字符").arg(kMaxScriptLength));
            // 脚本自己负责判定，expect 不参与执行，配置了直接报错避免误解。
            if (step.expect.present)
                add(base + QStringLiteral(".expect"),
                    QStringLiteral("script 步骤不支持 expect，请在脚本内判定"));
            break;

        case StepAction::ReadVersion:
            hasCheck = true;
            if (!step.expect.present) {
                add(base + QStringLiteral(".expect"),
                    QStringLiteral("readVersion 步骤必须配置 expect"));
            } else {
                const bool hasVersion = !step.expect.version.trimmed().isEmpty();
                const bool hasPattern = !step.expect.versionPattern.trimmed().isEmpty();
                if (!hasVersion && !hasPattern) {
                    add(base + QStringLiteral(".expect"),
                        QStringLiteral("必须配置 version 或 versionPattern"));
                } else if (hasVersion && hasPattern) {
                    add(base + QStringLiteral(".expect"),
                        QStringLiteral("version 与 versionPattern 只能配置一个"));
                }
                if (hasPattern) {
                    const QRegularExpression pattern(step.expect.versionPattern);
                    if (!pattern.isValid()) {
                        add(base + QStringLiteral(".expect.versionPattern"),
                            QStringLiteral("版本正则无效：%1")
                                .arg(pattern.errorString()));
                    }
                }
                if (step.expect.metric != Metric::Unknown
                    || step.expect.hasMin || step.expect.hasMax) {
                    add(base + QStringLiteral(".expect"),
                        QStringLiteral("readVersion 不支持 metric/min/max"));
                }
            }
            break;

        case StepAction::Plugin: {
            hasCheck = true;
            if (step.plugin.isEmpty()) {
                add(base + QStringLiteral(".params.name"),
                    QStringLiteral("plugin 步骤必须指定插件名（params.name）"));
            } else if (!PluginRunner::isSafeName(step.plugin)) {
                add(base + QStringLiteral(".params.name"),
                    QStringLiteral("插件名只能包含字母/数字/下划线/短横线"));
            } else if (!allowedPlugins.contains(step.plugin)) {
                add(base + QStringLiteral(".params.name"),
                    QStringLiteral("插件未在白名单内：%1（station.json plugins.enabled）")
                        .arg(step.plugin));
            } else if (!pluginsRoot.isEmpty()) {
                QStringList command;
                int timeoutMs = 0;
                QString error;
                if (!PluginRunner::readManifest(pluginsRoot, step.plugin, &command,
                                                &timeoutMs, &error)) {
                    add(base + QStringLiteral(".params.name"),
                        QStringLiteral("插件清单不可用：%1").arg(error));
                }
            }
            if (step.expect.present) {
                add(base + QStringLiteral(".expect"),
                    QStringLiteral("plugin 步骤不支持 expect，请在插件内判定"));
            }
            break;
        }

        case StepAction::SubFlow: {
            // 子流程自身的校验结果会递归追加；这里只做定位与防循环。
            hasCheck = true;
            if (step.flowRef.trimmed().isEmpty()) {
                add(base + QStringLiteral(".params.file"),
                    QStringLiteral("subFlow 步骤必须指定子流程文件（params.file）"));
                break;
            }
            if (step.expect.present)
                add(base + QStringLiteral(".expect"),
                    QStringLiteral("subFlow 步骤不支持 expect，请在子流程内判定"));
            if (depth >= kMaxSubFlowDepth) {
                add(base + QStringLiteral(".params.file"),
                    QStringLiteral("子流程嵌套层数超过 %1，疑似循环引用").arg(kMaxSubFlowDepth));
                break;
            }
            if (flow.baseDir.isEmpty())
                break; // 内存加载（无来源目录），无法定位文件，跳过存在性检查

            const QFileInfo info(QDir(flow.baseDir).filePath(step.flowRef));
            if (!info.exists() || !info.isFile()) {
                add(base + QStringLiteral(".params.file"),
                    QStringLiteral("子流程文件不存在：%1").arg(step.flowRef));
                break;
            }
            const QString canonical = info.canonicalFilePath();
            if (visited.contains(canonical)) {
                add(base + QStringLiteral(".params.file"),
                    QStringLiteral("子流程存在循环引用：%1").arg(step.flowRef));
                break;
            }

            const FlowLoader::LoadResult loaded =
                FlowLoader::loadFromFile(info.absoluteFilePath());
            if (!loaded.ok()) {
                const FlowIssue& first = loaded.errors.first();
                add(base + QStringLiteral(".params.file"),
                    QStringLiteral("子流程加载失败（%1）：%2")
                        .arg(first.fieldPath, first.message));
                break;
            }
            visited.insert(canonical);
            issues += validateInternal(loaded.flow, allowedPlugins, pluginsRoot,
                                       depth + 1, visited);
            visited.remove(canonical);
            break;
        }

        case StepAction::Prompt:
            // 人工确认不承担判定，仍需要流程里有 measure/readVersion 等。
            if (step.promptText.trimmed().isEmpty())
                add(base + QStringLiteral(".params.text"),
                    QStringLiteral("prompt 步骤必须提供提示文本（params.text）"));
            if (step.promptTimeoutMs < 0 || step.promptTimeoutMs > 600000)
                add(base + QStringLiteral(".params.timeoutMs"),
                    QStringLiteral("操作员确认等待时长必须在 0~600000 ms 之间"));
            if (step.expect.present)
                add(base + QStringLiteral(".expect"),
                    QStringLiteral("prompt 步骤不支持 expect"));
            break;

        case StepAction::Unknown:
            add(base + QStringLiteral(".action"),
                QStringLiteral("未知的动作类型（只支持 command/delay/measure/script/"
                               "readVersion/plugin/subFlow/prompt）"));
            break;
        }
    }

    // ---- 流程控制：跳转目标必须存在；自跳转必须带条件，否则必然死循环 ----
    for (int i = 0; i < flow.steps.size(); ++i) {
        const TestStep& step = flow.steps.at(i);
        if (step.gotoStep.isEmpty())
            continue;
        if (!seenIds.contains(step.gotoStep)) {
            add(QStringLiteral("steps[%1].goto").arg(i),
                QStringLiteral("跳转目标不存在：%1").arg(step.gotoStep));
        } else if (step.gotoStep == step.id && step.condition.isEmpty()) {
            add(QStringLiteral("steps[%1].goto").arg(i),
                QStringLiteral("goto 指向自身时必须配置 condition，否则会死循环"));
        }
    }

    // ---- 全局要求：至少一个测量/版本/脚本/插件/子流程步骤，否则"测试"没有判定依据 ----
    if (!hasCheck && !flow.steps.isEmpty())
        add(QStringLiteral("steps"),
            QStringLiteral("至少需要 1 个 measure、readVersion、script、plugin 或 subFlow 步骤"));

    return issues;
}

} // namespace aegis::test
