#pragma once // 头文件保护

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace aegis::test {

// 插件单次调用结果。
struct PluginResult {
    bool ok = false;        // 进程/协议层面是否成功（超时、崩溃、非 JSON 都算失败）
    bool pass = false;      // 插件给出的判定
    QString message;        // 插件返回的说明
    QJsonObject measures;   // 可选实测值（measure.value 会写入步骤实测值）
    QString log;            // stderr 日志
    QString error;          // ok=false 时的原因
};

// =============================================================================
// PluginRunner —— 用户插件调用（独立进程 + JSON stdio）
//
// 约束（隔离与安全）：
//   - 每个插件一个目录 plugins/<name>/，含 manifest.json：
//       { "entry": ["python", "check.py"], "timeoutMs": 3000 }
//   - 主程序以子进程启动入口，stdin 写一行 JSON，stdout 读一行 JSON：
//       {"pass": true, "message": "...", "measures": {...}}
//   - stderr 作为日志附加到步骤说明；
//   - 只调用白名单内的插件（白名单由调用方/校验器保证）；
//   - 超时、非零退出、输出非 JSON 一律按失败处理，不影响主进程。
//
// 线程：run() 会阻塞等待子进程，必须在工作线程调用（测试引擎所在线程）。
// =============================================================================
class PluginRunner {
public:
    // 读取 manifest。command 为入口命令（数组），timeoutMs 为清单默认超时。
    static bool readManifest(const QString& pluginsRoot, const QString& name,
                             QStringList* command, int* timeoutMs, QString* error);

    // 调用插件。timeoutMs <= 0 时使用 manifest/默认值。
    static PluginResult run(const QString& pluginsRoot, const QString& name,
                            const QJsonObject& input, int timeoutMs);

    // 插件名只允许字母/数字/下划线/短横线，防止路径穿越。
    static bool isSafeName(const QString& name);

    // 解析插件根目录（相对路径按程序目录解析）。
    static QString resolveRoot(const QString& pluginsRoot);
};

} // namespace aegis::test
