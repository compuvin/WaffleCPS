#include "../src/repeaterstore.h"

#include <QApplication>
#include <QAbstractButton>
#include <QDebug>
#include <QMessageBox>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTimer>
#include <QUrl>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    const bool clickNew = argc > 2 && QByteArray(argv[2]) == "--click-new";
    const bool openContacts = argc > 2 && QByteArray(argv[2]) == "--contacts";
    const bool exerciseAdd = argc > 2 && QByteArray(argv[2]) == "--add-talkgroup";
    const bool exerciseCreate = argc > 2 && QByteArray(argv[2]) == "--create-talkgroup";
    const bool demo = argc > 1 && QByteArray(argv[1]) == "--demo";
    RepeaterStore store;
    if (argc > 1 && QByteArray(argv[1]) == "--new")
        store.newProject();
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
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 2;
    bool newStayedOpen = true;
    bool contactsOpened = true;
    bool talkgroupAdded = true;
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
            if (QObject *channel = root->findChild<QObject *>(QStringLiteral("channelNameField")))
                channel->setProperty("text", QStringLiteral("North Regional"));
            if (QObject *button = root->findChild<QObject *>(QStringLiteral("confirmAddTalkgroup")))
                QMetaObject::invokeMethod(button, "clicked");
        });
        QTimer::singleShot(650, &app, [&store, &talkgroupAdded] {
            const QVariantList channels = store.slot1Talkgroups();
            talkgroupAdded = talkgroupAdded && channels.size() == 3
                && channels.last().toMap().value(QStringLiteral("channelName")).toString()
                    == QStringLiteral("North Regional");
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
    QTimer::singleShot(exerciseAdd || exerciseCreate ? 850 : 700, &app, [&engine] {
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()))
            window->close();
    });
    const int exitCode = app.exec();
    return qmlWarningCount == 0 && newStayedOpen && contactsOpened && talkgroupAdded ? exitCode : 3;
}
