#pragma once // 头文件保护

#include "core/station_config.h"

#include <QString>
#include <QVector>

namespace aegis::storage {
class TestRepository;
}

namespace aegis::ui {

// 导出单次测试的 PDF 审核报告（判定、追溯信息、步骤明细、规格限）。
bool exportRunPdf(storage::TestRepository* repository, int runId,
                  const QString& path, const core::StationConfig& station,
                  QString* errorMessage = nullptr);

// 批量导出：每个运行一页，首页附汇总。
bool exportRunsPdf(storage::TestRepository* repository, const QVector<int>& runIds,
                   const QString& path, const core::StationConfig& station,
                   QString* errorMessage = nullptr);

} // namespace aegis::ui
