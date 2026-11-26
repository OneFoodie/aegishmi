#pragma once // 头文件保护

#include <QDialog>

class QLabel;
class QLineEdit;

namespace aegis::security {
class AuthService;
}

namespace aegis::ui {

// =============================================================================
// LoginDialog —— 启动登录框
//
// 调用 exec() 返回 Accepted 表示已成功登录（AuthService 内部已切换当前用户）。
// 登录失败时在框内提示原因并允许重试，不直接退出。
// =============================================================================
class LoginDialog : public QDialog {
    Q_OBJECT

public:
    explicit LoginDialog(security::AuthService* auth, QWidget* parent = nullptr);

private slots:
    void tryLogin();

private:
    security::AuthService* auth_ = nullptr;
    QLineEdit* userEdit_ = nullptr;
    QLineEdit* passwordEdit_ = nullptr;
    QLabel* errorLabel_ = nullptr;
};

} // namespace aegis::ui
