#include "../src/codeplugconversion.h"
#include "../src/codeplugdefinition.h"
#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QJsonArray>
#include <stdexcept>
static void check(bool ok, const QString &message) {
    if (!ok) throw std::runtime_error(message.toStdString());
}
static bool contains(const QStringList &messages, const QString &text) {
    for (const auto &message : messages) if (message.contains(text)) return true;
    return false;
}
static CodeplugConversion::Session fixture(const QString &file) {
    QString error;
    CodeplugDefinition codec;
    check(CodeplugDefinition::load("definitions/" + file, &codec, &error), error);
    CodeplugConversion::Session s;
    s.targetBase = codec.defaultCodeplug();
    check(codec.read(s.targetBase, &s.baseline, &error), error);
    s.baseline.definitionFile = file;
    check(codec.conversionMetadata(s.targetBase, &s.metadata, &error), error);
    s.source = s.baseline;
    s.source.radioName = "TEST RADIO"; s.source.radioId = 3109101;
    s.source.channels.clear(); s.source.contacts.clear(); s.source.zones.clear(); s.source.scanLists.clear();
    for (int i = 0; i < 35; ++i) {
        ImportedChannel c; c.name = "Channel " + QString::number(i + 1);
        c.frequencyAHz = c.frequencyBHz = i == 1 ? 146520000 : 446000000;
        c.receiveCtcss = c.transmitCtcss = 0;
        c.setPowerValue("high");
        s.source.channels.append(c);
    }
    ImportedZone z; z.name = "Local"; z.channelIndexes = {0, 1, 2};
    s.source.zones.append(z);
    s.source.scanListsSupported = true;
    s.twoMeters = true; s.seventyCm = true;
    return s;
}
static void exportAndRead(CodeplugConversion::Session &s) {
    QString error;
    CodeplugDefinition codec;
    check(CodeplugDefinition::load("definitions/" + s.baseline.definitionFile, &codec, &error), error);
    QByteArray bytes;
    check(codec.write(s.targetBase, s.converted, s.converted.radioName, s.converted.radioId, &bytes, &error), error);
    ImportedCodeplug again;
    check(codec.read(bytes, &again, &error), error);
    check(again.scanLists.size() == s.converted.scanLists.size(), "Scan-list export count");
    for (int i = 0; i < again.scanLists.size(); ++i)
        check(again.scanLists[i].name == s.converted.scanLists[i].name
            && again.scanLists[i].channelIndexes == s.converted.scanLists[i].channelIndexes, "Scan-list export contents");
    for (int i = 0; i < again.channels.size(); ++i)
        check(again.channels[i].scanListIndex == s.converted.channels[i].scanListIndex, "Export assignment");
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QDir::setCurrent(WAFFLE_SOURCE_DIR);
    try {
        for (const QString file : {QString("dm32uv.wcp"), QString("cs700.wcp")}) {
            auto s = fixture(file);
            ImportedScanList large; large.name = "Long scan name";
            for (int i = 0; i < 35; ++i) large.channelIndexes.append(i);
            ImportedScanList special; special.name = "Selected"; special.channelIndexes = {-1, 2, 0};
            s.source.scanLists = {large, special};
            s.source.channels[0].scanListIndex = 1; s.source.channels[2].scanListIndex = 2;
            s.twoMeters = false;
            CodeplugConversion::rebuild(s, true);
            check(s.review.value("canConvert").toBool(), "Conversion blocked");
            check(s.converted.scanLists.size() == 2, "Lists not transferred");
            check(s.converted.scanLists[0].channelIndexes.size() == s.baseline.scanMemberCapacity, "List not truncated");
            QVector<int> expected;
            for (int i = 0; i < s.baseline.scanMemberCapacity; ++i) expected.append(i);
            check(s.converted.scanLists[0].channelIndexes == expected, "Truncation order or band remapping");
            check(s.converted.scanLists[1].channelIndexes == (s.baseline.scanCurrentChannelSupported
                ? QVector<int>{-1, 1, 0} : QVector<int>{1, 0}), "Current channel capability or order");
            check(s.converted.channels[0].scanListIndex == 1 && s.converted.channels[1].scanListIndex == 2, "Assignments lost");
            check(contains(s.review.value("losses").toStringList(), "Channel 2 (channel will not transfer)"), "Missing filtered member report");
            check(contains(s.review.value("losses").toStringList(), "list limit"), "Missing truncation report");
            if (!s.baseline.scanCurrentChannelSupported)
                check(contains(s.review.value("losses").toStringList(), "Current channel"), "Missing sentinel loss");
            exportAndRead(s);
            s.scanLists.remove(0);
            CodeplugConversion::rebuild(s);
            check(s.converted.scanLists.size() == 1 && s.converted.channels[0].scanListIndex == 0
                && s.converted.channels[1].scanListIndex == 1, "Deselection assignment remapping");
            exportAndRead(s);
            s.channels.remove(2);
            CodeplugConversion::rebuild(s);
            check(!s.review.value("scan_lists").toList().isEmpty(), "Missing scan review");
        }
        auto limited = fixture("dm32uv.wcp");
        limited.baseline.scanListCapacity = 1;
        for (int i = 0; i < 3; ++i) { ImportedScanList l; l.name = "Scan " + QString::number(i); l.channelIndexes = {0}; limited.source.scanLists.append(l); }
        limited.source.channels[0].scanListIndex = 3;
        CodeplugConversion::rebuild(limited, true);
        check(limited.converted.scanLists.size() == 1 && limited.converted.scanLists[0].name == "Scan 2"
            && limited.converted.channels[0].scanListIndex == 1, "Assigned lists not preferred");
        limited.scanLists.insert(0);
        CodeplugConversion::rebuild(limited);
        check(!limited.review.value("canConvert").toBool(), "Excess selected lists accepted");
        auto unsupported = fixture("md380.wcp");
        unsupported.source.scanLists = limited.source.scanLists; unsupported.source.channels[0].scanListIndex = 1;
        CodeplugConversion::rebuild(unsupported, true);
        check(unsupported.converted.scanLists.isEmpty() && unsupported.converted.channels[0].scanListIndex == 0, "Unsupported lists transferred");
        check(contains(unsupported.review.value("losses").toStringList(), "not supported"), "Unsupported destination not reported");
        auto empty = fixture("cs700.wcp");
        ImportedScanList l; l.name = "VHF"; l.channelIndexes = {1}; empty.source.scanLists.append(l);
        empty.source.channels[0].scanListIndex = 1; empty.twoMeters = false;
        CodeplugConversion::rebuild(empty, true);
        check(empty.converted.scanLists.isEmpty() && !empty.review.value("scan_lists").toList()[0].toMap().value("enabled").toBool(), "Emptied list retained");
        check(empty.converted.channels[0].scanListIndex == 0, "Assignment to empty list retained");
        auto currentOnly = fixture("cs700.wcp");
        l.name = "Current only"; l.channelIndexes = {-1}; currentOnly.source.scanLists.append(l);
        CodeplugConversion::rebuild(currentOnly, true);
        check(currentOnly.converted.scanLists.isEmpty()
            && contains(currentOnly.review.value("losses").toStringList(), "Current channel (not supported"), "Missing current-only list loss");
        qInfo() << "Scan-list conversion checks passed"; return 0;
    } catch (const std::exception &e) { qCritical() << e.what(); return 1; }
}
