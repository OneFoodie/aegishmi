#pragma once // 头文件保护

#include "storage/system_repository.h"

#include <QObject>
#include <QString>

namespace aegis::security {

// 角色：观察员（只读）/ 操作员（可连接、测试、确认报警）/ 质检员（可复核）/
// 管理员（全部）。
enum class Role {
    Viewer,
    Operator,
    Quality,
    Admin,
};

// 权限点。界面按权限点启用/禁用操作，避免把角色判断散落到各处。
enum class Permission {
    View,       // 查看波形、历史、趋势
    Operate,    // 连接设备、执行测试、确认报警、导出报表
    Approve,    // 关键操作复核（4-eyes）
    Administer, // 用户/审计等管理功能
};

struct UserInfo {
    QString name;
    Role role = Role::Viewer;
};

// =============================================================================
// AuthService —— 登录鉴权 + 操作审计
//
// 口令存储：随机盐 + SHA256 多轮摘要（不存明文）。
// 首次运行（用户表为空）自动创建三个演示账号：
//   admin / admin123     管理员
//   operator / operator123 操作员
//   viewer / viewer123   观察员
//
// 审计写入 audit_log；登录失败也会记录（用户名为尝试登录的账号）。
// =============================================================================
class AuthService : public QObject {
    Q_OBJECT

public:
    explicit AuthService(storage::SystemRepository* repository, QObject* parent = nullptr);

    // 首次运行创建默认账号；已有用户则直接返回 true。
    bool seedDefaults(QString* errorMessage = nullptr);

    // 校验口令并切换当前用户。失败时通过 errorMessage 返回原因，并写入审计。
    bool login(const QString& username, const QString& password,
               QString* errorMessage = nullptr);
    void logout();

    // 双人复核（4-eyes）：校验指定账号口令且具备操作权限，不切换当前登录用户。
    // 复核人必须与当前用户不同；成功/失败都会写审计（target 为被复核的操作）。
    bool verifyApprover(const QString& username, const QString& password,
                        const QString& action, UserInfo* approver = nullptr,
                        QString* errorMessage = nullptr);

    // 复核是否必须由质检角色完成（QA-07+）。默认 false，保持"操作员/管理员均可复核"
    // 的历史行为；工位配置 qualityApprovalOnly 打开后仅质检员/管理员可复核。
    void setRequireQualityApprover(bool require) { requireQualityApprover_ = require; }
    bool requireQualityApprover() const { return requireQualityApprover_; }

    bool isLoggedIn() const { return loggedIn_; }
    UserInfo currentUser() const { return user_; }
    bool can(Permission permission) const;

    // 以当前用户写一条审计记录（未登录时记为 "—"）。
    void audit(const QString& action, const QString& target,
               const QString& result, const QString& detail = QString());

    // ---- 角色/权限的文本与判定 ----
    static Role roleFromText(const QString& text);
    static QString roleText(Role role);        // admin / operator / viewer
    static QString roleDisplayName(Role role); // 管理员 / 操作员 / 观察员
    static bool roleAllows(Role role, Permission permission);

    // ---- 口令工具（登录框/测试可复用）----
    static QString makeSalt();
    static QString hashPassword(const QString& salt, const QString& password);

signals:
    void loggedIn(const QString& username);
    void loggedOut();

private:
    storage::SystemRepository* repository_ = nullptr;
    bool loggedIn_ = false;
    bool requireQualityApprover_ = false;
    UserInfo user_;
};

} // namespace aegis::security
