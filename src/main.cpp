#include "radiowriter.h"
#include "repeaterstore.h"
#include "radioreader.h"

#include <QApplication>
#include <QIcon>
#include <QFileInfo>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("WaffleCPS"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/waffle-radio.png")));

    RepeaterStore store;
    RadioReader radioReader;
    QObject::connect(&radioReader, &RadioReader::completed,
                     &store, &RepeaterStore::importRadioCodeplug);
    if (argc == 2) {
        const QString path = QString::fromLocal8Bit(argv[1]);
        const QString extension = QFileInfo(path).suffix().toLower();
        const bool project = extension == QStringLiteral("waffle") || extension == QStringLiteral("zip");
        if (!(project ? store.openProjectFromFile(path) : store.importCodeplugFromFile(path))) {
            qCritical() << "Could not open" << path;
            return 1;
        }
    }
    RadioWriter radioWriter;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("store"), &store);
    engine.rootContext()->setContextProperty(QStringLiteral("radioReader"), &radioReader);
    engine.rootContext()->setContextProperty(QStringLiteral("radioWriter"), &radioWriter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
