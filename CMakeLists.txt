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
    src/codeplugreader.cpp
    src/codeplugreader.h
    src/codeplugwriter.cpp
    src/codeplugwriter.h
    src/cs7000default.cpp
    src/cs7000default.h
    src/ziparchive.cpp
    src/ziparchive.h
)

qt_add_resources(wafflecps "qml"
    PREFIX "/"
    FILES
        assets/waffle-radio.png
        qml/Main.qml
        qml/SlotPanel.qml
        qml/ContactsView.qml
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
        src/codeplugreader.cpp
        src/codeplugreader.h
        src/codeplugwriter.cpp
        src/codeplugwriter.h
        src/cs7000default.cpp
        src/cs7000default.h
        src/ziparchive.cpp
        src/ziparchive.h
    )
    target_link_libraries(project_roundtrip_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets ZLIB::ZLIB)
    add_test(NAME project_roundtrip COMMAND project_roundtrip_test)

    qt_add_executable(codeplug_import_test
        tests/codeplug_import.cpp
        src/repeaterstore.cpp
        src/repeaterstore.h
        src/codeplugreader.cpp
        src/codeplugreader.h
        src/codeplugwriter.cpp
        src/codeplugwriter.h
        src/cs7000default.cpp
        src/cs7000default.h
        src/ziparchive.cpp
        src/ziparchive.h
    )
    target_link_libraries(codeplug_import_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets ZLIB::ZLIB)
    add_test(NAME codeplug_import COMMAND codeplug_import_test)

    qt_add_executable(ui_smoke_test
        tests/ui_smoke.cpp
        src/repeaterstore.cpp
        src/repeaterstore.h
        src/codeplugreader.cpp
        src/codeplugreader.h
        src/codeplugwriter.cpp
        src/codeplugwriter.h
        src/cs7000default.cpp
        src/cs7000default.h
        src/ziparchive.cpp
        src/ziparchive.h
    )
    qt_add_resources(ui_smoke_test "test_qml"
        PREFIX "/"
        FILES assets/waffle-radio.png qml/Main.qml qml/SlotPanel.qml qml/ContactsView.qml
    )
    target_link_libraries(ui_smoke_test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Widgets ZLIB::ZLIB)
    add_test(NAME ui_smoke COMMAND ui_smoke_test)
    set_tests_properties(ui_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME ui_smoke_new COMMAND ui_smoke_test --new)
    set_tests_properties(ui_smoke_new PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME ui_smoke_new_imported COMMAND ui_smoke_test --embedded --click-new)
    set_tests_properties(ui_smoke_new_imported PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME ui_smoke_add_talkgroup COMMAND ui_smoke_test --demo --add-talkgroup)
    set_tests_properties(ui_smoke_add_talkgroup PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME ui_smoke_create_talkgroup COMMAND ui_smoke_test --demo --create-talkgroup)
    set_tests_properties(ui_smoke_create_talkgroup PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME ui_smoke_power COMMAND ui_smoke_test --demo --power)
    set_tests_properties(ui_smoke_power PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME ui_smoke_settings COMMAND ui_smoke_test --demo --settings)
    set_tests_properties(ui_smoke_settings PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
