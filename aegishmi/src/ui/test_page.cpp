#include "ui/test_page.h"

#include "service/bom_verifier.h"   // 工位侧物料防错（扫码 + BOM 只读查询）
#include "service/mes_station_checker.h" // MES 过站/工单校验
#include <QEventLoop>
#include <QTimer>
#include "service/device_manager.h" // 多链路路由（testDevice）
#include "service/device_service.h" // 设备服务（ITestDevice 的生产实现）
#include "quality/spc.h"            // Cpk 联动停线
#include "security/auth_service.h"  // 操作审计
#include "storage/test_repository.h" // 运行结果落库
#include "test/flow_loader.h"       // 加载 JSON
#include "test/flow_validator.h"    // 校验规则
#include "test/test_engine.h"       // 执行引擎
#include "ui/flow_editor_dialog.h"  // 图形化方案编辑
#include "ui/label_report.h"        // 合格证标签
#include "ui/sop_dialog.h"          // 电子作业指导书

#include <atomic>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "ui/theme.h"

namespace aegis::ui {

// =============================================================================
// DeviceProxy —— 把 ITestDevice 调用转发到"当前选中的设备"
//
// 引擎在构造时绑定的是代理对象；界面切换设备只更新代理内部的原子指针，
// 不用重建引擎线程。运行期切换由界面禁止（见 runningChanged 信号）。
// =============================================================================
class DeviceProxy final : public test::ITestDevice {
public:
    void setDevice(test::ITestDevice* device) { device_.store(device); }

    bool sendCommand(test::DeviceCommand command, int channel, int value,
                     int timeoutMs, quint8& status,
                     const QString& linkId) override
    {
        test::ITestDevice* device = device_.load();
        return device != nullptr
            && device->sendCommand(command, channel, value, timeoutMs, status, linkId);
    }

    bool queryText(const QString& cmd, int timeoutMs,
                   QString* value, QString* errorMessage,
                   const QString& linkId) override
    {
        test::ITestDevice* device = device_.load();
        if (device == nullptr) {
            if (errorMessage)
                *errorMessage = QStringLiteral("未选择设备");
            return false;
        }
        return device->queryText(cmd, timeoutMs, value, errorMessage, linkId);
    }

    std::vector<std::int16_t> sampleWindow(int channel, int windowMs,
                                           const QString& linkId) override
    {
        test::ITestDevice* device = device_.load();
        return device == nullptr ? std::vector<std::int16_t>{}
                                 : device->sampleWindow(channel, windowMs, linkId);
    }

    int sampleRateHz(const QString& linkId) const override
    {
        test::ITestDevice* device = device_.load();
        return device == nullptr ? protocol::kSampleRateHz : device->sampleRateHz(linkId);
    }

private:
    std::atomic<test::ITestDevice*> device_{nullptr};
};

namespace {

// 表格列定义：序号 / 步骤 / 状态 / 实测值 / 判定 / 耗时
constexpr int kColumnCount = 6;

QString statusText(test::StepStatus status)
{
    switch (status) {
    case test::StepStatus::NotRun:
        return QStringLiteral("等待");
    case test::StepStatus::Pass:
        return QStringLiteral("通过");
    case test::StepStatus::Fail:
        return QStringLiteral("失败");
    case test::StepStatus::Skipped:
        return QStringLiteral("跳过");
    case test::StepStatus::Aborted:
        return QStringLiteral("中止");
    }
    return QStringLiteral("—");
}

QString verdictText(test::StepStatus status)
{
    switch (status) {
    case test::StepStatus::Pass:
        return QStringLiteral("PASS");
    case test::StepStatus::Fail:
        return QStringLiteral("FAIL");
    case test::StepStatus::Skipped:
        return QStringLiteral("SKIP");
    case test::StepStatus::Aborted:
        return QStringLiteral("ABORT");
    default:
        return QStringLiteral("—");
    }
}

QColor statusBackground(test::StepStatus status)
{
    switch (status) {
    case test::StepStatus::Pass:
        return QColor(0xE8, 0xF5, 0xE9); // 浅绿
    case test::StepStatus::Fail:
        return QColor(0xFD, 0xEC, 0xEA); // 浅红
    case test::StepStatus::Skipped:
        return QColor(0xFF, 0xF8, 0xE1); // 浅黄
    case test::StepStatus::Aborted:
        return QColor(0xFB, 0xE9, 0xE7); // 浅橙
    default:
        return QColor(Qt::white);
    }
}

} // namespace

TestPage::TestPage(storage::TestRepository* repository,
                   security::AuthService* auth,
                   const core::StationConfig& station, QWidget* parent)
    : QWidget(parent)
    , repository_(repository)
    , auth_(auth)
    , station_(station)
{
    // 跨线程信号携带自定义类型，必须注册到 Qt 元类型系统。
    qRegisterMetaType<test::StepResult>();
    qRegisterMetaType<test::RunResult>();

    buildUi();

    // 物料防错：判定与审计由 BomVerifier 负责，界面只做提示。
    bom_ = new service::BomVerifier(station_, nullptr, this);
    connect(bom_, &service::BomVerifier::lockChanged, this,
            [this](bool locked, const QString& reason) {
                if (locked) {
                    materialInfo_->setStyleSheet(QStringLiteral("color:#c0392b; font-weight:bold"));
                    materialInfo_->setText(QStringLiteral("工位已锁定：%1").arg(reason));
                } else {
                    materialInfo_->setStyleSheet(QStringLiteral("color:#27ae60"));
                    materialInfo_->setText(QStringLiteral("料号核对通过，可开始测试"));
                }
                updateButtons();
            });
    connect(bom_, &service::BomVerifier::auditRequested, this,
            [this](const QString& action, const QString& target,
                   const QString& result, const QString& detail) {
                if (auth_ != nullptr)
                    auth_->audit(action, target, result, detail);
            });
    connect(bom_, &service::BomVerifier::logMessage, this,
            [this](const QString& message) {
                if (bom_->locked())
                    return; // 锁定提示已由 lockChanged 展示
                materialInfo_->setStyleSheet(QStringLiteral("color:#2c3e50"));
                materialInfo_->setText(message);
            });
    materialInfo_->setText(bom_->enabled() ? QStringLiteral("待扫码核对料号")
                                           : QStringLiteral("物料防错：未配置"));
    unlockButton_->setVisible(bom_->enabled());

    // 引擎创建在主线程后再搬到工作线程；之后 run() 会在线程内阻塞执行。
    proxy_ = new DeviceProxy();
    engine_ = new test::TestEngine(proxy_, &clock_);

    // TP-14：操作员确认。引擎在工作线程，弹窗必须转回 GUI 线程并阻塞等待结果。
    engine_->setPromptHandler([this](const QString& text, int timeoutMs) {
        bool confirmed = false;
        QMetaObject::invokeMethod(
            this,
            [this, text, timeoutMs, &confirmed]() {
                QMessageBox box(QMessageBox::Question, QStringLiteral("操作员确认"), text,
                                QMessageBox::Ok | QMessageBox::Cancel, this);
                box.button(QMessageBox::Ok)->setText(QStringLiteral("确认"));
                box.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
                bool timedOut = false;
                if (timeoutMs > 0) {
                    auto* timer = new QTimer(&box);
                    timer->setSingleShot(true);
                    connect(timer, &QTimer::timeout, &box, [&box, &timedOut]() {
                        timedOut = true;
                        box.reject();
                    });
                    timer->start(timeoutMs);
                }
                confirmed = (box.exec() == QMessageBox::Ok) && !timedOut;
            },
            Qt::BlockingQueuedConnection);
        return confirmed;
    });

    engine_->moveToThread(&engineThread_);
    connect(&engineThread_, &QThread::finished, engine_, &QObject::deleteLater);
    connect(engine_, &test::TestEngine::stepStarted,
            this, &TestPage::onStepStarted, Qt::QueuedConnection);
    connect(engine_, &test::TestEngine::stepFinished,
            this, &TestPage::onStepFinished, Qt::QueuedConnection);
    connect(engine_, &test::TestEngine::runFinished,
            this, &TestPage::onRunFinished, Qt::QueuedConnection);
    engineThread_.start();

    onDeviceStateChanged(false, QStringLiteral("未选择设备"));
}

void TestPage::setOperationAllowed(bool allowed)
{
    operationAllowed_ = allowed;
    updateButtons();
}

// 切换生产工单。运行期间由界面禁止切换，这里再兜底一次。
void TestPage::setWorkOrder(const QString& orderNo, const QString& product)
{
    if (running_ && orderNo != workOrderNo_)
        return;

    const bool changed = orderNo != workOrderNo_;
    workOrderNo_ = orderNo;
    workOrderProduct_ = product;
    updateWorkOrderInfo();

    // 工单切换后料号需重新扫码核对（锁站状态仍由班组长解锁）。
    if (changed && materialEdit_ != nullptr) {
        materialEdit_->clear();
        if (materialInfo_ != nullptr && bom_ != nullptr) {
            materialInfo_->setStyleSheet(QStringLiteral("color:#2c3e50"));
            materialInfo_->setText(bom_->enabled() ? QStringLiteral("待扫码核对料号")
                                                   : QStringLiteral("物料防错：未配置"));
        }
    }

    if (workOrderNo_.isEmpty()) {
        snEdit_->clear();
        return;
    }

    // 工单切换时自动加载绑定方案；同一工单的重复设置不覆盖手工加载的方案。
    if (!changed || running_ || repository_ == nullptr || !repository_->isOpen())
        return;

    const storage::WorkOrder order = repository_->findWorkOrder(workOrderNo_);
    if (order.flowPath.isEmpty())
        return;

    if (loadFlowFile(order.flowPath, false)) {
        summary_->setStyleSheet(QStringLiteral("color:#27ae60"));
        summary_->setText(QStringLiteral("已按工单自动加载方案：%1").arg(flow_.name));
    } else {
        summary_->setStyleSheet(QStringLiteral("color:#c0392b"));
        summary_->setText(QStringLiteral("工单绑定的方案不可用：%1\n请检查配置或手工加载")
                              .arg(order.flowPath));
    }
}

void TestPage::updateWorkOrderInfo()
{
    if (workOrderInfo_ == nullptr)
        return;

    if (workOrderNo_.isEmpty()) {
        workOrderInfo_->setStyleSheet(QStringLiteral("color:#c0392b"));
        workOrderInfo_->setText(QStringLiteral("未选择工单\n请到「生产」页新建/选择工单"));
        return;
    }

    // 显示绑定的方案文件名，换线时一眼能确认"工单跑的是哪个程序"。
    QString binding = QStringLiteral("\n方案：未绑定（手工加载）");
    if (repository_ != nullptr && repository_->isOpen()) {
        const storage::WorkOrder order = repository_->findWorkOrder(workOrderNo_);
        if (!order.flowPath.isEmpty())
            binding = QStringLiteral("\n方案：%1")
                          .arg(QFileInfo(order.flowPath).fileName());
    }

    workOrderInfo_->setStyleSheet(QStringLiteral("color:#2c3e50"));
    workOrderInfo_->setText(QStringLiteral("工单：%1\n产品：%2%3")
                                .arg(workOrderNo_,
                                     workOrderProduct_.isEmpty() ? QStringLiteral("—")
                                                                 : workOrderProduct_,
                                     binding));
}

bool TestPage::validateSn(const QString& sn, QString* error) const
{
    if (sn.isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("请扫描或输入产品 SN");
        return false;
    }

    if (!station_.snPattern.isEmpty()) {
        const QRegularExpression pattern(station_.snPattern);
        if (pattern.isValid() && !pattern.match(sn).hasMatch()) {
            if (error != nullptr) {
                *error = QStringLiteral("SN 格式不符合工位规则：%1")
                             .arg(station_.snPattern);
            }
            return false;
        }
    }
    return true;
}

// 治具寿命/校准检查：不通过时给出可读原因。
bool TestPage::checkFixtureReady(QString* error)
{
    if (station_.fixtureId.isEmpty() || repository_ == nullptr
        || !repository_->isOpen()) {
        return true;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const storage::FixtureStatus status =
        repository_->fixtureStatus(station_.fixtureId);
    const qint64 due = status.calibratedUntil > 0
                           ? status.calibratedUntil
                           : station_.defaultFixtureDueMs(nowMs);

    if (due > 0 && nowMs > due) {
        if (error != nullptr) {
            *error = QStringLiteral("治具 %1 校准已过期（%2），请先校准")
                         .arg(station_.fixtureId,
                              QDateTime::fromMSecsSinceEpoch(due)
                                  .toString(QStringLiteral("yyyy-MM-dd")));
        }
        return false;
    }

    if (station_.fixtureMaxRuns > 0 && status.runCount >= station_.fixtureMaxRuns) {
        if (error != nullptr) {
            *error = QStringLiteral("治具 %1 已达寿命（%2/%3），请更换或复位")
                         .arg(station_.fixtureId)
                         .arg(status.runCount)
                         .arg(station_.fixtureMaxRuns);
        }
        return false;
    }
    return true;
}

// Andon 停线：管理员解除前禁止继续测试。
bool TestPage::checkAndon(QString* error)
{
    if (!station_.andonEnabled || repository_ == nullptr || !repository_->isOpen())
        return true;

    const storage::AndonState state = repository_->andonState(station_.stationId);
    if (!state.locked)
        return true;

    if (error != nullptr) {
        *error = QStringLiteral("工位已 Andon 停线：%1\n%2\n请管理员在「生产」页解除后继续")
                     .arg(state.reason,
                          state.detail.isEmpty() ? QStringLiteral("—") : state.detail);
    }
    return false;
}

// MES 过站/工单校验：未配置接口时直接放行；失败策略与物料核对一致。
bool TestPage::checkMesStation(const QString& workOrder, const QString& sn,
                               const QString& product, QString* error)
{
    service::MesStationChecker checker(station_);
    if (!checker.enabled())
        return true;

    connect(&checker, &service::MesStationChecker::auditRequested, this,
            [this](const QString& action, const QString& target,
                   const QString& result, const QString& detail) {
                if (auth_ != nullptr)
                    auth_->audit(action, target, result, detail);
            });
    connect(&checker, &service::MesStationChecker::logMessage, this,
            [this](const QString& message) {
                summary_->setStyleSheet(QStringLiteral("color:#2c3e50"));
                summary_->setText(message);
            });

    bool done = false;
    service::StationCheckResult captured;
    QEventLoop loop;
    connect(&checker, &service::MesStationChecker::checked, &loop,
            [&](const service::StationCheckResult& result) {
                captured = result;
                done = true;
                loop.quit();
            });

    summary_->setStyleSheet(QStringLiteral("color:#2c3e50"));
    summary_->setText(QStringLiteral("正在向 MES 校验过站…"));
    // 兜底超时：请求自身的 transferTimeout 之外再留 1 秒余量。
    QTimer::singleShot(station_.mesTimeoutMs + 1000, &loop, &QEventLoop::quit);
    checker.check(workOrder, sn, product,
                  auth_ != nullptr ? auth_->currentUser().name : QString(),
                  station_.fixtureId);
    loop.exec();

    if (!done) {
        // 超时：按 failOpen 决定放行或拦截。
        if (station_.mesFailOpen) {
            if (auth_ != nullptr) {
                auth_->audit(QStringLiteral("过站校验"), sn, QStringLiteral("降级放行"),
                             QStringLiteral("MES 无响应，超时后放行"));
            }
            return true;
        }
        if (error != nullptr)
            *error = QStringLiteral("MES 过站校验超时，请检查网络后重试");
        return false;
    }

    if (captured.ok && captured.passed)
        return true;
    if (!captured.ok && station_.mesFailOpen)
        return true;

    if (error != nullptr) {
        *error = captured.message.isEmpty()
            ? QStringLiteral("MES 未允许该 SN 过站")
            : QStringLiteral("MES 过站校验未通过：%1").arg(captured.message);
    }
    return false;
}

// 操作员资质：按产品白名单校验（未配置该产品时视为允许）。
bool TestPage::checkQualification(const QString& product, QString* error) const{
    const QString user = auth_ != nullptr ? auth_->currentUser().name
                                          : QStringLiteral("—");
    if (station_.isOperatorQualified(product, user))
        return true;

    if (error != nullptr) {
        *error = QStringLiteral("操作员 %1 没有产品 %2 的操作资质，请联系管理员授权")
                     .arg(user, product.isEmpty() ? QStringLiteral("—") : product);
    }
    return false;
}

// 电子 SOP：同一工单 + 产品 + 版本只需确认一次。
bool TestPage::ensureSopRead(const QString& product)
{
    const core::SopDocument* document = station_.sopForProduct(product);
    if (document == nullptr)
        return true;

    const QString key = QStringLiteral("%1|%2|%3")
                            .arg(workOrderNo_,
                                 document->product.isEmpty() ? QStringLiteral("*")
                                                             : document->product,
                                 document->version);
    if (sopConfirmed_.contains(key))
        return true;

    SopDialog dialog(*document, station_.sopRequireRead, this);
    if (dialog.exec() != QDialog::Accepted) {
        summary_->setStyleSheet(QStringLiteral("color:#c0392b"));
        summary_->setText(QStringLiteral("未确认阅读 SOP，已取消测试"));
        return false;
    }

    sopConfirmed_.insert(key);
    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("阅读SOP"), document->title,
                     QStringLiteral("已确认"),
                     QStringLiteral("版本=%1 工单=%2")
                         .arg(document->version, workOrderNo_));
    }
    return true;
}

// 落库后评估 Andon：连续 FAIL / FPY 低于阈值 / Cpk 不足时锁定工位。
void TestPage::evaluateAndon(const QString& verdict)
{
    Q_UNUSED(verdict)
    if (repository_ == nullptr || !repository_->isOpen() || workOrderNo_.isEmpty())
        return;
    if (!station_.andonEnabled && !station_.spcAndonEnabled)
        return;

    // 已经停线（例如另一条规则先触发）就不重复写事件。
    if (repository_->andonState(station_.stationId).locked)
        return;

    QString detail;
    if (station_.andonEnabled) {
        const int streak = repository_->consecutiveFailRuns(workOrderNo_);
        const storage::WorkOrderStats stats = repository_->statsOfOrder(workOrderNo_);
        if (streak >= station_.andonConsecutiveFails) {
            detail = QStringLiteral("连续 %1 件 FAIL（阈值 %2 件）")
                         .arg(streak)
                         .arg(station_.andonConsecutiveFails);
        } else if (stats.units >= station_.andonMinUnits
                   && stats.fpy * 100.0 < station_.andonFpyThreshold) {
            detail = QStringLiteral("FPY %1%（低于阈值 %2%%，产量 %3）")
                         .arg(stats.fpy * 100.0, 0, 'f', 1)
                         .arg(station_.andonFpyThreshold, 0, 'f', 1)
                         .arg(stats.units);
        }
    }

    // SPC 判异联动：任一测试项样本足够且 Cpk 低于阈值就停线。
    if (detail.isEmpty() && station_.spcAndonEnabled && flowValid_) {
        for (const test::TestStep& step : flow_.steps) {
            if (step.action != test::StepAction::Measure)
                continue;
            if (!step.expect.hasMin && !step.expect.hasMax)
                continue;

            const QVector<storage::SpcPoint> points = repository_->spcPoints(
                workOrderNo_, flow_.id, step.id, 100);
            if (points.size() < station_.spcMinSamples)
                continue;

            QVector<double> values;
            values.reserve(points.size());
            for (const storage::SpcPoint& point : points)
                values.append(point.value);

            const quality::SpcResult result = quality::compute(
                values, step.expect.hasMax, step.expect.max,
                step.expect.hasMin, step.expect.min);
            if (result.hasCpk && result.cpk < station_.spcCpkThreshold) {
                detail = QStringLiteral("过程能力不足：%1 Cpk=%2（阈值 %3，n=%4）")
                             .arg(step.name)
                             .arg(result.cpk, 0, 'f', 2)
                             .arg(station_.spcCpkThreshold, 0, 'f', 2)
                             .arg(points.size());
                break;
            }
        }
    }

    if (detail.isEmpty())
        return;

    if (!repository_->lockStation(station_.stationId,
                                  QStringLiteral("Andon 停线"), detail)) {
        return;
    }

    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("Andon停线"), station_.stationId,
                     QStringLiteral("已锁定"), detail);
    }
    QMessageBox::warning(this, QStringLiteral("Andon 停线"),
                         QStringLiteral("工位已自动停线：%1\n\n"
                                        "请排查原因，并由管理员在「生产」页解除。")
                             .arg(detail));
}

// 按钮状态集中在一次计算里，避免各处漏判"运行中/无权限/未连接"。
void TestPage::updateButtons(){
    loadButton_->setEnabled(!running_ && operationAllowed_);
    editButton_->setEnabled(!running_ && operationAllowed_ && flowValid_);
    stepButton_->setEnabled(!running_ && operationAllowed_ && flowValid_
                            && device_ != nullptr && device_->isConnected());
    startButton_->setEnabled(!running_ && operationAllowed_ && flowValid_
                             && device_ != nullptr && device_->isConnected());
    stopButton_->setEnabled(running_ && operationAllowed_);
    snEdit_->setEnabled(!running_);

    const bool bomEnabled = bom_ != nullptr && bom_->enabled();
    materialEdit_->setEnabled(!running_ && bomEnabled);
    materialButton_->setEnabled(!running_ && bomEnabled);
    unlockButton_->setVisible(bomEnabled);
    unlockButton_->setEnabled(bomEnabled && bom_->locked());
}

void TestPage::setDevice(service::DeviceManager* manager, const QString& deviceId)
{
    service::DeviceService* device = manager != nullptr ? manager->device(deviceId) : nullptr;
    test::ITestDevice* target = manager != nullptr ? manager->testDevice(deviceId) : nullptr;
    if (device_ == device)
        return;

    // 只断开本页与该设备之间的连接，不影响其它接收者。
    if (device_ != nullptr) {
        disconnect(device_, &service::DeviceService::connectedChanged,
                   this, &TestPage::onDeviceStateChanged);
    }

    device_ = device;
    proxy_->setDevice(target);

    if (device_ != nullptr) {
        connect(device_, &service::DeviceService::connectedChanged,
                this, &TestPage::onDeviceStateChanged);
        onDeviceStateChanged(device_->isConnected(),
                             device_->isConnected() ? QStringLiteral("已连接")
                                                    : QStringLiteral("未连接"));
    } else {
        onDeviceStateChanged(false, QStringLiteral("不可用"));
    }
}

TestPage::~TestPage()
{
    // 先请求停止（原子标志），再退出线程事件循环。
    if (engine_ != nullptr)
        engine_->requestStop();
    engineThread_.quit();
    engineThread_.wait(3000);
    engine_ = nullptr; // 由线程 finished -> deleteLater 回收

    delete proxy_;
    proxy_ = nullptr;
}

void TestPage::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    // ---- 流程提示条：先做什么、再做什么，一眼可见 ----
    auto* banner = new QFrame();
    banner->setProperty("role", QStringLiteral("card"));
    auto* bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(14, 10, 14, 10);
    auto* bannerText = new QLabel(QStringLiteral(
        "测试流程：① 加载方案 → ② 填写/扫描 SN → ③ 开始测试。"
        "运行中可“停止”；“单步调试”用于逐步排查配置问题。"));
    bannerText->setWordWrap(true);
    bannerLayout->addWidget(bannerText, 1);
    root->addWidget(banner);

    auto* content = new QHBoxLayout();
    content->setSpacing(12);

    // ---- 左栏：生产工单 + 方案加载与信息 ----
    auto* left = new QVBoxLayout();
    left->setSpacing(6);

    left->addWidget(makeSectionTitle(QStringLiteral("① 选择方案")));
    left->addWidget(new QLabel(QStringLiteral("生产工单")));
    workOrderInfo_ = new QLabel();
    workOrderInfo_->setWordWrap(true);
    workOrderInfo_->setToolTip(QStringLiteral(
        "来自生产看板当前选中的工单；测试结果会挂在该工单下，运行时不可切换。"));
    left->addWidget(workOrderInfo_);
    left->addSpacing(6);

    loadButton_ = makeButton(QStringLiteral("加载方案…"), QStringLiteral("primary"));
    loadButton_->setToolTip(QStringLiteral("选择测试方案 JSON（通常位于 configs/flows 或上次使用的目录）"));
    editButton_ = makeButton(QStringLiteral("编辑方案…"));
    editButton_->setToolTip(QStringLiteral("打开图形化编辑器，修改步骤、判定与流程控制"));
    stepButton_ = makeButton(QStringLiteral("单步调试"));
    stepButton_->setToolTip(QStringLiteral("只执行下一个步骤，便于逐步确认设备与配置是否正确"));
    flowInfo_ = new QLabel(QStringLiteral("未加载方案：点击“加载方案…”选择 JSON 文件"));
    flowInfo_->setWordWrap(true);
    flowInfo_->setToolTip(QStringLiteral("显示方案名称、版本、步骤数与校验结果"));
    deviceInfo_ = new QLabel(QStringLiteral("设备：未连接（在监控页选择并连接设备）"));
    deviceInfo_->setWordWrap(true);
    deviceInfo_->setProperty("role", QStringLiteral("hint"));
    deviceInfo_->setToolTip(QStringLiteral("测试目标来自监控页选中的设备；未连接时无法开始测试"));

    auto* flowButtons = new QHBoxLayout();
    flowButtons->addWidget(loadButton_);
    flowButtons->addWidget(editButton_);

    left->addLayout(flowButtons);
    left->addWidget(stepButton_);
    left->addWidget(makeSectionTitle(QStringLiteral("方案信息")));
    left->addWidget(flowInfo_);
    left->addSpacing(8);
    left->addWidget(deviceInfo_);
    left->addStretch(1);

    auto* leftWidget = new QWidget();
    leftWidget->setLayout(left);
    leftWidget->setFixedWidth(300);

    // ---- 右栏：工具栏 + 物料校验 + 步骤表格 + 结果摘要 ----
    auto* right = new QVBoxLayout();
    right->setSpacing(8);

    // 操作顺序与提示条一致：先 SN，再开始/停止，最后进度。
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);
    snEdit_ = new QLineEdit();
    snEdit_->setPlaceholderText(QStringLiteral("扫码/输入 SN（回车开始）"));
    snEdit_->setMaximumWidth(240);
    snEdit_->setToolTip(QStringLiteral("产品序列号；扫码枪通常回车结尾，可直接触发开始测试"));
    startButton_ = makeButton(QStringLiteral("开始测试"), QStringLiteral("primary"));
    startButton_->setToolTip(QStringLiteral("按当前方案完整执行一次测试；需先加载方案并填写 SN"));
    stopButton_ = makeButton(QStringLiteral("停止"), QStringLiteral("danger"));
    stopButton_->setToolTip(QStringLiteral("请求中止当前测试；已完成步骤的结果会保留"));
    progress_ = new QProgressBar();
    progress_->setRange(0, 1);
    progress_->setValue(0);
    progress_->setFormat(QStringLiteral("%v/%m 步"));
    toolbar->addWidget(new QLabel(QStringLiteral("SN")));
    toolbar->addWidget(snEdit_);
    toolbar->addWidget(startButton_);
    toolbar->addWidget(stopButton_);
    toolbar->addWidget(progress_, 1);

    // 物料防错：扫码料号 → MES 只读 BOM 核对 → 不符锁站。
    auto* materialBar = new QHBoxLayout();
    materialBar->setSpacing(8);
    materialEdit_ = new QLineEdit();
    materialEdit_->setPlaceholderText(QStringLiteral("扫描/输入料号（回车核对）"));
    materialEdit_->setMaximumWidth(240);
    materialEdit_->setToolTip(QStringLiteral("扫描物料条码后回车，或点击“核对料号”"));
    materialButton_ = makeButton(QStringLiteral("核对料号"));
    materialButton_->setToolTip(QStringLiteral("向 MES 查询 BOM 并比对当前料号"));
    unlockButton_ = makeButton(QStringLiteral("解锁工位"));
    unlockButton_->setToolTip(QStringLiteral("料号不符导致锁站后，核对正确料号或由管理员解锁"));
    materialInfo_ = new QLabel();
    materialInfo_->setWordWrap(true);
    materialBar->addWidget(new QLabel(QStringLiteral("料号")));
    materialBar->addWidget(materialEdit_);
    materialBar->addWidget(materialButton_);
    materialBar->addWidget(unlockButton_);
    materialBar->addWidget(materialInfo_, 1);

    table_ = new QTableWidget(0, kColumnCount);
    table_->setHorizontalHeaderLabels({QStringLiteral("#"),
                                       QStringLiteral("步骤"),
                                       QStringLiteral("状态"),
                                       QStringLiteral("实测值"),
                                       QStringLiteral("判定"),
                                       QStringLiteral("耗时")});
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setAlternatingRowColors(false);
    table_->setToolTip(QStringLiteral("步骤结果表：每行对应方案中的一个步骤，# 为执行序号"));
    table_->horizontalHeaderItem(2)->setToolTip(QStringLiteral(
        "等待 / 执行中 / 通过 / 失败 / 跳过 / 中止"));
    table_->horizontalHeaderItem(3)->setToolTip(QStringLiteral(
        "测量步骤显示统计值；命令/版本步骤显示设备返回文本"));
    table_->horizontalHeaderItem(4)->setToolTip(QStringLiteral(
        "失败原因；鼠标悬停该行可查看完整消息"));
    table_->horizontalHeaderItem(5)->setToolTip(QStringLiteral("该步骤耗时（含等待/测量窗口）"));

    summary_ = new QLabel(QStringLiteral("等待加载方案：先加载方案，再填写 SN 开始测试"));
    summary_->setWordWrap(true);

    right->addLayout(toolbar);
    right->addLayout(materialBar);
    right->addWidget(makeHint(QStringLiteral(
        "料号核对：扫描料号后回车；与 MES BOM 不符会锁定工位，核对正确料号或解锁后才能继续。")));
    right->addWidget(table_, 1);
    right->addWidget(summary_);

    content->addWidget(leftWidget);
    content->addLayout(right, 1);
    root->addLayout(content, 1);

    // 初始状态：没有方案、未连接，开始按钮不可用。
    startButton_->setEnabled(false);
    stopButton_->setEnabled(false);
    editButton_->setEnabled(false);
    stepButton_->setEnabled(false);

    connect(loadButton_, &QPushButton::clicked, this, &TestPage::loadFlow);
    connect(editButton_, &QPushButton::clicked, this, &TestPage::editFlow);
    connect(stepButton_, &QPushButton::clicked, this, &TestPage::debugStep);
    connect(startButton_, &QPushButton::clicked, this, &TestPage::startTest);
    connect(stopButton_, &QPushButton::clicked, this, &TestPage::stopTest);
    // 扫码枪通常以回车结尾：直接触发开始，减少一次鼠标操作。
    connect(snEdit_, &QLineEdit::returnPressed, this, [this] {
        if (startButton_->isEnabled())
            startTest();
    });
    connect(materialEdit_, &QLineEdit::returnPressed, this, &TestPage::checkMaterial);
    connect(materialButton_, &QPushButton::clicked, this, &TestPage::checkMaterial);
    connect(unlockButton_, &QPushButton::clicked, this, &TestPage::unlockMaterial);

    updateWorkOrderInfo();
}

void TestPage::loadFlow()
{
    QSettings settings;
    const QString lastDir =
        settings.value(QStringLiteral("test/lastFlowDir"),
                       QCoreApplication::applicationDirPath())
            .toString();

    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择测试流程"), lastDir,
        QStringLiteral("流程配置 (*.json)"));
    if (path.isEmpty())
        return;

    settings.setValue(QStringLiteral("test/lastFlowDir"), QFileInfo(path).absolutePath());
    loadFlowFile(path, true);
}

// 加载 + 校验 + 接受方案。interactive=false 用于工单自动加载（提示更简洁）。
bool TestPage::loadFlowFile(const QString& path, bool interactive)
{
    // 1) 加载
    const auto loaded = test::FlowLoader::loadFromFile(path);
    if (!loaded.ok()) {
        flowValid_ = false;
        QStringList lines;
        const int maxShow = 5;
        for (int i = 0; i < loaded.errors.size() && i < maxShow; ++i)
            lines << QStringLiteral("%1: %2").arg(loaded.errors.at(i).fieldPath,
                                                  loaded.errors.at(i).message);
        showFlowError((interactive ? QStringLiteral("加载失败\n")
                                   : QStringLiteral("工单绑定方案加载失败：%1\n").arg(path))
                      + lines.join(QLatin1Char('\n')));
        return false;
    }

    // 2) 校验
    const auto issues = test::FlowValidator::validate(loaded.flow, station_.plugins,
                                                      station_.pluginsDir);
    if (!issues.isEmpty()) {
        flowValid_ = false;
        QStringList lines;
        const int maxShow = 5;
        for (int i = 0; i < issues.size() && i < maxShow; ++i)
            lines << QStringLiteral("%1: %2").arg(issues.at(i).fieldPath,
                                                  issues.at(i).message);
        if (issues.size() > maxShow)
            lines << QStringLiteral("… 共 %1 个问题").arg(issues.size());
        showFlowError(QStringLiteral("配置校验未通过\n") + lines.join(QLatin1Char('\n')));
        return false;
    }

    // 3) 接受方案
    flow_ = loaded.flow;
    flowValid_ = true;
    flowPath_ = path;       // 记住来源文件，编辑器保存时回写同一文件
    debugIndex_ = 0;        // 换方案后单步调试从头开始
    showFlowInfo();
    resetTable();

    summary_->setStyleSheet(QString());
    summary_->setText(interactive ? QStringLiteral("方案已就绪，点击\"开始测试\"")
                                  : QStringLiteral("已加载方案：%1").arg(flow_.name));
    progress_->setRange(0, flow_.steps.size());
    progress_->setValue(0);
    updateButtons();
    return true;
}

void TestPage::editFlow()
{
    if (running_ || !flowValid_)
        return;

    FlowEditorDialog dialog(flow_, flowPath_, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString path = dialog.savedPath();
    if (path.isEmpty())
        return;

    // 重新从磁盘加载：编辑器写出了新内容，走一遍正常加载/校验流程，
    // 保证"界面看到的方案"和"即将执行的方案"完全一致。
    if (loadFlowFile(path, true) && auth_ != nullptr)
        auth_->audit(QStringLiteral("编辑测试方案"), flow_.name, path);
}

void TestPage::debugStep()
{
    if (!flowValid_ || running_ || !operationAllowed_)
        return;

    if (device_ == nullptr || !device_->isConnected()) {
        summary_->setStyleSheet(QStringLiteral("color:#c0392b"));
        summary_->setText(QStringLiteral("设备未连接，无法单步调试"));
        return;
    }

    if (flow_.steps.isEmpty())
        return;

    if (debugIndex_ < 0 || debugIndex_ >= flow_.steps.size())
        debugIndex_ = 0;

    const int index = debugIndex_;
    const test::TestFlow flow = flow_; // 拷贝一份，避免执行期间流程被替换

    debugMode_ = true;
    setRunning(true);
    summary_->setStyleSheet(QString());
    summary_->setText(QStringLiteral("单步调试：第 %1 步「%2」…")
                          .arg(index + 1)
                          .arg(flow.steps.at(index).name));

    const test::PluginContext plugins{
        !station_.plugins.isEmpty(), station_.pluginsDir,
        station_.plugins, station_.pluginTimeoutMs};
    QJsonObject context;
    context.insert(QStringLiteral("sn"), snEdit_->text().trimmed());
    context.insert(QStringLiteral("workOrder"), workOrderNo_);
    context.insert(QStringLiteral("product"), flow.product);
    context.insert(QStringLiteral("flow"), flow.id);

    QMetaObject::invokeMethod(
        engine_,
        [engine = engine_, flow, index, plugins, context] {
            engine->setPluginContext(plugins);
            engine->setRunContext(context);
            engine->runSingleStep(flow, index);
        },
        Qt::QueuedConnection);

    // 下一次单步从下一步开始，跑完最后一步回到第一步。
    debugIndex_ = (index + 1 >= flow.steps.size()) ? 0 : index + 1;
}

void TestPage::checkMaterial()
{
    if (running_ || bom_ == nullptr || !bom_->enabled())
        return;

    const QString material = materialEdit_->text().trimmed();
    if (material.isEmpty()) {
        materialInfo_->setStyleSheet(QStringLiteral("color:#c0392b"));
        materialInfo_->setText(QStringLiteral("请先扫描料号"));
        return;
    }
    if (workOrderNo_.isEmpty()) {
        materialInfo_->setStyleSheet(QStringLiteral("color:#c0392b"));
        materialInfo_->setText(QStringLiteral("请先选择生产工单"));
        return;
    }

    const QString product =
        workOrderProduct_.isEmpty() ? flow_.product : workOrderProduct_;
    materialInfo_->setStyleSheet(QStringLiteral("color:#2c3e50"));
    materialInfo_->setText(QStringLiteral("核对中…"));
    bom_->verify(workOrderNo_, product, material,
                 auth_ != nullptr ? auth_->currentUser().name : QString());
}

void TestPage::unlockMaterial()
{
    if (bom_ == nullptr || !bom_->locked())
        return;
    if (auth_ == nullptr || !auth_->can(security::Permission::Administer)) {
        QMessageBox::information(this, QStringLiteral("权限不足"),
                                 QStringLiteral("工位锁定需班组长（管理员）账号确认后解锁。"));
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("解锁工位"),
            QStringLiteral("确认解除料号不符锁定？\n原因：%1").arg(bom_->lockReason()))
        != QMessageBox::Yes) {
        return;
    }
    bom_->supervisorUnlock(auth_->currentUser().name);
}

void TestPage::startTest()
{
    if (!flowValid_ || running_ || !operationAllowed_)
        return;

    if (device_ == nullptr || !device_->isConnected()) {
        summary_->setStyleSheet(QStringLiteral("color:#c0392b"));
        summary_->setText(QStringLiteral("设备未连接，无法开始测试"));
        return;
    }

    // ---- 产线防呆 1：必须先有生产工单 ----
    if (workOrderNo_.isEmpty()) {
        summary_->setStyleSheet(QStringLiteral("color:#c0392b"));
        summary_->setText(QStringLiteral("请先在「生产」页新建/选择工单"));
        return;
    }

    // ---- 产线防呆 1.1：首件状态与治具状态 ----
    if (repository_ != nullptr && repository_->isOpen()) {
        const storage::WorkOrder order = repository_->findWorkOrder(workOrderNo_);
        if (order.orderNo.isEmpty()) {
            summary_->setStyleSheet(QStringLiteral("color:#c0392b"));
            summary_->setText(QStringLiteral("工单不存在，请重新选择"));
            return;
        }
        // 配方防错：方案产品必须与工单产品一致，避免换线后跑错程序。
        if (!order.product.isEmpty() && !flow_.product.isEmpty()
            && order.product.compare(flow_.product, Qt::CaseInsensitive) != 0) {
            summary_->setStyleSheet(QStringLiteral("color:#c0392b"));
            summary_->setText(QStringLiteral("当前方案产品（%1）与工单产品（%2）不一致，\n"
                                             "请加载与工单匹配的测试方案")
                                  .arg(flow_.product, order.product));
            return;
        }
        if (order.faiRequired && order.faiStatus != QLatin1String("PENDING")
            && order.faiStatus != QLatin1String("CONFIRMED")) {
            summary_->setStyleSheet(QStringLiteral("color:#c0392b"));
            summary_->setText(order.faiStatus == QLatin1String("WAITING")
                                  ? QStringLiteral("首件待确认：请在「生产」页确认后继续测试")
                                  : QStringLiteral("首件不合格：请先在「生产」页重新首件"));
            return;
        }
    }

    QString fixtureError;
    if (!checkFixtureReady(&fixtureError)) {
        summary_->setStyleSheet(QStringLiteral("color:#c0392b"));
        summary_->setText(fixtureError);
        return;
    }

    // ---- 产线防呆 1.2：Andon 停线 / 操作员资质 / 电子 SOP ----
    const QString product =
        workOrderProduct_.isEmpty() ? flow_.product : workOrderProduct_;

    QString andonError;
    if (!checkAndon(&andonError)) {
        summary_->setStyleSheet(QStringLiteral("color:#c0392b; font-weight:bold"));
        summary_->setText(andonError);
        return;
    }

    QString qualificationError;
    if (!checkQualification(product, &qualificationError)) {
        summary_->setStyleSheet(QStringLiteral("color:#c0392b"));
        summary_->setText(qualificationError);
        return;
    }

    if (!ensureSopRead(product))
        return;

    // ---- 产线防呆 2：SN 必填且符合工位规则 ----
    const QString sn = snEdit_->text().trimmed();
    QString snError;
    if (!validateSn(sn, &snError)) {
        summary_->setStyleSheet(QStringLiteral("color:#c0392b"));
        summary_->setText(snError);
        snEdit_->setFocus();
        return;
    }

    // ---- 产线防呆 3：一码一测；重复测试需确认并标记复测 ----
    // 发货清单非空时，SN 必须先在清单内（导入后不允许临时加测）。
    if (repository_ != nullptr && repository_->isOpen()
        && repository_->snWhitelistCount(workOrderNo_) > 0
        && !repository_->snInWhitelist(workOrderNo_, sn)) {
        summary_->setStyleSheet(QStringLiteral("color:#c0392b"));
        summary_->setText(QStringLiteral("SN %1 不在本工单发货清单内，请核对条码").arg(sn));
        snEdit_->selectAll();
        snEdit_->setFocus();
        return;
    }

    bool retest = false;
    if (repository_ != nullptr && repository_->isOpen()) {
        const int tested = repository_->snTestCount(workOrderNo_, sn);
        if (tested > 0) {
            const storage::HistoryRun last = repository_->lastRunOfSn(workOrderNo_, sn);
            const QString lastInfo = last.id > 0
                ? QStringLiteral("%1　%2")
                      .arg(last.verdict,
                           QDateTime::fromMSecsSinceEpoch(last.startedAt)
                               .toString(QStringLiteral("MM-dd HH:mm:ss")))
                : QStringLiteral("—");
            const QString text =
                QStringLiteral("SN %1 在本工单已测试 %2 次。\n最近一次：%3\n\n确定复测？")
                    .arg(sn)
                    .arg(tested)
                    .arg(lastInfo);
            if (QMessageBox::question(this, QStringLiteral("复测确认"), text)
                != QMessageBox::Yes) {
                snEdit_->selectAll();
                snEdit_->setFocus();
                return;
            }
            retest = true;
        }
    }

    // ---- 物料防错：未核对/已锁站则禁止开始 ----
    if (bom_ != nullptr && bom_->enabled()) {
        if (bom_->locked()) {
            summary_->setStyleSheet(QStringLiteral("color:#c0392b; font-weight:bold"));
            summary_->setText(QStringLiteral("工位已锁定：%1（请班组长解锁）")
                                  .arg(bom_->lockReason()));
            return;
        }
        if (materialEdit_->text().trimmed().isEmpty()) {
            summary_->setStyleSheet(QStringLiteral("color:#c0392b"));
            summary_->setText(QStringLiteral("请先扫描料号并完成核对"));
            materialEdit_->setFocus();
            return;
        }
    }

    // ---- MES 过站/工单校验（未配置接口时直接放行）----
    QString stationCheckError;
    if (!checkMesStation(workOrderNo_, sn, product, &stationCheckError)) {
        summary_->setStyleSheet(QStringLiteral("color:#c0392b; font-weight:bold"));
        summary_->setText(stationCheckError);
        snEdit_->selectAll();
        snEdit_->setFocus();
        return;
    }

    currentSn_ = sn;
    currentRetest_ = retest;
    resetTable();
    running_ = true;
    setRunning(true);

    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("开始测试"), flow_.name, QStringLiteral("开始"),
                     QStringLiteral("工单=%1 SN=%2 料号=%3 复测=%4")
                         .arg(workOrderNo_, sn,
                              materialEdit_ != nullptr
                                  ? materialEdit_->text().trimmed()
                                  : QString(),
                              retest ? QStringLiteral("是") : QStringLiteral("否")));
    }

    progress_->setRange(0, flow_.steps.size());
    progress_->setValue(0);
    summary_->setStyleSheet(QString());
    summary_->setText(QStringLiteral("执行中…"));

    // 把流程拷贝给工作线程执行，避免界面侧数据被改动。
    const test::TestFlow flow = flow_;
    const test::PluginContext plugins{
        !station_.plugins.isEmpty(), station_.pluginsDir,
        station_.plugins, station_.pluginTimeoutMs};
    QJsonObject context;
    context.insert(QStringLiteral("sn"), sn);
    context.insert(QStringLiteral("workOrder"), workOrderNo_);
    context.insert(QStringLiteral("product"),
                   workOrderProduct_.isEmpty() ? flow.product : workOrderProduct_);
    context.insert(QStringLiteral("material"), materialEdit_->text().trimmed());
    context.insert(QStringLiteral("flow"), flow.id);

    QMetaObject::invokeMethod(engine_, [engine = engine_, flow, plugins, context] {
        engine->setPluginContext(plugins);
        engine->setRunContext(context);
        engine->run(flow);
    }, Qt::QueuedConnection);
}

void TestPage::stopTest()
{
    if (!running_)
        return;
    engine_->requestStop();
    summary_->setText(QStringLiteral("正在停止…"));
    if (auth_ != nullptr)
        auth_->audit(QStringLiteral("停止测试"), flow_.name, QStringLiteral("请求停止"));
}

void TestPage::onStepStarted(int index)
{
    if (index < 0 || index >= table_->rowCount())
        return;

    if (auto* item = table_->item(index, 2))
        item->setText(QStringLiteral("执行中"));

    // 当前步骤整行高亮，便于一眼看出执行位置。
    const QColor highlight(0xE3, 0xF2, 0xFD);
    for (int column = 0; column < kColumnCount; ++column) {
        if (auto* item = table_->item(index, column))
            item->setBackground(highlight);
    }
}

void TestPage::onStepFinished(int index, const test::StepResult& result)
{
    updateRow(index, result);

    // 单步调试只跑一步，不推进进度条、不落库；结果直接显示在摘要区。
    if (debugMode_) {
        debugMode_ = false;
        setRunning(false);
        const bool failed = result.status == test::StepStatus::Fail;
        summary_->setStyleSheet(failed ? QStringLiteral("color:#c0392b") : QString());
        summary_->setText(QStringLiteral("单步结果：第 %1 步 %2　%3")
                              .arg(index + 1)
                              .arg(failed ? QStringLiteral("FAIL") : QStringLiteral("PASS"),
                                   result.message.isEmpty() ? QStringLiteral("—")
                                                            : result.message));
        return;
    }

    progress_->setValue(progress_->value() + 1);
}

void TestPage::onRunFinished(const test::RunResult& result)
{
    running_ = false;
    setRunning(false);

    // 跳过/中止的步骤不会单独发 stepFinished，这里统一刷一遍。
    for (int i = 0; i < result.steps.size(); ++i)
        updateRow(i, result.steps.at(i));

    progress_->setValue(progress_->maximum());

    int passed = 0;
    for (const auto& step : result.steps) {
        if (step.status == test::StepStatus::Pass)
            ++passed;
    }

    QString verdict;
    QString color;
    switch (result.verdict) {
    case test::RunVerdict::Pass:
        verdict = QStringLiteral("PASS");
        color = QStringLiteral("#27ae60");
        break;
    case test::RunVerdict::Fail:
        verdict = QStringLiteral("FAIL");
        color = QStringLiteral("#c0392b");
        break;
    case test::RunVerdict::Aborted:
        verdict = QStringLiteral("ABORTED");
        color = QStringLiteral("#e67e22");
        break;
    }

    // 自动落库：带上工单/工位/操作员/治具/复测标记，供追溯与 MES 上传。
    QString saveInfo;
    int savedRunId = 0;
    if (repository_ != nullptr && repository_->isOpen()) {
        storage::RunMeta meta;
        meta.sn = currentSn_;
        meta.workOrder = workOrderNo_;
        meta.lineId = station_.lineId;
        meta.stationId = station_.stationId;
        meta.operatorName = auth_ != nullptr ? auth_->currentUser().name
                                             : QStringLiteral("—");
        meta.fixture = station_.fixtureId;
        meta.retest = currentRetest_;
        meta.shift = station_.shiftAt(QDateTime::currentDateTime());

        QString error;
        if (repository_->saveRun(flow_, result, meta, &error)) {
            savedRunId = repository_->lastInsertedRunId();
            saveInfo = QStringLiteral("　已保存记录 #%1").arg(savedRunId);
        } else {
            saveInfo = QStringLiteral("　保存失败：%1").arg(error);
        }
    }

    // ---- 质量闭环：不良原因 / 首件状态 / 治具计数 ----
    QString postInfo;
    if (savedRunId > 0 && repository_ != nullptr) {
        if (verdict == QLatin1String("FAIL") && !station_.failReasons.isEmpty()) {
            bool picked = false;
            const QString reason = QInputDialog::getItem(
                this, QStringLiteral("不良原因"),
                QStringLiteral("请选择不良原因（可取消）："), station_.failReasons,
                0, false, &picked);
            if (picked && !reason.isEmpty()
                && repository_->updateFailReason(savedRunId, reason)) {
                postInfo += QStringLiteral("　不良原因：%1").arg(reason);
                if (auth_ != nullptr) {
                    auth_->audit(QStringLiteral("不良原因"), currentSn_,
                                 QStringLiteral("已记录"), reason);
                }
            }
        }

        if (!workOrderNo_.isEmpty()) {
            repository_->updateFaiAfterRun(workOrderNo_, verdict);
            const storage::WorkOrder order =
                repository_->findWorkOrder(workOrderNo_);
            if (order.faiRequired) {
                if (order.faiStatus == QLatin1String("WAITING"))
                    postInfo += QStringLiteral("　首件待确认");
                else if (order.faiStatus == QLatin1String("REJECTED"))
                    postInfo += QStringLiteral("　首件不合格");
                else if (order.faiStatus == QLatin1String("PENDING")) {
                    postInfo += QStringLiteral("　首件 %1/%2")
                                    .arg(order.faiTested)
                                    .arg(order.faiRequiredCount);
                }
            }
        }

        if (!station_.fixtureId.isEmpty()) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            const storage::FixtureStatus status =
                repository_->fixtureStatus(station_.fixtureId);
            const qint64 due = status.calibratedUntil > 0
                                   ? status.calibratedUntil
                                   : station_.defaultFixtureDueMs(nowMs);
            repository_->recordFixtureUse(station_.fixtureId, due, nowMs);
        }

        // ---- 合格证标签：PASS 后自动生成 PDF，供标签打印机取用 ----
        if (verdict == QLatin1String("PASS") && station_.labelEnabled) {
            QString dir = station_.labelDirectory;
            if (dir.isEmpty()) {
                dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                    + QStringLiteral("/labels");
            }
            QDir().mkpath(dir);

            QString safeSn = currentSn_;
            safeSn.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")),
                           QStringLiteral("_"));
            const QString stamp = QDateTime::currentDateTime()
                                      .toString(QStringLiteral("yyyyMMdd_HHmmss"));
            const QString labelPath =
                QStringLiteral("%1/%2_%3.pdf").arg(dir, safeSn, stamp);

            QString labelError;
            if (exportPassLabel(labelPath, station_, currentSn_, flow_.product,
                                workOrderNo_,
                                auth_ != nullptr ? auth_->currentUser().name
                                                 : QStringLiteral("—"),
                                station_.shiftAt(QDateTime::currentDateTime()),
                                QDateTime::currentMSecsSinceEpoch(), &labelError)) {
                postInfo += QStringLiteral("　标签：%1")
                                .arg(QFileInfo(labelPath).fileName());
                if (auth_ != nullptr) {
                    auth_->audit(QStringLiteral("生成标签"), currentSn_,
                                 QStringLiteral("成功"), labelPath);
                }
            } else {
                postInfo += QStringLiteral("　标签失败：%1").arg(labelError);
                if (auth_ != nullptr) {
                    auth_->audit(QStringLiteral("生成标签"), currentSn_,
                                 QStringLiteral("失败"), labelError);
                }
            }
        }
    }

    summary_->setStyleSheet(QStringLiteral("color:%1; font-weight:bold").arg(color));
    summary_->setText(QStringLiteral("判定：%1　总耗时：%2 ms　通过 %3/%4%5%6")
                          .arg(verdict)
                          .arg(result.totalElapsedMs)
                          .arg(passed)
                          .arg(result.steps.size())
                          .arg(saveInfo)
                          .arg(postInfo));

    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("测试结束"), flow_.name, verdict,
                     QStringLiteral("工单=%1 SN=%2 复测=%3 通过 %4/%5")
                         .arg(workOrderNo_, currentSn_,
                              currentRetest_ ? QStringLiteral("是") : QStringLiteral("否"))
                         .arg(passed)
                         .arg(result.steps.size()));
    }

    // 落库成功后先评估 Andon（可能弹窗并锁定工位），再通知看板刷新。
    if (savedRunId > 0)
        evaluateAndon(verdict);

    // 落库成功后通知主界面触发 MES 上传。
    if (savedRunId > 0)
        emit runSaved(savedRunId);

    // 选中 SN 文本：下一次扫码/输入会直接覆盖，减少手工清空。
    snEdit_->selectAll();
    snEdit_->setFocus();
}

void TestPage::onDeviceStateChanged(bool connected, const QString& detail)
{
    deviceInfo_->setText(connected ? QStringLiteral("设备：已连接")
                                   : QStringLiteral("设备：") + detail);
    deviceInfo_->setStyleSheet(connected ? QStringLiteral("color:#27ae60")
                                         : QStringLiteral("color:#c0392b"));

    updateButtons();
}

void TestPage::setRunning(bool running)
{
    if (running_ == running)
        return;
    running_ = running;
    updateButtons();
    emit runningChanged(running);
}

void TestPage::showFlowError(const QString& message)
{
    flowInfo_->setStyleSheet(QStringLiteral("color:#c0392b"));
    flowInfo_->setText(message);
    summary_->setStyleSheet(QStringLiteral("color:#c0392b"));
    summary_->setText(QStringLiteral("方案不可用"));
    startButton_->setEnabled(false);
    table_->setRowCount(0);
}

void TestPage::showFlowInfo()
{
    flowInfo_->setStyleSheet(QStringLiteral("color:#2c3e50"));
    flowInfo_->setText(QStringLiteral("名称：%1\n产品：%2\n步骤数：%3")
                           .arg(flow_.name,
                                flow_.product.isEmpty() ? QStringLiteral("—") : flow_.product)
                           .arg(flow_.steps.size()));
}

void TestPage::resetTable()
{
    table_->setRowCount(flow_.steps.size());

    for (int row = 0; row < flow_.steps.size(); ++row) {
        // 每行预创建 6 个单元格，后续只改文本/颜色，避免频繁插入控件。
        const QString texts[kColumnCount] = {
            QString::number(row + 1),
            flow_.steps.at(row).name,
            QStringLiteral("等待"),
            QStringLiteral("—"),
            QStringLiteral("—"),
            QStringLiteral("—"),
        };

        for (int column = 0; column < kColumnCount; ++column) {
            auto* item = new QTableWidgetItem(texts[column]);
            if (column != 1)
                item->setTextAlignment(Qt::AlignCenter);
            table_->setItem(row, column, item);
        }
    }
}

void TestPage::updateRow(int index, const test::StepResult& result)
{
    if (index < 0 || index >= table_->rowCount())
        return;

    auto setText = [&](int column, const QString& text) {
        if (auto* item = table_->item(index, column))
            item->setText(text);
    };

    setText(2, statusText(result.status));
    setText(3, result.hasMeasured ? QString::number(result.measured, 'f', 2)
                                  : QStringLiteral("—"));
    setText(4, verdictText(result.status));
    setText(5, QStringLiteral("%1 ms").arg(result.elapsedMs));

    const QColor background = statusBackground(result.status);
    for (int column = 0; column < kColumnCount; ++column) {
        if (auto* item = table_->item(index, column)) {
            item->setBackground(background);
            item->setToolTip(result.message);
        }
    }

    if (result.status == test::StepStatus::Fail) {
        if (auto* item = table_->item(index, 3))
            item->setForeground(QColor(0xC0, 0x39, 0x2B));
    }
}

} // namespace aegis::ui
