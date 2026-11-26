#include "ui/plugin_page.h"

#include "security/auth_service.h"
#include "ui/theme.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

namespace aegis::ui {

namespace {
constexpr int kColumnCount = 6;
} // namespace

PluginPage::PluginPage(const core::StationConfig& station, security::AuthService* auth,
                       QWidget* parent)
    : QWidget(parent)
    , station_(station)
    , auth_(auth)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    // 顶部说明：告诉使用者插件是什么、在哪里、如何复用。
    auto* banner = new QFrame();
    banner->setProperty("role", QStringLiteral("card"));
    auto* bannerLayout = new QVBoxLayout(banner);
    bannerLayout->setContentsMargins(14, 10, 14, 10);
    bannerLayout->setSpacing(2);
    auto* bannerTitle = new QLabel(QStringLiteral("用户插件（独立进程 + JSON stdio）"));
    bannerTitle->setStyleSheet(QStringLiteral("font-weight:600;color:#111827;"));
    bannerLayout->addWidget(bannerTitle);
    bannerLayout->addWidget(makeHint(QStringLiteral(
        "每个插件是一个目录（plugins/<name>/manifest.json）。插件用任意语言实现，"
        "崩溃/超时不会影响主程序；插件目录可整包复制到其它工位复用。")));
    root->addWidget(banner);

    auto* toolbar = new QHBoxLayout();
    refreshButton_ = makeButton(QStringLiteral("刷新"));
    refreshButton_->setToolTip(QStringLiteral("重新扫描 plugins 目录下的 manifest.json"));
    testButton_ = makeButton(QStringLiteral("自检选中插件"), QStringLiteral("primary"));
    testButton_->setToolTip(QStringLiteral(
        "用示例上下文调用一次插件，验证入口可启动、JSON 输出可解析"));
    openButton_ = makeButton(QStringLiteral("打开插件目录"));
    openButton_->setToolTip(QStringLiteral("在文件管理器中打开，便于复制/新增插件"));
    summary_ = new QLabel(QStringLiteral("尚未扫描"));
    summary_->setProperty("role", QStringLiteral("hint"));
    summary_->setWordWrap(true);

    toolbar->addWidget(refreshButton_);
    toolbar->addWidget(testButton_);
    toolbar->addWidget(openButton_);
    toolbar->addWidget(summary_, 1);
    root->addLayout(toolbar);

    table_ = new QTableWidget(0, kColumnCount);
    table_->setHorizontalHeaderLabels({QStringLiteral("插件"),
                                       QStringLiteral("版本"),
                                       QStringLiteral("API"),
                                       QStringLiteral("能力"),
                                       QStringLiteral("状态"),
                                       QStringLiteral("说明")});
    table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setToolTip(QStringLiteral("插件清单扫描结果；状态列会标出不可用原因"));
    root->addWidget(table_, 1);

    details_ = new QPlainTextEdit();
    details_->setReadOnly(true);
    details_->setMinimumHeight(120);
    details_->setProperty("role", QStringLiteral("log"));
    details_->setPlaceholderText(QStringLiteral("选中插件后显示清单元数据与使用说明"));
    root->addWidget(details_);

    connect(refreshButton_, &QPushButton::clicked, this, &PluginPage::refresh);
    connect(testButton_, &QPushButton::clicked, this, &PluginPage::runSelfTest);
    connect(openButton_, &QPushButton::clicked, this, &PluginPage::openPluginsDirectory);
    connect(table_, &QTableWidget::itemSelectionChanged,
            this, &PluginPage::onSelectionChanged);

    refresh();
}

QString PluginPage::pluginsRoot() const
{
    return test::PluginRunner::resolveRoot(station_.pluginsDir);
}

bool PluginPage::whitelisted(const QString& name) const
{
    // station.json 未配置白名单时（空）视为全部允许，与流程校验器保持一致。
    return station_.plugins.isEmpty() || station_.plugins.contains(name);
}

void PluginPage::refresh()
{
    plugins_ = test::PluginRunner::listPlugins(pluginsRoot());

    table_->setRowCount(plugins_.size());
    int available = 0;
    for (int row = 0; row < plugins_.size(); ++row) {
        const test::PluginInfo& plugin = plugins_.at(row);
        const bool enabled = whitelisted(plugin.name);
        if (plugin.valid && enabled)
            ++available;

        auto makeItem = [](const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            return item;
        };

        table_->setItem(row, 0, makeItem(plugin.name));
        table_->setItem(row, 1, makeItem(plugin.version));
        table_->setItem(row, 2, makeItem(QString::number(plugin.apiVersion)));
        table_->setItem(row, 3, makeItem(plugin.capabilities.isEmpty()
                                             ? QStringLiteral("—")
                                             : plugin.capabilities.join(QStringLiteral(", "))));

        QString status;
        QColor color(QStringLiteral("#2e7d5b"));
        if (!plugin.valid) {
            status = QStringLiteral("不可用");
            color = QColor(QStringLiteral("#b3261e"));
        } else if (!enabled) {
            status = QStringLiteral("未启用（白名单）");
            color = QColor(QStringLiteral("#8a5300"));
        } else {
            status = QStringLiteral("可用");
        }
        auto* statusItem = makeItem(status);
        statusItem->setForeground(color);
        statusItem->setToolTip(plugin.error);
        table_->setItem(row, 4, statusItem);
        table_->setItem(row, 5, makeItem(plugin.description));
    }

    summary_->setText(QStringLiteral("目录：%1　共 %2 个插件，可用 %3 个")
                          .arg(QDir::toNativeSeparators(pluginsRoot()))
                          .arg(plugins_.size())
                          .arg(available));

    if (table_->rowCount() > 0) {
        table_->selectRow(0);
        showDetails(0);
    } else {
        details_->setPlainText(QStringLiteral(
            "未找到插件。\n\n"
            "新增方式：在 plugins/ 下新建目录，放入 manifest.json 与插件程序，"
            "然后点“刷新”。清单格式见 plugins/README.md。"));
    }
    testButton_->setEnabled(operationAllowed_ && !plugins_.isEmpty());
}

void PluginPage::setOperationAllowed(bool allowed)
{
    operationAllowed_ = allowed;
    testButton_->setEnabled(allowed && !plugins_.isEmpty());
    testButton_->setToolTip(allowed
                                ? QStringLiteral("用示例上下文调用一次插件，验证入口与 JSON 输出")
                                : QStringLiteral("观察员仅可查看，不能执行插件自检"));
}

void PluginPage::onSelectionChanged()
{
    const int row = table_->currentRow();
    showDetails(row);
}

void PluginPage::showDetails(int row)
{
    if (row < 0 || row >= plugins_.size()) {
        details_->clear();
        return;
    }

    const test::PluginInfo& plugin = plugins_.at(row);
    QStringList lines;
    lines << QStringLiteral("插件：%1").arg(plugin.name)
          << QStringLiteral("版本：%1（API %2）").arg(plugin.version).arg(plugin.apiVersion)
          << QStringLiteral("能力：%1")
                 .arg(plugin.capabilities.isEmpty() ? QStringLiteral("—")
                                                    : plugin.capabilities.join(", "))
          << QStringLiteral("超时：%1 ms").arg(plugin.timeoutMs)
          << QStringLiteral("状态：%1%2")
                 .arg(plugin.valid ? QStringLiteral("清单可用")
                                   : QStringLiteral("清单不可用"))
                 .arg(plugin.valid ? QString() : QStringLiteral("（%1）").arg(plugin.error))
          << QStringLiteral("白名单：%1")
                 .arg(whitelisted(plugin.name) ? QStringLiteral("已启用")
                                               : QStringLiteral("未在 station.json plugins.enabled"))
          << QStringLiteral("目录：%1")
                 .arg(QDir::toNativeSeparators(QDir(pluginsRoot()).filePath(plugin.name)))
          << QString()
          << QStringLiteral("说明：%1")
                 .arg(plugin.description.isEmpty() ? QStringLiteral("—") : plugin.description)
          << QStringLiteral("调用协议：stdin 一行 JSON → stdout 一行 JSON，"
                            "响应字段 pass/message/measures。");
    details_->setPlainText(lines.join(QLatin1Char('\n')));
}

void PluginPage::runSelfTest()
{
    if (!operationAllowed_)
        return;

    const int row = table_->currentRow();
    if (row < 0 || row >= plugins_.size()) {
        QMessageBox::information(this, QStringLiteral("插件自检"), QStringLiteral("请先选择一个插件"));
        return;
    }

    const test::PluginInfo& plugin = plugins_.at(row);
    if (!plugin.valid) {
        QMessageBox::warning(this, QStringLiteral("插件自检"),
                             QStringLiteral("插件清单不可用：%1").arg(plugin.error));
        return;
    }
    if (!whitelisted(plugin.name)) {
        QMessageBox::warning(
            this, QStringLiteral("插件自检"),
            QStringLiteral("插件未在 configs/station.json 的 plugins.enabled 白名单中，"
                           "流程里也不会被允许调用。"));
        return;
    }

    // 自检使用示例上下文，只验证协议连通，不代表真实业务判定。
    QJsonObject context;
    context.insert(QStringLiteral("sn"), QStringLiteral("SELFTEST"));
    context.insert(QStringLiteral("product"), QStringLiteral("SELFTEST"));
    context.insert(QStringLiteral("source"), QStringLiteral("plugin-page"));
    context.insert(QStringLiteral("measures"), QJsonObject{{QStringLiteral("channel0"), 5000}});

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const test::PluginResult result =
        test::PluginRunner::run(pluginsRoot(), plugin.name, context,
                                qMin(plugin.timeoutMs, 3000));
    QApplication::restoreOverrideCursor();

    const bool ok = result.ok;
    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("插件自检"), plugin.name,
                     ok ? QStringLiteral("成功") : QStringLiteral("失败"),
                     ok ? result.message : result.error);
    }

    QStringList lines;
    lines << QStringLiteral("自检结果：%1").arg(ok ? QStringLiteral("调用成功")
                                                   : QStringLiteral("调用失败"))
          << QStringLiteral("插件：%1").arg(plugin.name);
    if (ok) {
        lines << QStringLiteral("判定：%1").arg(result.pass ? QStringLiteral("PASS")
                                                           : QStringLiteral("FAIL"))
              << QStringLiteral("说明：%1").arg(result.message);
        if (!result.measures.isEmpty())
            lines << QStringLiteral("实测：%1").arg(QString::fromUtf8(
                      QJsonDocument(result.measures).toJson(QJsonDocument::Compact)));
    } else {
        lines << QStringLiteral("原因：%1").arg(result.error);
    }
    if (!result.log.isEmpty())
        lines << QStringLiteral("日志：%1").arg(result.log);

    details_->setPlainText(lines.join(QLatin1Char('\n')));
    QMessageBox::information(this, QStringLiteral("插件自检"),
                             lines.join(QLatin1Char('\n')));
}

void PluginPage::openPluginsDirectory()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(pluginsRoot()));
}

} // namespace aegis::ui
