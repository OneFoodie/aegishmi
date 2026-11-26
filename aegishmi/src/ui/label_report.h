#pragma once // 头文件保护

#include "core/station_config.h"

#include <QString>

namespace aegis::ui {

// 生成一张 PASS 合格证标签（PDF，尺寸由 station.json 的 label 配置决定）。
bool exportPassLabel(const QString& path, const core::StationConfig& station,
                     const QString& sn, const QString& product,
                     const QString& orderNo, const QString& operatorName,
                     const QString& shift, qint64 finishedAt,
                     QString* errorMessage = nullptr);

} // namespace aegis::ui
