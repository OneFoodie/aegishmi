#include "ui/main_window.h"

#include "security/auth_service.h"   // 当前用户与审计
#include "service/alarm_monitor.h"   // 阈值报警
#include "service/device_manager.h"  // 多设备管理
#include "service/device_service.h"  // DeviceStats / ChunkRingBuffer 的完整定义
#include "service/mes_uploader.h"    // MES 上传
#include "service/update_checker.h"  // 轻量版本检测（SY-05）
#include "storage/summary_writer.h"  // 摘要异步写入
#include "storage/system_repository.h" // 审计/报警存储
#include "storage/test_repository.h" // SQLite 仓储
#include "ui/alarm_page.h"           // 报警页
#include "ui/audit_page.h"           // 审计页
#include "ui/dashboard_page.h"       // 生产看板
#include "ui/help_dialog.h"           // 快速上手指南
#include "ui/history_page.h"         // 历史记录页
#include "ui/login_dialog.h"         // 切换用户
#include "ui/shift_page.h"           // 班组与交接班
#include "ui/spc_page.h"             // SPC 控制图
#include "ui/test_page.h"            // 自动测试页
#include "ui/trend_page.h"           // 趋势页
#include "ui/theme.h"             // 全局主题与提示标签
#include "ui/user_page.h"            // 用户管理
#include "ui/waveform_view.h"        // 自绘波形控件

#include <QAbstractAnimation>
#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDebug> // qInfo：把界面日志同步写文件
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace aegis::ui {

namespace {

// 波形横轴窗口长度（秒）。
constexpr double kWindowSeconds = 3.0;

} // namespace

// =============================================================================
// 构造函数：创建服务 -> 组装界面 -> 连接信号 -> 启动定时器
// =============================================================================
MainWindow::MainWindow(security::AuthService* auth,
                       storage::SystemRepository* systemRepository, QWidget* parent)
    : QMainWindow(parent)
    , auth_(auth)
    , systemRepository_(systemRepository)
{
    // 0) 工位配置：工位号/线体/治具/SN 规则/MES 地址。
    stationConfig_ = core::StationConfig::loadActive();

    // 0.1) 质检复核策略：工位可要求复核人必须为质检员/管理员（QA-07+）。
    if (auth_ != nullptr)
        auth_->setRequireQualityApprover(stationConfig_.qualityApprovalOnly);

    // 0.2) 多语言（SY-06）：先安装翻译再建界面，保证首屏语言正确。
    languageManager_.restoreFromSettings();
    connect(&languageManager_, &core::LanguageManager::languageChanged, this,
            [this](const QString&) { retranslateUi(); });

    // 1) 多设备管理器：读取 configs/devices/fleet.json（缺失时退化为 1 台默认设备）。
    //    每台设备内部都有自己的通信线程与环形缓冲。
    manager_ = new service::DeviceManager(this);
    QString fleetError;
    manager_->loadFleet(QString(), &fleetError);

    // 2) 打开测试记录数据库。放在用户数据目录，避免程序目录没有写权限。
    repository_ = new storage::TestRepository();
    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    const QString dbPath = dataDir + QStringLiteral("/aegis.db");
    QString dbError;
    const bool dbReady = repository_->open(dbPath, &dbError);

    // 2.1) 波形摘要写线程：每台设备每秒的 min/max/avg 攒批写入同一个数据库。
    //      必须无父对象创建，否则 moveToThread 失败、写操作会落回 UI 线程并导致退出死锁。
    summaryWriter_ = new storage::SummaryWriter();
    summaryWriter_->start(dbPath);
    connect(manager_, &service::DeviceManager::deviceSummaryReady,
            this, [this](const QString& id, const service::DeviceSummary& summary) {
                if (summaryWriter_ != nullptr)
                    summaryWriter_->enqueue(id, summary);
            });

    // 2.2) 阈值报警：消费同一份秒级摘要，按回差判定高/低报警并落库。
    alarmMonitor_ = new service::AlarmMonitor(manager_, systemRepository_, this);
    connect(manager_, &service::DeviceManager::deviceSummaryReady,
            alarmMonitor_, &service::AlarmMonitor::onSummary);

    // 2.3) MES 上传：测试落库后把 JSON 写入本地 outbox，异步上传/重试。
    mesUploader_ = new service::MesUploader(repository_, stationConfig_, systemRepository_, this);
    connect(mesUploader_, &service::MesUploader::logMessage, this, &MainWindow::appendLog);
    connect(mesUploader_, &service::MesUploader::statusChanged, this, [this] {
        if (dashboardPage_ != nullptr)
            dashboardPage_->refresh();
    });

    // 3) 创建界面并填充设备列表（列表第一项会被选中）。
    buildUi();
    refreshDeviceList();

    // 4) 信号槽连接。
    connect(connectButton_, &QPushButton::clicked, this, &MainWindow::toggleConnection);
    connect(manager_, &service::DeviceManager::deviceConnectionChanged,
            this, &MainWindow::onDeviceConnectionChanged);
    connect(manager_, &service::DeviceManager::deviceLog,
            this, &MainWindow::onDeviceLog);
    connect(testPage_, &TestPage::runningChanged,
            this, &MainWindow::onTestRunningChanged);

    // 4.0) 产线联动：测试落库 -> MES 上传；看板切工单 -> 测试页。
    connect(testPage_, &TestPage::runSaved, this, [this](int runId) {
        if (mesUploader_ != nullptr)
            mesUploader_->enqueue(runId);
        // 首件状态、治具计数、看板统计都会在落库后变化。
        if (dashboardPage_ != nullptr)
            dashboardPage_->refresh();
    });
    connect(dashboardPage_, &DashboardPage::activeOrderChanged, this,
            [this](const QString& orderNo, const QString& product) {
                if (testPage_ == nullptr)
                    return;
                // 运行期间不允许换工单：把看板恢复成测试页正在使用的工单。
                if (testPage_->isRunning() && orderNo != testPage_->workOrderNo()) {
                    appendLog(QStringLiteral("测试执行中，不能切换工单"));
                    dashboardPage_->setActiveOrder(testPage_->workOrderNo(), false);
                    return;
                }

                testPage_->setWorkOrder(orderNo, product);
                if (shiftPage_ != nullptr)
                    shiftPage_->setActiveOrder(orderNo, product);
                QSettings settings;
                if (orderNo.isEmpty())
                    settings.remove(QStringLiteral("production/activeOrder"));
                else
                    settings.setValue(QStringLiteral("production/activeOrder"), orderNo);

                appendLog(orderNo.isEmpty()
                              ? QStringLiteral("当前工单已清空")
                              : QStringLiteral("当前工单：%1（%2）").arg(orderNo, product));
            });
    connect(dashboardPage_, &DashboardPage::mesRetryRequested, this, [this] {
        if (mesUploader_ != nullptr)
            mesUploader_->retryDead();
    });

    // 4.1) 报警事件：更新设备列表告警标记、页签计数、报警页。
    connect(alarmMonitor_, &service::AlarmMonitor::alarmRaised,
            this, [this](const storage::AlarmRecord& alarm) {
                alarmCounts_[alarm.deviceId] = alarmCounts_.value(alarm.deviceId) + 1;
                updateDeviceItem(alarm.deviceId);

                const QString levelText = alarm.level == QLatin1String("HIGH")
                                              ? QStringLiteral("高于上限")
                                              : QStringLiteral("低于下限");
                appendLog(QStringLiteral("[报警] %1 %2 %3：%4 %5（阈值 %6 %7）")
                              .arg(alarm.deviceName.isEmpty() ? alarm.deviceId
                                                              : alarm.deviceName)
                              .arg(alarm.channelName)
                              .arg(levelText)
                              .arg(alarm.value, 0, 'f', 1)
                              .arg(alarm.unit)
                              .arg(alarm.threshold, 0, 'f', 1)
                              .arg(alarm.unit));
                if (alarmPage_ != nullptr)
                    alarmPage_->refresh();
                updateAlarmTab();
            });
    connect(alarmMonitor_, &service::AlarmMonitor::alarmCleared,
            this, [this](int, const QString& deviceId) {
                auto it = alarmCounts_.find(deviceId);
                if (it != alarmCounts_.end()) {
                    if (--it.value() <= 0)
                        alarmCounts_.erase(it);
                }
                updateDeviceItem(deviceId);
                appendLog(QStringLiteral("[报警] %1 已恢复").arg(manager_->spec(deviceId).name));
                if (alarmPage_ != nullptr)
                    alarmPage_->refresh();
                updateAlarmTab();
            });

    // 5) 界面刷新定时器：50ms ≈ 20fps。固定刷新率避免"数据一到就重绘"导致的 CPU 抖动。
    uiTimer_ = new QTimer(this);
    uiTimer_->setInterval(50);
    connect(uiTimer_, &QTimer::timeout, this, &MainWindow::onTick);
    uiTimer_->start();

    appendLog(QStringLiteral("AegisHMI 启动，设备数：%1").arg(manager_->count()));
    if (!fleetError.isEmpty())
        appendLog(QStringLiteral("设备清单：%1").arg(fleetError));
    appendLog(dbReady ? QStringLiteral("测试记录数据库：%1").arg(dbPath)
                      : QStringLiteral("数据库打开失败：%1").arg(dbError));

    // 5.1) 启动 MES 上传（未配置则只提示，不影响本地测试）。
    if (mesUploader_ != nullptr) {
        mesUploader_->start();
        appendLog(mesUploader_->enabled()
                      ? QStringLiteral("MES 上传已启用：%1").arg(stationConfig_.mesUrl)
                      : QStringLiteral("MES 上传未启用（configs/station.json 的 mes.enabled）"));
    }

    // 5.2) 恢复上次使用的工单；不存在/已关闭则用看板当前选中项。
    QSettings settings;
    const QString savedOrder =
        settings.value(QStringLiteral("production/activeOrder")).toString();
    if (!savedOrder.isEmpty() && repository_->isOpen()
        && repository_->findWorkOrder(savedOrder).status == QLatin1String("ACTIVE")) {
        dashboardPage_->setActiveOrder(savedOrder, true);
    } else if (!dashboardPage_->activeOrderNo().isEmpty()) {
        dashboardPage_->setActiveOrder(dashboardPage_->activeOrderNo(), true);
    } else {
        testPage_->setWorkOrder(QString(), QString());
    }

    // 6) 按当前登录角色启用/禁用操作。
    applyRolePermissions();

    // 6.1) 首次启动显示"快速上手"，避免用户面对界面无从下手。
    QTimer::singleShot(800, this, [this] { HelpDialog::showIfNeeded(this); });

    // 7) 启动后自动连接所有 autoConnect 设备。
    QTimer::singleShot(300, this, [this] { manager_->connectAll(); });
}

MainWindow::~MainWindow()
{
    // 先停上传器（中止在途请求），避免它继续访问即将销毁的仓储。
    if (mesUploader_ != nullptr) {
        mesUploader_->stop();
        mesUploader_->disconnect(this);
    }

    // 先断开管理器的信号，避免子对象析构时（设备 flush 摘要）回调到本对象。
    if (manager_ != nullptr) {
        manager_->disconnect(this);
        if (alarmMonitor_ != nullptr)
            manager_->disconnect(alarmMonitor_);
    }
    // 刷完剩余摘要并退出写线程；该对象没有父对象，需要手动释放。
    if (summaryWriter_ != nullptr) {
        summaryWriter_->stop();
        delete summaryWriter_;
        summaryWriter_ = nullptr;
    }
    // 页面只持有仓储指针，不在析构中使用；这里统一释放。
    delete repository_;
    repository_ = nullptr;
}

// =============================================================================
// buildUi：生产（看板）+ 监控（连接 + 波形 + 日志）+ 自动测试 + 历史/趋势/报警/审计
// =============================================================================
void MainWindow::buildUi()
{
    tabs_ = new QTabWidget(this);

    // 生产看板放在首屏：工单、产量、良率、MES 状态一眼可见。
    dashboardPage_ = new DashboardPage(repository_, auth_, stationConfig_, this);
    tabs_->addTab(dashboardPage_, tr("生产"));

    tabs_->addTab(createMonitorPage(), tr("监控"));

    // 自动测试页需要访问设备服务（下发命令、采集窗口）与数据库（保存结果）。
    // 具体测试哪台设备由设备列表选择后调用 setDevice() 决定。
    testPage_ = new TestPage(repository_, auth_, stationConfig_, this);
    tabs_->addTab(testPage_, tr("自动测试"));

    historyPage_ = new HistoryPage(repository_, auth_, stationConfig_, this);
    tabs_->addTab(historyPage_, tr("历史"));

    // 趋势页：读 sample_summary 表，显示秒级 min/max 包络与 avg 曲线。
    trendPage_ = new TrendPage(repository_, manager_, this);
    tabs_->addTab(trendPage_, tr("趋势"));

    // SPC 页：按测试项计算 Cp/Cpk 并绘制 I-MR 控制图。
    spcPage_ = new SpcPage(repository_, this);
    tabs_->addTab(spcPage_, tr("SPC"));

    // 班组页：班次、操作员资质与交接班记录。
    shiftPage_ = new ShiftPage(repository_, auth_, stationConfig_, this);
    tabs_->addTab(shiftPage_, tr("班组"));

    // 报警页：阈值报警列表与确认（观察员只读）。
    alarmPage_ = new AlarmPage(systemRepository_, auth_, alarmMonitor_, this);
    alarmTabIndex_ = tabs_->addTab(alarmPage_, tr("报警"));

    // 审计页：仅管理员可见（applyRolePermissions 里控制显示）。
    auditPage_ = new AuditPage(systemRepository_, auth_, this);
    auditTabIndex_ = tabs_->addTab(auditPage_, tr("审计"));

    // 用户管理：仅管理员可见。
    userPage_ = new UserPage(systemRepository_, auth_, this);
    userTabIndex_ = tabs_->addTab(userPage_, tr("用户"));

    // 切到历史/趋势/报警/审计页时自动刷新，避免显示过期数据。
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        updatePageHeader(index);
        syncNavigation();
        if (dashboardPage_ != nullptr && tabs_->widget(index) == dashboardPage_)
            dashboardPage_->refresh();
        if (historyPage_ != nullptr && tabs_->widget(index) == historyPage_)
            historyPage_->refresh();
        if (trendPage_ != nullptr && tabs_->widget(index) == trendPage_)
            trendPage_->refresh();
        if (spcPage_ != nullptr && tabs_->widget(index) == spcPage_)
            spcPage_->refresh();
        if (shiftPage_ != nullptr && tabs_->widget(index) == shiftPage_)
            shiftPage_->refresh();
        if (alarmPage_ != nullptr && tabs_->widget(index) == alarmPage_)
            alarmPage_->refresh();
        if (auditPage_ != nullptr && tabs_->widget(index) == auditPage_)
            auditPage_->refresh();
        if (userPage_ != nullptr && tabs_->widget(index) == userPage_)
            userPage_->refresh();
    });

    // 工业风框架：左侧导航栏 + 深色控制台顶栏 + 页面标题区 + 内容页。
    // 仍沿用 QTabWidget 承载各页（保留现有刷新/权限逻辑），只隐藏它的标签栏。
    tabs_->setProperty("role", QStringLiteral("content"));
    tabs_->setDocumentMode(true);
    tabs_->tabBar()->hide();

    auto* central = new QWidget(this);
    auto* shell = new QHBoxLayout(central);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);
    shell->addWidget(createNavigationRail());

    auto* mainArea = new QWidget();
    mainArea->setProperty("role", QStringLiteral("contentArea"));
    auto* mainLayout = new QVBoxLayout(mainArea);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    mainLayout->addWidget(createStatusHeader());

    auto* content = new QWidget();
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(18, 14, 18, 14);
    contentLayout->setSpacing(10);

    pageHeader_ = new QWidget();
    auto* pageHeaderLayout = new QVBoxLayout(pageHeader_);
    pageHeaderLayout->setContentsMargins(2, 0, 2, 0);
    pageHeaderLayout->setSpacing(2);
    pageTitle_ = new QLabel();
    pageTitle_->setProperty("role", QStringLiteral("pageTitle"));
    pageSubtitle_ = new QLabel();
    pageSubtitle_->setProperty("role", QStringLiteral("pageSubtitle"));
    pageHeaderLayout->addWidget(pageTitle_);
    pageHeaderLayout->addWidget(pageSubtitle_);
    contentLayout->addWidget(pageHeader_);
    contentLayout->addWidget(tabs_, 1);
    mainLayout->addWidget(content, 1);

    shell->addWidget(mainArea, 1);
    setCentralWidget(central);
    resize(1360, 840);
    // 窗口标题/页签等主框架文案统一在 retranslateUi 里设置，语言切换时复用。
    retranslateUi();

    // ---- 版本检测：启动后延迟检查一次；离线/无清单时静默跳过 ----
    updateChecker_ = new service::UpdateChecker(stationConfig_, this);
    connect(updateChecker_, &service::UpdateChecker::checked, this,
            [this](service::UpdateChecker::Status status,
                   const service::VersionInfo& info, const QString& message) {
                if (status == service::UpdateChecker::Status::UpdateAvailable) {
                    QString text = message;
                    if (!info.notes.isEmpty())
                        text += QStringLiteral("\n\n更新说明：") + info.notes;
                    if (!info.url.isEmpty())
                        text += QStringLiteral("\n升级包：") + info.url;
                    QMessageBox::information(this, QStringLiteral("发现新版本"), text);
                } else if (manualUpdateCheck_) {
                    QMessageBox::information(this, QStringLiteral("检查更新"), message);
                }
                manualUpdateCheck_ = false;
                appendLog(message);
            });
    if (stationConfig_.updateCheckOnStartup) {
        QTimer::singleShot(3000, this, [this]() {
            manualUpdateCheck_ = false;
            updateChecker_->check();
        });
    }
}

// =============================================================================
// retranslateUi：语言切换后重设主框架文案（窗口标题 / 页签 / 左侧关键标签）。
// 页面内部文案迁移到 tr() 后会由 Qt 的 LanguageChange 事件自动刷新。
// =============================================================================
void MainWindow::retranslateUi()
{
    QString title = tr("AegisHMI - 工业设备自动测试上位机");
    const QString version = QCoreApplication::applicationVersion();
    if (!version.isEmpty())
        title += QStringLiteral("  v") + version;
    setWindowTitle(title);

    if (tabs_ != nullptr) {
        tabs_->setTabText(TabDashboard, tr("生产"));
        tabs_->setTabText(TabMonitor, tr("监控"));
        tabs_->setTabText(TabTest, tr("自动测试"));
        tabs_->setTabText(TabHistory, tr("历史"));
        tabs_->setTabText(TabTrend, tr("趋势"));
        tabs_->setTabText(TabSpc, tr("SPC"));
        tabs_->setTabText(TabShift, tr("班组"));
        if (alarmTabIndex_ >= 0)
            tabs_->setTabText(alarmTabIndex_, tr("报警"));
        if (auditTabIndex_ >= 0)
            tabs_->setTabText(auditTabIndex_, tr("审计"));
        if (userTabIndex_ >= 0)
            tabs_->setTabText(userTabIndex_, tr("用户"));

        // 每个页签都给出悬停说明：不知道"这个页是干什么的"时不用翻文档。
        tabs_->setTabToolTip(TabDashboard, tr("工单切换、产量/良率、MES 上传状态、工步耗时 Top"));
        tabs_->setTabToolTip(TabMonitor, tr("设备连接、实时波形、链路健康、捕获/点检/报文诊断、运行日志"));
        tabs_->setTabToolTip(TabTest, tr("加载/编辑方案、SN 与料号校验、执行测试与单步调试"));
        tabs_->setTabToolTip(TabHistory, tr("按 SN/工单/方案/判定检索，导出 PDF/CSV"));
        tabs_->setTabToolTip(TabTrend, tr("秒级 min/max 包络与平均值曲线"));
        tabs_->setTabToolTip(TabSpc, tr("Cp/Cpk 与控制图判异"));
        tabs_->setTabToolTip(TabShift, tr("班次、交接班记录与操作员资质"));
        if (alarmTabIndex_ >= 0)
            tabs_->setTabToolTip(alarmTabIndex_, tr("阈值报警列表与确认/恢复（标题显示未恢复数量）"));
        if (auditTabIndex_ >= 0)
            tabs_->setTabToolTip(auditTabIndex_, tr("登录、连接、测试、复核等操作留痕（仅管理员）"));
        if (userTabIndex_ >= 0)
            tabs_->setTabToolTip(userTabIndex_, tr("账号、角色与口令管理（仅管理员）"));
    }
    if (deviceListLabel_ != nullptr)
        deviceListLabel_->setText(tr("设备列表"));
    if (linkLabel_ != nullptr)
        linkLabel_->setText(tr("链路"));

    // 用户/角色与报警角标文案由既有逻辑统一刷新。
    applyRolePermissions();
    updateAlarmTab();
    syncNavigation();
    updatePageHeader(tabs_ != nullptr ? tabs_->currentIndex() : 0);
}

// =============================================================================
// createNavigationRail：左侧工业风导航栏
//
// 相比原先的顶部页签，左侧纵向导航更符合工业上位机/SCADA 的操作习惯：
// 目标更大、可带图标与状态角标、当前页有明确高亮。
// =============================================================================
QWidget* MainWindow::createNavigationRail()
{
    auto* rail = new QFrame();
    rail->setProperty("role", QStringLiteral("rail"));
    rail->setFixedWidth(178);

    auto* layout = new QVBoxLayout(rail);
    layout->setContentsMargins(12, 18, 12, 14);
    layout->setSpacing(3);

    auto* brand = new QLabel(QStringLiteral("AEGIS"));
    brand->setProperty("role", QStringLiteral("brand"));
    auto* brandSub = new QLabel(QStringLiteral("工位测试终端"));
    brandSub->setProperty("role", QStringLiteral("brandSub"));
    layout->addWidget(brand);
    layout->addWidget(brandSub);
    layout->addSpacing(18);

    // 与页签顺序一一对应；文字统一在 syncNavigation 里跟随页签文案（支持多语言）。
    const char* const iconKinds[] = {
        "production", "monitor", "test", "history", "trend",
        "spc", "shift", "alarm", "audit", "user",
    };
    const int kindCount = static_cast<int>(sizeof(iconKinds) / sizeof(iconKinds[0]));

    auto* group = new QButtonGroup(rail);
    group->setExclusive(true);
    navButtons_.clear();
    for (int i = 0; i < tabs_->count() && i < kindCount; ++i) {
        auto* button = new QPushButton();
        button->setProperty("role", QStringLiteral("nav"));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setIcon(navIcon(QString::fromLatin1(iconKinds[i])));
        button->setIconSize(QSize(18, 18));
        button->setFixedHeight(38);
        group->addButton(button, i);
        layout->addWidget(button);
        navButtons_.append(button);
        connect(button, &QPushButton::clicked, this, [this, i] {
            if (tabs_ != nullptr)
                tabs_->setCurrentIndex(i);
        });
    }

    layout->addStretch(1);
    const QString version = QCoreApplication::applicationVersion();
    auto* versionLabel = new QLabel(version.isEmpty() ? QStringLiteral("HMI")
                                                      : QStringLiteral("v%1").arg(version));
    versionLabel->setProperty("role", QStringLiteral("brandSub"));
    layout->addWidget(versionLabel);
    return rail;
}

void MainWindow::syncNavigation()
{
    if (tabs_ == nullptr || navButtons_.isEmpty())
        return;

    const int current = tabs_->currentIndex();
    for (int i = 0; i < navButtons_.size() && i < tabs_->count(); ++i) {
        QPushButton* button = navButtons_.at(i);
        button->setText(tabs_->tabText(i));
        button->setToolTip(tabs_->tabToolTip(i));
        button->setVisible(tabs_->isTabVisible(i));
        button->setChecked(i == current);
    }
}

// 页面标题区：标题取自页签文案，副标题给出一句话说明；切换时做轻微淡入。
void MainWindow::updatePageHeader(int index)
{
    if (pageTitle_ == nullptr || pageSubtitle_ == nullptr || tabs_ == nullptr || index < 0)
        return;

    pageTitle_->setText(tabs_->tabText(index));

    static const QStringList subtitles = {
        QStringLiteral("工单、产量良率、MES 上传状态与瓶颈工步"),
        QStringLiteral("设备连接、实时波形、链路健康与诊断工具"),
        QStringLiteral("方案加载、SN/料号校验、执行与单步调试"),
        QStringLiteral("按 SN/工单/方案检索历史结果并导出报告"),
        QStringLiteral("秒级趋势包络与多通道对比"),
        QStringLiteral("Cp/Cpk 与控制图判异"),
        QStringLiteral("班次、交接班记录与人员资质"),
        QStringLiteral("阈值报警确认与搁置处理"),
        QStringLiteral("登录、连接、测试与复核的审计留痕"),
        QStringLiteral("账号、角色与口令管理"),
    };
    pageSubtitle_->setText(index < subtitles.size() ? subtitles.at(index) : QString());

    // 淡入过渡：让页面切换有反馈，但只作用于标题区，不影响波形刷新性能。
    if (pageHeader_->graphicsEffect() != nullptr)
        pageHeader_->setGraphicsEffect(nullptr);
    auto* effect = new QGraphicsOpacityEffect(pageHeader_);
    pageHeader_->setGraphicsEffect(effect);
    auto* animation = new QPropertyAnimation(effect, "opacity", this);
    animation->setDuration(170);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);
    connect(animation, &QPropertyAnimation::finished, this, [this] {
        if (pageHeader_ != nullptr)
            pageHeader_->setGraphicsEffect(nullptr);
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

// =============================================================================
// createStatusHeader：顶部常驻状态条 + 报警横幅
//
// 借鉴 WinCC/Ignition 的做法：不管当前在哪个页签，链路在线数、未恢复报警、
// 当前工单与登录用户都一眼可见；报警可一键跳到报警页处理。
// =============================================================================
QWidget* MainWindow::createStatusHeader()
{
    auto* container = new QWidget();
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* bar = new QFrame();
    bar->setProperty("role", QStringLiteral("topbar"));
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(12, 6, 12, 6);
    row->setSpacing(10);

    auto* appTitle = new QLabel(QStringLiteral("控制台"));
    appTitle->setProperty("role", QStringLiteral("chip"));
    row->addWidget(appTitle);

    onlineChip_ = new QLabel(QStringLiteral("链路 0/0 在线"));
    onlineChip_->setProperty("role", QStringLiteral("chip"));
    onlineChip_->setToolTip(QStringLiteral("已连接链路数 / 总链路数（所有设备合计）"));

    orderChip_ = new QLabel(QStringLiteral("工单 —"));
    orderChip_->setProperty("role", QStringLiteral("chip"));
    orderChip_->setToolTip(QStringLiteral("当前生产工单；在“生产”页切换"));

    userChip_ = new QLabel(QStringLiteral("用户 —"));
    userChip_->setProperty("role", QStringLiteral("chip"));
    userChip_->setToolTip(QStringLiteral("当前登录用户与角色"));

    alarmChip_ = makeButton(QStringLiteral("无报警"), QStringLiteral("chip"));
    alarmChip_->setToolTip(QStringLiteral("未恢复报警数量；点击打开报警页"));

    row->addWidget(onlineChip_);
    row->addWidget(alarmChip_);
    row->addWidget(orderChip_);
    row->addWidget(userChip_);
    row->addStretch(1);

    helpButton_ = makeButton(QStringLiteral("使用说明"));
    helpButton_->setToolTip(QStringLiteral("打开快速上手指南：操作流程、页签功能地图与常见问题"));
    row->addWidget(helpButton_);
    layout->addWidget(bar);

    alarmBanner_ = new QPushButton();
    alarmBanner_->setProperty("role", QStringLiteral("alarmBanner"));
    alarmBanner_->setVisible(false);
    layout->addWidget(alarmBanner_);

    auto openAlarmPage = [this] {
        if (alarmTabIndex_ >= 0 && tabs_ != nullptr)
            tabs_->setCurrentIndex(alarmTabIndex_);
    };
    connect(alarmChip_, &QPushButton::clicked, this, openAlarmPage);
    connect(alarmBanner_, &QPushButton::clicked, this, openAlarmPage);
    connect(helpButton_, &QPushButton::clicked, this, &MainWindow::showQuickStart);

    return container;
}

// 刷新顶部状态条：链路在线数、未恢复报警、工单与用户。
void MainWindow::refreshHeader()
{
    if (manager_ == nullptr)
        return;

    int total = 0;
    int online = 0;
    const QVector<service::LinkHealth> health = manager_->linkHealth();
    for (const service::LinkHealth& item : health) {
        ++total;
        if (item.connected)
            ++online;
    }

    if (onlineChip_ != nullptr) {
        onlineChip_->setText(QStringLiteral("链路 %1/%2 在线").arg(online).arg(total));
        setStatusTone(onlineChip_,
                      total > 0 && online == total ? StatusTone::Ok
                      : online > 0               ? StatusTone::Warn
                                                 : StatusTone::Neutral);
    }

    const int alarms = alarmMonitor_ != nullptr ? alarmMonitor_->openCount() : 0;
    if (alarmChip_ != nullptr) {
        alarmChip_->setText(alarms > 0 ? QStringLiteral("报警 %1").arg(alarms)
                                       : QStringLiteral("无报警"));
        setTone(alarmChip_, alarms > 0 ? StatusTone::Error : StatusTone::Ok);
    }
    if (alarmBanner_ != nullptr) {
        alarmBanner_->setText(
            QStringLiteral("⚠  %1 条报警未恢复 — 点击查看并处理").arg(alarms));
        alarmBanner_->setVisible(alarms > 0);
    }

    QString order = testPage_ != nullptr ? testPage_->workOrderNo() : QString();
    if (order.isEmpty() && dashboardPage_ != nullptr)
        order = dashboardPage_->activeOrderNo();
    if (orderChip_ != nullptr) {
        orderChip_->setText(order.isEmpty() ? QStringLiteral("工单 —")
                                            : QStringLiteral("工单 %1").arg(order));
    }

    if (userChip_ != nullptr && auth_ != nullptr) {
        const security::UserInfo info = auth_->currentUser();
        userChip_->setText(QStringLiteral("用户 %1 · %2")
                               .arg(info.name,
                                    security::AuthService::roleDisplayName(info.role)));
    }

    refreshFaceplate();
}

// 设备 Faceplate：波形上方的身份卡片（名称/地址/链路/状态）。
void MainWindow::refreshFaceplate()
{
    if (faceplateName_ == nullptr || manager_ == nullptr)
        return;

    if (currentDeviceId_.isEmpty()) {
        faceplateName_->setText(QStringLiteral("未选择设备"));
        faceplateMeta_->setText(QStringLiteral("在左侧选择设备后显示地址与链路"));
        faceplateState_->setText(QStringLiteral("未连接"));
        setStatusTone(faceplateState_, StatusTone::Neutral);
        return;
    }

    const service::DeviceSpec spec = manager_->spec(currentDeviceId_);
    const QString linkId = currentLinkId_.isEmpty() ? manager_->mainLinkId(currentDeviceId_)
                                                    : currentLinkId_;
    const service::LinkSpec link = manager_->linkSpec(currentDeviceId_, linkId);

    QString address;
    if (link.protocol == QLatin1String("opcUa")) {
        address = link.endpoint.isEmpty()
            ? QStringLiteral("opc.tcp://%1:%2").arg(link.host).arg(link.port)
            : link.endpoint;
    } else if (link.protocol == QLatin1String("modbusRtu")) {
        address = QStringLiteral("%1 @ %2 bps").arg(link.host).arg(link.baudRate);
    } else {
        address = QStringLiteral("%1:%2").arg(link.host).arg(link.port);
    }

    faceplateName_->setText(spec.name);
    faceplateMeta_->setText(QStringLiteral("%1 · %2 · 链路 %3%4")
                                .arg(link.protocol, address, linkId,
                                     link.isMain() ? QStringLiteral("（主）")
                                                   : QStringLiteral("（辅助）")));

    const service::DeviceService* service = manager_->link(currentDeviceId_, currentLinkId_);
    const bool connected = service != nullptr && service->isConnected();
    faceplateState_->setText(connected ? QStringLiteral("已连接") : QStringLiteral("未连接"));
    setStatusTone(faceplateState_, connected ? StatusTone::Ok : StatusTone::Neutral);
}

// =============================================================================
// createMonitorPage：左侧控制面板 + 右侧波形
// =============================================================================
QWidget* MainWindow::createMonitorPage()
{
    auto* page = new QWidget();
    auto* root = new QHBoxLayout(page);
    auto* left = new QVBoxLayout();
    left->setContentsMargins(2, 2, 2, 2);
    left->setSpacing(6);

    // ---- 当前用户（角色决定可用操作）----
    auto* userRow = new QHBoxLayout();
    userLabel_ = new QLabel();
    userLabel_->setWordWrap(true);
    switchUserButton_ = makeButton(QStringLiteral("切换用户"));
    switchUserButton_->setToolTip(QStringLiteral("退出当前账号并切换到其他用户（测试运行中不可切换）"));
    // 语言切换（SY-06）：立即生效，选择会记住。
    languageCombo_ = new QComboBox();
    for (const core::LanguageOption& option : core::LanguageManager::availableLanguages())
        languageCombo_->addItem(option.displayName, option.code);
    languageCombo_->setCurrentIndex(
        languageCombo_->findData(languageManager_.current()));
    languageCombo_->setToolTip(QStringLiteral("界面语言（切换后立即生效）"));
    // 手动检查更新（SY-05）：结果只提示，不自动下载/覆盖运行中的程序。
    auto* checkUpdateButton = new QPushButton(QStringLiteral("检查更新"));
    checkUpdateButton->setToolTip(
        QStringLiteral("读取工位配置的版本清单，发现新版本时提示说明与升级包地址"));
    userRow->addWidget(userLabel_, 1);
    userRow->addWidget(languageCombo_);
    userRow->addWidget(checkUpdateButton);
    userRow->addWidget(switchUserButton_);
    left->addLayout(userRow);
    left->addWidget(makeSectionTitle(QStringLiteral("① 选择设备并连接")));
    left->addWidget(makeHint(QStringLiteral(
        "选中设备后先核对协议与地址，再点击“连接”；连接期间参数锁定，断开后可修改。")));
    connect(switchUserButton_, &QPushButton::clicked, this, &MainWindow::switchUser);
    connect(languageCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                const QString code = languageCombo_->itemData(index).toString();
                if (code.isEmpty() || code == languageManager_.current())
                    return;
                QString error;
                if (!languageManager_.apply(code, &error)) {
                    QMessageBox::warning(this, QStringLiteral("语言"), error);
                    return;
                }
                languageManager_.saveToSettings();
            });
    connect(checkUpdateButton, &QPushButton::clicked, this, [this]() {
        if (updateChecker_ == nullptr)
            return;
        manualUpdateCheck_ = true;
        updateChecker_->check();
    });
    left->addSpacing(6);

    // ---- 设备列表（多设备并发）：选中哪台就显示哪台的波形/统计 ----
    deviceListLabel_ = new QLabel(QStringLiteral("设备列表"));
    deviceListLabel_->setProperty("role", QStringLiteral("section"));
    left->addWidget(deviceListLabel_);
    deviceList_ = new QListWidget();
    deviceList_->setMinimumHeight(96);
    deviceList_->setMaximumHeight(120);
    deviceList_->setToolTip(QStringLiteral(
        "● 在线　○ 离线　⚠ 有未恢复报警\n点击设备查看其波形、统计与链路"));
    left->addWidget(deviceList_);
    left->addWidget(makeHint(QStringLiteral(
        "标记说明：● 在线　○ 离线　⚠ 有报警。点击选择要查看/测试的设备。")));
    connect(deviceList_, &QListWidget::currentRowChanged,
            this, &MainWindow::onDeviceSelected);
    left->addSpacing(6);

    // ---- 链路选择（EQ-06 多链路）：单链路设备只有一项 ----
    auto* linkRow = new QHBoxLayout();
    linkLabel_ = new QLabel(QStringLiteral("链路"));
    linkRow->addWidget(linkLabel_);
    linkCombo_ = new QComboBox();
    linkCombo_->setToolTip(QStringLiteral(
        "一台设备可配置主/辅多条链路：主链路负责波形与摘要，辅助链路只做命令/测量。\n"
        "连接/断开只作用于当前选中的链路。"));
    linkRow->addWidget(linkCombo_, 1);
    left->addLayout(linkRow);
    connect(linkCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index < 0 || currentDeviceId_.isEmpty())
                    return;
                currentLinkId_ = linkCombo_->itemData(index).toString();
                applySpecToControls(currentDeviceId_);
                const service::DeviceService* service =
                    manager_->link(currentDeviceId_, currentLinkId_);
                const bool connected = service != nullptr && service->isConnected();
                statusLabel_->setText(connected ? QStringLiteral("已连接")
                                                : QStringLiteral("未连接"));
                setStatusTone(statusLabel_, connected ? StatusTone::Ok : StatusTone::Neutral);
                connectButton_->setText(connected ? QStringLiteral("断开")
                                                  : QStringLiteral("连接"));
                setButtonRole(connectButton_,
                              connected ? QStringLiteral("danger") : QStringLiteral("primary"));
                refreshFaceplate();
            });
    left->addSpacing(6);

    // ---- 连接参数 ----
    left->addWidget(makeHint(QStringLiteral(
        "协议决定下面哪些参数生效：自定义协议用端口；Modbus 用站号；RTU 用串口与波特率；"
        "OPC UA 用端点 URL；SCPI 用查询命令与换算系数。")));
    auto* form = new QFormLayout();

    protocolCombo_ = new QComboBox();
    protocolCombo_->addItem(QStringLiteral("自定义协议 (TCP)"), 0);
    protocolCombo_->addItem(QStringLiteral("Modbus TCP"), 1);
    protocolCombo_->addItem(QStringLiteral("Modbus RTU (串口)"), 2);
    protocolCombo_->addItem(QStringLiteral("OPC UA"), 3);
    protocolCombo_->addItem(QStringLiteral("SCPI 仪器 (TCP)"), 4);

    hostEdit_ = new QLineEdit(QStringLiteral("127.0.0.1"));
    portSpin_ = new QSpinBox();
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(9527); // 自定义协议模拟器默认端口

    unitSpin_ = new QSpinBox();
    unitSpin_->setRange(1, 247); // Modbus 从站地址合法范围
    unitSpin_->setValue(1);
    unitSpin_->setEnabled(false);

    baudCombo_ = new QComboBox();
    baudCombo_->addItems({QStringLiteral("9600"), QStringLiteral("19200"),
                          QStringLiteral("38400"), QStringLiteral("57600"),
                          QStringLiteral("115200")});
    baudCombo_->setCurrentText(QStringLiteral("115200"));
    baudCombo_->setEnabled(false);

    // SCPI 仪器专属参数：周期查询命令、轮询周期、读数换算系数。
    scpiQueryEdit_ = new QLineEdit(QStringLiteral("MEAS:VOLT:DC?"));
    scpiQueryEdit_->setToolTip(
        QStringLiteral("设备周期查询命令，如 MEAS:VOLT:DC? / MEAS:CURR:DC?"));
    scpiPollSpin_ = new QSpinBox();
    scpiPollSpin_->setRange(50, 5000); // 与 DeviceManager 的 clamp 一致
    scpiPollSpin_->setSuffix(QStringLiteral(" ms"));
    scpiPollSpin_->setValue(100);
    scpiScaleSpin_ = new QDoubleSpinBox();
    scpiScaleSpin_->setRange(0.001, 1000.0);
    scpiScaleSpin_->setDecimals(3);
    scpiScaleSpin_->setValue(1.0);
    scpiScaleSpin_->setToolTip(
        QStringLiteral("仪器读数换算到通道单位，如 V→mV 填 1000"));
    scpiQueryEdit_->setEnabled(false);
    scpiPollSpin_->setEnabled(false);
    scpiScaleSpin_->setEnabled(false);

    // 每个参数都带悬停说明：现场人员不查文档也能判断该填什么。
    protocolCombo_->setToolTip(QStringLiteral(
        "通信协议。切换后端口会按协议给默认值；连接期间不可修改。"));
    hostEdit_->setToolTip(QStringLiteral(
        "设备地址：IP、串口名（如 COM3）或 OPC UA 端点 URL。"));
    portSpin_->setToolTip(QStringLiteral(
        "TCP 端口：自定义协议默认 9527，Modbus TCP 502，SCPI 5025。"));
    unitSpin_->setToolTip(QStringLiteral("Modbus 从站地址（1~247）。"));
    baudCombo_->setToolTip(QStringLiteral("Modbus RTU 串口波特率，需与设备一致。"));
    scpiQueryEdit_->setToolTip(QStringLiteral(
        "周期查询命令，例如 MEAS:VOLT:DC?；结果写入 CH0。"));
    scpiPollSpin_->setToolTip(QStringLiteral(
        "查询周期。越短刷新越快，但会增加仪器负载。"));
    scpiScaleSpin_->setToolTip(QStringLiteral(
        "读数换算到通道单位：仪器返回 V、通道用 mV 时填 1000。"));

    form->addRow(QStringLiteral("协议"), protocolCombo_);
    form->addRow(QStringLiteral("地址/串口"), hostEdit_);
    form->addRow(QStringLiteral("端口"), portSpin_);
    form->addRow(QStringLiteral("站号"), unitSpin_);
    form->addRow(QStringLiteral("波特率"), baudCombo_);
    form->addRow(QStringLiteral("查询命令"), scpiQueryEdit_);
    form->addRow(QStringLiteral("轮询周期"), scpiPollSpin_);
    form->addRow(QStringLiteral("读数系数"), scpiScaleSpin_);
    left->addLayout(form);

    // 切换协议时调整控件可用性与默认值。
    connect(protocolCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const int type = protocolCombo_->itemData(index).toInt();
        const bool modbus = (type == 1 || type == 2);
        const bool rtu = type == 2;
        const bool opcua = type == 3;

        unitSpin_->setEnabled(modbus);
        baudCombo_->setEnabled(rtu);
        portSpin_->setEnabled(type == 0 || type == 1 || type == 4);

        const bool scpi = (type == 4);
        scpiQueryEdit_->setEnabled(scpi);
        scpiPollSpin_->setEnabled(scpi);
        scpiScaleSpin_->setEnabled(scpi);

        if (opcua) {
            hostEdit_->setPlaceholderText(QStringLiteral("opc.tcp://127.0.0.1:4840"));
            if (hostEdit_->text() == QLatin1String("127.0.0.1"))
                hostEdit_->setText(QStringLiteral("opc.tcp://127.0.0.1:4840"));
        } else {
            hostEdit_->setPlaceholderText(rtu ? QStringLiteral("COM3")
                                              : QStringLiteral("127.0.0.1"));
            if (hostEdit_->text().startsWith(QLatin1String("opc.tcp://")))
                hostEdit_->setText(QStringLiteral("127.0.0.1"));
        }

        if (!rtu && (portSpin_->value() == 9527 || portSpin_->value() == 5020
                     || portSpin_->value() == 5025))
            portSpin_->setValue(type == 1 ? 5020 : type == 4 ? 5025 : 9527);
    });

    // ---- 操作与状态：连接按钮是主操作，状态用颜色胶囊表达 ----
    connectButton_ = makeButton(QStringLiteral("连接"), QStringLiteral("primary"));
    connectButton_->setToolTip(QStringLiteral(
        "连接当前选中的设备/链路。已连接时按钮变为“断开”。"));
    statusLabel_ = new QLabel(QStringLiteral("未连接"));
    statusLabel_->setProperty("role", QStringLiteral("status"));
    statsLabel_ = new QLabel(QStringLiteral("暂无数据：连接设备后显示采样率与统计"));
    statsLabel_->setWordWrap(true);

    auto* statusRow = new QHBoxLayout();
    statusRow->addWidget(connectButton_);
    statusRow->addWidget(statusLabel_); // 胶囊按文字宽度显示，不拉伸成整条
    statusRow->addStretch(1);
    left->addLayout(statusRow);
    left->addWidget(statsLabel_);

    // ---- 链路健康面板（EQ-06 收尾）：每链路在线/延迟/帧数 ----
    left->addWidget(makeSectionTitle(QStringLiteral("② 查看数据与链路健康")));
    left->addWidget(makeHint(QStringLiteral(
        "右侧波形实时刷新；下表每秒更新每条链路的在线状态、最近入站延迟与累计帧数。")));
    linkHealthTable_ = new QTableWidget(0, 5);
    linkHealthTable_->setHorizontalHeaderLabels(
        {QStringLiteral("链路"), QStringLiteral("协议"), QStringLiteral("状态"),
         QStringLiteral("延迟"), QStringLiteral("帧数")});
    linkHealthTable_->verticalHeader()->setVisible(false);
    linkHealthTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    linkHealthTable_->horizontalHeader()->setStretchLastSection(true);
    linkHealthTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    linkHealthTable_->setSelectionMode(QAbstractItemView::NoSelection);
    linkHealthTable_->setMaximumHeight(96);
    linkHealthTable_->setToolTip(QStringLiteral(
        "状态：在线/离线\n延迟：最近一次收到数据距今的毫秒数（越小越好）\n"
        "帧数：本次连接累计解析的数据帧数"));
    linkHealthTable_->horizontalHeaderItem(2)->setToolTip(QStringLiteral("绿色=在线，红色=离线"));
    linkHealthTable_->horizontalHeaderItem(3)->setToolTip(
        QStringLiteral("最近一次入站数据距今的毫秒数；-- 表示本次连接还没有收到过数据"));
    left->addWidget(linkHealthTable_);

    // ---- 工具：捕获波形 / 日常点检 / 报文诊断 ----
    left->addWidget(makeSectionTitle(QStringLiteral("③ 工具与诊断")));
    auto* captureRow = new QHBoxLayout();
    captureButton_ = makeButton(QStringLiteral("捕获波形…"));
    captureButton_->setToolTip(
        QStringLiteral("开始捕获最近 20 秒原始采样，再次点击保存为 CSV"));
    checkButton_ = makeButton(QStringLiteral("日常点检"));
    checkButton_->setToolTip(
        QStringLiteral("按 active.json 的 check/checkTolerance 检查各通道金样值"));
    captureLabel_ = new QLabel(QStringLiteral("未捕获"));
    captureLabel_->setProperty("role", QStringLiteral("hint"));
    captureRow->addWidget(captureButton_);
    captureRow->addWidget(checkButton_);
    captureRow->addWidget(captureLabel_, 1);
    left->addLayout(captureRow);
    connect(captureButton_, &QPushButton::clicked,
            this, &MainWindow::toggleWaveformCapture);
    connect(checkButton_, &QPushButton::clicked, this, &MainWindow::runDailyCheck);

    hexCheck_ = new QCheckBox(QStringLiteral("报文诊断（HEX）"));
    hexCheck_->setToolTip(
        QStringLiteral("在运行日志中显示收发报文（前 96 字节），用于现场排查通信问题"));
    left->addWidget(hexCheck_);
    connect(hexCheck_, &QCheckBox::toggled,
            this, &MainWindow::onHexLoggingToggled);
    left->addSpacing(8);

    // ---- 日志区：占据左栏剩余空间 ----
    left->addWidget(makeSectionTitle(QStringLiteral("运行日志")));
    logView_ = new QPlainTextEdit();
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(500); // 只保留最近 500 行
    logView_->setMinimumHeight(150);
    logView_->setProperty("role", QStringLiteral("log"));
    logView_->setPlaceholderText(QStringLiteral(
        "连接、命令、报警、MES 上传等事件会显示在这里。"));
    logView_->setToolTip(QStringLiteral("按时间顺序显示运行事件；文件日志保存在用户数据目录 logs 下"));
    left->addWidget(logView_);

    // 左栏内容较多：放进滚动区，小屏幕/低分辨率下也不会挤掉日志或按钮。
    auto* leftScroll = new QScrollArea();
    leftScroll->setWidgetResizable(true);
    leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftScroll->setFixedWidth(340);
    auto* leftWidget = new QWidget();
    leftWidget->setLayout(left);
    leftScroll->setWidget(leftWidget);

    // ---- 右侧：设备 Faceplate（身份/地址/状态）+ 实时波形 ----
    waveform_ = new WaveformView();
    waveform_->setWindowSeconds(kWindowSeconds);
    waveform_->setToolTip(QStringLiteral(
        "实时波形：滚轮缩放；左键放置 A 游标，右键放置 B 游标，双击清除游标。"));

    auto* faceplate = new QFrame();
    faceplate->setProperty("role", QStringLiteral("card"));
    auto* faceplateLayout = new QHBoxLayout(faceplate);
    faceplateLayout->setContentsMargins(14, 8, 14, 8);
    faceplateLayout->setSpacing(12);

    auto* faceplateText = new QVBoxLayout();
    faceplateText->setSpacing(2);
    faceplateName_ = new QLabel(QStringLiteral("未选择设备"));
    faceplateName_->setProperty("role", QStringLiteral("faceplateName"));
    faceplateMeta_ = new QLabel(QStringLiteral("在左侧选择设备后显示地址与链路"));
    faceplateMeta_->setProperty("role", QStringLiteral("hint"));
    faceplateText->addWidget(faceplateName_);
    faceplateText->addWidget(faceplateMeta_);
    faceplateState_ = new QLabel(QStringLiteral("未连接"));
    faceplateState_->setProperty("role", QStringLiteral("status"));
    faceplateLayout->addLayout(faceplateText, 1);
    faceplateLayout->addWidget(faceplateState_);

    auto* right = new QVBoxLayout();
    right->setSpacing(8);
    right->addWidget(faceplate);
    right->addWidget(waveform_, 1);

    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);
    root->addWidget(leftScroll);
    root->addLayout(right, 1);

    return page;
}

// =============================================================================
// toggleConnection：连接/断开"当前选中设备"
// =============================================================================
void MainWindow::toggleConnection()
{
    if (currentDeviceId_.isEmpty())
        return;

    // 观察员只能查看，不能改变设备连接状态。
    if (auth_ == nullptr || !auth_->can(security::Permission::Operate)) {
        appendLog(QStringLiteral("当前用户无操作权限（观察员只读）"));
        return;
    }

    service::DeviceService* device = currentLink();
    if (device != nullptr && device->isConnected()) {
        manager_->disconnectLink(currentDeviceId_, currentLinkId_);
        return;
    }

    const int type = protocolCombo_->currentData().toInt();
    const QString modeName = type == 0 ? QStringLiteral("自定义协议")
                            : type == 1 ? QStringLiteral("Modbus TCP")
                            : type == 2 ? QStringLiteral("Modbus RTU")
                            : type == 3 ? QStringLiteral("OPC UA")
                                        : QStringLiteral("SCPI 仪器");

    // 用界面上的参数覆盖选中链路的参数，再交给管理器按协议分发。
    const QString linkId = currentLinkId_.isEmpty() ? manager_->mainLinkId(currentDeviceId_)
                                                    : currentLinkId_;
    service::LinkSpec link = manager_->linkSpec(currentDeviceId_, linkId);
    link.id = linkId;
    link.protocol = type == 0 ? QStringLiteral("custom")
                  : type == 1 ? QStringLiteral("modbusTcp")
                  : type == 2 ? QStringLiteral("modbusRtu")
                  : type == 3 ? QStringLiteral("opcUa")
                              : QStringLiteral("scpi");
    if (type == 3) {
        link.endpoint = hostEdit_->text().trimmed();
    } else {
        link.host = hostEdit_->text().trimmed();
        link.port = static_cast<quint16>(portSpin_->value());
        link.unitId = unitSpin_->value();
        link.baudRate = baudCombo_->currentText().toInt();
        if (type == 4) {
            link.scpiQuery = scpiQueryEdit_->text().trimmed();
            link.scpiPollMs = scpiPollSpin_->value();
            link.scpiScale = scpiScaleSpin_->value();
        }
    }

    appendLog(QStringLiteral("[%1/%2] 正在连接 %3（%4）...")
                  .arg(manager_->spec(currentDeviceId_).name, linkId, link.host, modeName));
    manager_->connectLink(currentDeviceId_, linkId, link);
}

// 某条链路连接状态变化：刷新列表项、日志；若是当前设备/链路再刷新右侧状态。
void MainWindow::onDeviceConnectionChanged(const QString& id, const QString& linkId,
                                           bool connected, const QString& detail)
{
    updateDeviceItem(id);

    const service::DeviceSpec spec = manager_->spec(id);
    const QString linkText = QStringLiteral("/%1").arg(linkId);
    appendLog(QStringLiteral("[%1%2] %3").arg(spec.name, linkText, detail));

    // 连接/断开结果写审计（自动连接也会记录，便于还原现场）。
    if (auth_ != nullptr) {
        auth_->audit(connected ? QStringLiteral("连接设备") : QStringLiteral("断开设备"),
                     spec.name + linkText,
                     connected ? QStringLiteral("成功") : QStringLiteral("已断开"), detail);
    }

    // 主链路断开才清空波形；辅助链路断开不影响主链路采集。
    const bool isMain = linkId == manager_->mainLinkId(id);
    if (!connected && isMain) {
        waveform_->clearData();
        resetCapture();
    }

    if (id != currentDeviceId_ || linkId != currentLinkId_)
        return;

    refreshHeader();
    statusLabel_->setText(connected ? QStringLiteral("已连接") : QStringLiteral("未连接"));
    statusLabel_->setToolTip(detail);
    setStatusTone(statusLabel_, connected ? StatusTone::Ok : StatusTone::Neutral);
    connectButton_->setText(connected ? QStringLiteral("断开") : QStringLiteral("连接"));
    setButtonRole(connectButton_,
                  connected ? QStringLiteral("danger") : QStringLiteral("primary"));

    // 连接期间禁止修改地址/端口/协议，避免界面参数与实际连接不一致。
    const int type = protocolCombo_->currentData().toInt();
    hostEdit_->setEnabled(!connected);
    portSpin_->setEnabled(!connected && (type == 0 || type == 1 || type == 4));
    protocolCombo_->setEnabled(!connected);
    unitSpin_->setEnabled(!connected && (type == 1 || type == 2));
    baudCombo_->setEnabled(!connected && type == 2);
    scpiQueryEdit_->setEnabled(!connected && type == 4);
    scpiPollSpin_->setEnabled(!connected && type == 4);
    scpiScaleSpin_->setEnabled(!connected && type == 4);
}

void MainWindow::onDeviceLog(const QString& id, const QString& linkId, const QString& message)
{
    const QString suffix = manager_->linkIds(id).size() > 1
        ? QStringLiteral("/%1").arg(linkId)
        : QString();
    appendLog(QStringLiteral("[%1%2] %3").arg(manager_->spec(id).name, suffix, message));
}

service::DeviceService* MainWindow::currentDevice() const
{
    if (currentDeviceId_.isEmpty())
        return nullptr;
    return manager_->device(currentDeviceId_);
}

service::DeviceService* MainWindow::currentLink() const
{
    if (currentDeviceId_.isEmpty())
        return nullptr;
    return manager_->link(currentDeviceId_, currentLinkId_);
}

bool MainWindow::anyLinkConnected(const QString& id) const
{
    const QStringList links = manager_->linkIds(id);
    for (const QString& linkId : links) {
        const service::DeviceService* service = manager_->link(id, linkId);
        if (service != nullptr && service->isConnected())
            return true;
    }
    return false;
}

// 按 fleet 配置重建左侧设备列表；默认选中第一台。
void MainWindow::refreshDeviceList()
{
    if (deviceList_ == nullptr)
        return;

    {
        QSignalBlocker blocker(deviceList_);
        deviceList_->clear();
        for (const QString& id : manager_->ids()) {
            const service::DeviceSpec spec = manager_->spec(id);
            auto* item = new QListWidgetItem(QStringLiteral("○ %1").arg(spec.name));
            item->setData(Qt::UserRole, id);
            item->setToolTip(QStringLiteral("%1  %2:%3")
                                 .arg(spec.protocol, spec.host)
                                 .arg(spec.port));
            deviceList_->addItem(item);
        }
    }

    if (deviceList_->count() > 0)
        deviceList_->setCurrentRow(0);

    // setCurrentRow 可能因为"行号未变化"不发信号，这里显式触发一次。
    onDeviceSelected(deviceList_->currentRow());
}

void MainWindow::updateDeviceItem(const QString& id)
{
    if (deviceList_ == nullptr)
        return;

    for (int i = 0; i < deviceList_->count(); ++i) {
        QListWidgetItem* item = deviceList_->item(i);
        if (item->data(Qt::UserRole).toString() != id)
            continue;

        const QString name = manager_->spec(id).name;
        const bool connected = anyLinkConnected(id);
        const bool alarmed = alarmCounts_.value(id, 0) > 0;
        item->setText(QStringLiteral("%1 %2%3")
                          .arg(connected ? QStringLiteral("●") : QStringLiteral("○"),
                               name,
                               alarmed ? QStringLiteral(" ⚠") : QString()));
        // 有未恢复报警时用橙色，优先于连接状态色。
        item->setForeground(alarmed ? QColor(0xE6, 0x7E, 0x22)
                                    : connected ? QColor(0x27, 0xAE, 0x60)
                                                : QColor(0x60, 0x60, 0x60));
        return;
    }
}

// 设备 -> 链路下拉：显示 id 与主/辅角色，默认选主链路。
void MainWindow::populateLinkCombo(const QString& id)
{
    if (linkCombo_ == nullptr)
        return;

    QSignalBlocker blocker(linkCombo_);
    linkCombo_->clear();

    const QStringList links = manager_->linkIds(id);
    for (const QString& linkId : links) {
        const service::LinkSpec spec = manager_->linkSpec(id, linkId);
        linkCombo_->addItem(QStringLiteral("%1（%2）")
                                .arg(linkId, spec.isMain() ? QStringLiteral("主链路")
                                                           : QStringLiteral("辅助链路")),
                            linkId);
    }

    currentLinkId_ = manager_->mainLinkId(id);
    if (currentLinkId_.isEmpty())
        currentLinkId_ = links.value(0);
    const int index = linkCombo_->findData(currentLinkId_);
    if (index >= 0)
        linkCombo_->setCurrentIndex(index);
}

// 把设备/链路 spec 填回连接表单（不覆盖用户正在编辑的字段时由调用方保证）。
void MainWindow::applySpecToControls(const QString& id)
{
    const service::LinkSpec spec = manager_->linkSpec(id, currentLinkId_);
    const int type = spec.protocol == QLatin1String("modbusTcp") ? 1
                   : spec.protocol == QLatin1String("modbusRtu") ? 2
                   : spec.protocol == QLatin1String("opcUa") ? 3
                   : spec.protocol == QLatin1String("scpi") ? 4
                                                             : 0;

    protocolCombo_->setCurrentIndex(type);
    if (type == 3) {
        hostEdit_->setText(spec.endpoint.isEmpty()
                               ? QStringLiteral("opc.tcp://%1:%2").arg(spec.host).arg(spec.port)
                               : spec.endpoint);
    } else {
        hostEdit_->setText(spec.host);
    }
    portSpin_->setValue(spec.port);
    unitSpin_->setValue(spec.unitId);
    baudCombo_->setCurrentText(QString::number(spec.baudRate));
    scpiQueryEdit_->setText(spec.scpiQuery);
    scpiPollSpin_->setValue(spec.scpiPollMs);
    scpiScaleSpin_->setValue(spec.scpiScale);

    const service::DeviceService* device = manager_->link(id, currentLinkId_);
    const bool connected = device != nullptr && device->isConnected();
    hostEdit_->setEnabled(!connected);
    portSpin_->setEnabled(!connected && (type == 0 || type == 1 || type == 4));
    protocolCombo_->setEnabled(!connected);
    unitSpin_->setEnabled(!connected && (type == 1 || type == 2));
    baudCombo_->setEnabled(!connected && type == 2);
    scpiQueryEdit_->setEnabled(!connected && type == 4);
    scpiPollSpin_->setEnabled(!connected && type == 4);
    scpiScaleSpin_->setEnabled(!connected && type == 4);
}

void MainWindow::onDeviceSelected(int row)
{
    if (deviceList_ == nullptr || row < 0 || row >= deviceList_->count())
        return;

    // 测试执行期间禁止切换设备，避免命令/采集落到一半换目标。
    if (testPage_ != nullptr && testPage_->isRunning()) {
        appendLog(QStringLiteral("测试执行中，不能切换设备"));
        return;
    }

    const QString id = deviceList_->item(row)->data(Qt::UserRole).toString();
    if (id.isEmpty())
        return;
    if (id == currentDeviceId_) {
        if (linkCombo_ != nullptr && linkCombo_->count() == 0)
            populateLinkCombo(id);
        return;
    }

    currentDeviceId_ = id;
    populateLinkCombo(id);
    applySpecToControls(id);

    // 切换设备后波形从零开始（时间轴/通道数都可能不同）。
    waveform_->clearData();
    applyChannelLimits(manager_->device(id));
    resetCapture();
    pointsLastSecond_ = pointsAdded_; // 采样率统计从 0 重新开始
    if (testPage_ != nullptr)
        testPage_->setDevice(manager_, id);
    if (service::DeviceService* device = manager_->device(id))
        device->setHexLogging(hexLogging_);

    const service::DeviceService* device = manager_->link(id, currentLinkId_);
    const bool connected = device != nullptr && device->isConnected();
    statusLabel_->setText(connected ? QStringLiteral("已连接") : QStringLiteral("未连接"));
    setStatusTone(statusLabel_, connected ? StatusTone::Ok : StatusTone::Neutral);
    connectButton_->setText(connected ? QStringLiteral("断开") : QStringLiteral("连接"));
    setButtonRole(connectButton_,
                  connected ? QStringLiteral("danger") : QStringLiteral("primary"));
    updateStatsLabel();
    refreshHeader();

    appendLog(QStringLiteral("切换到设备 %1").arg(manager_->spec(id).name));
}

void MainWindow::onTestRunningChanged(bool running)
{
    if (deviceList_ != nullptr)
        deviceList_->setEnabled(!running);
    if (dashboardPage_ != nullptr)
        dashboardPage_->setOrderSelectionEnabled(!running);
    if (running)
        appendLog(QStringLiteral("测试开始，设备切换与工单切换已锁定"));
}

// =============================================================================
// applyRolePermissions：按当前登录用户角色统一启用/禁用操作
//
// 观察员：只能看波形/历史/趋势/报警，不能连接、测试、确认、导出；
// 操作员：在观察员基础上可连接、执行测试、确认报警、导出报表；
// 管理员：额外可见"审计"页。
// =============================================================================
void MainWindow::applyRolePermissions()
{
    if (auth_ == nullptr)
        return;

    const security::UserInfo info = auth_->currentUser();
    const bool operate = auth_->can(security::Permission::Operate);
    const bool admin = auth_->can(security::Permission::Administer);

    if (userLabel_ != nullptr) {
        userLabel_->setText(QStringLiteral("用户：%1（%2）")
                                .arg(info.name,
                                     security::AuthService::roleDisplayName(info.role)));
    }

    if (connectButton_ != nullptr) {
        connectButton_->setEnabled(operate);
        connectButton_->setToolTip(operate ? QString()
                                           : QStringLiteral("观察员仅可查看，不能连接设备"));
    }
    if (testPage_ != nullptr)
        testPage_->setOperationAllowed(operate);
    if (dashboardPage_ != nullptr)
        dashboardPage_->setOperationAllowed(operate);
    if (historyPage_ != nullptr)
        historyPage_->setExportAllowed(operate);
    if (shiftPage_ != nullptr)
        shiftPage_->setOperationAllowed(operate);
    if (alarmPage_ != nullptr)
        alarmPage_->setAckAllowed(operate);
    if (tabs_ != nullptr && auditTabIndex_ >= 0)
        tabs_->setTabVisible(auditTabIndex_, admin);
    if (tabs_ != nullptr && userTabIndex_ >= 0)
        tabs_->setTabVisible(userTabIndex_, admin);

    syncNavigation();
    refreshHeader();
}

// 报警页签标题显示未恢复数量，切到别的页也能一眼看到异常。
void MainWindow::updateAlarmTab()
{
    if (tabs_ == nullptr || alarmTabIndex_ < 0)
        return;

    const int count = alarmMonitor_ != nullptr ? alarmMonitor_->openCount() : 0;
    tabs_->setTabText(alarmTabIndex_, count > 0
                                          ? QStringLiteral("报警 (%1)").arg(count)
                                          : QStringLiteral("报警"));
    syncNavigation();
    refreshHeader();
}

// 切换用户：先登出（写审计），再弹出登录框。取消登录视为退出程序。
void MainWindow::switchUser()
{
    if (auth_ == nullptr)
        return;

    if (testPage_ != nullptr && testPage_->isRunning()) {
        appendLog(QStringLiteral("测试执行中，不能切换用户"));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("切换用户"),
                              QStringLiteral("确定退出当前账号并重新登录吗？"))
        != QMessageBox::Yes) {
        return;
    }

    auth_->logout();

    LoginDialog dialog(auth_, this);
    if (dialog.exec() != QDialog::Accepted) {
        appendLog(QStringLiteral("已取消登录，程序退出"));
        close();
        return;
    }

    applyRolePermissions();
    appendLog(QStringLiteral("切换用户：%1（%2）")
                  .arg(auth_->currentUser().name,
                       security::AuthService::roleDisplayName(auth_->currentUser().role)));
}

// 把当前设备的通道阈值/报警限叠加到波形上（虚线）。
void MainWindow::applyChannelLimits(service::DeviceService* device)
{
    QVector<ChannelLimit> limits;
    if (device != nullptr) {
        const core::DeviceConfig& config = device->config();
        limits.reserve(config.channelCount);
        for (int ch = 0; ch < config.channelCount; ++ch) {
            ChannelLimit limit;
            limit.name = config.channelName(ch);
            const core::DeviceChannel& channel =
                ch < config.channels.size() ? config.channels.at(ch)
                                            : core::DeviceChannel{};
            limit.hasHigh = channel.hasHigh;
            limit.high = channel.high;
            limit.hasLow = channel.hasLow;
            limit.low = channel.low;
            limits.append(limit);
        }
    }
    waveform_->setChannelLimits(limits);
}

void MainWindow::resetCapture()
{
    captureActive_ = false;
    captureReady_ = false;
    captureChannelCount_ = 0;
    captureDeviceName_.clear();
    captureChannelNames_.clear();
    captureTimes_.clear();
    captureValues_.clear();
    if (captureButton_ != nullptr) {
        captureButton_->setText(QStringLiteral("捕获波形…"));
        captureLabel_->setText(QStringLiteral("未捕获"));
    }
}

bool MainWindow::saveCaptureCsv()
{
    if (!captureReady_ || captureTimes_.empty() || captureChannelCount_ <= 0)
        return false;

    QString base = QStringLiteral("waveform_%1_%2.csv")
                       .arg(captureDeviceName_.isEmpty() ? QStringLiteral("device")
                                                         : captureDeviceName_)
                       .arg(QDateTime::currentDateTime()
                                .toString(QStringLiteral("yyyyMMdd_HHmmss")));
    base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")),
                 QStringLiteral("_"));

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存原始波形"), base,
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty())
        return false;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        appendLog(QStringLiteral("波形保存失败：%1").arg(file.errorString()));
        return false;
    }

    // UTF-8 BOM：Excel 直接打开中文通道名不乱码。
    file.write("\xEF\xBB\xBF");

    QString header = QStringLiteral("time_s");
    for (const QString& name : captureChannelNames_)
        header += QLatin1Char(',') + name;
    file.write((header + QLatin1String("\r\n")).toUtf8());

    const int channels = captureChannelCount_;
    for (std::size_t row = 0; row < captureTimes_.size(); ++row) {
        QString line = QString::number(captureTimes_[row], 'f', 6);
        for (int ch = 0; ch < channels; ++ch) {
            line += QLatin1Char(',')
                 + QString::number(captureValues_[row * static_cast<std::size_t>(channels)
                                                  + static_cast<std::size_t>(ch)]);
        }
        line += QLatin1String("\r\n");
        file.write(line.toUtf8());
    }
    file.close();

    appendLog(QStringLiteral("原始波形已导出：%1（%2 点 × %3 通道）")
                  .arg(path)
                  .arg(captureTimes_.size())
                  .arg(channels));
    captureLabel_->setText(
        QStringLiteral("已保存：%1").arg(QFileInfo(path).fileName()));
    captureReady_ = false;
    captureTimes_.clear();
    captureValues_.clear();
    captureChannelCount_ = 0;
    captureButton_->setText(QStringLiteral("捕获波形…"));
    return true;
}

void MainWindow::toggleWaveformCapture()
{
    if (captureActive_) {
        // 停止并立即保存。
        captureActive_ = false;
        captureReady_ = !captureTimes_.empty();
        captureButton_->setText(QStringLiteral("保存波形…"));
        captureLabel_->setText(QStringLiteral("已捕获，选择文件保存"));
        if (captureReady_)
            saveCaptureCsv();
        return;
    }

    if (captureReady_) {
        if (!saveCaptureCsv())
            return;
        return;
    }

    service::DeviceService* device = currentDevice();
    if (device == nullptr || !device->isConnected()) {
        appendLog(QStringLiteral("请先连接设备再捕获原始波形"));
        captureLabel_->setText(QStringLiteral("设备未连接"));
        return;
    }

    resetCapture();
    captureActive_ = true;
    captureDeviceName_ = manager_->spec(currentDeviceId_).name;
    const core::DeviceConfig& config = device->config();
    for (int ch = 0; ch < config.channelCount; ++ch) {
        QString name = config.channelName(ch);
        const QString unit = config.channelUnit(ch);
        if (!unit.isEmpty())
            name += QStringLiteral("(%1)").arg(unit);
        captureChannelNames_.append(name);
    }
    captureButton_->setText(QStringLiteral("停止并保存"));
    captureLabel_->setText(QStringLiteral("捕获中…"));
    appendLog(QStringLiteral("开始捕获原始波形（最多 20 秒）"));
}

// 日常点检：对配置了 check 的通道采一段窗口，比较金样值与容差。
void MainWindow::runDailyCheck()
{
    service::DeviceService* device = currentDevice();
    if (device == nullptr || !device->isConnected()) {
        appendLog(QStringLiteral("请先连接设备再做日常点检"));
        return;
    }

    const core::DeviceConfig& config = device->config();
    QStringList lines;
    bool anyChecked = false;
    bool allPass = true;

    for (int ch = 0; ch < config.channelCount; ++ch) {
        const core::DeviceChannel& channel = config.channels.at(ch);
        if (!channel.hasCheck)
            continue;

        anyChecked = true;
        const std::vector<std::int16_t> samples = device->sampleWindow(ch, 500);
        if (samples.empty()) {
            lines << QStringLiteral("%1：无数据").arg(config.channelName(ch));
            allPass = false;
            continue;
        }

        double sum = 0.0;
        for (std::int16_t value : samples)
            sum += value;
        const double average = sum / static_cast<double>(samples.size());
        const bool pass =
            std::abs(average - channel.checkValue) <= channel.checkTolerance;
        allPass = allPass && pass;
        lines << QStringLiteral("%1：实测 %2，参考 %3 ± %4 → %5")
                     .arg(config.channelName(ch))
                     .arg(average, 0, 'f', 1)
                     .arg(channel.checkValue, 0, 'f', 1)
                     .arg(channel.checkTolerance, 0, 'f', 1)
                     .arg(pass ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    }

    if (!anyChecked) {
        QMessageBox::information(
            this, QStringLiteral("日常点检"),
            QStringLiteral("当前设备未配置点检通道。\n"
                           "在 configs/devices/active.json 的通道里增加 "
                           "\"check\" 与 \"checkTolerance\" 后重试。"));
        return;
    }

    const QString detail = lines.join(QLatin1Char('\n'));
    QMessageBox::information(this, QStringLiteral("日常点检"),
                             detail + (allPass ? QStringLiteral("\n\n结论：PASS")
                                               : QStringLiteral("\n\n结论：FAIL")));
    appendLog(QStringLiteral("日常点检 %1：%2")
                  .arg(allPass ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                       QString(detail).replace(QLatin1Char('\n'),
                                               QStringLiteral(" | "))));
    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("日常点检"), stationConfig_.stationId,
                     allPass ? QStringLiteral("通过") : QStringLiteral("失败"), detail);
    }
}

// 报文诊断开关：对所有设备统一生效，切换设备后也会重新应用。
void MainWindow::onHexLoggingToggled(bool enabled)
{
    hexLogging_ = enabled;
    if (service::DeviceService* device = currentDevice())
        device->setHexLogging(enabled);
    appendLog(enabled ? QStringLiteral("报文诊断已开启（日志显示收发 HEX）")
                      : QStringLiteral("报文诊断已关闭"));
}

// =============================================================================
// onTick：界面刷新主循环（每 50ms）
//
// 1. 从环形缓冲批量取数据
// 2. 按时间抽稀（300Hz）
// 3. 把抽稀后的点交给 WaveformView（只入队，不立即绘制）
// 4. 统一触发一次重绘
// =============================================================================
void MainWindow::onTick()
{
    service::DeviceService* device = currentDevice();
    if (device == nullptr)
        return;

    // 消费当前可读的所有数据块。每块只做一次"按像素桶聚合"，不逐点重绘。
    bool appended = false;
    device->ring()->consume([this, device, &appended](const core::ChunkView& chunk) {
        waveform_->appendChunk(chunk.startSec, chunk.samples, chunk.pointCount,
                               chunk.channelCount, device->sampleRateHz());
        pointsAdded_ += static_cast<std::uint64_t>(chunk.pointCount);
        appended = true;

        // 原始波形捕获：把消费到的块原样累积，供导出 CSV（不额外占用通信线程）。
        if (captureActive_) {
            const int rate = std::max(1, device->sampleRateHz());
            captureChannelCount_ = chunk.channelCount;
            captureTimes_.reserve(captureTimes_.size()
                                  + static_cast<std::size_t>(chunk.pointCount));
            for (int i = 0; i < chunk.pointCount; ++i) {
                captureTimes_.push_back(chunk.startSec
                                        + static_cast<double>(i) / rate);
                const std::int16_t* point =
                    chunk.samples + static_cast<std::size_t>(i) * chunk.channelCount;
                captureValues_.insert(captureValues_.end(), point,
                                      point + chunk.channelCount);
            }

            const qint64 maxPoints = static_cast<qint64>(rate) * 20; // 最多 20 秒
            if (static_cast<qint64>(captureTimes_.size()) >= maxPoints) {
                captureActive_ = false;
                captureReady_ = true;
                captureButton_->setText(QStringLiteral("保存波形…"));
                captureLabel_->setText(QStringLiteral("已捕获 20 秒，点击保存"));
            } else {
                captureLabel_->setText(
                    QStringLiteral("捕获中：%1 点（约 %2 秒）")
                        .arg(captureTimes_.size())
                        .arg(captureTimes_.size() / static_cast<double>(rate), 0, 'f', 1));
            }
        }
    });

    // 一批数据只触发一次重绘，而不是每块都 update()。
    if (appended)
        waveform_->update();

    // 每约 1 秒刷新一次统计文字（20 次 tick）。
    static int tickCounter = 0;
    if (++tickCounter % 20 == 0) {
        updateStatsLabel();
        refreshLinkHealth();
        refreshHeader();
    }
}

// =============================================================================
// updateStatsLabel：显示采样速率与链路统计
// =============================================================================
void MainWindow::updateStatsLabel()
{
    service::DeviceService* device = currentDevice();
    if (device == nullptr) {
        statsLabel_->setText(QStringLiteral("暂无设备"));
        return;
    }
    if (!device->isConnected()) {
        statsLabel_->setText(QStringLiteral("未连接：点击上方“连接”后显示实时采样率与统计"));
        return;
    }

    const auto& stats = device->stats();

    // 两次刷新之间的新增点数差 ≈ 最近 1 秒的采样点数。
    const std::uint64_t rate = pointsAdded_ - pointsLastSecond_;
    pointsLastSecond_ = pointsAdded_;

    statsLabel_->setText(QStringLiteral("采样率: %1 kS/s\n通道数: %2\n数据帧: %3\nCRC 错误: %4\n丢块: %5\n接收: %6 KB")
                             .arg(rate / 1000.0, 0, 'f', 1)
                             .arg(device->config().channelCount)
                             .arg(stats.frames)
                             .arg(stats.crcErrors)
                             .arg(stats.dropped)
                             .arg(stats.bytes / 1024.0, 0, 'f', 1));
}

// 链路健康面板：每链路在线状态、最近入站延迟与累计帧数。
void MainWindow::refreshLinkHealth()
{
    if (linkHealthTable_ == nullptr || manager_ == nullptr || currentDeviceId_.isEmpty())
        return;

    const QVector<service::LinkHealth> items = manager_->linkHealth(currentDeviceId_);

    auto setText = [this](int row, int column, const QString& text) {
        QTableWidgetItem* item = linkHealthTable_->item(row, column);
        if (item == nullptr) {
            item = new QTableWidgetItem();
            linkHealthTable_->setItem(row, column, item);
        }
        item->setText(text);
    };

    if (items.isEmpty()) {
        linkHealthTable_->setRowCount(1);
        setText(0, 0, QStringLiteral("暂无链路数据"));
        setText(0, 1, QString());
        setText(0, 2, QString());
        setText(0, 3, QString());
        setText(0, 4, QString());
        return;
    }

    linkHealthTable_->setRowCount(items.size());
    for (int row = 0; row < items.size(); ++row) {
        const service::LinkHealth& health = items.at(row);

        setText(row, 0, health.linkId);
        setText(row, 1, health.protocol);
        setText(row, 2, health.connected ? QStringLiteral("在线") : QStringLiteral("离线"));
        setText(row, 3, health.lastInboundAgeMs < 0
                       ? QStringLiteral("--")
                       : QStringLiteral("%1 ms").arg(health.lastInboundAgeMs));
        setText(row, 4, QString::number(health.frames));

        if (QTableWidgetItem* status = linkHealthTable_->item(row, 2)) {
            status->setForeground(health.connected ? QColor(QStringLiteral("#2e7d5b"))
                                                   : QColor(QStringLiteral("#b3261e")));
        }
    }
}

// 打开"快速上手指南"。首次启动会自动弹一次（见构造函数）。
void MainWindow::showQuickStart()
{
    HelpDialog dialog(this);
    dialog.exec();
}

// 日志统一加时间戳，方便事后定位问题。
void MainWindow::appendLog(const QString& message)
{
    logView_->appendPlainText(QStringLiteral("[%1] %2")
                                  .arg(QTime::currentTime().toString("HH:mm:ss"))
                                  .arg(message));
    // 同步写文件日志：界面只保留最近 500 行，现场排障需要完整历史。
    qInfo().noquote() << message;
}

} // namespace aegis::ui
