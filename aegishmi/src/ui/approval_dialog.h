#pragma once // 头文件保护

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;

namespace aegis::security {
class AuthService;
}

namespace aegis::ui {

// =============================================================================
// ApprovalDialog —— 关键操作双人复核（4-eyes）
//
// 解除 Andon 停线、重新首件、治具校准等不可逆/高影响动作，需要第二个人
// 输入自己的账号口令授权。复核人不能是当前操作人，且必须具备操作权限。
// =============================================================================
class ApprovalDialog : public QDialog {
    Q_OBJECT

public:
    ApprovalDialog(security::AuthService* auth, const QString& action,
                   QWidget* parent = nullptr);

    QString approverName() const { return approverName_; }

private slots:
    void submit();

private:
    security::AuthService* auth_ = nullptr;
    QString action_;
    QString approverName_;
    QLineEdit* userEdit_ = nullptr;
    QLineEdit* passwordEdit_ = nullptr;
    QLabel* errorLabel_ = nullptr;
};

} // namespace aegis::ui
