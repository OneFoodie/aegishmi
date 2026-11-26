#include "ui/pdf_report.h"

#include "storage/test_repository.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QTextDocument>
#include <QUrl>

namespace aegis::ui {

namespace {

QString esc(const QString& text)
{
    return text.toHtmlEscaped();
}

QString formatTime(qint64 milliseconds)
{
    if (milliseconds <= 0)
        return QStringLiteral("—");
    return QDateTime::fromMSecsSinceEpoch(milliseconds)
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString limitText(const storage::HistoryStep& step)
{
    const QString unit = step.unit.isEmpty() ? QString() : QStringLiteral(" %1").arg(step.unit);
    if (step.hasLsl && step.hasUsl) {
        return QStringLiteral("%1 ~ %2%3")
            .arg(step.lsl, 0, 'f', 2)
            .arg(step.usl, 0, 'f', 2)
            .arg(unit);
    }
    if (step.hasUsl)
        return QStringLiteral("≤ %1%2").arg(step.usl, 0, 'f', 2).arg(unit);
    if (step.hasLsl)
        return QStringLiteral("≥ %1%2").arg(step.lsl, 0, 'f', 2).arg(unit);
    return QStringLiteral("—");
}

QString statusColor(const QString& status)
{
    if (status == QLatin1String("PASS"))
        return QStringLiteral("#27ae60");
    if (status == QLatin1String("FAIL"))
        return QStringLiteral("#c0392b");
    return QStringLiteral("#e67e22");
}

// 标题主色：只接受 #RRGGBB，非法值回退默认色，避免配置错误破坏整份报告。
QString accentColor(const core::StationConfig& station)
{
    static const QRegularExpression pattern(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    const QString color = station.reportAccentColor.trimmed();
    return pattern.match(color).hasMatch() ? color : QStringLiteral("#2c3e50");
}

// Logo 占位：图片由 printHtml() 以文档资源方式注入，这里只放引用。
QString logoHtml(const core::StationConfig& station)
{
    if (station.reportLogoPath.isEmpty()
        || !QFileInfo::exists(station.reportLogoPath)) {
        return QString();
    }
    return QStringLiteral(
        "<p style=\"margin:0 0 8px 0;\">"
        "<img src=\"aegis-report-logo\" width=\"150\"></p>");
}

// 页脚：生成时间 + 可选签名栏 + 自定义页脚文字。
QString footerHtml(const core::StationConfig& station, const QString& operatorName,
                   bool withSignature)
{
    QString line = QStringLiteral("生成时间：%1")
                       .arg(QDateTime::currentDateTime().toString(
                           QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    if (withSignature && station.reportShowSignature) {
        line += QStringLiteral("　%1：%2　%3：____________")
                    .arg(esc(station.reportInspectorLabel), esc(operatorName),
                         esc(station.reportReviewerLabel));
    }
    QString html = QStringLiteral(
                       "<p style=\"color:#7f8c8d;margin-top:18px;\">%1</p>")
                       .arg(line);
    if (!station.reportFooter.isEmpty()) {
        html += QStringLiteral(
                    "<p style=\"color:#95a5a6;font-size:9pt;\">%1</p>")
                    .arg(esc(station.reportFooter));
    }
    return html;
}

// 一次运行的报告正文（不含 html/body 包装，便于批量拼接）。
QString runSection(storage::TestRepository* repository, int runId,
                   const core::StationConfig& station, bool includeSummary)
{
    const storage::HistoryRun run = repository->runById(runId);
    if (run.id <= 0)
        return QStringLiteral("<p style=\"color:#c0392b\">找不到运行记录 #%1</p>")
            .arg(runId);

    const QVector<storage::HistoryStep> steps = repository->stepsOfRun(runId);

    QString html;
    const QString accent = accentColor(station);
    html += logoHtml(station);
    html += QStringLiteral(
                "<h1 style=\"color:%1;\">%2</h1>"
                "<p style=\"color:#7f8c8d;\">%3　报告编号：R%4</p>")
                .arg(accent, esc(station.reportTitle), esc(station.reportCompany))
                .arg(runId);

    if (includeSummary) {
        html += QStringLiteral("<h2 style=\"color:%1;\">结果概览</h2>").arg(accent);
        html += QStringLiteral(
            "<h2 style=\"color:#2c3e50;\">结果概览</h2>"
            "<table cellspacing=\"0\" cellpadding=\"6\" width=\"100%\" "
            "style=\"border:1px solid #d0d7de;\">"
            "<tr><td style=\"background:#f4f6f8;\">判定</td>"
            "<td style=\"color:%1;font-weight:bold;font-size:16pt;\">%2</td>"
            "<td style=\"background:#f4f6f8;\">产品 / 方案</td><td>%3 / %4</td></tr>"
            "<tr><td style=\"background:#f4f6f8;\">SN</td><td>%5</td>"
            "<td style=\"background:#f4f6f8;\">开始时间</td><td>%6</td></tr>"
            "<tr><td style=\"background:#f4f6f8;\">工单 / 线体</td><td>%7 / %8</td>"
            "<td style=\"background:#f4f6f8;\">工位 / 治具</td><td>%9 / %10</td></tr>"
            "<tr><td style=\"background:#f4f6f8;\">操作员 / 班次</td><td>%11 / %12</td>"
            "<td style=\"background:#f4f6f8;\">复测 / 耗时</td><td>%13 / %14 ms</td></tr>"
            "</table>")
            .arg(statusColor(run.verdict), esc(run.verdict), esc(run.product),
                 esc(run.flowName), esc(run.sn), formatTime(run.startedAt),
                 esc(run.workOrder), esc(station.lineId), esc(run.stationId))
            .arg(esc(run.fixture.isEmpty() ? station.fixtureId : run.fixture),
                 esc(run.operatorName),
                 esc(run.shift.isEmpty() ? QStringLiteral("—") : run.shift),
                 run.retest ? QStringLiteral("是") : QStringLiteral("否"))
            .arg(run.durationMs);

        if (!run.failReason.isEmpty()) {
            html += QStringLiteral(
                        "<p style=\"color:#c0392b;\">不良原因：%1</p>")
                        .arg(esc(run.failReason));
        }
    }

    html += QStringLiteral("<h2 style=\"color:%1;\">步骤明细</h2>").arg(accent);
    html += QStringLiteral(
        "<table cellspacing=\"0\" cellpadding=\"5\" width=\"100%\" "
        "style=\"border:1px solid #d0d7de;\">"
        "<tr style=\"background:#eef2f5;\">"
        "<th align=\"left\">#</th><th align=\"left\">步骤</th>"
        "<th align=\"left\">动作</th><th align=\"left\">状态</th>"
        "<th align=\"right\">实测值</th><th align=\"left\">规格限</th>"
        "<th align=\"right\">耗时</th><th align=\"left\">说明</th></tr>");

    for (const storage::HistoryStep& step : steps) {
        const QString measured = step.hasMeasured
            ? QStringLiteral("%1 %2").arg(step.measured, 0, 'f', 2).arg(step.unit)
            : QStringLiteral("—");
        html += QStringLiteral(
                    "<tr>"
                    "<td>%1</td><td>%2</td><td>%3</td>"
                    "<td style=\"color:%4;font-weight:bold;\">%5</td>"
                    "<td align=\"right\">%6</td><td>%7</td>"
                    "<td align=\"right\">%8 ms</td><td>%9</td></tr>")
                    .arg(step.index + 1)
                    .arg(esc(step.name), esc(step.action),
                         statusColor(step.status), esc(step.status), esc(measured),
                         esc(limitText(step)))
                    .arg(step.elapsedMs)
                    .arg(esc(step.message));
    }
    html += QStringLiteral("</table>");

    html += footerHtml(station, run.operatorName, true);
    return html;
}

bool printHtml(const QString& html, const QString& path,
               const core::StationConfig& station, QString* errorMessage)
{
    // 先确认目标路径可写，避免 QPdfWriter 静默失败后只留下空文件。
    QFile probe(path);
    if (!probe.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("无法写入文件：%1").arg(probe.errorString());
        return false;
    }
    probe.close();

    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(15, 15, 15, 15), QPageLayout::Millimeter);
    writer.setTitle(QStringLiteral("AegisHMI 测试报告"));

    QTextDocument document;
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPointSize(10);
    document.setDefaultFont(font);
    document.setHtml(html);

    // Logo 以文档资源注入：HTML 里只写 <img src="aegis-report-logo">，避免依赖本地路径。
    if (!station.reportLogoPath.isEmpty()) {
        QImage logo;
        if (logo.load(station.reportLogoPath) && !logo.isNull()) {
            if (logo.width() > 300)
                logo = logo.scaledToWidth(300, Qt::SmoothTransformation);
            document.addResource(QTextDocument::ImageResource,
                                 QUrl(QStringLiteral("aegis-report-logo")), logo);
        }
    }

    document.print(&writer);

    if (!QFileInfo::exists(path) || QFileInfo(path).size() <= 0) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("PDF 写入失败（文件为空）");
        return false;
    }
    return true;
}

QString wrap(const QString& body)
{
    return QStringLiteral(
               "<html><head><meta charset=\"utf-8\"></head>"
               "<body style=\"font-family:'Microsoft YaHei';\">%1</body></html>")
        .arg(body);
}

} // namespace

bool exportRunPdf(storage::TestRepository* repository, int runId,
                  const QString& path, const core::StationConfig& station,
                  QString* errorMessage)
{
    if (repository == nullptr || !repository->isOpen() || runId <= 0) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开或运行记录无效");
        return false;
    }
    return printHtml(wrap(runSection(repository, runId, station, true)), path,
                     station, errorMessage);
}

bool exportRunsPdf(storage::TestRepository* repository, const QVector<int>& runIds,
                   const QString& path, const core::StationConfig& station,
                   QString* errorMessage)
{
    if (repository == nullptr || !repository->isOpen() || runIds.isEmpty()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("数据库未打开或没有可导出的记录");
        return false;
    }

    // 首页汇总：数量与判定分布。
    int pass = 0;
    int fail = 0;
    int aborted = 0;
    for (int runId : runIds) {
        const storage::HistoryRun run = repository->runById(runId);
        if (run.verdict == QLatin1String("PASS"))
            ++pass;
        else if (run.verdict == QLatin1String("FAIL"))
            ++fail;
        else if (!run.verdict.isEmpty())
            ++aborted;
    }

    QString body = QStringLiteral(
                       "<h1 style=\"color:%1;\">%2</h1>"
                       "<p style=\"color:#7f8c8d;\">%3　批量导出</p>"
                       "<table cellspacing=\"0\" cellpadding=\"6\" width=\"100%\" "
                       "style=\"border:1px solid #d0d7de;\">"
                       "<tr><td style=\"background:#f4f6f8;\">报告数</td><td>%4</td>"
                       "<td style=\"background:#f4f6f8;\">PASS</td><td>%5</td></tr>"
                       "<tr><td style=\"background:#f4f6f8;\">FAIL</td><td>%6</td>"
                       "<td style=\"background:#f4f6f8;\">ABORTED</td><td>%7</td></tr>"
                       "<tr><td style=\"background:#f4f6f8;\">工位 / 线体</td><td>%8 / %9</td>"
                       "<td style=\"background:#f4f6f8;\">导出时间</td><td>%10</td></tr>"
                       "</table>")
                       .arg(accentColor(station), esc(station.reportTitle),
                            esc(station.reportCompany))
                       .arg(runIds.size())
                       .arg(pass)
                       .arg(fail)
                       .arg(aborted)
                       .arg(esc(station.stationId), esc(station.lineId),
                            QDateTime::currentDateTime().toString(
                                QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    body = logoHtml(station) + body;
    body += footerHtml(station, QString(), false);

    for (int runId : runIds) {
        // 每次运行另起一页；Qt 富文本支持 page-break-before。
        body += QStringLiteral("<div style=\"page-break-before:always;\">%1</div>")
                    .arg(runSection(repository, runId, station, true));
    }

    return printHtml(wrap(body), path, station, errorMessage);
}

} // namespace aegis::ui
