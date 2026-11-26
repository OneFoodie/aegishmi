#pragma once // 头文件保护

#include <QMetaType>
#include <QObject>
#include <QString>

namespace aegis::core {
struct StationConfig;
}

namespace aegis::service {

// version.json 的内容。只有 version 必填，其余字段按需提供。
struct VersionInfo {
    QString version;    // 最新版本号，如 "0.2.0"
    QString notes;      // 更新说明
    QString url;        // 升级包地址（本地路径或 http(s)）
    QString sha256;     // 升级包 SHA-256（小写十六进制）
    QString minVersion; // 允许从该版本起直接升级（可选，仅展示）
};

// 版本号比较：a 比 b 新返回 >0，相同返回 0，a 更旧返回 <0。
// 规则：忽略前导 v/V、忽略 -pre / +build 后缀，按点分数字段逐段比较，
// 缺失字段按 0 处理（"1.2" 与 "1.2.0" 视为相同）。
int compareVersions(const QString& a, const QString& b);

// 解析 version.json；失败时通过 error 返回原因。
bool parseVersionManifest(const QByteArray& data, VersionInfo* info,
                          QString* error = nullptr);

// 计算文件 SHA-256（小写十六进制）；读取失败返回空字符串。
QString fileSha256(const QString& path);

// =============================================================================
// UpdateChecker —— 轻量版本检测（SY-05）
//
// 只读清单并比较版本，不在程序内静默下载或覆盖运行中的文件：
//   - 清单 / 升级包放在内网 HTTP、共享目录或随升级包分发；
//   - 无清单、离线、未启用时静默跳过，不影响开机；
//   - 发现新版本后由界面提示版本号、说明与包地址；
//   - 升级包摘要用 verifyPackage() 校验，不匹配即拒绝。
// =============================================================================
class UpdateChecker : public QObject {
    Q_OBJECT

public:
    enum class Status {
        Skipped,         // 未启用 / 无清单 / 离线：静默跳过
        UpToDate,        // 已是最新版本
        UpdateAvailable, // 有新版本
        Invalid,         // 清单格式错误
        Failed,          // 其他错误（网络 / IO）
    };

    explicit UpdateChecker(const core::StationConfig& config, QObject* parent = nullptr);

    bool enabled() const { return enabled_; }
    QString currentVersion() const { return currentVersion_; }
    void setCurrentVersion(const QString& version) { currentVersion_ = version; }
    Status status() const { return status_; }

    // 读取并解析清单；成功返回 true。
    bool loadManifest(VersionInfo* info, QString* error = nullptr);

    // 检查一次（同步）；结果通过 checked 信号发出。
    void check();

    // 校验升级包 SHA-256；expected 为空表示跳过校验。
    static bool verifyPackage(const QString& path, const QString& expectedSha256,
                              QString* error = nullptr);

signals:
    void checked(Status status, const aegis::service::VersionInfo& info,
                 const QString& message);
    void logMessage(const QString& message);

private:
    bool readManifestData(QByteArray* data, QString* error) const;

    bool enabled_ = false;
    QString manifest_;
    QString currentVersion_;
    Status status_ = Status::Skipped;
};

} // namespace aegis::service

Q_DECLARE_METATYPE(aegis::service::UpdateChecker::Status)
Q_DECLARE_METATYPE(aegis::service::VersionInfo)
