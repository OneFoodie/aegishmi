#pragma once // 头文件保护

#include <QObject>
#include <QString>
#include <QVector>

class QTranslator;

namespace aegis::core {

// 可选语言：代码用于 QSettings/翻译文件名，显示名用于界面下拉框。
struct LanguageOption {
    QString code;
    QString displayName;
};

// =============================================================================
// LanguageManager —— 运行期语言切换（SY-06）
//
// 原文为简体中文（代码里直接用中文 tr()）；英文由 translations/aegis_en_US.qm
// 提供。切换时安装/卸载 QTranslator 并发出 languageChanged，界面收到后重设文案，
// 无需重启。翻译文件缺失时保持中文，不影响功能。
// =============================================================================
class LanguageManager : public QObject {
    Q_OBJECT

public:
    explicit LanguageManager(QObject* parent = nullptr);
    ~LanguageManager() override;

    static QVector<LanguageOption> availableLanguages();

    // 翻译文件目录：<exe>/translations（构建时由 CMake 生成到该目录）。
    static QString translationsDir();

    QString current() const { return current_; }

    // 切换语言；"zh_CN" 表示中文原文（卸载翻译）。
    bool apply(const QString& code, QString* error = nullptr);

    // 启动时按 QSettings 恢复上次选择；未设置时跟随系统语言（仅区分中/英）。
    void restoreFromSettings();
    void saveToSettings() const;

signals:
    void languageChanged(const QString& code);

private:
    void removeTranslator();

    QTranslator* translator_ = nullptr; // 由本对象管理生命周期
    QString current_ = QStringLiteral("zh_CN");
};

} // namespace aegis::core
