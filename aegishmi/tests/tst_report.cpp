// Qt Test 框架
#include <QtTest>

#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include "core/station_config.h"
#include "storage/test_repository.h"
#include "ui/label_report.h"
#include "ui/pdf_report.h"

using namespace aegis;

namespace {

test::TestFlow makeFlow()
{
    test::TestFlow flow;
    flow.schemaVersion = 1;
    flow.id = QStringLiteral("power");
    flow.name = QStringLiteral("电源测试");
    flow.product = QStringLiteral("PM-5V");

    test::TestStep measure;
    measure.id = QStringLiteral("s1");
    measure.name = QStringLiteral("测量输出电压");
    measure.action = test::StepAction::Measure;
    measure.channel = 0;
    measure.windowMs = 100;
    measure.expect.present = true;
    measure.expect.metric = test::Metric::Avg;
    measure.expect.hasMin = true;
    measure.expect.min = 4750;
    measure.expect.hasMax = true;
    measure.expect.max = 5250;
    measure.expect.unit = QStringLiteral("mV");
    flow.steps = {measure};
    return flow;
}

test::RunResult makeResult(double measured)
{
    test::RunResult result;
    result.verdict = test::RunVerdict::Pass;
    result.totalElapsedMs = 500;

    test::StepResult step;
    step.index = 0;
    step.id = QStringLiteral("s1");
    step.name = QStringLiteral("测量输出电压");
    step.status = test::StepStatus::Pass;
    step.measured = measured;
    step.hasMeasured = true;
    step.elapsedMs = 400;
    result.steps = {step};
    return result;
}

storage::RunMeta makeMeta(const QString& sn)
{
    storage::RunMeta meta;
    meta.sn = sn;
    meta.workOrder = QStringLiteral("WO-REPORT");
    meta.lineId = QStringLiteral("L1");
    meta.stationId = QStringLiteral("ST-01");
    meta.operatorName = QStringLiteral("admin");
    meta.fixture = QStringLiteral("FX-01");
    meta.shift = QStringLiteral("白班");
    return meta;
}

} // namespace

// =============================================================================
// TstReport —— 工位配置解析 + PDF 审核报告
// =============================================================================
class TstReport : public QObject {
    Q_OBJECT

private slots:
    void stationConfigParsesNewSections();
    void pdfSingleAndBatch();
    void passLabelPdf();
};

void TstReport::stationConfigParsesNewSections()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray json = R"({
        "stationId": "ST-09",
        "lineId": "L2",
        "sop": {
            "enabled": true,
            "requireRead": true,
            "documents": [
                { "product": "PM-5V", "title": "5V 模块 SOP", "version": "A.2",
                  "steps": ["步骤一", "步骤二"] },
                { "product": "*", "title": "通用须知", "version": "A.0",
                  "steps": ["戴防静电手环"] }
            ]
        },
        "qualification": {
            "enabled": true,
            "products": { "PM-5V": ["operator"], "*": ["admin"] }
        },
        "shifts": [
            { "name": "白班", "start": "08:00", "end": "20:00" },
            { "name": "夜班", "start": "20:00", "end": "08:00" }
        ],
        "andon": { "enabled": true, "consecutiveFails": 4, "fpyThreshold": 75, "minUnits": 6 },
        "spc": { "andonOnCpk": true, "cpkThreshold": 1.1, "minSamples": 12 },
        "label": { "enabled": true, "widthMm": 50, "heightMm": 30 },
        "report": {
            "company": "测试公司",
            "title": "测试报告",
            "logo": "configs/logo.png",
            "footer": "机密文件，请勿外传",
            "accentColor": "#1f6f8b",
            "signature": false,
            "inspectorLabel": "测试员",
            "reviewerLabel": "复核"
        }
    })";

    const QString path = dir.filePath(QStringLiteral("station.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(json);
    file.close();

    QString error;
    const core::StationConfig config = core::StationConfig::load(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QCOMPARE(config.stationId, QStringLiteral("ST-09"));
    QCOMPARE(config.lineId, QStringLiteral("L2"));

    QVERIFY(config.sopEnabled);
    QVERIFY(config.sopRequireRead);
    QCOMPARE(config.sopDocuments.size(), 2);

    // 精确产品优先于 "*"。
    const core::SopDocument* doc = config.sopForProduct(QStringLiteral("PM-5V"));
    QVERIFY(doc != nullptr);
    QCOMPARE(doc->title, QStringLiteral("5V 模块 SOP"));
    QCOMPARE(doc->steps.size(), 2);

    const core::SopDocument* fallback = config.sopForProduct(QStringLiteral("OTHER"));
    QVERIFY(fallback != nullptr);
    QCOMPARE(fallback->title, QStringLiteral("通用须知"));

    // 资质：PM-5V 只授权 operator；其它产品用 "*"（admin）。
    QVERIFY(config.isOperatorQualified(QStringLiteral("PM-5V"), QStringLiteral("operator")));
    QVERIFY(!config.isOperatorQualified(QStringLiteral("PM-5V"), QStringLiteral("admin")));
    QVERIFY(config.isOperatorQualified(QStringLiteral("OTHER"), QStringLiteral("admin")));

    // 班次跨零点：23:30 → 夜班；09:00 → 白班。
    QCOMPARE(config.shiftAt(QDateTime(QDate(2026, 1, 1), QTime(23, 30))),
             QStringLiteral("夜班"));
    QCOMPARE(config.shiftAt(QDateTime(QDate(2026, 1, 1), QTime(9, 0))),
             QStringLiteral("白班"));

    QVERIFY(config.andonEnabled);
    QCOMPARE(config.andonConsecutiveFails, 4);
    QCOMPARE(config.andonFpyThreshold, 75.0);
    QCOMPARE(config.andonMinUnits, 6);
    QVERIFY(config.spcAndonEnabled);
    QCOMPARE(config.spcCpkThreshold, 1.1);
    QCOMPARE(config.spcMinSamples, 12);
    QVERIFY(config.labelEnabled);
    QCOMPARE(config.labelWidthMm, 50);
    QCOMPARE(config.labelHeightMm, 30);
    QCOMPARE(config.reportCompany, QStringLiteral("测试公司"));
    QCOMPARE(config.reportTitle, QStringLiteral("测试报告"));
    // 报告模板：Logo/页脚/主色/签名栏都可配置。
    QCOMPARE(config.reportLogoPath, QStringLiteral("configs/logo.png"));
    QCOMPARE(config.reportFooter, QStringLiteral("机密文件，请勿外传"));
    QCOMPARE(config.reportAccentColor, QStringLiteral("#1f6f8b"));
    QVERIFY(!config.reportShowSignature);
    QCOMPARE(config.reportInspectorLabel, QStringLiteral("测试员"));
    QCOMPARE(config.reportReviewerLabel, QStringLiteral("复核"));
}

void TstReport::pdfSingleAndBatch()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    storage::TestRepository repository;
    QString error;
    QVERIFY2(repository.open(dir.filePath(QStringLiteral("test.db")), &error),
             qPrintable(error));

    QVERIFY2(repository.saveRun(makeFlow(), makeResult(5001.0),
                                makeMeta(QStringLiteral("SN-001")), &error),
             qPrintable(error));
    const int firstId = repository.lastInsertedRunId();
    QVERIFY2(repository.saveRun(makeFlow(), makeResult(4998.0),
                                makeMeta(QStringLiteral("SN-002")), &error),
             qPrintable(error));
    const int secondId = repository.lastInsertedRunId();

    core::StationConfig station;
    station.stationId = QStringLiteral("ST-01");
    station.lineId = QStringLiteral("L1");
    station.reportCompany = QStringLiteral("AegisLab");
    // 模板化：自定义 Logo、页脚、主色与签名栏文案。
    const QString logoPath = dir.filePath(QStringLiteral("logo.png"));
    QImage logo(80, 24, QImage::Format_ARGB32);
    logo.fill(QColor(QStringLiteral("#1f6f8b")));
    QVERIFY(logo.save(logoPath));
    station.reportLogoPath = logoPath;
    station.reportFooter = QStringLiteral("测试页脚");
    station.reportAccentColor = QStringLiteral("#1f6f8b");
    station.reportInspectorLabel = QStringLiteral("测试员");
    station.reportReviewerLabel = QStringLiteral("复核");

    const QString singlePath = dir.filePath(QStringLiteral("single.pdf"));
    QVERIFY2(ui::exportRunPdf(&repository, firstId, singlePath, station, &error),
             qPrintable(error));

    QFile single(singlePath);
    QVERIFY(single.open(QIODevice::ReadOnly));
    const QByteArray singleData = single.readAll();
    QVERIFY(singleData.startsWith("%PDF"));
    QVERIFY(singleData.size() > 1000);

    const QString batchPath = dir.filePath(QStringLiteral("batch.pdf"));
    QVERIFY2(ui::exportRunsPdf(&repository, {firstId, secondId}, batchPath, station,
                               &error),
             qPrintable(error));

    QFile batch(batchPath);
    QVERIFY(batch.open(QIODevice::ReadOnly));
    const QByteArray batchData = batch.readAll();
    QVERIFY(batchData.startsWith("%PDF"));
    // 批量报告包含两条记录，体积应大于单份。
    QVERIFY(batchData.size() > singleData.size());
}

// 合格证标签：PASS 后生成小尺寸 PDF。
void TstReport::passLabelPdf()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    core::StationConfig station;
    station.reportCompany = QStringLiteral("AegisLab");
    station.labelWidthMm = 60;
    station.labelHeightMm = 40;

    const QString path = dir.filePath(QStringLiteral("label.pdf"));
    QString error;
    QVERIFY2(ui::exportPassLabel(path, station, QStringLiteral("SN-001"),
                                 QStringLiteral("PM-5V"), QStringLiteral("WO-1"),
                                 QStringLiteral("operator"), QStringLiteral("白班"),
                                 1700000000000, &error),
             qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray data = file.readAll();
    QVERIFY(data.startsWith("%PDF"));
    QVERIFY(data.size() > 500);

    // 空 SN 不允许生成（避免产生无法追溯的标签）。
    QVERIFY(!ui::exportPassLabel(dir.filePath(QStringLiteral("bad.pdf")), station,
                                 QString(), QStringLiteral("PM-5V"),
                                 QStringLiteral("WO-1"), QStringLiteral("operator"),
                                 QStringLiteral("白班"), 1700000000000, &error));
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(TstReport)
#include "tst_report.moc"
