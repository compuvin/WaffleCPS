cmake_minimum_required(VERSION 3.21)

project(WaffleCPS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Widgets)
find_package(ZLIB REQUIRED)
qt_standard_project_setup()

qt_add_executable(wafflecps
    src/main.cpp
    src/repeaterstore.cpp
    src/repeaterstore.h
    src/ziparchive.cpp
    src/ziparchive.h
)

qt_add_resources(wafflecps "qml"
    PREFIX "/"
    FILES
        assets/waffle-radio.png
        qml/Main.qml
        qml/SlotPanel.qml
)

target_link_libraries(wafflecps PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
    ZLIB::ZLIB
)

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(project_roundtrip_test
        tests/project_roundtrip.cpp
        src/repeaterstore.cpp
        src/repeaterstore.h
        src/ziparchive.cpp
        src/ziparchive.h
    )
    target_link_libraries(project_roundtrip_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets ZLIB::ZLIB)
    add_test(NAME project_roundtrip COMMAND project_roundtrip_test)
endif()
