#include "../src/radiowriter.h"
#include "../src/repeaterstore.h"
#include "../src/cs7000default.h"
#include "../src/radioreader.h"
#include "../src/codeplugdefinitions.h"
#include "channel_order_fixture.h"

#include <QApplication>
#include <QAbstractButton>
#include <QDebug>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>
#include <QImage>
#include <QMessageBox>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQuickImageProvider>
#include <QTimer>
#include <QUrl>
#include <QThread>
#include <QJSValue>
#include <stdexcept>
#include <memory>

class TestRadioIconProvider : public QQuickImageProvider
{
public:
    TestRadioIconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override
    {
        QImage icon = QImage::fromData(CodeplugDefinitions::iconPng(id), "PNG");
        if (icon.isNull()) icon.load(QStringLiteral(":/assets/waffle-radio.png"));
        if (size) *size = icon.size();
        return requestedSize.isValid()
            ? icon.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            : icon;
    }
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    const bool switchManual = argc > 2 && QByteArray(argv[2]) == "--switch-manual";
    const bool switchSaved = argc > 2 && QByteArray(argv[2]) == "--switch-saved";
    const bool switchLive = argc > 2 && QByteArray(argv[2]) == "--switch-saved-live";
    const bool exerciseSwitching = switchManual || switchSaved || switchLive;
    std::unique_ptr<QTemporaryDir> switchDirectory;
    QString switchPath;
    const bool requiredContact = argc > 2 && QByteArray(argv[2]) == "--required-contact";
    const bool advancedNew = argc > 2 && QByteArray(argv[2]) == "--advanced-zone-new";
    const bool advancedLarge = argc > 2 && QByteArray(argv[2]) == "--advanced-zone-contacts";
    const bool exerciseAdvanced = (argc > 2 && QByteArray(argv[2]) == "--advanced-zone") || advancedNew || advancedLarge;
    const bool liveImportNew = argc > 2 && QByteArray(argv[2]) == "--live-import-new";
    const bool removeNoContact = argc > 2 && QByteArray(argv[2]) == "--remove-no-contact";
    const bool largeZones = argc > 2 && QByteArray(argv[2]) == "--zones-large";
    const bool freshZones = argc > 2 && QByteArray(argv[2]) == "--zones-new";
    const bool exerciseZones = (argc > 2 && QByteArray(argv[2]) == "--zones") || freshZones || largeZones;
    const bool clickNew = argc > 2 && QByteArray(argv[2]) == "--click-new";
    const bool openContacts = argc > 2 && QByteArray(argv[2]) == "--contacts";
    const bool exerciseAdd = argc > 2 && QByteArray(argv[2]) == "--add-talkgroup";
    const bool exerciseCreate = argc > 2 && QByteArray(argv[2]) == "--create-talkgroup";
    const bool exercisePower = argc > 2 && QByteArray(argv[2]) == "--power";
    const bool exerciseSettings = argc > 2 && QByteArray(argv[2]) == "--settings";
    const bool manualOrder = argc > 2 && QByteArray(argv[2]) == "--manual-order";
    const bool exerciseOrder = argc > 2 && QByteArray(argv[2]) == "--order" || manualOrder;
    const bool captureScreenshots = argc > 2 && QByteArray(argv[2]) == "--capture-screenshots";
    const bool demo = argc > 1 && QByteArray(argv[1]) == "--demo";
    const QByteArray radioMode = argc > 2 ? QByteArray(argv[2]) : QByteArray();
    const bool exerciseRadio = radioMode == "--radio-success" || radioMode == "--radio-cancel"
                               || radioMode == "--radio-failure";
    RepeaterStore store;
    RadioReader::ReadOperation read = Cs7000Usb::read;
    if (exerciseRadio) {
        read = [radioMode](const auto &progress, const auto &cancelled) {
            for (int i = 1; i <= 100; ++i) {
                if (cancelled.load()) throw Cs7000Usb::Cancelled();
                progress("main", uint32_t(i), 100);
                QThread::msleep(5);
            }
            if (radioMode == "--radio-failure") throw std::runtime_error("Simulated USB disconnect");
            auto bytes = cs7000DefaultCodeplug();
            const auto mask = bytes.mid(0x300, 256);
            for (qsizetype i = 0; i < bytes.size(); ++i) bytes[i] = bytes[i] ^ mask[i % 256];
            return Cs7000Usb::Bytes(bytes.begin(), bytes.end());
        };
    }
    RadioReader radioReader(nullptr, std::move(read));
    QObject::connect(&radioReader, &RadioReader::completed,
                     &store, &RepeaterStore::importRadioCodeplug);
    if (exerciseSwitching) {
        const auto addSimplex = [](RepeaterStore &sample) {
            sample.selectRepeater(0);
            if (!sample.addSimplexToSelectedRepeater("North simplex", "446.500", false, 0, 0)) return false;
            sample.selectRepeater(1);
            if (!sample.addSimplexToSelectedRepeater("River simplex", "146.520", false, 0, 0)
                || !sample.addSimplexToSelectedRepeater("River simplex 2", "446.025", false, 0, 0)) return false;
            sample.selectRepeater(0);
            return true;
        };
        if (switchManual) {
            if (!addSimplex(store)) return 1;
        } else {
            switchDirectory = std::make_unique<QTemporaryDir>();
            if (!switchDirectory->isValid()) return 1;
            switchPath = switchDirectory->filePath("switching.waffle");
            RepeaterStore sample;
            if (!addSimplex(sample) || !sample.createCustomZone("Mixed test")
                || !sample.addExistingChannelToSelectedZone(1) || !sample.addExistingChannelToSelectedZone(5)
                || !sample.createCustomZone("Empty test") || !sample.saveProjectToFile(switchPath)) return 1;
            if (switchSaved && !store.openProjectFromFile(switchPath)) return 1;
        }
    }
    else if (requiredContact) {
        store.newProject();
        if (!store.createCustomZone("Contact test")) return 1;
    }
    else if (advancedNew) {
        store.newProject();
        if (!store.addRepeater("First", "443.000", "448.000", 1)
            || !store.addTalkgroup(1, "Alpha", 9, "First Alpha")
            || !store.addRepeater("Second", "444.350", "449.350", 2)
            || !store.addTalkgroup(1, "Alpha", 9, "Second Alpha")
            || !store.addTalkgroup(2, "Bravo", 3100, "Second Bravo")
            || !store.addContact("Private Example", 1234567, false)) return 1;
    }
    else if (exerciseAdvanced) {
        QString error;
        const auto base = channelOrderFixture(&error);
        ImportedCodeplug data;
        if (!CodeplugReader::read(base, &data, &error)) return 1;
        data.channels[3].contactIndex = 0; // Legacy files must still open and allow correction.
        data.contacts.append({"Private Example", 1234567, false});
        if (advancedLarge)
            while (data.contacts.size() < 2000)
                data.contacts.append({QString("Contact %1").arg(data.contacts.size()), 100000 + int(data.contacts.size()), true});
        QByteArray source;
        if (!CodeplugWriter::write(base, data, "TEST RADIO", 3109101, &source, &error) || !store.importRadioCodeplug(source)) return 1;
    }
    else if (liveImportNew) store.newProject();
    else if (removeNoContact) {
        QString error;
        const auto original = channelOrderFixture(&error);
        ImportedCodeplug data;
        if (!CodeplugReader::read(original, &data, &error)) return 1;
        data.channels[0].contactIndex = 0;
        data.channels[2].contactIndex = 0;
        QByteArray source;
        if (!CodeplugWriter::write(original, data, "TEST RADIO", 3109101, &source, &error)
            || !store.importRadioCodeplug(source)) return 1;
    }
    else if (freshZones) {
        store.newProject();
        if (!store.addRepeater("Master", "444.350", "449.350", 2)
            || !store.addTalkgroup(2, "Alpha", 9, "Master Alpha 2")
            || !store.addTalkgroup(2, "Bravo", 3100, "Master Bravo 2")
            || !store.addTalkgroup(1, "Alpha", 9, "Master Alpha 1")
            || !store.addTalkgroup(1, "Bravo", 3100, "Master Bravo 1")
            || !store.addTalkgroup(1, "Charlie", 123, "Master Charlie")
            || !store.addSimplexToSelectedRepeater("Simplex", "446.000", true, 1, 1)) return 1;
    }
    else if (exerciseZones) {
        QString error;
        auto source = channelOrderFixture(&error);
        if (largeZones) {
            ImportedCodeplug large;
            if (!CodeplugReader::read(source, &large, &error)) return 1;
            const auto prototype = large.channels.first();
            while (large.channels.size() < 2000) {
                auto channel = prototype;
                channel.name = QString("Catalog %1").arg(large.channels.size() + 1);
                large.channels.append(channel);
            }
            QByteArray expanded;
            if (!CodeplugWriter::write(source, large, "TEST RADIO", 3109101, &expanded, &error)) return 1;
            source = expanded;
        }
        if (!store.importRadioCodeplug(source)) return 1;
    }
    else if (manualOrder) {
        store.newProject();
        if (!store.addRepeater("Manual", "444.350", "449.350", 2)
            || !store.addTalkgroup(2, "Alpha", 9, "TS2 Alpha")
            || !store.addTalkgroup(2, "Bravo", 3100, "TS2 Bravo")
            || !store.addTalkgroup(1, "Alpha", 9, "TS1 Alpha 1")
            || !store.addTalkgroup(1, "Bravo", 3100, "TS1 Bravo")
            || !store.addTalkgroup(1, "Charlie", 123, "TS1 Alpha 2")
            || !store.addSimplexToSelectedRepeater("Simplex", "446.000", true, 1, 1)) return 1;
    }
    else if (exerciseOrder) {
        QString error;
        const auto source = channelOrderFixture(&error);
        if (source.isEmpty() || !store.importRadioCodeplug(source)) {
            qCritical() << "Could not create ordering UI fixture:" << error;
            return 1;
        }
        store.selectImportedGroup(0);
    }
    else if (argc > 1 && QByteArray(argv[1]) == "--new")
        store.newProject();
    else if (argc > 1 && QByteArray(argv[1]) == "--embedded") {
        if (!store.importCodeplugFromBytes(cs7000DefaultCodeplug()))
            return 1;
    }
    else if (argc > 1 && !demo && !store.importCodeplugFromFile(QString::fromLocal8Bit(argv[1]))) {
        qCritical() << "Could not import sample for UI test";
        return 1;
    }
    const bool exerciseWrite = radioMode == "--write-success" || radioMode == "--write-failure";
    if (exerciseWrite) {
        QString error;
        const auto fixture = channelOrderFixture(&error);
        if (fixture.isEmpty() || !store.importRadioCodeplug(fixture)) return 19;
    }
    bool writeStarted = false, writeCompleted = false, writeFailed = false;
    RadioWriter::WriteOperation write = Cs7000Usb::write;
    if (exerciseWrite) write = [&](const auto &image, const auto &progress) {
        Cs7000Usb::planWrite(image);
        for (int i = 1; i <= 50; ++i) {
            progress("main", uint32_t(i), 50);
            QThread::msleep(10);
        }
        if (radioMode == "--write-failure") throw std::runtime_error("Simulated write disconnect");
        progress("contacts", 1, 1);
    };
    RadioWriter radioWriter(nullptr, std::move(write));
    QObject::connect(&radioWriter, &RadioWriter::completed, &app, [&] { writeCompleted = true; });
    QObject::connect(&radioWriter, &RadioWriter::failed, &app, [&](const QString &) { writeFailed = true; });
    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("radioicons"), new TestRadioIconProvider);
    int qmlWarningCount = 0;
    QObject::connect(&engine, &QQmlEngine::warnings, &app,
                     [&qmlWarningCount](const QList<QQmlError> &warnings) {
                         qmlWarningCount += warnings.size();
                     });
    engine.rootContext()->setContextProperty(QStringLiteral("store"), &store);
    engine.rootContext()->setContextProperty(QStringLiteral("radioReader"), &radioReader);
    engine.rootContext()->setContextProperty(QStringLiteral("radioWriter"), &radioWriter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 2;
    if (radioMode == "--radio-definition-gate") {
        QObject *root = engine.rootObjects().constFirst();
        auto *readButton = root->findChild<QQuickItem *>("readRadioButton");
        auto *writeButton = root->findChild<QQuickItem *>("writeRadioButton");
        if (!readButton || !writeButton || !readButton->isEnabled() || !writeButton->isEnabled()) return 40;
        const QString sample = QCoreApplication::applicationDirPath()
            + "/../Codeplugs/CS800-KB1JDX 2017-01-22.rdb";
        if (!store.importCodeplugFromFile(sample, "cs800.wcp")) return 41;
        QCoreApplication::processEvents();
        if (readButton->isEnabled() || writeButton->isEnabled()) return 42;
        store.newProject();
        QCoreApplication::processEvents();
        if (!readButton->isEnabled() || !writeButton->isEnabled()) return 43;
        if (!store.importCodeplugFromBytes(cs7000DefaultCodeplug())) return 44;
        QCoreApplication::processEvents();
        if (!readButton->isEnabled() || !writeButton->isEnabled() || qmlWarningCount) return 45;
        qInfo() << "Radio transfer controls follow companion .wrd availability";
        return 0;
    }
    if (radioMode == "--contact-import-counts") {
        QString error;
        const auto first = channelOrderFixture(&error);
        ImportedCodeplug expanded;
        if (first.isEmpty() || !CodeplugReader::read(first, &expanded, &error)) return 70;
        expanded.contacts.append({"Person", 1234567, false});
        expanded.contacts.append({"Charlie", 123, true});
        QByteArray second;
        if (!CodeplugWriter::write(first, expanded, "TEST RADIO", 3109101, &second, &error)) return 70;
        QTemporaryDir directory;
        if (!directory.isValid()) return 70;
        const QString firstPath = directory.filePath("first.rdbx");
        const QString secondPath = directory.filePath("second.rdbx");
        for (const auto &entry : {qMakePair(firstPath, first), qMakePair(secondPath, second)}) {
            QFile file(entry.first);
            if (!file.open(QIODevice::WriteOnly) || file.write(entry.second) != entry.second.size()) return 70;
        }
        QObject *root = engine.rootObjects().constFirst();
        root->setProperty("currentSection", "contacts");
        int step = 0;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &app, [&] {
            if (step > 0) {
                const bool expanded = step == 2;
                const int people = expanded ? 1 : 0, groups = expanded ? 3 : 2;
                bool foundPeople = false, foundGroups = false;
                for (auto *object : root->findChildren<QObject *>()) {
                    const auto text = object->property("text").toString();
                    if (text.startsWith("People (") || text.startsWith("Talkgroups (")) {
                        const bool personTab = text.startsWith("People (");
                        const QString expected = personTab ? QString("People (%1)").arg(people)
                                                           : QString("Talkgroups (%1)").arg(groups);
                        if (personTab) foundPeople = true; else foundGroups = true;
                        if (text != expected) {
                            qCritical() << "Stale contact count after import" << step << text << "expected" << expected;
                            app.exit(71); return;
                        }
                    }
                }
                if (!foundPeople || !foundGroups || store.contacts().size() != people + groups
                    || root->property("clearingProject").toBool() || qmlWarningCount) {
                    qCritical() << "Import did not refresh contact view" << step;
                    app.exit(72); return;
                }
                if (step == 3) {
                    qInfo() << "Contact counts refreshed across three consecutive file imports";
                    app.quit(); return;
                }
            }
            if (!store.importCodeplugFromFile(step == 1 ? secondPath : firstPath)) { app.exit(73); return; }
            root->setProperty("currentSection", "contacts");
            ++step;
        });
        timer.start(400);
        QTimer::singleShot(15000, &app, [&] { app.exit(74); });
        return app.exec();
    }
    if (exerciseSwitching) {
        QObject *root = engine.rootObjects().constFirst();
        const QStringList expectedNames{"North Ridge", "Riverbend", "Mixed test", "Empty test"};
        QTimer switching;
        int iterations = 0;
        QObject::connect(&switching, &QTimer::timeout, &app, [&] {
            const int target = switchManual || iterations < 100 ? iterations % 2 : (iterations - 100) % 4;
            if (switchManual) store.selectRepeater(target);
            else store.selectImportedGroup(target);
            const QString actual = switchManual ? store.selectedName() : store.selectedImportedGroup().value("name").toString();
            if (actual != expectedNames[target]) { qCritical() << "Switch selected wrong entry" << actual; app.exit(66); return; }
            if (++iterations == 200) {
                switching.stop();
                if (qmlWarningCount || !qobject_cast<QQuickWindow *>(root)->isVisible() || (!switchManual && store.modified())) {
                    qCritical() << "Switching changed the project or raised QML warnings" << qmlWarningCount;
                    app.exit(67); return;
                }
                qInfo() << "200 repeater/zone switches completed" << switchManual << switchLive;
                app.quit();
            }
        });
        QTimer::singleShot(100, &app, [&] {
            if (switchLive && !store.openProjectFromFile(switchPath)) { app.exit(68); return; }
            switching.start(25);
        });
        QTimer::singleShot(20000, &app, [&] { qCritical() << "Repeater switching timed out"; app.exit(69); });
        return app.exec();
    }
    if (requiredContact) {
        QObject *root = engine.rootObjects().constFirst();
        auto control = [root](const char *name) { return root->findChild<QObject *>(QString::fromLatin1(name)); };
        auto click = [&app](QObject *object) {
            if (!object || !QMetaObject::invokeMethod(object, "clicked")) { app.exit(59); }
        };
        QTimer::singleShot(100, &app, [&] {
            click(control("zoneAddChannelButton"));
            if (control("zoneChannelSave")->property("enabled").toBool() || control("zoneNoContact")) { app.exit(60); return; }
            control("zoneChannelName")->setProperty("text", "Analog test");
            control("zoneChannelRx")->setProperty("text", "446.000");
            control("zoneChannelTx")->setProperty("text", "446.000");
            control("zoneChannelMode")->setProperty("currentIndex", 1);
            if (!control("zoneChannelSave")->property("enabled").toBool()) { app.exit(61); return; }
            click(control("zoneChannelSave"));
        });
        QTimer::singleShot(260, &app, [&] {
            if (!store.addContact("Local", 9, true)) { app.exit(62); return; }
            click(control("zoneAddChannelButton"));
            control("zoneChannelName")->setProperty("text", "Digital test");
            control("zoneChannelRx")->setProperty("text", "444.350");
            control("zoneChannelTx")->setProperty("text", "449.350");
            if (control("zoneChannelSave")->property("enabled").toBool()) { app.exit(63); return; }
            control("zoneContactSearch")->setProperty("text", "Local");
        });
        QTimer::singleShot(420, &app, [&] {
            const auto find = [&](auto &&self, QQuickItem *item) -> QQuickItem * {
                if (!item) return nullptr;
                if (item->objectName() == "zoneContactChoice" && item->property("contactNumber").toInt() == 1) return item;
                for (auto *child : item->childItems()) if (auto *found = self(self, child)) return found;
                return nullptr;
            };
            click(find(find, qobject_cast<QQuickWindow *>(root)->contentItem()));
            if (!control("zoneChannelSave")->property("enabled").toBool()) { app.exit(64); return; }
            click(control("zoneChannelSave"));
        });
        QTimer::singleShot(580, &app, [&] {
            const auto channels = store.selectedImportedGroup().value("zones").toList().first().toMap().value("channels").toList();
            if (channels.size() != 2 || channels[0].toMap().value("mode").toString() == "Digital"
                || channels[0].toMap().value("contactIndex").toInt() != 0
                || channels[1].toMap().value("contactIndex").toInt() != 1 || qmlWarningCount) { app.exit(65); return; }
            qInfo() << "Digital Add requires contact selection; analog Add works without a contact";
            app.quit();
        });
        return app.exec();
    }
    if (exerciseAdvanced) {
        QObject *root = engine.rootObjects().constFirst();
        auto control = [root](const char *name) { return root->findChild<QObject *>(QString::fromLatin1(name)); };
        auto visual = [root](const QString &name, const char *property, int value) -> QQuickItem * {
            const auto find = [&](auto &&self, QQuickItem *item) -> QQuickItem * {
                if (!item) return nullptr;
                if (item->objectName() == name && item->property(property).toInt() == value && item->isVisible()) return item;
                for (auto *child : item->childItems()) if (auto *found = self(self, child)) return found;
                return nullptr;
            };
            return find(find, qobject_cast<QQuickWindow *>(root)->contentItem());
        };
        auto click = [&app](QObject *object) {
            if (!object || !QMetaObject::invokeMethod(object, "clicked")) { qCritical() << "Advanced editor control missing"; app.exit(50); }
        };
        auto firstChannel = [&store] {
            return store.selectedImportedGroup().value("zones").toList().first().toMap().value("channels").toList().first().toMap();
        };
        int originalContact = 0;
        const QString expectedName = advancedNew ? QStringLiteral("Second") : QStringLiteral("Ordering test");
        QTimer::singleShot(100, &app, [&] { click(control(advancedNew ? "manualEditAsZoneButton" : "importedEditAsZoneButton")); });
        QTimer::singleShot(260, &app, [&] {
            if (!store.editingAsZone() || store.selectedImportedGroup().value("name").toString() != expectedName
                || !store.importedRepeaterNames().contains(expectedName)) { app.exit(51); return; }
            originalContact = firstChannel().value("contactIndex").toInt();
            click(visual("zoneEditChannelButton", "zonePosition", 1));
            if (control("zoneChannelSave")->property("enabled").toBool() != (originalContact > 0)
                || control("zoneNoContact")) { app.exit(58); return; }
        });
        QTimer::singleShot(380, &app, [&] {
            if (advancedLarge && control("zoneContactChoices")->property("count").toInt() != 2000) { app.exit(52); return; }
            control("zoneContactSearch")->setProperty("text", "pRiVaTe eXaMpLe");
            if (control("zoneChannelDialog")->property("selectedContactIndex").toInt() != originalContact
                || control("zoneContactChoices")->property("count").toInt() != 1) { app.exit(53); return; }
        });
        QTimer::singleShot(500, &app, [&] { click(visual("zoneContactChoice", "contactNumber", 3)); click(control("zoneChannelCancel")); });
        QTimer::singleShot(620, &app, [&] {
            if (firstChannel().value("contactIndex").toInt() != originalContact) { app.exit(54); return; }
            click(visual("zoneEditChannelButton", "zonePosition", 1));
            control("zoneContactSearch")->setProperty("text", "1234567");
        });
        QTimer::singleShot(760, &app, [&] { click(visual("zoneContactChoice", "contactNumber", 3)); click(control("zoneChannelSave")); });
        QTimer::singleShot(900, &app, [&] {
            if (!store.editingAsZone() || firstChannel().value("contactIndex").toInt() != 3
                || firstChannel().value("contactGroupCall").toBool()) { app.exit(55); return; }
            click(visual("zoneEditChannelButton", "zonePosition", 1));
        });
        QTimer::singleShot(1040, &app, [&] { control("zoneContactSearch")->setProperty("text", "3100"); });
        QTimer::singleShot(1180, &app, [&] { click(visual("zoneContactChoice", "contactNumber", 2)); click(control("zoneChannelSave")); });
        QTimer::singleShot(1320, &app, [&] {
            if (!store.editingAsZone() || firstChannel().value("contactIndex").toInt() != 2) { app.exit(56); return; }
            click(control("radioSettingsNav"));
        });
        QTimer::singleShot(1480, &app, [&] {
            if (store.editingAsZone() || root->property("currentSection").toString() != "settings"
                || !store.importedRepeaterNames().contains(expectedName) || qmlWarningCount) {
                qCritical() << "Advanced editor navigation failed" << qmlWarningCount; app.exit(57); return;
            }
            qInfo() << "Advanced zone view and searchable contacts verified" << advancedNew << advancedLarge;
            app.quit();
        });
        return app.exec();
    }
    if (liveImportNew) {
        QTemporaryDir temp;
        const auto path = temp.filePath("live-import.rdbx");
        QString error;
        const auto source = channelOrderFixture(&error);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(source) != source.size()) return 42;
        file.close();
        QObject *root = engine.rootObjects().constFirst();
        bool clickedNew = false;
        int heartbeats = 0;
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++heartbeats; });
        heartbeat.start(20);
        QTimer::singleShot(80, &app, [&] {
            if (!store.importCodeplugFromFile(path)) { app.exit(43); return; }
            QTimer::singleShot(200, &app, [&] {
                if (root->property("clearingProject").toBool()) { app.exit(44); return; }
                QTimer::singleShot(80, &app, [] {
                    for (auto *widget : QApplication::topLevelWidgets())
                        if (auto *box = qobject_cast<QMessageBox *>(widget)) box->button(QMessageBox::Yes)->click();
                });
                for (auto *object : root->findChildren<QObject *>())
                    if (object->property("text").toString() == "New") {
                        QMetaObject::invokeMethod(object, "clicked");
                        clickedNew = true;
                        break;
                    }
            });
        });
        QTimer completion;
        QObject::connect(&completion, &QTimer::timeout, &app, [&] {
            if (!clickedNew || store.hasImportedCodeplug() || root->property("clearingProject").toBool()) return;
            if (!qobject_cast<QQuickWindow *>(root)->isVisible() || !store.repeaterNames().isEmpty() || heartbeats < 3 || qmlWarningCount) {
                app.exit(45); return;
            }
            qInfo() << "Live file import followed by confirmed New kept an empty responsive window";
            app.quit();
        });
        completion.start(40);
        QTimer::singleShot(15000, &app, [&] { qCritical() << "Live import/New timed out"; app.exit(46); });
        return app.exec();
    }
    if (removeNoContact) {
        QObject *root = engine.rootObjects().constFirst();
        QTimer::singleShot(100, &app, [&] {
            const auto find = [&](auto &&self, QQuickItem *item) -> QQuickItem * {
                if (!item) return nullptr;
                if (item->objectName() == "removeSlotChannelButton" && item->property("zonePosition").toInt() == 5 && item->isVisible()) return item;
                for (auto *child : item->childItems()) if (auto *found = self(self, child)) return found;
                return nullptr;
            };
            auto *button = find(find, qobject_cast<QQuickWindow *>(root)->contentItem());
            if (!button) { app.exit(47); return; }
            QMetaObject::invokeMethod(button, "clicked");
        });
        QTimer::singleShot(300, &app, [&] {
            const auto zones = store.selectedImportedGroup().value("zones").toList();
            const auto channels = zones.first().toMap().value("channels").toList();
            if (channels.size() != 5 || channels[1].toMap().value("contactIndex").toInt() != 0 || qmlWarningCount) { app.exit(48); return; }
            store.selectImportedGroup(1);
            const auto shared = store.selectedImportedGroup().value("zones").toList().first().toMap().value("channels").toList();
            if (shared.size() != 2 || shared.first().toMap().value("number").toInt() != 3) { app.exit(49); return; }
            qInfo() << "No-contact Remove targets one channel and preserves the other zone";
            app.quit();
        });
        return app.exec();
    }
    if (exerciseZones) {
        QObject *root = engine.rootObjects().constFirst();
        auto control = [root](const char *name) { return root->findChild<QObject *>(QString::fromLatin1(name)); };
        auto click = [&app](QObject *object) {
            if (!object || !QMetaObject::invokeMethod(object, "clicked")) { qCritical() << "Zone UI control missing"; app.exit(31); }
        };
        auto memberNumbers = [&store] {
            QVariantList result;
            const auto zones = store.selectedImportedGroup().value("zones").toList();
            if (zones.isEmpty()) return result;
            for (const auto &channel : zones.first().toMap().value("channels").toList())
                result.append(channel.toMap().value("number"));
            return result;
        };
        auto frequencyButton = [root]() -> QObject * {
            const auto find = [&](auto &&self, QQuickItem *item) -> QQuickItem * {
                if (!item) return nullptr;
                if (item->objectName() == QStringLiteral("zoneEditChannelButton")
                    && item->property("zonePosition").toInt() == 2 && item->isVisible()) return item;
                for (auto *child : item->childItems())
                    if (auto *found = self(self, child)) return found;
                return nullptr;
            };
            return find(find, qobject_cast<QQuickWindow *>(root)->contentItem());
        };
        int copiedChannelNumber = 0;
        QTimer::singleShot(100, &app, [&] {
            click(control("createZoneButton"));
            control("createZoneName")->setProperty("text", "UI Mixed");
            click(control("createZoneSave"));
        });
        auto searchFor = [&store, &control](int number) {
            for (const auto &value : store.availableZoneChannels())
                if (value.toMap().value("number").toInt() == number)
                    control("zoneChannelSearch")->setProperty("text", value.toMap().value("name").toString().toUpper());
        };
        auto selectExisting = [root, &click](int number) {
            const auto find = [&](auto &&self, QQuickItem *item) -> QQuickItem * {
                if (!item) return nullptr;
                if (item->objectName() == QStringLiteral("zoneExistingChannelChoice")
                    && item->property("channelNumber").toInt() == number) return item;
                for (auto *child : item->childItems())
                    if (auto *found = self(self, child)) return found;
                return nullptr;
            };
            click(find(find, qobject_cast<QQuickWindow *>(root)->contentItem()));
        };
        QTimer::singleShot(240, &app, [&] {
            click(control("zoneAddChannelButton"));
            if (largeZones && control("zoneExistingChannels")->property("count").toInt() != 2000) { app.exit(39); return; }
            control("zoneChannelSearch")->setProperty("text", "3100");
            if (control("zoneExistingChannels")->property("count").toInt() != 2) { app.exit(40); return; }
            control("zoneChannelSearch")->setProperty("text", "444.350");
            if (control("zoneExistingChannels")->property("count").toInt() == 0) { app.exit(41); return; }
            control("zoneChannelSearch")->setProperty("text", "no-such-channel");
            if (control("zoneChannelSave")->property("enabled").toBool()) { app.exit(36); return; }
            searchFor(1);
        });
        QTimer::singleShot(360, &app, [&] { selectExisting(1); click(control("zoneChannelSave")); });
        QTimer::singleShot(480, &app, [&] { click(control("zoneAddChannelButton")); searchFor(3); });
        QTimer::singleShot(600, &app, [&] { selectExisting(3); click(control("zoneChannelSave")); });
        QTimer::singleShot(720, &app, [&] {
            if (memberNumbers() != QVariantList{1, 3} || !store.updateSelectedZoneChannelFrequencies(2, "444.400", "449.400")) {
                qCritical() << "Zone UI membership failed" << memberNumbers(); app.exit(32); return;
            }
            copiedChannelNumber = memberNumbers()[1].toInt();
            store.leaveZoneEditor(); // Still mixed; establish the initial classification.
            click(frequencyButton());
        });
        QTimer::singleShot(840, &app, [&] {
            if (control("zoneChannelMode")->property("currentIndex").toInt() != 0
                || control("zoneChannelRx")->property("text").toString() != "444.4000") { app.exit(37); return; }
            control("zoneChannelName")->setProperty("text", "Canceled name");
            control("zoneChannelColorCode")->setProperty("value", 7);
            control("zoneChannelPower")->setProperty("currentIndex", 1);
            control("zoneChannelRx")->setProperty("text", "444.350");
            control("zoneChannelTx")->setProperty("text", "449.350");
            click(control("zoneChannelCancel"));
        });
        QTimer::singleShot(960, &app, [&] {
            const auto channels = store.selectedImportedGroup().value("zones").toList().first().toMap().value("channels").toList();
            if (channels[1].toMap().value("receiveFrequency").toString() != "444.4000") { app.exit(33); return; }
            click(frequencyButton());
        });
        QTimer::singleShot(1080, &app, [&] {
            control("zoneChannelName")->setProperty("text", "Edited channel");
            control("zoneChannelPower")->setProperty("currentIndex", 1);
            control("zoneChannelTimeSlot")->setProperty("currentIndex", 1);
            control("zoneChannelDialog")->setProperty("selectedContactIndex", 2);
            control("zoneChannelRx")->setProperty("text", "444.350");
            control("zoneChannelTx")->setProperty("text", "449.350");
            click(control("zoneChannelSave"));
        });
        QTimer::singleShot(1200, &app, [&] {
            if (!store.importedZoneNames().contains("UI Mixed") || store.selectedImportedGroup().value("isRepeater").toBool()) {
                qCritical() << "Zone moved during UI correction"; app.exit(34); return;
            }
            const auto edited = store.selectedImportedGroup().value("zones").toList().first().toMap().value("channels").toList()[1].toMap();
            if (edited.value("name").toString() != "Edited channel" || edited.value("timeSlot").toInt() != 2
                || edited.value("power").toString() != "Low" || edited.value("contactIndex").toInt() != 2) { app.exit(38); return; }
            click(control("zoneOrderChannelsButton"));
        });
        QTimer::singleShot(1320, &app, [&] { click(control("channelOrderCancel")); click(control("radioSettingsNav")); });
        QTimer::singleShot(1480, &app, [&] {
            if (!store.importedRepeaterNames().contains("UI Mixed") || root->property("currentSection").toString() != "settings"
                || memberNumbers() != QVariantList{1, copiedChannelNumber} || qmlWarningCount) {
                qCritical() << "Deferred zone UI navigation failed" << qmlWarningCount << memberNumbers(); app.exit(35); return;
            }
            qInfo() << "Zone UI creation, adding channels, frequency Cancel/Save and deferred classification verified";
            app.quit();
        });
        return app.exec();
    }
    if (exerciseOrder) {
        QObject *root = engine.rootObjects().constFirst();
        QObject *button = root->findChild<QObject *>(manualOrder ? QStringLiteral("manualOrderChannelsButton") : QStringLiteral("orderChannelsButton"));
        QObject *dialog = root->findChild<QObject *>(QStringLiteral("channelOrderDialog"));
        QObject *cancel = root->findChild<QObject *>(QStringLiteral("channelOrderCancel"));
        QObject *save = root->findChild<QObject *>(QStringLiteral("channelOrderSave"));
        QObject *slot1 = root->findChild<QObject *>(manualOrder ? QStringLiteral("manualSlot1") : QStringLiteral("importedSlot1"));
        QObject *slot2 = root->findChild<QObject *>(manualOrder ? QStringLiteral("manualSlot2") : QStringLiteral("importedSlot2"));
        if (!button || !dialog || !cancel || !save || !slot1 || !slot2) return 20;
        auto order = [&store, manualOrder] {
            if (manualOrder) {
                QVariantList numbers;
                for (const auto &entry : store.selectedRepeaterChannels())
                    numbers.append(entry.toMap().value(QStringLiteral("number")));
                return numbers;
            }
            QVariantList numbers;
            const auto zones = store.selectedImportedGroup().value(QStringLiteral("zones")).toList();
            for (const auto &item : zones.constFirst().toMap().value(QStringLiteral("channels")).toList())
                numbers.append(item.toMap().value(QStringLiteral("number")));
            return numbers;
        };
        auto names = [](QObject *panel) {
            const auto value = panel->property("channelCards");
            const auto cards = value.metaType() == QMetaType::fromType<QJSValue>()
                                   ? value.value<QJSValue>().toVariant().toList() : value.toList();
            QStringList result;
            for (const auto &card : cards)
                result.append(card.toMap().value(QStringLiteral("channelName")).toString());
            return result;
        };
        auto move = [root, &app](const QString &objectName, int row) {
            QObject *list = root->findChild<QObject *>(QStringLiteral("channelOrderList"));
            auto *content = list ? list->property("contentItem").value<QQuickItem *>() : nullptr;
            const auto find = [&](auto &&self, QQuickItem *item) -> QQuickItem * {
                if (!item) return nullptr;
                if (item->objectName() == objectName && item->property("rowIndex").toInt() == row
                    && item->isEnabled())
                    return item;
                for (auto *child : item->childItems())
                    if (auto *found = self(self, child)) return found;
                return nullptr;
            };
            if (auto *control = find(find, content)) {
                QMetaObject::invokeMethod(control, "clicked");
                return;
            }
            qCritical() << "Ordering control not found" << objectName << row;
            app.exit(21);
        };
        const QVariantList original = manualOrder ? QVariantList{1, 2, 3, 4, 5, 6} : QVariantList{4, 1, 2, 5, 3, 6};
        QTimer::singleShot(80, &app, [&] {
            if (names(slot1) != QStringList({"TS1 Alpha 1", "TS1 Bravo", "TS1 Alpha 2"})) {
                qCritical() << "Slot cards regrouped repeated contacts instead of following zone order";
                app.exit(22);
                return;
            }
            QMetaObject::invokeMethod(button, "clicked");
        });
        QTimer::singleShot(160, &app, [&] {
            if (!dialog->property("visible").toBool()) { app.exit(23); return; }
            move(QStringLiteral("channelOrderDown"), 0);
            if (order() != original) { app.exit(24); return; }
            QMetaObject::invokeMethod(cancel, "clicked");
        });
        QTimer::singleShot(260, &app, [&] {
            if (order() != original) { app.exit(25); return; }
            QMetaObject::invokeMethod(button, "clicked");
        });
        QTimer::singleShot(340, &app, [&] { move(QStringLiteral("channelOrderDown"), 0); });
        QTimer::singleShot(420, &app, [&] { move(QStringLiteral("channelOrderUp"), 3); });
        QTimer::singleShot(500, &app, [&] { move(QStringLiteral("channelOrderUp"), 2); });
        QTimer::singleShot(580, &app, [&] { QMetaObject::invokeMethod(save, "clicked"); });
        QTimer::singleShot(740, &app, [&] {
            if (dialog->property("visible").toBool() || order() != (manualOrder ? QVariantList{2, 4, 1, 3, 5, 6} : QVariantList{1, 5, 4, 2, 3, 6})
                || names(slot1) != (manualOrder ? QStringList{"TS1 Bravo", "TS1 Alpha 1", "TS1 Alpha 2"}
                                                      : QStringList{"TS1 Alpha 1", "TS1 Bravo", "TS1 Alpha 2"})
                || names(slot2) != QStringList({"TS2 Bravo", "TS2 Alpha"}) || qmlWarningCount) {
                qCritical() << "Ordering UI failed" << order() << names(slot1) << names(slot2) << qmlWarningCount;
                app.exit(26);
                return;
            }
            qInfo() << "Ordering dialog Cancel/Save and filtered slot-card order verified";
            app.quit();
        });
        return app.exec();
    }
    if (exerciseWrite) {
        QObject *root = engine.rootObjects().constFirst();
        auto *menuButton = root->findChild<QQuickItem *>("radioMenuButton");
        auto *button = root->findChild<QQuickItem *>("writeRadioButton");
        auto *confirm = root->findChild<QObject *>("radioWriteConfirmation");
        auto *accept = root->findChild<QObject *>("confirmRadioWriteButton");
        auto *dialog = root->findChild<QObject *>("radioWriteProgress");
        auto *complete = root->findChild<QObject *>("radioWriteComplete");
        auto *readButton = root->findChild<QQuickItem *>("readRadioButton");
        if (!menuButton || !button || !confirm || !accept || !dialog || !complete || !readButton) return 20;
        const auto before = store.exportCodeplugBytes();
        if (before.isEmpty()) return 19;
        const bool modified = store.modified();
        int beats = 0;
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &app, [&] { ++beats; });
        timer.start(10);
        QTimer::singleShot(50, &app, [&] {
            QMetaObject::invokeMethod(menuButton, "clicked");
            QMetaObject::invokeMethod(button, "clicked");
            if (!confirm->property("visible").toBool() || radioWriter.busy()) { app.exit(21); return; }
            QMetaObject::invokeMethod(accept, "clicked");
            writeStarted = true;
        });
        QTimer::singleShot(250, &app, [&] {
            if (!radioWriter.busy() || !dialog->property("visible").toBool()
                || button->isEnabled() || readButton->isEnabled()) app.exit(22);
        });
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &app, [&] {
            if (!writeStarted || radioWriter.busy()) return;
            const bool success = radioMode == "--write-success";
            if (writeCompleted != success || writeFailed == success || beats < 10
                || dialog->property("visible").toBool() || !button->isEnabled()
                || complete->property("visible").toBool() != success
                || store.modified() != modified || store.exportCodeplugBytes() != before || qmlWarningCount) {
                qCritical() << "Write UI flow failed" << radioMode << qmlWarningCount
                            << "completed" << writeCompleted << "failed" << writeFailed << "beats" << beats
                            << "progress" << dialog->property("visible") << "button" << button->isEnabled()
                            << "complete dialog" << complete->property("visible")
                            << "modified" << store.modified() << modified
                            << "same export" << (store.exportCodeplugBytes() == before);
                app.exit(23); return;
            }
            qInfo() << "Write UI flow verified" << radioMode;
            app.quit();
        });
        poll.start(30);
        QTimer::singleShot(15000, &app, [&] { app.exit(24); });
        return app.exec();
    }
    if (exerciseRadio) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        window->resize(800, 720);
        QObject *root = engine.rootObjects().constFirst();
        auto *menuButton = root->findChild<QQuickItem *>(QStringLiteral("radioMenuButton"));
        auto *button = root->findChild<QQuickItem *>(QStringLiteral("readRadioButton"));
        QObject *dialog = root->findChild<QObject *>(QStringLiteral("radioReadProgress"));
        QObject *confirmation = root->findChild<QObject *>(QStringLiteral("radioReadConfirmation"));
        QObject *cancel = root->findChild<QObject *>(QStringLiteral("cancelRadioReadButton"));
        if (!menuButton || !button || !dialog || !confirmation || !cancel) return 10;
        int heartbeats = 0;
        bool readStarted = false;
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++heartbeats; });
        heartbeat.start(10);
        QTimer::singleShot(50, &app, [&] {
            const QPointF position = menuButton->mapToScene(QPointF(0, 0));
            if (position.x() < 0 || position.x() + menuButton->width() > window->width()) {
                qCritical() << "Radio button overflows toolbar at minimum window width";
                app.exit(11);
                return;
            }
            store.addContact(QStringLiteral("Pending contact"), 1234567, false);
            QMetaObject::invokeMethod(menuButton, "clicked");
            QMetaObject::invokeMethod(button, "clicked");
            if (!confirmation->property("visible").toBool() || radioReader.busy()) {
                qCritical() << "Unsaved project was not confirmed before reading";
                app.exit(12);
                return;
            }
            QMetaObject::invokeMethod(confirmation, "close");
            store.newProject();
            QMetaObject::invokeMethod(menuButton, "clicked");
            QMetaObject::invokeMethod(button, "clicked");
            readStarted = true;
        });
        QTimer::singleShot(200, &app, [&] {
            if (!radioReader.busy() || !dialog->property("visible").toBool() || button->isEnabled()) {
                qCritical() << "Read did not show modal progress and block project editing";
                app.exit(13);
                return;
            }
            if (radioMode == "--radio-cancel") QMetaObject::invokeMethod(cancel, "clicked");
        });
        QTimer completionPoll;
        QObject::connect(&completionPoll, &QTimer::timeout, &app, [&] {
            if (!readStarted || radioReader.busy() || root->property("clearingProject").toBool()) return;
            const bool success = radioMode == "--radio-success";
            if (radioReader.busy() || dialog->property("visible").toBool() || !button->isEnabled()
                || store.hasImportedCodeplug() != success || heartbeats < 10
                || root->property("clearingProject").toBool() || qmlWarningCount) {
                qCritical() << "Radio UI flow failed" << radioMode << "warnings" << qmlWarningCount
                            << "busy" << radioReader.busy() << "dialog" << dialog->property("visible")
                            << "button enabled" << button->isEnabled() << "imported" << store.hasImportedCodeplug()
                            << "heartbeats" << heartbeats << "clearing" << root->property("clearingProject");
                app.exit(14);
                return;
            }
            qInfo() << "Radio UI flow verified:" << radioMode;
            app.quit();
        });
        completionPoll.start(20);
        QTimer::singleShot(15000, &app, [&] {
            qCritical() << "Radio UI flow timed out" << radioMode;
            app.exit(15);
        });
        return app.exec();
    }
    if (captureScreenshots) {
        if (argc < 4 || !QDir().mkpath(QString::fromLocal8Bit(argv[3]))) {
            qCritical() << "Could not create screenshot directory";
            return 4;
        }
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (!window || !store.hasImportedCodeplug()) {
            qCritical() << "Screenshots need an imported codeplug";
            return 4;
        }
        window->resize(1280, 800);
        const QString outputDirectory = QString::fromLocal8Bit(argv[3]);
        bool saved = true;
        auto capture = [window, outputDirectory, &saved](const QString &name) {
            const QImage image = window->grabWindow();
            const QString path = QDir(outputDirectory).filePath(name);
            if (image.isNull() || !image.save(path)) {
                qCritical() << "Could not capture" << path;
                saved = false;
            } else {
                qInfo() << "Captured" << path << image.size();
            }
        };
        const QStringList names = store.importedRepeaterNames();
        const int ares = names.indexOf(QStringLiteral("CT-Brooklyn Ares"));
        const int brooklyn = names.indexOf(QStringLiteral("CT-Brooklyn"));
        if (ares < 0 || brooklyn < 0) {
            qCritical() << "The selected codeplug has no Brooklyn example zones";
            return 4;
        }
        QTimer::singleShot(100, &app, [&store, ares] { store.selectImportedGroup(ares); });
        QTimer::singleShot(550, &app, [capture] { capture(QStringLiteral("repeater.png")); });
        QTimer::singleShot(700, &app, [&store, brooklyn] { store.selectImportedGroup(brooklyn); });
        QTimer::singleShot(900, &app, [&engine] {
            for (QObject *object : engine.rootObjects().constFirst()->findChildren<QObject *>(QStringLiteral("addTalkgroupButton"))) {
                auto *item = qobject_cast<QQuickItem *>(object);
                if (item && item->isVisible() && item->isEnabled()) {
                    QMetaObject::invokeMethod(item, "clicked");
                    break;
                }
            }
        });
        QTimer::singleShot(1100, &app, [&engine] {
            if (QObject *search = engine.rootObjects().constFirst()->findChild<QObject *>(QStringLiteral("talkgroupSearch")))
                search->setProperty("text", QStringLiteral("NY NJ Metro"));
        });
        QTimer::singleShot(1250, &app, [&engine] {
            QObject *root = engine.rootObjects().constFirst();
            if (QObject *list = root->findChild<QObject *>(QStringLiteral("talkgroupChoices"))) {
                if (auto *content = list->property("contentItem").value<QQuickItem *>()) {
                    for (QQuickItem *item : content->childItems()) {
                        if (item->objectName() == QStringLiteral("talkgroupChoice")
                            && item->property("text").toString().startsWith(QStringLiteral("NY NJ Metro"))) {
                            QMetaObject::invokeMethod(item, "clicked");
                            break;
                        }
                    }
                }
            }
            if (QObject *channel = root->findChild<QObject *>(QStringLiteral("channelNameField")))
                channel->setProperty("text", QStringLiteral("Brooklyn Metro"));
        });
        QTimer::singleShot(1450, &app, [capture] { capture(QStringLiteral("add-talkgroup.png")); });
        QTimer::singleShot(1600, &app, [&engine] {
            if (QObject *dialog = engine.rootObjects().constFirst()->findChild<QObject *>(QStringLiteral("addTalkgroupDialog")))
                QMetaObject::invokeMethod(dialog, "close");
            for (QObject *object : engine.rootObjects().constFirst()->findChildren<QObject *>()) {
                auto *item = qobject_cast<QQuickItem *>(object);
                if (item && item->isVisible() && item->property("text").toString() == QStringLiteral("Contacts")) {
                    QMetaObject::invokeMethod(item, "clicked");
                    break;
                }
            }
        });
        QTimer::singleShot(1900, &app, [&engine] {
            if (QObject *list = engine.rootObjects().constFirst()->findChild<QObject *>(QStringLiteral("contactsList"))) {
                if (auto *content = list->property("contentItem").value<QQuickItem *>()) {
                    for (QQuickItem *item : content->childItems()) {
                        if (item->objectName() == QStringLiteral("contactChoice")) {
                            QMetaObject::invokeMethod(item, "clicked");
                            break;
                        }
                    }
                }
            }
        });
        QTimer::singleShot(2200, &app, [capture] { capture(QStringLiteral("contacts.png")); });
        QTimer::singleShot(2350, &app, [window] { window->close(); });
        const int exitCode = app.exec();
        return saved && qmlWarningCount == 0 ? exitCode : 4;
    }
    bool newStayedOpen = true;
    bool contactsOpened = true;
    bool talkgroupAdded = true;
    bool powerEdited = true;
    bool settingsEdited = true;
    if (exerciseSettings) {
        QTimer::singleShot(100, &app, [&engine, &settingsEdited] {
            QObject *root = engine.rootObjects().constFirst();
            QObject *button = root->findChild<QObject *>(QStringLiteral("radioSettingsNav"));
            settingsEdited = button && QMetaObject::invokeMethod(button, "clicked");
        });
        QTimer::singleShot(300, &app, [&engine, &settingsEdited] {
            QObject *root = engine.rootObjects().constFirst();
            QObject *name = root->findChild<QObject *>(QStringLiteral("radioNameField"));
            QObject *id = root->findChild<QObject *>(QStringLiteral("radioIdField"));
            QObject *button = root->findChild<QObject *>(QStringLiteral("saveRadioSettingsButton"));
            settingsEdited = settingsEdited && name && id && button;
            if (!settingsEdited)
                return;
            name->setProperty("text", QStringLiteral("K1ABC"));
            id->setProperty("text", QStringLiteral("3208609"));
            settingsEdited = QMetaObject::invokeMethod(button, "clicked");
        });
        QTimer::singleShot(550, &app, [&store, &settingsEdited] {
            settingsEdited = settingsEdited && store.radioName() == QStringLiteral("K1ABC")
                && store.radioId() == 3208609;
            if (!settingsEdited)
                qCritical() << "Radio Settings page did not save the entered values";
        });
    }
    if (exercisePower) {
        QTimer::singleShot(100, &app, [&engine, &powerEdited] {
            bool opened = false;
            for (QObject *object : engine.rootObjects().constFirst()->findChildren<QObject *>()) {
                auto *item = qobject_cast<QQuickItem *>(object);
                if (item && item->isVisible() && item->property("text").toString() == QStringLiteral("Edit")) {
                    opened = QMetaObject::invokeMethod(item, "clicked");
                    break;
                }
            }
            powerEdited = opened;
        });
        QTimer::singleShot(250, &app, [&engine, &powerEdited] {
            QObject *field = engine.rootObjects().constFirst()->findChild<QObject *>(QStringLiteral("repeaterPowerField"));
            powerEdited = powerEdited && field && field->property("currentIndex").toInt() == 0;
            if (field)
                field->setProperty("currentIndex", 1);
            for (QObject *object : engine.rootObjects().constFirst()->findChildren<QObject *>()) {
                auto *item = qobject_cast<QQuickItem *>(object);
                if (item && item->isVisible() && item->property("text").toString() == QStringLiteral("Save changes")) {
                    powerEdited = powerEdited && QMetaObject::invokeMethod(item, "clicked");
                    break;
                }
            }
        });
        QTimer::singleShot(550, &app, [&store, &powerEdited] {
            powerEdited = powerEdited && store.selectedPower() == QStringLiteral("Low");
            if (!powerEdited)
                qCritical() << "Repeater power editor did not save Low power";
        });
    }
    if (exerciseAdd) {
        QTimer::singleShot(100, &app, [&engine] {
            bool opened = false;
            for (QObject *object : engine.rootObjects().constFirst()->findChildren<QObject *>(QStringLiteral("addTalkgroupButton"))) {
                auto *item = qobject_cast<QQuickItem *>(object);
                if (item && item->isVisible()) {
                    QMetaObject::invokeMethod(object, "clicked");
                    opened = true;
                    break;
                }
            }
            if (!opened)
                qCritical() << "No visible Add talkgroup button found";
        });
        QTimer::singleShot(250, &app, [&engine] {
            QObject *root = engine.rootObjects().constFirst();
            if (QObject *search = root->findChild<QObject *>(QStringLiteral("talkgroupSearch")))
                search->setProperty("text", QStringLiteral("Regional"));
        });
        QTimer::singleShot(350, &app, [&engine, &talkgroupAdded] {
            QObject *root = engine.rootObjects().constFirst();
            QObject *list = root->findChild<QObject *>(QStringLiteral("talkgroupChoices"));
            talkgroupAdded = list && list->property("count").toInt() == 1;
            if (list) {
                if (auto *content = list->property("contentItem").value<QQuickItem *>()) {
                    bool selected = false;
                    for (QQuickItem *item : content->childItems()) {
                        if (item->objectName() == QStringLiteral("talkgroupChoice")
                            && item->property("text").toString().startsWith(QStringLiteral("Regional"))) {
                            selected = QMetaObject::invokeMethod(item, "clicked");
                            break;
                        }
                    }
                    talkgroupAdded = talkgroupAdded && selected;
                }
            }
        });
        QTimer::singleShot(450, &app, [&engine] {
            QObject *root = engine.rootObjects().constFirst();
            if (QObject *button = root->findChild<QObject *>(QStringLiteral("confirmAddTalkgroup")))
                QMetaObject::invokeMethod(button, "clicked");
        });
        QTimer::singleShot(650, &app, [&store, &talkgroupAdded] {
            const QVariantList channels = store.slot1Talkgroups();
            talkgroupAdded = talkgroupAdded && channels.size() == 3
                && channels.last().toMap().value(QStringLiteral("channelName")).toString()
                    == QStringLiteral("North Regional 2");
            if (!talkgroupAdded)
                qCritical() << "Talkgroup picker did not add the chosen contact with its channel name"
                            << "channels" << channels.size();
        });
    }
    if (exerciseCreate) {
        QTimer::singleShot(100, &app, [&engine] {
            for (QObject *object : engine.rootObjects().constFirst()->findChildren<QObject *>(QStringLiteral("addTalkgroupButton"))) {
                auto *item = qobject_cast<QQuickItem *>(object);
                if (item && item->isVisible()) {
                    QMetaObject::invokeMethod(object, "clicked");
                    break;
                }
            }
        });
        QTimer::singleShot(250, &app, [&engine] {
            if (QObject *button = engine.rootObjects().constFirst()->findChild<QObject *>(QStringLiteral("createTalkgroupButton")))
                QMetaObject::invokeMethod(button, "clicked");
        });
        QTimer::singleShot(350, &app, [&engine] {
            QObject *root = engine.rootObjects().constFirst();
            if (QObject *name = root->findChild<QObject *>(QStringLiteral("newTalkgroupName")))
                name->setProperty("text", QStringLiteral("UI Test Group"));
            if (QObject *id = root->findChild<QObject *>(QStringLiteral("newTalkgroupId")))
                id->setProperty("text", QStringLiteral("7654"));
            if (QObject *channel = root->findChild<QObject *>(QStringLiteral("channelNameField")))
                channel->setProperty("text", QStringLiteral("UI Test Channel"));
            if (QObject *button = root->findChild<QObject *>(QStringLiteral("confirmAddTalkgroup")))
                QMetaObject::invokeMethod(button, "clicked");
        });
        QTimer::singleShot(650, &app, [&store, &talkgroupAdded] {
            const QVariantList channels = store.slot1Talkgroups();
            bool contactFound = false;
            for (const QVariant &value : store.contacts())
                if (value.toMap().value(QStringLiteral("id")).toInt() == 7654)
                    contactFound = true;
            talkgroupAdded = channels.size() == 3
                && channels.last().toMap().value(QStringLiteral("channelName")).toString()
                    == QStringLiteral("UI Test Channel")
                && contactFound;
            if (!talkgroupAdded)
                qCritical() << "Creating a talkgroup in the Add flow did not add its named channel";
        });
    }
    if (openContacts) {
        QTimer::singleShot(100, &app, [&engine] {
            for (QObject *object : engine.rootObjects().constFirst()->findChildren<QObject *>()) {
                auto *item = qobject_cast<QQuickItem *>(object);
                if (item && item->isVisible() && item->property("text").toString() == QStringLiteral("Contacts")) {
                    QMetaObject::invokeMethod(item, "clicked");
                    break;
                }
            }
        });
        QTimer::singleShot(500, &app, [&engine, &store, &contactsOpened] {
            auto *view = engine.rootObjects().constFirst()->findChild<QQuickItem *>(QStringLiteral("contactsView"));
            contactsOpened = view && view->isVisible()
                && view->property("contacts").toList().size() == store.contacts().size();
            if (!contactsOpened)
                qCritical() << "Contacts view did not open or show the store's contacts";
        });
    }
    if (clickNew) {
        QTimer::singleShot(100, &app, [&engine] {
            for (QObject *object : engine.rootObjects().constFirst()->findChildren<QObject *>()) {
                if (object->property("text").toString() == QStringLiteral("New")) {
                    QMetaObject::invokeMethod(object, "clicked");
                    break;
                }
            }
        });
        QTimer::singleShot(200, &app, [&engine, &app] {
            auto *blank = engine.rootObjects().constFirst()
                ->findChild<QObject *>(QStringLiteral("blankProjectOption"));
            if (!blank || !QMetaObject::invokeMethod(blank, "clicked"))
                app.exit(59);
        });
        QTimer::singleShot(300, &app, [] {
            for (QWidget *widget : QApplication::topLevelWidgets())
                if (auto *dialog = qobject_cast<QMessageBox *>(widget))
                    if (auto *yes = dialog->button(QMessageBox::Yes))
                        yes->click();
        });
        QTimer::singleShot(500, &app, [&engine, &store, &newStayedOpen] {
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
            newStayedOpen = window && window->isVisible() && !store.hasImportedCodeplug()
                && store.repeaterNames().isEmpty();
            if (!newStayedOpen)
                qCritical() << "New did not leave an empty project in a visible window:"
                            << "visible" << (window && window->isVisible())
                            << "imported" << store.hasImportedCodeplug()
                            << "repeaters" << store.repeaterNames().size();
        });
    }
    QTimer::singleShot(exerciseAdd || exerciseCreate || exercisePower || exerciseSettings ? 850 : 700, &app, [&engine] {
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()))
            window->close();
    });
    const int exitCode = app.exec();
    return qmlWarningCount == 0 && newStayedOpen && contactsOpened && talkgroupAdded && powerEdited
        && settingsEdited ? exitCode : 3;
}
