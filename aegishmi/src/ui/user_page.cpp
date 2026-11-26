#include "ui/user_page.h"

#include "security/auth_service.h"
#include "storage/system_repository.h"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace aegis::ui {

namespace {

QTableWidgetItem* makeItem(const QString& text,
                           Qt::Alignment alignment = Qt::AlignCenter)
{
    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(alignment);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

} // namespace

UserPage::UserPage(storage::SystemRepository* repository,
                   security::AuthService* auth, QWidget* parent)
    : QWidget(parent)
    , repository_(repository)
    , auth_(auth)
{
    auto* root = new QVBoxLayout(this);

    auto* toolbar = new QHBoxLayout();
    newButton_ = new QPushButton(QStringLiteral("新建用户"));
    resetButton_ = new QPushButton(QStringLiteral("重置口令"));
    roleButton_ = new QPushButton(QStringLiteral("修改角色"));
    toggleButton_ = new QPushButton(QStringLiteral("启用/停用"));
    toolbar->addWidget(newButton_);
    toolbar->addWidget(resetButton_);
    toolbar->addWidget(roleButton_);
    toolbar->addWidget(toggleButton_);
    toolbar->addStretch(1);

    auto* hint = new QLabel(QStringLiteral(
        "账号停用后无法登录；至少保留一个启用状态的管理员。"));
    hint->setStyleSheet(QStringLiteral("color:#7f8c8d"));
    toolbar->addWidget(hint);
    root->addLayout(toolbar);

    table_ = new QTableWidget(0, 4);
    table_->setHorizontalHeaderLabels({QStringLiteral("用户名"),
                                       QStringLiteral("角色"),
                                       QStringLiteral("状态"),
                                       QStringLiteral("创建时间")});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    root->addWidget(table_, 1);

    connect(newButton_, &QPushButton::clicked, this, &UserPage::createUser);
    connect(resetButton_, &QPushButton::clicked, this, &UserPage::resetPassword);
    connect(roleButton_, &QPushButton::clicked, this, &UserPage::changeRole);
    connect(toggleButton_, &QPushButton::clicked, this, &UserPage::toggleEnabled);
    connect(table_, &QTableWidget::itemSelectionChanged,
            this, &UserPage::onSelectionChanged);

    refresh();
}

void UserPage::refresh()
{
    const QString previous = selectedUsername();

    table_->setRowCount(0);
    if (repository_ == nullptr || !repository_->isOpen())
        return;

    const QVector<storage::UserRow> users = repository_->users();
    table_->setRowCount(users.size());
    for (int row = 0; row < users.size(); ++row) {
        const storage::UserRow& user = users.at(row);
        auto* nameItem = makeItem(user.username, Qt::AlignLeft | Qt::AlignVCenter);
        nameItem->setData(Qt::UserRole, user.username);
        nameItem->setData(Qt::UserRole + 1, user.role);
        nameItem->setData(Qt::UserRole + 2, user.enabled);
        table_->setItem(row, 0, nameItem);

        table_->setItem(row, 1,
                        makeItem(security::AuthService::roleDisplayName(
                                     security::AuthService::roleFromText(user.role))));

        auto* stateItem = makeItem(user.enabled ? QStringLiteral("启用")
                                                : QStringLiteral("停用"));
        stateItem->setForeground(user.enabled ? QColor(0x27, 0xAE, 0x60)
                                              : QColor(0xC0, 0x39, 0x2B));
        table_->setItem(row, 2, stateItem);

        table_->setItem(row, 3,
                        makeItem(user.createdAt > 0
                                     ? QDateTime::fromMSecsSinceEpoch(user.createdAt)
                                           .toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                     : QStringLiteral("—")));

        if (user.username == previous)
            table_->selectRow(row);
    }

    applyControlState();
}

QString UserPage::selectedUsername() const
{
    const auto rows = table_->selectionModel() != nullptr
                          ? table_->selectionModel()->selectedRows()
                          : QModelIndexList();
    if (rows.isEmpty())
        return QString();
    auto* item = table_->item(rows.first().row(), 0);
    return item != nullptr ? item->data(Qt::UserRole).toString() : QString();
}

QString UserPage::selectedRole() const
{
    const auto rows = table_->selectionModel() != nullptr
                          ? table_->selectionModel()->selectedRows()
                          : QModelIndexList();
    if (rows.isEmpty())
        return QString();
    auto* item = table_->item(rows.first().row(), 0);
    return item != nullptr ? item->data(Qt::UserRole + 1).toString() : QString();
}

bool UserPage::selectedEnabled() const
{
    const auto rows = table_->selectionModel() != nullptr
                          ? table_->selectionModel()->selectedRows()
                          : QModelIndexList();
    if (rows.isEmpty())
        return false;
    auto* item = table_->item(rows.first().row(), 0);
    return item != nullptr && item->data(Qt::UserRole + 2).toBool();
}

void UserPage::applyControlState()
{
    const bool hasSelection = !selectedUsername().isEmpty();
    resetButton_->setEnabled(hasSelection);
    roleButton_->setEnabled(hasSelection);
    toggleButton_->setEnabled(hasSelection);
    toggleButton_->setText(hasSelection && selectedEnabled()
                               ? QStringLiteral("停用用户")
                               : QStringLiteral("启用用户"));
    newButton_->setEnabled(repository_ != nullptr && repository_->isOpen());
}

void UserPage::onSelectionChanged()
{
    applyControlState();
}

void UserPage::createUser()
{
    if (repository_ == nullptr || !repository_->isOpen())
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("新建用户"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();

    auto* nameEdit = new QLineEdit();
    nameEdit->setPlaceholderText(QStringLiteral("3~32 位字母/数字/下划线"));
    auto* passwordEdit = new QLineEdit();
    passwordEdit->setEchoMode(QLineEdit::Password);
    passwordEdit->setPlaceholderText(QStringLiteral("至少 6 位"));
    auto* roleCombo = new QComboBox();
    roleCombo->addItem(QStringLiteral("操作员"), QStringLiteral("operator"));
    roleCombo->addItem(QStringLiteral("质检员"), QStringLiteral("quality"));
    roleCombo->addItem(QStringLiteral("观察员"), QStringLiteral("viewer"));
    roleCombo->addItem(QStringLiteral("管理员"), QStringLiteral("admin"));

    form->addRow(QStringLiteral("用户名"), nameEdit);
    form->addRow(QStringLiteral("初始口令"), passwordEdit);
    form->addRow(QStringLiteral("角色"), roleCombo);
    layout->addLayout(form);

    auto* errorLabel = new QLabel();
    errorLabel->setStyleSheet(QStringLiteral("color:#c0392b"));
    layout->addWidget(errorLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("创建"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString username = nameEdit->text().trimmed();
    const QString password = passwordEdit->text();
    const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_.-]{3,32}$"));
    if (!pattern.match(username).hasMatch()) {
        QMessageBox::warning(this, QStringLiteral("新建用户"),
                             QStringLiteral("用户名需为 3~32 位字母、数字、下划线、点或短横线"));
        return;
    }
    if (password.size() < 6) {
        QMessageBox::warning(this, QStringLiteral("新建用户"),
                             QStringLiteral("口令至少 6 位"));
        return;
    }

    storage::UserRow existing;
    if (repository_->findUser(username, &existing)) {
        QMessageBox::warning(this, QStringLiteral("新建用户"),
                             QStringLiteral("用户已存在：%1").arg(username));
        return;
    }

    const QString salt = security::AuthService::makeSalt();
    QString error;
    if (!repository_->insertUser(
            username, salt,
            security::AuthService::hashPassword(salt, password),
            roleCombo->currentData().toString(), &error)) {
        QMessageBox::warning(this, QStringLiteral("新建用户"), error);
        return;
    }

    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("新建用户"), username, QStringLiteral("成功"),
                     QStringLiteral("角色=%1").arg(roleCombo->currentData().toString()));
    }
    refresh();
}

void UserPage::resetPassword()
{
    const QString username = selectedUsername();
    if (username.isEmpty() || repository_ == nullptr)
        return;

    bool ok = false;
    const QString password = QInputDialog::getText(
        this, QStringLiteral("重置口令"),
        QStringLiteral("为 %1 设置新口令（至少 6 位）：").arg(username),
        QLineEdit::Password, QString(), &ok);
    if (!ok)
        return;
    if (password.size() < 6) {
        QMessageBox::warning(this, QStringLiteral("重置口令"),
                             QStringLiteral("口令至少 6 位"));
        return;
    }

    const QString salt = security::AuthService::makeSalt();
    QString error;
    if (!repository_->updateUserPassword(
            username, salt,
            security::AuthService::hashPassword(salt, password), &error)) {
        QMessageBox::warning(this, QStringLiteral("重置口令"), error);
        return;
    }

    if (auth_ != nullptr)
        auth_->audit(QStringLiteral("重置口令"), username, QStringLiteral("成功"));
}

void UserPage::changeRole()
{
    const QString username = selectedUsername();
    if (username.isEmpty() || repository_ == nullptr)
        return;

    if (auth_ != nullptr && username == auth_->currentUser().name) {
        QMessageBox::information(this, QStringLiteral("修改角色"),
                                 QStringLiteral("不能修改自己的角色，请让其他管理员操作"));
        return;
    }

    const QString currentRole = selectedRole();
    const QStringList labels = {QStringLiteral("管理员"), QStringLiteral("操作员"),
                                QStringLiteral("质检员"), QStringLiteral("观察员")};
    const QStringList values = {QStringLiteral("admin"), QStringLiteral("operator"),
                                QStringLiteral("quality"), QStringLiteral("viewer")};
    const int current = values.indexOf(currentRole);

    bool ok = false;
    const QString label = QInputDialog::getItem(
        this, QStringLiteral("修改角色"),
        QStringLiteral("用户 %1 的角色：").arg(username), labels,
        qMax(0, current), false, &ok);
    if (!ok)
        return;

    const QString role = values.at(qMax(0, labels.indexOf(label)));
    if (role == currentRole)
        return;

    // 保护：不能把最后一个启用管理员降级。
    if (currentRole == QLatin1String("admin") && role != QLatin1String("admin")
        && repository_->enabledAdminCount() <= 1) {
        QMessageBox::warning(this, QStringLiteral("修改角色"),
                             QStringLiteral("系统必须保留至少一个启用状态的管理员"));
        return;
    }

    QString error;
    if (!repository_->updateUserRole(username, role, &error)) {
        QMessageBox::warning(this, QStringLiteral("修改角色"), error);
        return;
    }

    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("修改角色"), username, QStringLiteral("成功"),
                     QStringLiteral("%1 → %2").arg(currentRole, role));
    }
    refresh();
}

void UserPage::toggleEnabled()
{
    const QString username = selectedUsername();
    if (username.isEmpty() || repository_ == nullptr)
        return;

    const bool enabled = selectedEnabled();
    if (enabled) {
        if (auth_ != nullptr && username == auth_->currentUser().name) {
            QMessageBox::information(this, QStringLiteral("停用用户"),
                                     QStringLiteral("不能停用当前登录的账号"));
            return;
        }
        if (selectedRole() == QLatin1String("admin")
            && repository_->enabledAdminCount() <= 1) {
            QMessageBox::warning(this, QStringLiteral("停用用户"),
                                 QStringLiteral("系统必须保留至少一个启用状态的管理员"));
            return;
        }
    }

    const QString action = enabled ? QStringLiteral("停用") : QStringLiteral("启用");
    if (QMessageBox::question(this, QStringLiteral("%1用户").arg(action),
                              QStringLiteral("确定%1用户 %2？").arg(action, username))
        != QMessageBox::Yes) {
        return;
    }

    QString error;
    if (!repository_->setUserEnabled(username, !enabled, &error)) {
        QMessageBox::warning(this, QStringLiteral("%1用户").arg(action), error);
        return;
    }

    if (auth_ != nullptr)
        auth_->audit(action + QStringLiteral("用户"), username, QStringLiteral("成功"));
    refresh();
}

} // namespace aegis::ui
