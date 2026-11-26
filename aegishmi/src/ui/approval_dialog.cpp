#include "ui/approval_dialog.h"

#include "security/auth_service.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace aegis::ui {

ApprovalDialog::ApprovalDialog(security::AuthService* auth, const QString& action,
                               QWidget* parent)
    : QDialog(parent)
    , auth_(auth)
    , action_(action)
{
    setWindowTitle(QStringLiteral("双人复核"));
    setModal(true);
    setMinimumWidth(380);

    auto* root = new QVBoxLayout(this);

    auto* title = new QLabel(QStringLiteral("该操作需要第二人复核"));
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    const QString operatorName =
        auth_ != nullptr ? auth_->currentUser().name : QStringLiteral("—");
    auto* info = new QLabel(QStringLiteral("操作：%1\n操作人：%2")
                                .arg(action_, operatorName));
    info->setStyleSheet(QStringLiteral("color:#34495e"));
    info->setWordWrap(true);
    root->addWidget(info);

    auto* form = new QFormLayout();
    userEdit_ = new QLineEdit();
    userEdit_->setPlaceholderText(QStringLiteral("复核人账号"));
    passwordEdit_ = new QLineEdit();
    passwordEdit_->setEchoMode(QLineEdit::Password);
    passwordEdit_->setPlaceholderText(QStringLiteral("复核人口令"));
    form->addRow(QStringLiteral("复核人"), userEdit_);
    form->addRow(QStringLiteral("口令"), passwordEdit_);
    root->addLayout(form);

    errorLabel_ = new QLabel();
    errorLabel_->setStyleSheet(QStringLiteral("color:#c0392b"));
    errorLabel_->setWordWrap(true);
    root->addWidget(errorLabel_);

    auto* hint = new QLabel(QStringLiteral(
        "复核人须为其他操作员或管理员账号，观察员不具备复核权限。\n"
        "授权记录会写入审计日志。"));
    hint->setStyleSheet(QStringLiteral("color:#7f8c8d"));
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("取消"));
    auto* approveButton = new QPushButton(QStringLiteral("确认授权"));
    approveButton->setDefault(true);
    buttons->addWidget(cancelButton);
    buttons->addWidget(approveButton);
    root->addLayout(buttons);

    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(approveButton, &QPushButton::clicked, this, &ApprovalDialog::submit);
    connect(passwordEdit_, &QLineEdit::returnPressed, this, &ApprovalDialog::submit);

    userEdit_->setFocus();
}

void ApprovalDialog::submit()
{
    if (auth_ == nullptr) {
        reject();
        return;
    }

    QString error;
    if (auth_->verifyApprover(userEdit_->text(), passwordEdit_->text(), action_,
                              nullptr, &error)) {
        approverName_ = userEdit_->text().trimmed();
        accept();
        return;
    }

    errorLabel_->setText(error);
    passwordEdit_->selectAll();
    passwordEdit_->setFocus();
}

} // namespace aegis::ui
