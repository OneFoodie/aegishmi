#pragma once // 头文件保护

#include <QSqlDatabase>
#include <QString>
#include <QVector>

namespace aegis::storage {

// =============================================================================
// 系统表仓储（用户 / 审计 / 报警）
//
// 与 TestRepository（测试记录）分开：职责不同，但共用同一个 aegis.db。
//   app_user    用户账号（口令只存"盐 + SHA256 摘要"，不存明文）
//   audit_log   操作审计：谁在什么时间做了什么，结果如何
//   alarm_event 报警事件：进入报警 / 恢复 / 确认
//
// 每个实例使用独立连接名，避免多实例（主界面 / 单元测试）互相干扰。
// =============================================================================

// 用户表的一行（鉴权时读取）。
struct UserRow {
    QString username;
    QString salt;
    QString hash;
    QString role; // admin / operator / viewer
    bool enabled = true;
    qint64 createdAt = 0;
};

// 一条审计记录。
struct AuditEntry {
    qint64 timestampMs = 0; // 0 表示由仓储填充当前时间
    QString username;
    QString action; // 登录 / 连接设备 / 开始测试 …
    QString target; // 对象：设备名 / 方案名 / 报表文件名
    QString result; // 成功 / 失败 / 已确认 …
    QString detail; // 补充说明
};

// 一条报警事件。
struct AlarmRecord {
    int id = 0;
    QString deviceId;
    QString deviceName;
    int channel = 0;
    QString channelName;
    QString unit;
    QString level; // HIGH / LOW
    double value = 0.0;
    double threshold = 0.0;
    qint64 raisedAtMs = 0;
    qint64 clearedAtMs = 0; // 0 表示仍未恢复
    bool acknowledged = false;
    QString ackUser;
    qint64 ackAtMs = 0;
};

class SystemRepository {
public:
    SystemRepository() = default;
    ~SystemRepository();

    // 打开数据库并建表（首次运行自动创建）。失败时通过 errorMessage 返回原因。
    bool open(const QString& dbPath, QString* errorMessage = nullptr);
    bool isOpen() const { return db_.isOpen(); }

    // ---- 用户 ----
    bool findUser(const QString& username, UserRow* row);
    bool insertUser(const QString& username, const QString& salt, const QString& hash,
                    const QString& role, QString* errorMessage = nullptr);
    int userCount();
    // 全部用户（按用户名排序），供管理员界面展示。
    QVector<UserRow> users();
    bool setUserEnabled(const QString& username, bool enabled,
                        QString* errorMessage = nullptr);
    bool updateUserRole(const QString& username, const QString& role,
                        QString* errorMessage = nullptr);
    bool updateUserPassword(const QString& username, const QString& salt,
                            const QString& hash, QString* errorMessage = nullptr);
    // 启用状态的管理员数量：用于阻止"停用/降级最后一个管理员"。
    int enabledAdminCount();

    // ---- 数据库维护（与审计/用户同一库文件）----
    // 备份到指定路径（VACUUM INTO，自动包含 WAL 中已提交的数据）。
    bool backupTo(const QString& path, QString* errorMessage = nullptr);
    // 压缩数据库（VACUUM），回收删除记录占用的空间。
    bool vacuum(QString* errorMessage = nullptr);

    // ---- 审计 ----
    bool appendAudit(const AuditEntry& entry);
    QVector<AuditEntry> recentAudit(int limit = 500);

    // ---- 报警 ----
    // 保存一条新报警，成功返回自增 id（失败返回 0）。
    int saveAlarm(const AlarmRecord& alarm);
    bool clearAlarm(int alarmId, qint64 clearedAtMs);
    bool acknowledgeAlarm(int alarmId, const QString& user, qint64 ackAtMs);
    // 一键确认所有未确认报警，返回影响行数。
    int acknowledgeAll(const QString& user, qint64 ackAtMs);
    // 最近的报警（onlyActive 为 true 时只返回未恢复的）。
    QVector<AlarmRecord> recentAlarms(int limit = 500, bool onlyActive = false);
    int activeAlarmCount();

private:
    bool ensureSchema(QString* errorMessage);
    bool ensureColumn(const QString& table, const QString& column,
                      const QString& definition, QString* errorMessage);

    QSqlDatabase db_;
    QString connectionName_;
};

} // namespace aegis::storage
