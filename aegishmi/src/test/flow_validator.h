#pragma once // 头文件保护

#include "test/flow_types.h"

#include <QSet>
#include <QStringList>
#include <QVector>

namespace aegis::test {

// =============================================================================
// FlowValidator —— 流程语义校验
//
// 与 FlowLoader 的分工：加载器只保证"结构读得出来"，校验器负责判断"配置是否可用"。
// 所有问题一次性返回（不是遇到第一个就停），方便用户一轮改完。
// =============================================================================
class FlowValidator {
public:
    // 返回空列表表示校验通过。
    // allowedPlugins：station.json 的 plugins.enabled 白名单（空 = 不允许任何插件）。
    // pluginsRoot：插件根目录（空 = 跳过 manifest 存在性检查）。
    static QVector<FlowIssue> validate(const TestFlow& flow,
                                       const QStringList& allowedPlugins = {},
                                       const QString& pluginsRoot = {});

private:
    // 内部递归实现：子流程需要深度限制与循环引用检测。
    static QVector<FlowIssue> validateInternal(const TestFlow& flow,
                                               const QStringList& allowedPlugins,
                                               const QString& pluginsRoot,
                                               int depth,
                                               QSet<QString>& visited);
};

} // namespace aegis::test
