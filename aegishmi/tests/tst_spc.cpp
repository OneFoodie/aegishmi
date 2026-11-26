// Qt Test 框架
#include <QtTest>

#include "quality/spc.h"

using namespace aegis;

// =============================================================================
// TstSpc —— SPC 过程能力与控制图计算
// =============================================================================
class TstSpc : public QObject {
    Q_OBJECT

private slots:
    void emptyAndSingleSample();
    void cpCpkAndControlLimits();
    void sigmaZeroHasNoCapability();
    void ruleSameSide();
    void ruleTrend();
    void ruleOutOfControl();
    void gradeTable();
};

void TstSpc::emptyAndSingleSample()
{
    const quality::SpcResult empty = quality::compute({}, true, 10.0, true, 0.0);
    QCOMPARE(empty.n, 0);
    QVERIFY(!empty.hasCp);
    QVERIFY(!empty.hasCpk);

    const quality::SpcResult single = quality::compute({5.0}, true, 6.0, true, 4.0);
    QCOMPARE(single.n, 1);
    QCOMPARE(single.mean, 5.0);
    QVERIFY(!single.hasCp); // 单点无法估计波动
    QVERIFY(single.movingRanges.isEmpty());
}

void TstSpc::cpCpkAndControlLimits()
{
    // 均值 10，样本标准差 0.1581；USL 10.5 / LSL 9.5。
    const QVector<double> values{9.8, 9.9, 10.0, 10.1, 10.2};
    const quality::SpcResult result =
        quality::compute(values, true, 10.5, true, 9.5);

    QCOMPARE(result.n, 5);
    QVERIFY(qAbs(result.mean - 10.0) < 1e-9);
    QVERIFY(qAbs(result.sigma - 0.1581139) < 1e-6);

    QVERIFY(result.hasCp);
    QVERIFY(result.hasCpk);
    QVERIFY(qAbs(result.cp - 1.05409) < 1e-4);
    QVERIFY(qAbs(result.cpk - 1.05409) < 1e-4);

    // 移动极差均为 0.1，控制限 = mean ± 2.66 × 0.1。
    QVERIFY(qAbs(result.mrBar - 0.1) < 1e-9);
    QVERIFY(qAbs(result.uclX - 10.266) < 1e-9);
    QVERIFY(qAbs(result.lclX - 9.734) < 1e-9);
    QVERIFY(qAbs(result.uclMr - 0.3267) < 1e-9);
    QVERIFY(result.outOfControl.isEmpty());
}

void TstSpc::sigmaZeroHasNoCapability()
{
    const QVector<double> values{5.0, 5.0, 5.0, 5.0};
    const quality::SpcResult result =
        quality::compute(values, true, 6.0, true, 4.0);

    QCOMPARE(result.sigma, 0.0);
    QVERIFY(!result.hasCp);
    QVERIFY(!result.hasCpk);
    // 数据完全不变时控制限退化为均值线，但不应该把所有点判成越限。
    QVERIFY(result.outOfControl.isEmpty());
}

void TstSpc::ruleSameSide()
{
    // 均值 0.7，后 7 个点都在中心线同侧。
    QVector<double> values;
    for (int i = 0; i < 3; ++i)
        values.append(0.0);
    for (int i = 0; i < 7; ++i)
        values.append(1.0);

    const quality::SpcResult result = quality::compute(values, false, 0.0, false, 0.0);
    QVERIFY(result.violations.size() >= 1);
    bool found = false;
    for (const QString& text : result.violations) {
        if (text.contains(QStringLiteral("同一侧")))
            found = true;
    }
    QVERIFY(found);
}

void TstSpc::ruleTrend()
{
    QVector<double> values;
    for (int i = 0; i < 8; ++i)
        values.append(i);

    const quality::SpcResult result = quality::compute(values, false, 0.0, false, 0.0);
    bool found = false;
    for (const QString& text : result.violations) {
        if (text.contains(QStringLiteral("上升")))
            found = true;
    }
    QVERIFY(found);
}

void TstSpc::ruleOutOfControl()
{
    // 大多数点在 0 附近，最后一个点明显偏离，应越出控制限。
    const QVector<double> values{0.0, 0.1, -0.1, 0.05, -0.05, 5.0};
    const quality::SpcResult result = quality::compute(values, false, 0.0, false, 0.0);
    QVERIFY(!result.outOfControl.isEmpty());
    QCOMPARE(result.outOfControl.last(), 5);
}

void TstSpc::gradeTable()
{
    QCOMPARE(quality::gradeForCpk(1.8, true), QStringLiteral("A+"));
    QCOMPARE(quality::gradeForCpk(1.4, true), QStringLiteral("A"));
    QCOMPARE(quality::gradeForCpk(1.1, true), QStringLiteral("B"));
    QCOMPARE(quality::gradeForCpk(0.8, true), QStringLiteral("C"));
    QCOMPARE(quality::gradeForCpk(0.2, true), QStringLiteral("D"));
    QCOMPARE(quality::gradeForCpk(0.0, false), QStringLiteral("—"));
}

QTEST_GUILESS_MAIN(TstSpc)
#include "tst_spc.moc"
