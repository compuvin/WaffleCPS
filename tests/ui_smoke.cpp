#include "../src/repeaterstore.h"
#include "../src/cs7000default.h"
#include "../src/radioreader.h"

#include <QApplication>
#include <QAbstractButton>
#include <QDebug>
#include <QDir>
#include <QImage>
#include <QMessageBox>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTimer>
#include <QUrl>
#include <QThread>
#include <stdexcept>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    const bool clickNew = argc > 2 && QByteArray(argv[2]) == "--click-new";
    const bool openContacts = argc > 2 && QByteArray(argv[2]) == "--contacts";
    const bool exerciseAdd = argc > 2 && QByteArray(argv[2]) == "--add-talkgroup";
    const bool exerciseCreate = argc > 2 && QByteArray(argv[2]) == "--create-talkgroup";
    const bool exercisePower = argc > 2 && QByteArray(argv[2]) == "--power";
    const bool exerciseSettings = argc > 2 && QByteArray(argv[2]) == "--settings";
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
    if (argc > 1 && QByteArray(argv[1]) == "--new")
        store.newProject();
    else if (argc > 1 && QByteArray(argv[1]) == "--embedded") {
        if (!store.importCodeplugFromBytes(cs7000DefaultCodeplug()))
            return 1;
    }
    else if (argc > 1 && !demo && !store.importCodeplugFromFile(QString::fromLocal8Bit(argv[1]))) {
        qCritical() << "Could not import sample for UI test";
        return 1;
    }
    QQmlApplicationEngine engine;
    int qmlWarningCount = 0;
    QObject::connect(&engine, &QQmlEngine::warnings, &app,
                     [&qmlWarningCount](const QList<QQmlError> &warnings) {
                         qmlWarningCount += warnings.size();
                     });
    engine.rootContext()->setContextProperty(QStringLiteral("store"), &store);
    engine.rootContext()->setContextProperty(QStringLiteral("radioReader"), &radioReader);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 2;
    if (exerciseRadio) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        window->resize(800, 720);
        QObject *root = engine.rootObjects().constFirst();
        auto *button = root->findChild<QQuickItem *>(QStringLiteral("readRadioButton"));
        QObject *dialog = root->findChild<QObject *>(QStringLiteral("radioReadProgress"));
        QObject *confirmation = root->findChild<QObject *>(QStringLiteral("radioReadConfirmation"));
        QObject *cancel = root->findChild<QObject *>(QStringLiteral("cancelRadioReadButton"));
        if (!button || !dialog || !confirmation || !cancel) return 10;
        int heartbeats = 0;
        bool readStarted = false;
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++heartbeats; });
        heartbeat.start(10);
        QTimer::singleShot(50, &app, [&] {
            const QPointF position = button->mapToScene(QPointF(0, 0));
            if (position.x() < 0 || position.x() + button->width() > window->width()) {
                qCritical() << "Radio button overflows toolbar at minimum window width";
                app.exit(11);
                return;
            }
            store.addContact(QStringLiteral("Pending contact"), 1234567, false);
            QMetaObject::invokeMethod(button, "clicked");
            if (!confirmation->property("visible").toBool() || radioReader.busy()) {
                qCritical() << "Unsaved project was not confirmed before reading";
                app.exit(12);
                return;
            }
            QMetaObject::invokeMethod(confirmation, "close");
            store.newProject();
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
