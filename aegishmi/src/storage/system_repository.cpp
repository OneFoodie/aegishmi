#include "storage/system_repository.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace aegis::storage {

SystemRepository::~SystemRepository()
{
    // 先关闭数据库再移除连接，避免 Qt 输出 "connection still in use" 警告。
    if (db_.isValid()) {
        db_.close();
        db_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName_);
    }
}

bool SystemRepository::open(const QString& dbPath, QString* errorMessage)
{
    if (db_.isValid()) {
        db_.close();
        db_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName_);
    }

    // 每实例一个连接名：主界面与单元测试可以同时打开同一个文件。
    connectionName_ = QStringLiteral("aegis_system_%1")
                          .arg(reinterpret_cast<quintptr>(this));

    db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    db_.setDatabaseName(dbPath);

    if (!db_.open()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("打开数据库失败：%1").arg(db_.lastError().text());
        return false;
    }

    // 与 TestRepository 相同的连接参数：WAL + 等待锁 + 外键。
    QSqlQuery pragma(db_);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=3000"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));

    return ensureSchema(errorMessage);
}

bool SystemRepository::ensureSchema(QString* errorMessage)
{
    QSqlQuery query(db_);

    // 用户表：口令存盐与摘要，不存明文。
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS app_user ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " username TEXT NOT NULL UNIQUE,"
            " salt TEXT NOT NULL,"
            " hash TEXT NOT NULL,"
            " role TEXT NOT NULL,"
            " enabled INTEGER NOT NULL DEFAULT 1,"
            " created_at INTEGER NOT NULL)"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("建表 app_user 失败：%1").arg(query.lastError().text());
        return false;
    }

    // 旧库兼容：新增"启用状态"列，默认启用。
    if (!ensureColumn(QStringLiteral("app_user"), QStringLiteral("enabled"),
                      QStringLiteral("INTEGER NOT NULL DEFAULT 1"), errorMessage)) {
        return false;
    }

    // 审计表：只追加，不做修改/删除。
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS audit_log ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " ts INTEGER NOT NULL,"
            " username TEXT NOT NULL,"
            " action TEXT NOT NULL,"
            " target TEXT,"
            " result TEXT,"
            " detail TEXT)"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("建表 audit_log 失败：%1").arg(query.lastError().text());
        return false;
    }

    if (!query.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit_log(ts)"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("创建审计时间索引失败：%1")
                                .arg(query.lastError().text());
        return false;
    }

    // 报警表：一条记录代表一次"进入报警 → 恢复"的完整过程。
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS alarm_event ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " device_id TEXT NOT NULL,"
            " device_name TEXT,"
            " channel INTEGER NOT NULL,"
            " channel_name TEXT,"
            " unit TEXT,"
            " level TEXT NOT NULL,"
            " value REAL NOT NULL,"
            " threshold REAL NOT NULL,"
            " raised_at INTEGER NOT NULL,"
            " cleared_at INTEGER,"
            " acknowledged INTEGER NOT NULL DEFAULT 0,"
            " ack_user TEXT,"
            " ack_at INTEGER)"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("建表 alarm_event 失败：%1")
                                .arg(query.lastError().text());
        return false;
    }

    if (!query.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_alarm_raised ON alarm_event(raised_at)"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("创建报警时间索引失败：%1")
                                .arg(query.lastError().text());
        return false;
    }

    return true;
}

// =============================================================================
// 用户
// =============================================================================
bool SystemRepository::ensureColumn(const QString& table, const QString& column,
                                    const QString& definition, QString* errorMessage)
{
    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("读取表结构失败（%1）：%2")
                                .arg(table, query.lastError().text());
        return false;
    }

    while (query.next()) {
        if (query.value(1).toString() == column)
            return true;
    }

    QSqlQuery alter(db_);
    if (!alter.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3")
                        .arg(table, column, definition))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("为 %1 增加列 %2 失败：%3")
                                .arg(table, column, alter.lastError().text());
        return false;
    }
    return true;
}

bool SystemRepository::findUser(const QString& username, UserRow* row)
{
    if (!db_.isOpen() || row == nullptr)
        return false;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT username, salt, hash, role, enabled, created_at"
        " FROM app_user WHERE username = ?"));
    query.addBindValue(username);
    if (!query.exec() || !query.next())
        return false;

    row->username = query.value(0).toString();
    row->salt = query.value(1).toString();
    row->hash = query.value(2).toString();
    row->role = query.value(3).toString();
    row->enabled = query.value(4).toInt() != 0;
    row->createdAt = query.value(5).toLongLong();
    return true;
}

bool SystemRepository::insertUser(const QString& username, const QString& salt,
                                  const QString& hash, const QString& role,
                                  QString* errorMessage)
{
    if (!db_.isOpen()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开");
        return false;
    }

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT INTO app_user(username, salt, hash, role, created_at)"
        " VALUES(?, ?, ?, ?, ?)"));
    query.addBindValue(username);
    query.addBindValue(salt);
    query.addBindValue(hash);
    query.addBindValue(role);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());

    if (!query.exec()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("写入用户失败：%1").arg(query.lastError().text());
        return false;
    }
    return true;
}

int SystemRepository::userCount()
{
    if (!db_.isOpen())
        return 0;

    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM app_user")) || !query.next())
        return 0;
    return query.value(0).toInt();
}

QVector<UserRow> SystemRepository::users()
{
    QVector<UserRow> rows;
    if (!db_.isOpen())
        return rows;

    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral(
            "SELECT username, role, enabled, created_at FROM app_user"
            " ORDER BY username ASC"))) {
        return rows;
    }

    while (query.next()) {
        UserRow row;
        row.username = query.value(0).toString();
        row.role = query.value(1).toString();
        row.enabled = query.value(2).toInt() != 0;
        row.createdAt = query.value(3).toLongLong();
        rows.append(row);
    }
    return rows;
}

bool SystemRepository::setUserEnabled(const QString& username, bool enabled,
                                      QString* errorMessage)
{
    if (!db_.isOpen()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开");
        return false;
    }

    QSqlQuery query(db_);
    query.prepare(QStringLiteral("UPDATE app_user SET enabled = ? WHERE username = ?"));
    query.addBindValue(enabled ? 1 : 0);
    query.addBindValue(username.trimmed());
    if (!query.exec()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("更新用户状态失败：%1").arg(query.lastError().text());
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool SystemRepository::updateUserRole(const QString& username, const QString& role,
                                      QString* errorMessage)
{
    if (!db_.isOpen()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开");
        return false;
    }

    QSqlQuery query(db_);
    query.prepare(QStringLiteral("UPDATE app_user SET role = ? WHERE username = ?"));
    query.addBindValue(role);
    query.addBindValue(username.trimmed());
    if (!query.exec()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("更新用户角色失败：%1").arg(query.lastError().text());
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool SystemRepository::updateUserPassword(const QString& username, const QString& salt,
                                          const QString& hash, QString* errorMessage)
{
    if (!db_.isOpen()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开");
        return false;
    }

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "UPDATE app_user SET salt = ?, hash = ? WHERE username = ?"));
    query.addBindValue(salt);
    query.addBindValue(hash);
    query.addBindValue(username.trimmed());
    if (!query.exec()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("重置口令失败：%1").arg(query.lastError().text());
        return false;
    }
    return query.numRowsAffected() > 0;
}

int SystemRepository::enabledAdminCount()
{
    if (!db_.isOpen())
        return 0;

    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral(
            "SELECT COUNT(*) FROM app_user WHERE role = 'admin' AND enabled = 1"))
        || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

bool SystemRepository::backupTo(const QString& path, QString* errorMessage)
{
    if (!db_.isOpen() || path.trimmed().isEmpty()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开或备份路径为空");
        return false;
    }

    // VACUUM INTO：生成一个一致的副本，且包含 WAL 中已提交的数据。
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("VACUUM INTO ?"));
    query.addBindValue(path);
    if (!query.exec()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("备份失败：%1").arg(query.lastError().text());
        return false;
    }
    return true;
}

bool SystemRepository::vacuum(QString* errorMessage)
{
    if (!db_.isOpen())
        return false;

    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral("VACUUM"))) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("压缩失败：%1").arg(query.lastError().text());
        return false;
    }
    return true;
}

// =============================================================================
// 审计
// =============================================================================
bool SystemRepository::appendAudit(const AuditEntry& entry)
{
    if (!db_.isOpen())
        return false;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT INTO audit_log(ts, username, action, target, result, detail)"
        " VALUES(?, ?, ?, ?, ?, ?)"));
    // 允许调用方不填时间（例如鉴权服务），统一在这里补当前时间。
    query.addBindValue(entry.timestampMs > 0 ? entry.timestampMs
                                             : QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(entry.username);
    query.addBindValue(entry.action);
    query.addBindValue(entry.target);
    query.addBindValue(entry.result);
    query.addBindValue(entry.detail);
    return query.exec();
}

QVector<AuditEntry> SystemRepository::recentAudit(int limit)
{
    QVector<AuditEntry> entries;
    if (!db_.isOpen())
        return entries;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "SELECT ts, username, action, target, result, detail"
        " FROM audit_log ORDER BY ts DESC, id DESC LIMIT ?"));
    query.addBindValue(limit);
    if (!query.exec())
        return entries;

    while (query.next()) {
        AuditEntry entry;
        entry.timestampMs = query.value(0).toLongLong();
        entry.username = query.value(1).toString();
        entry.action = query.value(2).toString();
        entry.target = query.value(3).toString();
        entry.result = query.value(4).toString();
        entry.detail = query.value(5).toString();
        entries.append(entry);
    }
    return entries;
}

// =============================================================================
// 报警
// =============================================================================
int SystemRepository::saveAlarm(const AlarmRecord& alarm)
{
    if (!db_.isOpen())
        return 0;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT INTO alarm_event(device_id, device_name, channel, channel_name, unit,"
        " level, value, threshold, raised_at, cleared_at, acknowledged)"
        " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0)"));
    query.addBindValue(alarm.deviceId);
    query.addBindValue(alarm.deviceName);
    query.addBindValue(alarm.channel);
    query.addBindValue(alarm.channelName);
    query.addBindValue(alarm.unit);
    query.addBindValue(alarm.level);
    query.addBindValue(alarm.value);
    query.addBindValue(alarm.threshold);
    query.addBindValue(alarm.raisedAtMs > 0 ? alarm.raisedAtMs
                                            : QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(alarm.clearedAtMs > 0 ? QVariant(alarm.clearedAtMs) : QVariant());

    if (!query.exec())
        return 0;
    return query.lastInsertId().toInt();
}

bool SystemRepository::clearAlarm(int alarmId, qint64 clearedAtMs)
{
    if (!db_.isOpen() || alarmId <= 0)
        return false;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "UPDATE alarm_event SET cleared_at = ? WHERE id = ? AND cleared_at IS NULL"));
    query.addBindValue(clearedAtMs);
    query.addBindValue(alarmId);
    return query.exec() && query.numRowsAffected() > 0;
}

bool SystemRepository::acknowledgeAlarm(int alarmId, const QString& user, qint64 ackAtMs)
{
    if (!db_.isOpen() || alarmId <= 0)
        return false;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "UPDATE alarm_event SET acknowledged = 1, ack_user = ?, ack_at = ?"
        " WHERE id = ? AND acknowledged = 0"));
    query.addBindValue(user);
    query.addBindValue(ackAtMs);
    query.addBindValue(alarmId);
    return query.exec() && query.numRowsAffected() > 0;
}

int SystemRepository::acknowledgeAll(const QString& user, qint64 ackAtMs)
{
    if (!db_.isOpen())
        return 0;

    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "UPDATE alarm_event SET acknowledged = 1, ack_user = ?, ack_at = ?"
        " WHERE acknowledged = 0"));
    query.addBindValue(user);
    query.addBindValue(ackAtMs);
    if (!query.exec())
        return 0;
    return query.numRowsAffected();
}

QVector<AlarmRecord> SystemRepository::recentAlarms(int limit, bool onlyActive)
{
    QVector<AlarmRecord> alarms;
    if (!db_.isOpen())
        return alarms;

    QString sql = QStringLiteral(
        "SELECT id, device_id, device_name, channel, channel_name, unit, level,"
        " value, threshold, raised_at, cleared_at, acknowledged, ack_user, ack_at"
        " FROM alarm_event");
    if (onlyActive)
        sql += QStringLiteral(" WHERE cleared_at IS NULL");
    // 未恢复的排前面，其余按时间倒序。
    sql += QStringLiteral(" ORDER BY (cleared_at IS NULL) DESC, raised_at DESC, id DESC LIMIT ?");

    QSqlQuery query(db_);
    query.prepare(sql);
    query.addBindValue(limit);
    if (!query.exec())
        return alarms;

    while (query.next()) {
        AlarmRecord alarm;
        alarm.id = query.value(0).toInt();
        alarm.deviceId = query.value(1).toString();
        alarm.deviceName = query.value(2).toString();
        alarm.channel = query.value(3).toInt();
        alarm.channelName = query.value(4).toString();
        alarm.unit = query.value(5).toString();
        alarm.level = query.value(6).toString();
        alarm.value = query.value(7).toDouble();
        alarm.threshold = query.value(8).toDouble();
        alarm.raisedAtMs = query.value(9).toLongLong();
        alarm.clearedAtMs = query.value(10).isNull() ? 0 : query.value(10).toLongLong();
        alarm.acknowledged = query.value(11).toInt() != 0;
        alarm.ackUser = query.value(12).toString();
        alarm.ackAtMs = query.value(13).isNull() ? 0 : query.value(13).toLongLong();
        alarms.append(alarm);
    }
    return alarms;
}

int SystemRepository::activeAlarmCount()
{
    if (!db_.isOpen())
        return 0;

    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral(
            "SELECT COUNT(*) FROM alarm_event WHERE cleared_at IS NULL"))
        || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

} // namespace aegis::storage
