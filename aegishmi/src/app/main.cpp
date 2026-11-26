#include "security/auth_service.h"  // 登录鉴权 + 审计
#include "storage/system_repository.h" // 用户/审计/报警表
#include "ui/login_dialog.h"        // 启动登录框
#include "ui/main_window.h"         // 主窗口类
#include "ui/theme.h"            // 全局主题（字体/配色/控件样式）

#include <QApplication> // 创建 Qt GUI 应用对象：管理事件循环、窗口、剪贴板等全局资源
#include <QDateTime>    // 日志时间戳、按天切分文件名
#include <QDir>         // 创建/清理日志目录
#include <QFile>        // 写日志文件
#include <QFileInfo>    // 判断日志文件修改时间（清理旧日志）
#include <QMessageBox>  // 数据库初始化失败提示
#include <QMutex>       // 多线程写同一文件时需要串行化
#include <QStandardPaths> // 用户数据目录（放日志与数据库）
#include <QStringConverter> // QTextStream 的 UTF-8 编码设置
#include <QTextStream>    // 统一 UTF-8 文本输出

namespace {

// =============================================================================
// 文件日志
//
// 为什么需要：界面日志只保留最近 500 行，关闭程序就没了；
// 现场排障（掉线、超时、解析失败）往往发生在几小时前，必须落盘。
//
// 策略：
//   - 目录：%APPDATA%/AegisLab/AegisHMI/logs（与 aegis.db 同级）
//   - 文件：app-yyyyMMdd.log，按天切分
//   - 保留：启动时清理 7 天前的日志
//   - 同时保留控制台输出（调用上一个 handler），方便调试时直接看终端
// =============================================================================
QMutex g_logMutex;          // 保证多线程写文件不交错
QFile g_logFile;            // 当前打开的日志文件
QString g_logDay;           // 当前文件对应的日期（yyyyMMdd）
QtMessageHandler g_previousHandler = nullptr; // Qt 默认 handler

QString logDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/logs");
}

// 清理超过保留天数的日志文件；失败不影响程序运行。
void cleanupOldLogs(const QString& dir)
{
    QDir directory(dir);
    const QDateTime deadline = QDateTime::currentDateTime().addDays(-7);
    const QStringList files =
        directory.entryList({QStringLiteral("app-*.log")}, QDir::Files);
    for (const QString& name : files) {
        const QFileInfo info(directory.filePath(name));
        if (info.lastModified() < deadline)
            QFile::remove(info.absoluteFilePath());
    }
}

// 按需打开/切换当天的日志文件。调用方需持有 g_logMutex。
void ensureLogFileOpen()
{
    const QString day = QDate::currentDate().toString(QStringLiteral("yyyyMMdd"));
    if (g_logFile.isOpen() && day == g_logDay)
        return;

    if (g_logFile.isOpen())
        g_logFile.close();

    const QString dir = logDirectory();
    QDir().mkpath(dir);
    g_logFile.setFileName(dir + QStringLiteral("/app-") + day + QStringLiteral(".log"));
    g_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    g_logDay = day;
}

// 自定义消息处理器：写文件 + 转发给默认处理器（控制台/调试器）。
void fileMessageHandler(QtMsgType type, const QMessageLogContext& context,
                        const QString& message)
{
    {
        QMutexLocker locker(&g_logMutex);
        ensureLogFileOpen();
        if (g_logFile.isOpen()) {
            const char* level = "INFO";
            switch (type) {
            case QtDebugMsg:
                level = "DEBUG";
                break;
            case QtInfoMsg:
                level = "INFO";
                break;
            case QtWarningMsg:
                level = "WARN";
                break;
            case QtCriticalMsg:
                level = "ERROR";
                break;
            case QtFatalMsg:
                level = "FATAL";
                break;
            }

            QTextStream stream(&g_logFile);
            stream.setEncoding(QStringConverter::Utf8);
            stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
                   << " [" << level << "] " << message << '\n';
            stream.flush();
        }
    }

    if (g_previousHandler != nullptr)
        g_previousHandler(type, context, message);
}

void installFileLogger()
{
    cleanupOldLogs(logDirectory());
    g_previousHandler = qInstallMessageHandler(fileMessageHandler);
}

} // namespace

// =============================================================================
// 程序入口
//
// Qt GUI 程序的固定骨架：
//   1. 构造 QApplication（必须有且只有一个，且必须在创建任何控件之前）
//   2. 创建并显示主窗口
//   3. 进入事件循环 app.exec()，程序在此"卡住"直到窗口关闭
// =============================================================================
int main(int argc, char* argv[])
{
    QApplication app(argc, argv); // 解析命令行参数并初始化 Qt

    // 应用名/组织名会用于 QSettings 的注册表路径、日志目录等；提前设置是好习惯。
    app.setApplicationName(QStringLiteral("AegisHMI"));
    app.setOrganizationName(QStringLiteral("AegisLab"));
    // 版本号与 CMake 的 project(VERSION) 保持一致，版本检测与"关于"共用。
    app.setApplicationVersion(QStringLiteral(AEGIS_VERSION));

    // 全局视觉风格：统一字体、配色、按钮层级与表格样式（必须在建窗口前应用）。
    aegis::ui::applyTheme(app);

    // 安装文件日志（必须在设置应用名之后：日志目录依赖 AppDataLocation）。
    installFileLogger();
    qInfo("AegisHMI 启动");

    // ---- 系统表（用户/审计/报警）：登录前就要打开数据库 ----
    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    const QString dbPath = dataDir + QStringLiteral("/aegis.db");

    aegis::storage::SystemRepository systemRepository;
    QString dbError;
    if (!systemRepository.open(dbPath, &dbError)) {
        QMessageBox::critical(nullptr, QStringLiteral("AegisHMI"),
                              QStringLiteral("数据库打开失败：%1").arg(dbError));
        return 1;
    }

    aegis::security::AuthService auth(&systemRepository);
    QString seedError;
    if (!auth.seedDefaults(&seedError)) {
        QMessageBox::critical(nullptr, QStringLiteral("AegisHMI"),
                              QStringLiteral("初始化默认用户失败：%1").arg(seedError));
        return 1;
    }

    // ---- 登录：用户取消则直接退出，不进入主界面 ----
    aegis::ui::LoginDialog loginDialog(&auth);
    if (loginDialog.exec() != QDialog::Accepted) {
        qInfo("用户取消登录，程序退出");
        return 0;
    }

    aegis::ui::MainWindow window(&auth, &systemRepository); // 构造时完成界面、服务与定时器初始化
    window.show();                                          // 显示窗口（默认是隐藏的）

    // 进入事件循环：所有鼠标/键盘/定时器/网络事件都在这里被分发。
    // 返回值作为进程退出码。
    const int code = app.exec();

    auth.logout(); // 正常退出时补一条登出审计
    qInfo("AegisHMI 退出，代码 %d", code);
    return code;
}
