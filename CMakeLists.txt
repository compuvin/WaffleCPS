cmake_minimum_required(VERSION 3.21)

project(WaffleCPS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Widgets)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
find_package(Threads REQUIRED)
qt_standard_project_setup()

add_library(cs7000usb STATIC src/cs7000usb.cpp src/cs7000usb.h)
target_link_libraries(cs7000usb PUBLIC PkgConfig::LIBUSB Threads::Threads)
add_executable(cs7000-read tools/cs7000_read.cpp)
target_link_libraries(cs7000-read PRIVATE cs7000usb)

add_library(radioreader STATIC
    src/radioreader.cpp src/radioreader.h
    src/radiocodeplug.cpp src/radiocodeplug.h)
target_link_libraries(radioreader PUBLIC Qt6::Core cs7000usb)

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
        qml/ZoneEditor.qml
)

target_link_libraries(wafflecps PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
    ZLIB::ZLIB
    radioreader
)

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(zone_editor_test
        tests/zone_editor.cpp tests/channel_order_fixture.h
        src/repeaterstore.cpp src/repeaterstore.h
        src/codeplugreader.cpp src/codeplugreader.h
        src/codeplugwriter.cpp src/codeplugwriter.h
        src/cs7000default.cpp src/cs7000default.h
        src/ziparchive.cpp src/ziparchive.h)
    target_link_libraries(zone_editor_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets ZLIB::ZLIB)
    add_test(NAME zone_editor COMMAND zone_editor_test)

    qt_add_executable(channel_order_test
        tests/channel_order.cpp tests/channel_order_fixture.h
        src/repeaterstore.cpp src/repeaterstore.h
        src/codeplugreader.cpp src/codeplugreader.h
        src/codeplugwriter.cpp src/codeplugwriter.h
        src/cs7000default.cpp src/cs7000default.h
        src/ziparchive.cpp src/ziparchive.h)
    target_link_libraries(channel_order_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets ZLIB::ZLIB)
    add_test(NAME channel_order COMMAND channel_order_test)

    qt_add_executable(radio_import_test
        tests/radio_import.cpp
        src/repeaterstore.cpp src/repeaterstore.h
        src/codeplugreader.cpp src/codeplugreader.h
        src/codeplugwriter.cpp src/codeplugwriter.h
        src/cs7000default.cpp src/cs7000default.h
        src/ziparchive.cpp src/ziparchive.h)
    target_link_libraries(radio_import_test PRIVATE radioreader Qt6::Gui Qt6::Widgets ZLIB::ZLIB)
    add_test(NAME radio_import COMMAND radio_import_test)

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
        FILES assets/waffle-radio.png qml/Main.qml qml/SlotPanel.qml qml/ContactsView.qml qml/ZoneEditor.qml
    )
    target_link_libraries(ui_smoke_test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Widgets ZLIB::ZLIB radioreader)
    foreach(mode IN ITEMS manual saved saved-live)
        add_test(NAME ui_repeater_switch_${mode} COMMAND ui_smoke_test --demo --switch-${mode})
        set_tests_properties(ui_repeater_switch_${mode} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    endforeach()
    add_test(NAME ui_required_contact COMMAND ui_smoke_test --demo --required-contact)
    set_tests_properties(ui_required_contact PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    foreach(mode IN ITEMS imported new contacts)
        if(mode STREQUAL "imported")
            set(advanced_argument "--advanced-zone")
        else()
            set(advanced_argument "--advanced-zone-${mode}")
        endif()
        add_test(NAME ui_advanced_zone_${mode} COMMAND ui_smoke_test --demo ${advanced_argument})
        set_tests_properties(ui_advanced_zone_${mode} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
    add_test(NAME ui_live_import_new COMMAND ui_smoke_test --demo --live-import-new)
    set_tests_properties(ui_live_import_new PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME ui_remove_no_contact COMMAND ui_smoke_test --demo --remove-no-contact)
    set_tests_properties(ui_remove_no_contact PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME ui_zone_editor_large COMMAND ui_smoke_test --demo --zones-large)
    set_tests_properties(ui_zone_editor_large PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME ui_zone_editor_new COMMAND ui_smoke_test --demo --zones-new)
    set_tests_properties(ui_zone_editor_new PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME ui_zone_editor COMMAND ui_smoke_test --demo --zones)
    set_tests_properties(ui_zone_editor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
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
    add_test(NAME ui_manual_channel_order COMMAND ui_smoke_test --demo --manual-order)
    set_tests_properties(ui_manual_channel_order PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME ui_channel_order COMMAND ui_smoke_test --demo --order)
    set_tests_properties(ui_channel_order PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    foreach(flow success cancel failure)
        add_test(NAME ui_radio_${flow} COMMAND ui_smoke_test --demo --radio-${flow})
        set_tests_properties(ui_radio_${flow} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
endif()
