import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

PageDialog {
    id: conversion
    objectName: "conversionDialog"
    signal converted()
    signal warningRequested()
    property int stage: 0
    property string targetFile: ""
    property string targetName: ""
    property var review: ({})
    function showPicker() {
        store.cancelConversion()
        stage = 0
        radioSearch.text = ""
        review = ({})
        open()
    }
    function choose(kind, index, included) {
        const result = store.setConversionChoice(kind, index, included)
        if (result.target) review = result
    }
    function summaryRows() {
        let result = []
        for (const group of [{ key: "blockers", title: "Needs attention" },
                             { key: "changes", title: "Will change" },
                             { key: "losses", title: "Will not transfer" }]) {
            const entries = review[group.key] || []
            if (!entries.length) continue
            result.push({ heading: true, text: group.title })
            for (const text of entries) result.push({ heading: false, text: text })
        }
        if (!result.length) result.push({ heading: false, text: "All selected channels, contacts, and repeater/zone memberships fit the destination." })
        return result
    }
    title: stage === 0 ? "Convert codeplug — choose a radio" : "Convert to " + targetName
    modal: true
    width: Math.min(stage === 0 ? 520 : 900, parent.width - 32)
    height: Math.min(680, parent.height - 32)
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    standardButtons: Dialog.NoButton
    onClosed: store.cancelConversion()

    component Choices: ColumnLayout {
        id: choices
        property string kind
        PageField {
            id: search
            Layout.fillWidth: true
            placeholderText: "Search " + (choices.kind === "zones" ? "repeaters/zones" : choices.kind)
            Accessible.name: placeholderText
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            model: (conversion.review[choices.kind] || []).filter(function(row) {
                const query = search.text.trim().toLowerCase()
                return row.name.toLowerCase().indexOf(query) !== -1
                    || row.detail.toLowerCase().indexOf(query) !== -1
            })
            delegate: Rectangle {
                required property var modelData
                width: list.width - 12
                implicitHeight: details.implicitHeight + 20
                radius: 10
                color: "#f7faf7"
                border.color: "#dce4df"
                RowLayout {
                    id: details
                    x: 10; y: 10
                    width: parent.width - 20
                    spacing: 10
                    PageCheckBox {
                        Layout.preferredWidth: 28
                        checked: modelData.checked
                        enabled: modelData.enabled
                        Accessible.name: "Keep " + modelData.name
                        onClicked: conversion.choose(choices.kind, modelData.index, checked)
                    }
                    ColumnLayout {
                        Layout.minimumWidth: 0
                        Layout.fillWidth: true
                        spacing: 3
                        Label {
                            text: modelData.name
                            font.bold: true
                            color: "#173d30"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }
                        Label {
                            text: modelData.detail
                            color: "#64776c"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 12
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: conversion.stage
            ColumnLayout {
                spacing: 12
                Label { text: "Choose a destination radio"; font.pixelSize: 22; font.bold: true; color: "#173d30" }
                PageField {
                    id: radioSearch
                    Layout.fillWidth: true
                    placeholderText: "Search radios"
                    Accessible.name: "Search destination radios"
                }
                ListView {
                    id: radios
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 8
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    model: store.newProjectOptions.filter(function(option) {
                        const query = radioSearch.text.trim().toLowerCase()
                        return option.model.toLowerCase().indexOf(query) !== -1
                            || option.fileName.toLowerCase().indexOf(query) !== -1
                    })
                    delegate: ItemDelegate {
                        id: card
                        required property var modelData
                        width: radios.width - 12
                        height: 76
                        enabled: modelData.available
                        onClicked: {
                            conversion.targetFile = modelData.fileName
                            conversion.targetName = modelData.model
                            twoMeters.checked = false
                            seventyCm.checked = false
                            conversion.warningRequested()
                        }
                        background: Rectangle {
                            radius: 12
                            color: card.hovered && card.enabled ? "#e7f2e9" : "#f7faf7"
                            border.color: "#cbded0"
                        }
                        contentItem: RowLayout {
                            spacing: 12
                            Image {
                                Layout.preferredWidth: 48
                                Layout.preferredHeight: 48
                                fillMode: Image.PreserveAspectFit
                                source: card.modelData.hasIcon ? "image://radioicons/" + card.modelData.fileName : "qrc:/assets/waffle-radio.png"
                            }
                            Label {
                                text: card.modelData.model
                                font.pixelSize: 17
                                font.bold: true
                                color: card.enabled ? "#173d30" : "#839188"
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }
                Label { visible: radios.count === 0; text: "No matching radios"; color: "#64776c" }
            }
            ColumnLayout {
                spacing: 16
                Label {
                    text: "Select the bands supported by the destination radio."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#173d30"
                }
                PageCheckBox { id: twoMeters; objectName: "conversionTwoMeters"; text: "VHF (136–174 MHz)" }
                PageCheckBox { id: seventyCm; objectName: "conversionSeventyCm"; text: "UHF (400–470 MHz)" }
                Label {
                    text: "Channels outside these ranges will be listed in the review. Used contacts are preferred, with group calls ahead of private contacts. Oversized repeaters/zones are split automatically."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#64776c"
                }
                Item { Layout.fillHeight: true }
            }
            ColumnLayout {
                spacing: 10
                Label {
                    text: "Review the conversion to " + conversion.targetName + ". Bands: " + (conversion.review.bandSummary || "") + "."
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#173d30"
                    font.bold: true
                }
                Label {
                    text: conversion.review.notice || ""
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#64776c"
                    font.pixelSize: 12
                }
                Label {
                    text: (conversion.review.contactCount || 0) + " / " + (conversion.review.contactCapacity || 0) + " contacts · "
                        + (conversion.review.channelCount || 0) + " / " + (conversion.review.channelCapacity || 0) + " channels · "
                        + (conversion.review.zoneCount || 0) + " / " + (conversion.review.zoneCapacity || 0) + " repeaters/zones"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#173d30"
                }
                TabBar {
                    id: tabs
                    Layout.fillWidth: true
                    PageTabButton { text: "Summary" }
                    PageTabButton { text: "Contacts" }
                    PageTabButton { text: "Channels" }
                    PageTabButton { text: "Repeaters/Zones" }
                }
                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: tabs.currentIndex
                    ListView {
                        id: summary
                        clip: true
                        spacing: 8
                        model: conversion.summaryRows()
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        delegate: Label {
                            required property var modelData
                            width: summary.width - 12
                            text: modelData.text
                            font.bold: modelData.heading
                            font.pixelSize: modelData.heading ? 17 : 14
                            color: modelData.heading ? "#173d30" : "#64776c"
                            wrapMode: Text.WordWrap
                        }
                    }
                    Choices { kind: "contacts" }
                    Choices { kind: "channels" }
                    Choices { kind: "zones" }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            PageAction {
                text: "Back"
                visible: conversion.stage > 0
                onClicked: conversion.stage -= 1
            }
            Item { Layout.fillWidth: true }
            PageAction { text: "Cancel"; onClicked: conversion.close() }
            PageAction {
                objectName: "conversionContinueButton"
                visible: conversion.stage > 0
                text: conversion.stage === 1 ? "Review changes" : "Convert"
                primary: true
                enabled: conversion.stage === 1 ? twoMeters.checked || seventyCm.checked : !!conversion.review.canConvert
                onClicked: {
                    if (conversion.stage === 1) {
                        const result = store.beginConversion(conversion.targetFile, twoMeters.checked, seventyCm.checked)
                        if (result.target) {
                            conversion.review = result
                            tabs.currentIndex = 0
                            conversion.stage = 2
                        }
                    } else if (store.applyConversion()) {
                        conversion.close()
                        Qt.callLater(() => conversion.converted())
                    }
                }
            }
        }
    }
}
