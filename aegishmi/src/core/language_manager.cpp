#include "core/language_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

namespace aegis::core {

namespace {

// 翻译文件名前缀：translations/aegis_en_US.qm
constexpr char kTranslationPrefix[] = "aegis_";

// 只区分中文/英文：其他系统语言回退到中文原文。
QString normalizeCode(const QString& code)
{
    return code.startsWith(QLatin1String("en"), Qt::CaseInsensitive)
        ? QStringLiteral("en_US")
        : QStringLiteral("zh_CN");
}

} // namespace

LanguageManager::LanguageManager(QObject* parent)
    : QObject(parent)
{
}

LanguageManager::~LanguageManager()
{
    removeTranslator();
}

QVector<LanguageOption> LanguageManager::availableLanguages()
{
    return {{QStringLiteral("zh_CN"), QStringLiteral("简体中文")},
            {QStringLiteral("en_US"), QStringLiteral("English")}};
}

QString LanguageManager::translationsDir()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/translations");
}

void LanguageManager::removeTranslator()
{
    if (translator_ == nullptr)
        return;
    QCoreApplication::removeTranslator(translator_);
    delete translator_;
    translator_ = nullptr;
}

bool LanguageManager::apply(const QString& code, QString* error)
{
    const QString normalized = normalizeCode(code);

    // 先卸载旧翻译，避免中英文混用。
    removeTranslator();

    if (normalized == QLatin1String("en_US")) {
        const QString file = translationsDir() + QLatin1String("/aegis_")
            + normalized + QLatin1String(".qm");

        auto* translator = new QTranslator(this);
        if (!translator->load(file)) {
            delete translator;
            if (error != nullptr) {
                *error = QStringLiteral("翻译文件缺失：%1").arg(
                    QDir::toNativeSeparators(file));
            }
            // 保持当前语言不变（首次切换失败时仍是中文原文）。
            return false;
        }
        if (!QCoreApplication::installTranslator(translator)) {
            delete translator;
            if (error != nullptr)
                *error = QStringLiteral("翻译加载失败：%1").arg(file);
            return false;
        }
        translator_ = translator;
    }

    current_ = normalized;
    emit languageChanged(current_);
    return true;
}

void LanguageManager::restoreFromSettings()
{
    QSettings settings;
    QString code = settings.value(QStringLiteral("ui/language")).toString();
    if (code.isEmpty())
        code = QLocale::system().name(); // 例如 "en_US" / "zh_CN"
    // 恢复失败（缺 qm）时保持中文，不打断启动。
    apply(code);
}

void LanguageManager::saveToSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("ui/language"), current_);
}

} // namespace aegis::core
