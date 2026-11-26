#include "ui/audit_page.h"

#include "security/auth_service.h"
#include "storage/system_repository.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace aegis::ui {

namespace {

QTableWidgetItem* makeItem(const QString& text, Qt::Alignment alignment = Qt::AlignCenter)
{
    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(alignment);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString csvField(const QString& text)
{
    QString value = text;
    if (value.contains(QLatin1Char(',')) || value.contains(QLatin1Char('"'))
        || value.contains(QLatin1Char('\n'))) {
        value.replace(QLatin1String("\""), QLatin1String("\"\""));
        return QLatin1Char('"') + value + QLatin1Char('"');
    }
    return value;
}

} // namespace

AuditPage::AuditPage(storage::SystemRepository* repository,
                     security::AuthService* auth, QWidget* parent)
    : QWidget(parent)
    , repository_(repository)
    , auth_(auth)
{
    auto* root = new QVBoxLayout(this);

    auto* toolbar = new QHBoxLayout();
    auto* refreshButton = new QPushButton(QStringLiteral("刷新"));
    auto* exportButton = new QPushButton(QStringLiteral("导出 CSV"));
    auto* backupButton = new QPushButton(QStringLiteral("备份数据库…"));
    auto* vacuumButton = new QPushButton(QStringLiteral("压缩数据库"));
    info_ = new QLabel(QStringLiteral("暂无记录"));
    toolbar->addWidget(refreshButton);
    toolbar->addWidget(exportButton);
    toolbar->addWidget(backupButton);
    toolbar->addWidget(vacuumButton);
    toolbar->addWidget(info_, 1);

    table_ = new QTableWidget(0, 6);
    table_->setHorizontalHeaderLabels({QStringLiteral("时间"),
                                       QStringLiteral("用户"),
                                       QStringLiteral("动作"),
                                       QStringLiteral("对象"),
                                       QStringLiteral("结果"),
                                       QStringLiteral("详情")});
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setAlternatingRowColors(true);

    root->addLayout(toolbar);
    root->addWidget(table_, 1);

    connect(refreshButton, &QPushButton::clicked, this, &AuditPage::refresh);
    connect(exportButton, &QPushButton::clicked, this, &AuditPage::exportCsv);
    connect(backupButton, &QPushButton::clicked, this, &AuditPage::backupDatabase);
    connect(vacuumButton, &QPushButton::clicked, this, &AuditPage::vacuumDatabase);
    refresh();
}

void AuditPage::refresh()
{
    if (repository_ == nullptr || !repository_->isOpen()) {
        table_->setRowCount(0);
        info_->setText(QStringLiteral("数据库未打开"));
        return;
    }

    const auto entries = repository_->recentAudit(500);
    table_->setRowCount(entries.size());

    for (int row = 0; row < entries.size(); ++row) {
        const auto& entry = entries.at(row);
        table_->setItem(row, 0, makeItem(QDateTime::fromMSecsSinceEpoch(entry.timestampMs)
                                             .toString(QStringLiteral("MM-dd HH:mm:ss"))));
        table_->setItem(row, 1, makeItem(entry.username));
        table_->setItem(row, 2, makeItem(entry.action));

        auto* resultItem = makeItem(entry.result);
        if (entry.result == QStringLiteral("失败"))
            resultItem->setForeground(QColor(0xC0, 0x39, 0x2B));
        else if (entry.result == QStringLiteral("成功"))
            resultItem->setForeground(QColor(0x27, 0xAE, 0x60));

        table_->setItem(row, 3, makeItem(entry.target, Qt::AlignLeft | Qt::AlignVCenter));
        table_->setItem(row, 4, resultItem);
        table_->setItem(row, 5, makeItem(entry.detail, Qt::AlignLeft | Qt::AlignVCenter));
    }

    info_->setText(entries.isEmpty() ? QStringLiteral("暂无记录")
                                     : QStringLiteral("最近 %1 条记录").arg(entries.size()));
}

void AuditPage::exportCsv()
{
    if (repository_ == nullptr || !repository_->isOpen())
        return;

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出审计日志"),
        QStringLiteral("audit_%1.csv")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))),
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty())
        return;

    const QVector<storage::AuditEntry> entries = repository_->recentAudit(500);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        info_->setText(QStringLiteral("导出失败：%1").arg(file.errorString()));
        if (auth_ != nullptr) {
            auth_->audit(QStringLiteral("导出审计"), path, QStringLiteral("失败"),
                         file.errorString());
        }
        return;
    }

    // UTF-8 BOM：Excel 直接打开中文不乱码。
    file.write("\xEF\xBB\xBF");

    auto writeLine = [&file](const QStringList& fields) {
        QStringList escaped;
        escaped.reserve(fields.size());
        for (const QString& field : fields)
            escaped << csvField(field);
        file.write((escaped.join(QLatin1Char(',')) + QLatin1String("\r\n")).toUtf8());
    };

    writeLine({QStringLiteral("时间"), QStringLiteral("用户"), QStringLiteral("动作"),
               QStringLiteral("对象"), QStringLiteral("结果"), QStringLiteral("详情")});
    for (const storage::AuditEntry& entry : entries) {
        writeLine({QDateTime::fromMSecsSinceEpoch(entry.timestampMs)
                       .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                   entry.username, entry.action, entry.target, entry.result, entry.detail});
    }
    file.close();

    info_->setText(QStringLiteral("已导出 %1 条审计记录：%2").arg(entries.size()).arg(path));
    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("导出审计"), path, QStringLiteral("成功"),
                     QStringLiteral("共 %1 条").arg(entries.size()));
    }
}

// 数据库备份：VACUUM INTO 生成一致副本（含 WAL 中已提交的数据）。
void AuditPage::backupDatabase()
{
    if (repository_ == nullptr || !repository_->isOpen())
        return;

    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                  + QStringLiteral("/backups");
    QDir().mkpath(dir);
    const QString defaultPath =
        dir + QStringLiteral("/aegis_backup_%1.db")
                  .arg(QDateTime::currentDateTime()
                           .toString(QStringLiteral("yyyyMMdd_HHmmss")));

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("备份数据库"), defaultPath,
        QStringLiteral("SQLite 数据库 (*.db)"));
    if (path.isEmpty())
        return;

    QString error;
    if (!repository_->backupTo(path, &error)) {
        info_->setText(QStringLiteral("备份失败：%1").arg(error));
        if (auth_ != nullptr)
            auth_->audit(QStringLiteral("备份数据库"), path, QStringLiteral("失败"), error);
        return;
    }

    info_->setText(QStringLiteral("已备份：%1").arg(path));
    if (auth_ != nullptr)
        auth_->audit(QStringLiteral("备份数据库"), path, QStringLiteral("成功"));
}

// 压缩数据库：回收历史删除占用的空间，操作期间短暂锁库。
void AuditPage::vacuumDatabase()
{
    if (repository_ == nullptr || !repository_->isOpen())
        return;

    if (QMessageBox::question(this, QStringLiteral("压缩数据库"),
                              QStringLiteral("压缩期间会短暂锁定数据库，确定继续？"))
        != QMessageBox::Yes) {
        return;
    }

    QString error;
    if (!repository_->vacuum(&error)) {
        info_->setText(QStringLiteral("压缩失败：%1").arg(error));
        if (auth_ != nullptr)
            auth_->audit(QStringLiteral("压缩数据库"), QStringLiteral("本地库"),
                         QStringLiteral("失败"), error);
        return;
    }

    info_->setText(QStringLiteral("数据库已压缩"));
    if (auth_ != nullptr)
        auth_->audit(QStringLiteral("压缩数据库"), QStringLiteral("本地库"),
                     QStringLiteral("成功"));
}

} // namespace aegis::ui
