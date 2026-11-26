#include "security/auth_service.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>

namespace aegis::security {

namespace {

// 口令摘要轮数：单次 SHA256 太容易被离线暴力破解，这里做 1 万轮迭代。
constexpr int kHashRounds = 10000;

// 常量时间比较，降低通过响应时间猜测摘要的可能。
bool constantTimeEquals(const QString& a, const QString& b)
{
    const QByteArray left = a.toUtf8();
    const QByteArray right = b.toUtf8();
    if (left.size() != right.size())
        return false;

    unsigned char diff = 0;
    for (int i = 0; i < left.size(); ++i)
        diff |= static_cast<unsigned char>(left.at(i) ^ right.at(i));
    return diff == 0;
}

} // namespace

AuthService::AuthService(storage::SystemRepository* repository, QObject* parent)
    : QObject(parent)
    , repository_(repository)
{
}

bool AuthService::seedDefaults(QString* errorMessage)
{
    if (repository_ == nullptr || !repository_->isOpen()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开");
        return false;
    }
    if (repository_->userCount() > 0)
        return true;

    struct DefaultUser {
        const char* name;
        const char* password;
        Role role;
    };
    const DefaultUser defaults[] = {
        {"admin", "admin123", Role::Admin},
        {"operator", "operator123", Role::Operator},
        {"viewer", "viewer123", Role::Viewer},
    };

    for (const DefaultUser& item : defaults) {
        const QString salt = makeSalt();
        const QString username = QString::fromLatin1(item.name);
        if (!repository_->insertUser(username, salt,
                                     hashPassword(salt, QString::fromLatin1(item.password)),
                                     roleText(item.role), errorMessage)) {
            return false;
        }
    }
    return true;
}

bool AuthService::login(const QString& username, const QString& password,
                        QString* errorMessage)
{
    const QString name = username.trimmed();
    if (repository_ == nullptr || !repository_->isOpen()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开，无法登录");
        return false;
    }

    // 无论成功失败都留痕：审计里能看出谁在什么时候尝试登录。
    storage::AuditEntry entry;
    entry.username = name.isEmpty() ? QStringLiteral("—") : name;
    entry.action = QStringLiteral("登录");
    entry.target = QStringLiteral("本机");

    storage::UserRow row;
    if (name.isEmpty() || !repository_->findUser(name, &row)) {
        entry.result = QStringLiteral("失败");
        entry.detail = QStringLiteral("用户不存在或口令错误");
        repository_->appendAudit(entry);
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("用户名或密码错误");
        return false;
    }

    if (!constantTimeEquals(hashPassword(row.salt, password), row.hash)) {
        entry.result = QStringLiteral("失败");
        entry.detail = QStringLiteral("用户不存在或口令错误");
        repository_->appendAudit(entry);
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("用户名或密码错误");
        return false;
    }

    // 停用账号：口令正确也不允许登录（管理员在用户管理页恢复）。
    if (!row.enabled) {
        entry.result = QStringLiteral("失败");
        entry.detail = QStringLiteral("账号已停用");
        repository_->appendAudit(entry);
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("账号已停用，请联系管理员");
        return false;
    }

    user_.name = row.username;
    user_.role = roleFromText(row.role);
    loggedIn_ = true;

    entry.result = QStringLiteral("成功");
    entry.detail = roleDisplayName(user_.role);
    repository_->appendAudit(entry);

    emit loggedIn(user_.name);
    return true;
}

bool AuthService::verifyApprover(const QString& username, const QString& password,
                                 const QString& action, UserInfo* approver,
                                 QString* errorMessage)
{
    const QString name = username.trimmed();
    const QString actionName =
        action.isEmpty() ? QStringLiteral("关键操作") : action;

    // 复核记录独立落库：审计里同时能看到复核人、操作人和被复核的动作。
    storage::AuditEntry entry;
    entry.username = name.isEmpty() ? QStringLiteral("—") : name;
    entry.action = QStringLiteral("复核授权");
    entry.target = actionName;

    const auto reject = [&](const QString& detail, const QString& message) {
        entry.result = QStringLiteral("失败");
        entry.detail = detail;
        if (repository_ != nullptr && repository_->isOpen())
            repository_->appendAudit(entry);
        if (errorMessage != nullptr)
            *errorMessage = message;
        return false;
    };

    if (repository_ == nullptr || !repository_->isOpen())
        return reject(QStringLiteral("数据库未打开"),
                      QStringLiteral("数据库未打开，无法复核"));

    if (name.isEmpty())
        return reject(QStringLiteral("复核账号为空"),
                      QStringLiteral("请输入复核人账号"));

    // 4-eyes 的核心约束：复核人不能是当前操作人本人。
    if (loggedIn_ && name.compare(user_.name, Qt::CaseInsensitive) == 0)
        return reject(QStringLiteral("复核人与操作人相同"),
                      QStringLiteral("复核人不能与当前操作人相同"));

    storage::UserRow row;
    if (!repository_->findUser(name, &row))
        return reject(QStringLiteral("用户不存在或口令错误"),
                      QStringLiteral("复核人账号或口令错误"));
    if (!constantTimeEquals(hashPassword(row.salt, password), row.hash))
        return reject(QStringLiteral("用户不存在或口令错误"),
                      QStringLiteral("复核人账号或口令错误"));
    if (!row.enabled)
        return reject(QStringLiteral("账号已停用"),
                      QStringLiteral("复核人账号已停用"));
    const Role role = roleFromText(row.role);
    // 严格模式下复核人必须具备 Approve 权限（质检员/管理员）；否则沿用历史行为，
    // 只要具备操作权限即可复核。
    const Permission required =
        requireQualityApprover_ ? Permission::Approve : Permission::Operate;
    if (!roleAllows(role, required)) {
        return reject(QStringLiteral("权限不足（%1）").arg(roleDisplayName(role)),
                      requireQualityApprover_
                          ? QStringLiteral("复核人必须为质检员或管理员")
                          : QStringLiteral("观察员不具备复核权限"));
    }

    entry.result = QStringLiteral("成功");
    entry.detail = QStringLiteral("%1（%2）复核 %3 的%4")
                       .arg(row.username, roleDisplayName(role),
                            loggedIn_ ? user_.name : QStringLiteral("—"), actionName);
    repository_->appendAudit(entry);

    if (approver != nullptr) {
        approver->name = row.username;
        approver->role = role;
    }
    return true;
}

void AuthService::logout()
{
    if (!loggedIn_)
        return;

    audit(QStringLiteral("登出"), QStringLiteral("本机"), QStringLiteral("成功"));
    loggedIn_ = false;
    user_ = UserInfo();
    emit loggedOut();
}

bool AuthService::can(Permission permission) const
{
    return loggedIn_ && roleAllows(user_.role, permission);
}

void AuthService::audit(const QString& action, const QString& target,
                        const QString& result, const QString& detail)
{
    if (repository_ == nullptr || !repository_->isOpen())
        return;

    storage::AuditEntry entry;
    entry.username = loggedIn_ ? user_.name : QStringLiteral("—");
    entry.action = action;
    entry.target = target;
    entry.result = result;
    entry.detail = detail;
    repository_->appendAudit(entry);
}

Role AuthService::roleFromText(const QString& text)
{
    if (text == QLatin1String("admin"))
        return Role::Admin;
    if (text == QLatin1String("quality"))
        return Role::Quality;
    if (text == QLatin1String("operator"))
        return Role::Operator;
    return Role::Viewer;
}

QString AuthService::roleText(Role role)
{
    switch (role) {
    case Role::Admin:
        return QStringLiteral("admin");
    case Role::Quality:
        return QStringLiteral("quality");
    case Role::Operator:
        return QStringLiteral("operator");
    case Role::Viewer:
        break;
    }
    return QStringLiteral("viewer");
}

QString AuthService::roleDisplayName(Role role)
{
    switch (role) {
    case Role::Admin:
        return QStringLiteral("管理员");
    case Role::Quality:
        return QStringLiteral("质检员");
    case Role::Operator:
        return QStringLiteral("操作员");
    case Role::Viewer:
        break;
    }
    return QStringLiteral("观察员");
}

bool AuthService::roleAllows(Role role, Permission permission)
{
    switch (permission) {
    case Permission::View:
        return true; // 所有角色都能查看
    case Permission::Operate:
        return role == Role::Operator || role == Role::Admin;
    case Permission::Approve:
        // 质检员专责复核；管理员保留兜底能力，便于现场无人时临时授权。
        return role == Role::Quality || role == Role::Admin;
    case Permission::Administer:
        return role == Role::Admin;
    }
    return false;
}

QString AuthService::makeSalt()
{
    // 16 字节随机盐，用十六进制字符串保存，便于直接看库。
    QByteArray bytes(16, 0);
    for (int i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>(QRandomGenerator::system()->generate() & 0xFF);
    return QString::fromLatin1(bytes.toHex());
}

QString AuthService::hashPassword(const QString& salt, const QString& password)
{
    // 迭代 SHA256：digest = H(digest + salt + password)。盐保证相同口令摘要不同。
    QByteArray digest = (salt + password).toUtf8();
    for (int round = 0; round < kHashRounds; ++round) {
        QCryptographicHash hasher(QCryptographicHash::Sha256);
        hasher.addData(digest);
        hasher.addData(salt.toUtf8());
        digest = hasher.result();
    }
    return QString::fromLatin1(digest.toHex());
}

} // namespace aegis::security
