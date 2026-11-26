#include "ui/login_dialog.h"

#include "security/auth_service.h"
#include "ui/theme.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace aegis::ui {

LoginDialog::LoginDialog(security::AuthService* auth, QWidget* parent)
    : QDialog(parent)
    , auth_(auth)
{
    setWindowTitle(QStringLiteral("AegisHMI 登录"));
    setModal(true);
    setMinimumWidth(360);

    auto* root = new QVBoxLayout(this);

    auto* title = new QLabel(QStringLiteral("工业设备自动测试上位机"));
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 3);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignCenter);
    root->addWidget(title);

    auto* form = new QFormLayout();
    userEdit_ = new QLineEdit(QStringLiteral("admin"));
    userEdit_->setToolTip(QStringLiteral("账号由管理员在“用户”页维护"));
    passwordEdit_ = new QLineEdit();
    passwordEdit_->setEchoMode(QLineEdit::Password);
    passwordEdit_->setPlaceholderText(QStringLiteral("请输入密码"));
    passwordEdit_->setToolTip(QStringLiteral("输入口令后按回车直接登录"));
    form->addRow(QStringLiteral("用户名"), userEdit_);
    form->addRow(QStringLiteral("密码"), passwordEdit_);
    root->addLayout(form);

    errorLabel_ = new QLabel();
    errorLabel_->setStyleSheet(QStringLiteral("color:#a3271f"));
    errorLabel_->setWordWrap(true);
    root->addWidget(errorLabel_);

    root->addWidget(makeHint(QStringLiteral(
        "默认账号：admin / admin123（管理员）\n"
        "operator / operator123（操作员）　viewer / viewer123（观察员）\n"
        "观察员只读；操作员可连接设备与执行测试；管理员额外可见审计与用户管理。")));

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* cancelButton = makeButton(QStringLiteral("退出"));
    auto* loginButton = makeButton(QStringLiteral("登录"), QStringLiteral("primary"));
    loginButton->setDefault(true);
    buttons->addWidget(cancelButton);
    buttons->addWidget(loginButton);
    root->addLayout(buttons);

    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(loginButton, &QPushButton::clicked, this, &LoginDialog::tryLogin);
    // 回车直接登录，符合日常使用习惯。
    connect(passwordEdit_, &QLineEdit::returnPressed, this, &LoginDialog::tryLogin);

    passwordEdit_->setFocus();
}

void LoginDialog::tryLogin()
{
    if (auth_ == nullptr) {
        reject();
        return;
    }

    QString error;
    if (auth_->login(userEdit_->text(), passwordEdit_->text(), &error)) {
        accept();
        return;
    }

    errorLabel_->setText(error);
    passwordEdit_->selectAll();
    passwordEdit_->setFocus();
}

} // namespace aegis::ui
