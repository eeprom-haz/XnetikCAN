cmake_minimum_required(VERSION 3.19)
project(XnetikCan VERSION 0.1 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Qt Creator sets CMAKE_PREFIX_PATH automatically.
# CLI build: cmake -DCMAKE_PREFIX_PATH=J:/Qt/6.11.1/mingw_64 ..
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network SerialBus)

qt_add_executable(XnetikCan
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/ConnectDialog.h
    src/ConnectDialog.cpp
    src/can/RingBuffer.h
    src/can/CanBridge.h
    src/can/CanBridge.cpp
    src/can/BridgeProtocol.h
    src/can/BridgeDevice.h
    src/can/BridgeDevice.cpp
    src/can/GsUsbDevice.h
    src/can/GsUsbDevice.cpp
    src/can/ReplayEngine.h
    src/can/ReplayEngine.cpp
    src/filter/FilterEngine.h
    src/filter/FilterEngine.cpp
    src/diagnose/ClaudeClient.h
    src/diagnose/ClaudeClient.cpp
    src/diagnose/DiagnosePanel.h
    src/diagnose/DiagnosePanel.cpp
    src/obd/ObdPids.h
    src/obd/ObdPids.cpp
    src/obd/LiveDataDock.h
    src/obd/LiveDataDock.cpp
    src/obd/DidScanDialog.h
    src/obd/DidScanDialog.cpp
    src/obd/CodesDialog.h
    src/obd/CodesDialog.cpp
    src/obd/Units.h
    src/obd/Units.cpp
    src/obd/OverlayWindow.h
    src/obd/OverlayWindow.cpp
    src/diag/DtcService.h
    src/diag/DtcService.cpp
    src/diag/DtcDatabase.h
    src/diag/DtcDatabase.cpp
    src/diag/IsoTp.h
    src/diag/IsoTp.cpp
    src/diag/DiagPoller.h
    src/diag/DiagPoller.cpp
    src/diag/EnhancedLibrary.h
    src/diag/EnhancedLibrary.cpp
    src/obd/VehicleId.h
    src/obd/VehicleId.cpp
    src/obd/IdentifyDialog.h
    src/obd/IdentifyDialog.cpp
    src/diag/StreamSession.h
    src/diag/StreamSession.cpp
    src/diag/DiagLog.h
    src/diag/DiagLog.cpp
    src/diag/OeLibrary.h
    src/diag/OeLibrary.cpp
    src/dbc/DbcFile.h
    src/dbc/DbcFile.cpp
    src/dbc/DbcLibrary.h
    src/dbc/DbcLibrary.cpp
    src/plot/LivePlot.h
    src/plot/LivePlot.cpp
    resources/resources.qrc
    resources/app.rc
)

target_include_directories(XnetikCan PRIVATE src)

target_link_libraries(XnetikCan PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::SerialBus
    winusb
    setupapi
)

set_target_properties(XnetikCan PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# Ship the vehicle-definition library and the 32-bit J2534 bridge next to the exe.
add_custom_command(TARGET XnetikCan POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/../library $<TARGET_FILE_DIR:XnetikCan>/library
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/../build-bridge32-mingw5/J2534Bridge.exe $<TARGET_FILE_DIR:XnetikCan>
    COMMENT "Copying library/ and J2534Bridge.exe")
