#include "../src/radiocodeplug.h"
#include "../src/radioreader.h"
#include "../src/repeaterstore.h"
#include "../src/cs7000default.h"
#include "../src/ziparchive.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <QDebug>
#include <stdexcept>

namespace {
void check(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}
QByteArray decode(const QByteArray &encoded)
{
    const auto mask = encoded.mid(0x300, 256);
    QByteArray result(encoded.size(), Qt::Uninitialized);
    for (qsizetype i = 0; i < result.size(); ++i) result[i] = encoded[i] ^ mask[i % 256];
    return result;
}
void waitFor(RadioReader &reader)
{
    QElapsedTimer timeout;
    timeout.start();
    while (reader.busy() && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    check(!reader.busy(), "Worker did not finish");
    QCoreApplication::processEvents();
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir directory;
        check(directory.isValid(), "Cannot create temporary test directory");
        auto raw = decode(cs7000DefaultCodeplug());
        // Unknown bytes and erased padding must survive conversion and project storage.
        raw.replace(0x600, 32, QByteArray(32, char(0xff)));
        raw.replace(0x900, 19, QByteArray("opaque radio config"));
        raw.replace(0x10, 16, QByteArray("*-DR7800-RCDB-*\1\0", 16));
        const QString name = QStringLiteral("TEST RADIO");
        for (int i = 0; i < 16; ++i)
            qToLittleEndian<quint16>(i < name.size() ? name.at(i).unicode() : 0,
                                    reinterpret_cast<uchar *>(raw.data() + 0x1300 + 2 * i));
        qToLittleEndian<quint32>(3109101, reinterpret_cast<uchar *>(raw.data() + raw.size() - 0xc10c93));
        QByteArray encoded;
        QString error;
        check(radioCodeplugFromRaw(raw, &encoded, &error), qPrintable(error));
        check(decode(encoded) == raw, "Conversion changed downloaded bytes");

        auto invalid = raw;
        invalid[0x300] = 1;
        QByteArray untouched("sentinel");
        check(!radioCodeplugFromRaw(invalid, &untouched, &error)
              && untouched == "sentinel", "Unsupported layout was converted");
        check(!radioCodeplugFromRaw(QByteArray(), &untouched, &error), "Truncation accepted");

        RepeaterStore store;
        const QString oldName = store.projectDisplayName();
        const auto oldContacts = store.contacts();
        bool failed = false, completed = false;
        RadioReader failing(nullptr, [](const auto &, const auto &) -> Cs7000Usb::Bytes {
            throw std::runtime_error("simulated USB disconnect");
        });
        QObject::connect(&failing, &RadioReader::failed, &app, [&](const QString &message) {
            failed = message.contains(QStringLiteral("disconnect"));
        });
        QObject::connect(&failing, &RadioReader::completed, &store, &RepeaterStore::importRadioCodeplug);
        failing.start();
        waitFor(failing);
        check(failed && store.projectDisplayName() == oldName && store.contacts() == oldContacts,
              "USB failure changed current project");

        failed = false;
        RadioReader invalidReader(nullptr, [](const auto &, const auto &) { return Cs7000Usb::Bytes{}; });
        QObject::connect(&invalidReader, &RadioReader::failed, &app, [&](const QString &) { failed = true; });
        QObject::connect(&invalidReader, &RadioReader::completed, &store, &RepeaterStore::importRadioCodeplug);
        invalidReader.start();
        waitFor(invalidReader);
        check(failed && store.contacts() == oldContacts, "Invalid result changed current project");
        check(!store.importRadioCodeplug(QByteArray("invalid"))
              && store.contacts() == oldContacts, "Invalid import changed current project");

        RadioReader cancelling(nullptr, [](const auto &progress, const auto &cancelled) -> Cs7000Usb::Bytes {
            progress("contacts", 1, 10);
            while (!cancelled.load()) QThread::msleep(1);
            throw Cs7000Usb::Cancelled();
        });
        failed = completed = false;
        QObject::connect(&cancelling, &RadioReader::failed, &app, [&](const QString &) { failed = true; });
        QObject::connect(&cancelling, &RadioReader::completed, &app, [&](const QByteArray &) { completed = true; });
        cancelling.start();
        QTimer::singleShot(20, &cancelling, &RadioReader::cancel);
        waitFor(cancelling);
        check(!failed && !completed && store.contacts() == oldContacts,
              "Cancellation delivered a result or changed current project");

        int heartbeats = 0;
        std::atomic_int readCalls{0};
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++heartbeats; });
        heartbeat.start(5);
        RadioReader reader(nullptr, [&](const auto &progress, const auto &) {
            ++readCalls;
            for (int i = 1; i <= 10; ++i) {
                progress("main", uint32_t(i), 10);
                QThread::msleep(5);
            }
            return Cs7000Usb::Bytes(raw.begin(), raw.end());
        });
        QObject::connect(&reader, &RadioReader::completed, &store, [&](const QByteArray &bytes) {
            completed = store.importRadioCodeplug(bytes);
        });
        completed = false;
        reader.start();
        reader.start(); // A second click must not open a second transport.
        waitFor(reader);
        check(completed && heartbeats > 2 && readCalls == 1, "Worker blocked UI or started twice");
        check(store.hasImportedCodeplug() && store.sourceCodeplugName() == "radio-read.rdbx"
              && store.modified() && store.radioName() == name && store.radioId() == 3109101,
              "Radio result was not imported");
        ImportedCodeplug parsed;
        check(CodeplugReader::read(encoded, &parsed, &error), "Cannot parse fixture");
        check(store.contacts().size() == parsed.contacts.size(), "Old contacts leaked into radio project");

        const auto project = directory.filePath("radio.waffle");
        check(store.saveProjectToFile(project), "Radio project did not save");
        QMap<QString, QByteArray> entries;
        check(ZipArchive::read(project, &entries, &error), qPrintable(error));
        const auto source = entries.value("source/original-codeplug.bin");
        check(source == encoded && decode(source) == raw, "Project attachment lost unknown bytes");
        RepeaterStore loaded;
        check(loaded.openProjectFromFile(project) && loaded.radioName() == name,
              "Radio project did not reopen");
        const auto exported = directory.filePath("radio-export.rdbx");
        check(loaded.exportCodeplugToFile(exported), "Radio project did not export");
        QFile output(exported);
        check(output.open(QIODevice::ReadOnly) && output.readAll() == encoded,
              "Unedited export changed original radio bytes");
        check(loaded.updateRadioSettings(QStringLiteral("EDITED RADIO"), QStringLiteral("3109101")),
              "Controlled radio-name edit failed");
        check(loaded.exportCodeplugToFile(exported), "Edited project did not export");
        output.close();
        check(output.open(QIODevice::ReadOnly), "Cannot open edited export");
        const auto edited = decode(output.readAll());
        for (qsizetype i = 0; i < raw.size(); ++i) {
            const auto idMarker = raw.size() - 0xc10c93 + 4;
            if ((i >= 0x1300 && i < 0x1320) || i == idMarker) continue;
            check(edited[i] == raw[i], "Radio-name edit changed unrelated radio data");
        }
        qInfo() << "Radio conversion, source preservation, export, error/cancel and worker responsiveness passed";
        return 0;
    } catch (const std::exception &error) {
        qCritical() << error.what();
        return 1;
    }
}
