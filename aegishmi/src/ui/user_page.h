#pragma once // 头文件保护

#include <QWidget>

class QPushButton;
class QTableWidget;

namespace aegis::security {
class AuthService;
}

namespace aegis::storage {
class SystemRepository;
}

namespace aegis::ui {

// =============================================================================
// UserPage —— 用户管理（仅管理员可见）
//
// 支持：新建用户、重置口令、修改角色、启用/停用。
// 保护规则：不能停用自己；不能停用/降级最后一个启用状态的管理员。
// =============================================================================
class UserPage : public QWidget {
    Q_OBJECT

public:
    UserPage(storage::SystemRepository* repository, security::AuthService* auth,
             QWidget* parent = nullptr);

    void refresh();

private slots:
    void createUser();
    void resetPassword();
    void changeRole();
    void toggleEnabled();
    void onSelectionChanged();

private:
    QString selectedUsername() const;
    QString selectedRole() const;
    bool selectedEnabled() const;
    void applyControlState();

    storage::SystemRepository* repository_ = nullptr;
    security::AuthService* auth_ = nullptr;

    QTableWidget* table_ = nullptr;
    QPushButton* newButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QPushButton* roleButton_ = nullptr;
    QPushButton* toggleButton_ = nullptr;
};

} // namespace aegis::ui
