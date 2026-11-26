// 多语言（SY-06）：翻译加载、关键文案、未知语言回退
#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>

#include "core/language_manager.h"

using namespace aegis;

class TstI18n : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void languagesListed();
    void englishTranslationLoads();
    void unknownLanguageFallsBackToChinese();
    void switchBackToChinese();
};

void TstI18n::initTestCase()
{
    // 依赖构建产物：build/bin/translations/aegis_en_US.qm（由 lrelease 生成）。
    const QString file =
        core::LanguageManager::translationsDir() + QStringLiteral("/aegis_en_US.qm");
    QVERIFY2(QFile::exists(file), qPrintable(QStringLiteral("缺少翻译文件：%1").arg(file)));
}

// 可选语言固定为中/英两项，界面下拉框据此填充。
void TstI18n::languagesListed()
{
    const auto languages = core::LanguageManager::availableLanguages();
    QCOMPARE(languages.size(), 2);
    QCOMPARE(languages.at(0).code, QStringLiteral("zh_CN"));
    QCOMPARE(languages.at(1).code, QStringLiteral("en_US"));
}

// 英文翻译安装后，主框架关键文案能被翻译。
void TstI18n::englishTranslationLoads()
{
    core::LanguageManager manager;
    QSignalSpy spy(&manager, &core::LanguageManager::languageChanged);

    QString error;
    QVERIFY2(manager.apply(QStringLiteral("en_US"), &error), qPrintable(error));
    QCOMPARE(manager.current(), QStringLiteral("en_US"));
    QCOMPARE(spy.count(), 1);

    const auto translate = [](const char* text) {
        return QCoreApplication::translate("aegis::ui::MainWindow", text);
    };
    QCOMPARE(translate("生产"), QStringLiteral("Production"));
    QCOMPARE(translate("自动测试"), QStringLiteral("Auto Test"));
    QCOMPARE(translate("设备列表"), QStringLiteral("Devices"));
    QCOMPARE(translate("用户"), QStringLiteral("Users"));
}

// 未提供翻译的语言回退到中文原文，不报错、不中断界面。
void TstI18n::unknownLanguageFallsBackToChinese()
{
    core::LanguageManager manager;
    QVERIFY(manager.apply(QStringLiteral("fr_FR")));
    QCOMPARE(manager.current(), QStringLiteral("zh_CN"));
    QCOMPARE(QCoreApplication::translate("aegis::ui::MainWindow", "生产"),
             QString::fromUtf8("生产"));
}

// 切回中文：卸载翻译后恢复原文。
void TstI18n::switchBackToChinese()
{
    core::LanguageManager manager;
    QString error;
    QVERIFY2(manager.apply(QStringLiteral("en_US"), &error), qPrintable(error));
    QCOMPARE(QCoreApplication::translate("aegis::ui::MainWindow", "监控"),
             QStringLiteral("Monitor"));

    QVERIFY2(manager.apply(QStringLiteral("zh_CN"), &error), qPrintable(error));
    QCOMPARE(manager.current(), QStringLiteral("zh_CN"));
    QCOMPARE(QCoreApplication::translate("aegis::ui::MainWindow", "监控"),
             QString::fromUtf8("监控"));
}

QTEST_MAIN(TstI18n)
#include "tst_i18n.moc"
