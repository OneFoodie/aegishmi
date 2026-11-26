# =============================================================================
# AegisHMI 工程构建脚本（CMake + Qt6 + MSVC）
#
# 构建方式（在 aegishmi 目录下，任选其一）：
#   1. 双击运行 scripts\build.bat Release
#   2. 手动执行：
#        cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
#              -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/msvc2022_64
#        cmake --build build
# =============================================================================

# 要求的最低 CMake 版本。低版本打开本工程会直接报错，避免出现难懂的行为差异。
cmake_minimum_required(VERSION 3.21)

# 声明工程名、版本号、使用的语言（CXX 表示 C++）。
# 这一步会生成一系列全局变量（如 PROJECT_NAME），也是 CMake 的标准入口。
project(AegisHMI VERSION 0.1.0 LANGUAGES CXX)

# 指定使用 C++17 标准：本工程用到了 std::atomic、std::array、嵌套命名空间等特性。
set(CMAKE_CXX_STANDARD 17)
# 如果编译器不支持 C++17，直接报错，而不是悄悄降级标准（避免隐藏的兼容问题）。
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 所有可执行文件统一输出到 build/bin 目录，方便打包和查找。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 查找 Qt6 的以下模块（缺少任意一个都会在配置阶段报错）：
#   Core      —— Qt 基础类型、容器、QObject 机制
#   Network   —— QTcpSocket / QTcpServer（TCP 通信、模拟器）
#   SerialPort—— QSerialPort（串口通信）
#   Widgets   —— 桌面 UI 控件
#   Sql       —— SQLite 存储测试记录
#   Test      —— 单元测试框架 QTest
#   Qml       —— QJSEngine（测试流程脚本动作）
#   说明：PDF 报告使用 QPdfWriter，属于 Qt6::Gui，不需要额外的 PrintSupport 模块。
find_package(Qt6 REQUIRED COMPONENTS Core Network SerialPort Widgets Sql Test Qml LinguistTools)

# Qt 官方推荐的项目初始化：自动开启 MOC/UIC/RCC（见下方说明），并设置编译选项。
#   AUTOMOC：扫描带 Q_OBJECT 的头文件并生成 moc 代码（信号槽的元对象代码）
#   AUTOUIC：把 .ui 文件编译成 C++ 头文件（本工程暂未使用）
#   AUTORCC：把 .qrc 资源文件编译进程序（本工程暂未使用）
qt_standard_project_setup()

# 源码里有中文注释；MSVC 默认按系统代码页（GBK）解析源文件，容易报 C4819 或乱码。
# /utf-8 强制 MSVC 以 UTF-8 读写源文件，彻底避免中文注释引起的编译问题。
if(MSVC)
    add_compile_options(/utf-8)
endif()

# =============================================================================
# 第三方依赖：open62541（OPC UA 协议栈）
# 源码放在 third_party/open62541（由 scripts/fetch_open62541.ps1 下载）。
# 未放置源码时自动关闭 OPC UA 支持，其余功能不受影响。
# =============================================================================
set(AEGIS_OPCUA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/open62541)
if(EXISTS ${AEGIS_OPCUA_DIR}/CMakeLists.txt)
    set(UA_ENABLE_AMALGAMATION ON CACHE BOOL "" FORCE)
    set(UA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(UA_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(UA_ENABLE_ENCRYPTION OFF CACHE BOOL "" FORCE)
    set(UA_ENABLE_HISTORIZING OFF CACHE BOOL "" FORCE)
    set(UA_ENABLE_DA OFF CACHE BOOL "" FORCE)
    set(UA_BUILD_FUZZING OFF CACHE BOOL "" FORCE)
    # 只用到客户端读节点 + 服务端变量，关闭 PubSub（MinGW 下其生成代码会引入 SJLJ 展开符号）
    set(UA_ENABLE_PUBSUB OFF CACHE BOOL "" FORCE)
    set(UA_ENABLE_PUBSUB_INFORMATIONMODEL OFF CACHE BOOL "" FORCE)
    set(UA_ENABLE_PUBSUB_INFORMATIONMODEL_METHODS OFF CACHE BOOL "" FORCE)
    # 开启订阅：客户端用 Subscription/MonitoredItem 接收变化通知，避免高频轮询占用带宽
    set(UA_ENABLE_SUBSCRIPTIONS ON CACHE BOOL "" FORCE)
    set(UA_ENABLE_SUBSCRIPTIONS_EVENTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE) # 静态链接，避免额外 DLL
    # MinGW 13 已移除 libssp，open62541 仍会追加 -lssp；用空归档兜底
    link_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/lib)
    add_subdirectory(${AEGIS_OPCUA_DIR} ${CMAKE_BINARY_DIR}/open62541 EXCLUDE_FROM_ALL)
    set(AEGIS_HAS_OPCUA ON)
    message(STATUS "OPC UA enabled (open62541)")
else()
    set(AEGIS_HAS_OPCUA OFF)
    message(STATUS "open62541 not found, OPC UA support disabled")
endif()

# =============================================================================
# 核心静态库 aegis_core
# 作用：把"与界面无关"的代码（协议解析、环形缓冲、通信、业务服务）编译成一个库，
#       供上位机、模拟器、单元测试共同复用。静态库会被直接链接进最终 exe。
# =============================================================================
add_library(aegis_core STATIC
    src/protocol/crc16.cpp
    src/protocol/crc16.h
    src/protocol/frame.h
    src/protocol/frame_codec.cpp
    src/protocol/frame_codec.h
    src/protocol/modbus.cpp
    src/protocol/modbus.h
    src/core/spsc_ring_buffer.h
    src/core/chunk_ring_buffer.h
    src/core/device_config.cpp
    src/core/device_config.h
    src/core/language_manager.cpp
    src/core/language_manager.h
    src/core/station_config.cpp
    src/core/station_config.h
    src/quality/spc.cpp
    src/quality/spc.h
    src/infra/tcp_transport.cpp
    src/infra/tcp_transport.h
    src/infra/modbus_tcp_client.cpp
    src/infra/modbus_tcp_client.h
    src/infra/modbus_rtu_client.cpp
    src/infra/modbus_rtu_client.h
    src/infra/opcua_client.cpp
    src/infra/opcua_client.h
    src/infra/scpi_transport.cpp
    src/infra/scpi_transport.h
    src/infra/scpi_client.cpp
    src/infra/scpi_client.h
    src/infra/serial_transport.cpp
    src/infra/serial_transport.h
    src/service/device_service.cpp
    src/service/device_service.h
    src/service/device_manager.cpp
    src/service/device_manager.h
    src/service/device_summary.h
    src/service/alarm_monitor.cpp
    src/service/alarm_monitor.h
    src/service/mes_uploader.cpp
    src/service/mes_uploader.h
    src/service/bom_verifier.cpp
    src/service/bom_verifier.h
    src/service/mes_station_checker.cpp
    src/service/mes_station_checker.h
    src/service/update_checker.cpp
    src/service/update_checker.h
    src/test/plugin_runner.cpp
    src/test/plugin_runner.h
    src/security/auth_service.cpp
    src/security/auth_service.h
    src/test/flow_types.h
    src/test/flow_loader.cpp
    src/test/flow_loader.h
    src/test/flow_catalog.cpp
    src/test/flow_catalog.h
    src/test/flow_validator.cpp
    src/test/flow_validator.h
    src/test/flow_writer.cpp
    src/test/flow_writer.h
    src/test/device_command.h
    src/test/test_result.h
    src/test/test_device.h
    src/test/test_engine.cpp
    src/test/test_engine.h
    src/storage/test_repository.cpp
    src/storage/test_repository.h
    src/storage/system_repository.cpp
    src/storage/system_repository.h
    src/storage/summary_writer.cpp
    src/storage/summary_writer.h
)

# PUBLIC：本工程内所有目标都能以 "protocol/frame.h" 这种形式包含头文件。
target_include_directories(aegis_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# PUBLIC：链接 aegis_core 的目标会自动继承这些 Qt 依赖，无需重复声明。
target_link_libraries(aegis_core PUBLIC Qt6::Core Qt6::Network Qt6::SerialPort Qt6::Sql Qt6::Qml)

# 程序版本号：由 project(... VERSION) 统一生成，界面/版本检测共用同一来源。
target_compile_definitions(aegis_core PUBLIC AEGIS_VERSION="${PROJECT_VERSION}")

# OPC UA：链接 open62541 并把宏传给所有依赖方（客户端/模拟器按需使用）。
if(AEGIS_HAS_OPCUA)
    target_link_libraries(aegis_core PUBLIC open62541)
    target_compile_definitions(aegis_core PUBLIC AEGIS_HAS_OPCUA=1)
endif()

# =============================================================================
# 上位机主程序 aegis_hmi
# WIN32 关键字表示生成"窗口子系统"程序：双击运行时不会弹出黑色控制台窗口。
# =============================================================================
qt_add_executable(aegis_hmi WIN32
    src/app/main.cpp
    src/ui/alarm_page.cpp
    src/ui/alarm_page.h
    src/ui/approval_dialog.cpp
    src/ui/approval_dialog.h
    src/ui/audit_page.cpp
    src/ui/audit_page.h
    src/ui/dashboard_page.cpp
    src/ui/dashboard_page.h
    src/ui/flow_editor_dialog.cpp
    src/ui/flow_editor_dialog.h
    src/ui/history_page.cpp
    src/ui/history_page.h
    src/ui/help_dialog.cpp
    src/ui/help_dialog.h
    src/ui/label_report.cpp
    src/ui/label_report.h
    src/ui/login_dialog.cpp
    src/ui/login_dialog.h
    src/ui/main_window.cpp
    src/ui/main_window.h
    src/ui/pdf_report.cpp
    src/ui/pdf_report.h
    src/ui/plugin_page.cpp
    src/ui/plugin_page.h
    src/ui/shift_page.cpp
    src/ui/shift_page.h
    src/ui/sop_dialog.cpp
    src/ui/sop_dialog.h
    src/ui/spc_page.cpp
    src/ui/spc_page.h
    src/ui/test_page.cpp
    src/ui/test_page.h
    src/ui/trend_page.cpp
    src/ui/trend_page.h
    src/ui/trend_view.cpp
    src/ui/trend_view.h
    src/ui/theme.cpp
    src/ui/theme.h
    src/ui/user_page.cpp
    src/ui/user_page.h
    src/ui/waveform_view.cpp
    src/ui/waveform_view.h
    src/ui/work_order_dialog.cpp
    src/ui/work_order_dialog.h
)
# PRIVATE：这些依赖只给 aegis_hmi 自己用，不向外传播。
target_link_libraries(aegis_hmi PRIVATE aegis_core Qt6::Widgets)

# =============================================================================
# 设备模拟器 aegis_sim
# 作用：模拟一台通过 TCP 持续上报采样数据的下位机，使项目在没有真实硬件时也能演示。
# 这是一个控制台程序（没有 WIN32），运行时可以看到日志输出。
# =============================================================================
qt_add_executable(aegis_sim
    simulator/main.cpp
)
target_link_libraries(aegis_sim PRIVATE aegis_core)

# =============================================================================
# 多语言（SY-06）：把 translations/*.ts 编译成 build/bin/translations/*.qm。
# 运行期由 core::LanguageManager 从 <exe>/translations 加载。
# =============================================================================
set(AEGIS_QM_DIR ${CMAKE_BINARY_DIR}/bin/translations)
if(TARGET Qt6::lrelease)
    add_custom_command(
        OUTPUT ${AEGIS_QM_DIR}/aegis_en_US.qm
        COMMAND ${CMAKE_COMMAND} -E make_directory ${AEGIS_QM_DIR}
        COMMAND Qt6::lrelease ${CMAKE_CURRENT_SOURCE_DIR}/translations/aegis_en_US.ts
                -qm ${AEGIS_QM_DIR}/aegis_en_US.qm
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/translations/aegis_en_US.ts
        COMMENT "Compiling translations (lrelease)"
        VERBATIM
    )
    add_custom_target(aegis_translations ALL DEPENDS ${AEGIS_QM_DIR}/aegis_en_US.qm)
    add_dependencies(aegis_hmi aegis_translations)
    set(AEGIS_HAS_TRANSLATIONS ON)
else()
    message(STATUS "lrelease not found, runtime language switch uses Chinese only")
    set(AEGIS_HAS_TRANSLATIONS OFF)
endif()

# =============================================================================
# 单元测试 tst_core
# enable_testing + add_test 让 ctest 能发现并执行测试；也可以直接运行 tst_core.exe。
# =============================================================================
enable_testing()
qt_add_executable(tst_core
    tests/tst_core.cpp
)
target_link_libraries(tst_core PRIVATE aegis_core Qt6::Test)
add_test(NAME tst_core COMMAND tst_core)

# 测试流程配置：加载 + 校验
qt_add_executable(tst_flow
    tests/tst_flow.cpp
)
target_link_libraries(tst_flow PRIVATE aegis_core Qt6::Test)
# 把源码目录传给测试，用于加载仓库里真实的示例配置
target_compile_definitions(tst_flow PRIVATE AEGIS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME tst_flow COMMAND tst_flow)

# 测试流程执行引擎
qt_add_executable(tst_engine
    tests/tst_engine.cpp
)
target_link_libraries(tst_engine PRIVATE aegis_core Qt6::Test)
add_test(NAME tst_engine COMMAND tst_engine)

# 设备服务与引擎的集成测试（进程内假设备，不依赖模拟器进程）
qt_add_executable(tst_deviceservice
    tests/tst_deviceservice.cpp
)
target_link_libraries(tst_deviceservice PRIVATE aegis_core Qt6::Test)
add_test(NAME tst_deviceservice COMMAND tst_deviceservice)

# SQLite 仓储：保存、查询、CSV 导出
qt_add_executable(tst_repository
    tests/tst_repository.cpp
)
target_link_libraries(tst_repository PRIVATE aegis_core Qt6::Test)
add_test(NAME tst_repository COMMAND tst_repository)

# Modbus 编解码
qt_add_executable(tst_modbus
    tests/tst_modbus.cpp
)
target_link_libraries(tst_modbus PRIVATE aegis_core Qt6::Test)
add_test(NAME tst_modbus COMMAND tst_modbus)

# OPC UA 客户端集成测试（进程内启动 UA 服务端，不依赖外部进程）
qt_add_executable(tst_opcua
    tests/tst_opcua.cpp)
target_link_libraries(tst_opcua PRIVATE aegis_core Qt6::Test)
add_test(NAME tst_opcua COMMAND tst_opcua)

# SCPI 仪器客户端：进程内假仪器（QTcpServer）+ 查询/只写/超时/轮询
qt_add_executable(tst_scpi
    tests/tst_scpi.cpp
)
target_link_libraries(tst_scpi PRIVATE aegis_core Qt6::Test)
add_test(NAME tst_scpi COMMAND tst_scpi)

# MES 上传：进程内 HTTP 假服务端 + outbox 重试
qt_add_executable(tst_mes
    tests/tst_mes.cpp
)
target_link_libraries(tst_mes PRIVATE aegis_core Qt6::Test)
add_test(NAME tst_mes COMMAND tst_mes)

# SPC 计算：Cp/Cpk、I-MR 控制限、判异规则
qt_add_executable(tst_spc
    tests/tst_spc.cpp
)
target_link_libraries(tst_spc PRIVATE aegis_core Qt6::Test)
add_test(NAME tst_spc COMMAND tst_spc)

# 用户插件（TP-11）：独立进程 + JSON stdio；plugin_stub 为测试替身
qt_add_executable(plugin_stub
    tests/plugin_stub.cpp
)
target_link_libraries(plugin_stub PRIVATE Qt6::Core)

qt_add_executable(tst_plugin
    tests/tst_plugin.cpp
)
target_link_libraries(tst_plugin PRIVATE aegis_core Qt6::Test)
add_test(NAME tst_plugin COMMAND tst_plugin)
add_dependencies(tst_plugin plugin_stub)

# 轻量版本检测（SY-05）：版本比较、清单解析、SHA-256 校验、离线静默跳过
qt_add_executable(tst_update
    tests/tst_update.cpp
)
target_link_libraries(tst_update PRIVATE aegis_core Qt6::Test)
add_test(NAME tst_update COMMAND tst_update)

# 多语言（SY-06）：翻译文件可加载、关键文案可翻译、未知语言回退中文
if(AEGIS_HAS_TRANSLATIONS)
    qt_add_executable(tst_i18n
        tests/tst_i18n.cpp
    )
    target_link_libraries(tst_i18n PRIVATE aegis_core Qt6::Test)
    add_dependencies(tst_i18n aegis_translations)
    add_test(NAME tst_i18n COMMAND tst_i18n)
endif()

# 工位配置解析 + PDF 审核报告（QPdfWriter 属于 Qt6::Gui；离屏运行）
qt_add_executable(tst_report
    tests/tst_report.cpp
    src/ui/pdf_report.cpp
    src/ui/pdf_report.h
    src/ui/label_report.cpp
    src/ui/label_report.h
)
target_link_libraries(tst_report PRIVATE aegis_core Qt6::Test Qt6::Widgets)
add_test(NAME tst_report COMMAND tst_report)
set_tests_properties(tst_report PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 性能基准：解码吞吐 + 环形缓冲开销（输出到控制台，供性能报告引用）
qt_add_executable(aegis_bench
    tools/bench.cpp
)
target_link_libraries(aegis_bench PRIVATE aegis_core)

# OPC UA 快速验证工具：订阅节点 / 写设定值（无 GUI 时排查现场问题）
qt_add_executable(aegis_opcua_probe
    tools/opcua_probe.cpp
)
target_link_libraries(aegis_opcua_probe PRIVATE aegis_core)

# 连接测试工具：按 fleet.json 连接全部设备/链路并检查入站数据（模拟器集群验收用）
qt_add_executable(aegis_linkcheck
    tools/linkcheck.cpp
)
target_link_libraries(aegis_linkcheck PRIVATE aegis_core)
