#include "repeaterstore.h"

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
    if (argc == 2) {
        const QString path = QString::fromLocal8Bit(argv[1]);
        const bool imported = QFileInfo(path).suffix().compare(QStringLiteral("rdbx"), Qt::CaseInsensitive) == 0;
        if (!(imported ? store.importCodeplugFromFile(path) : store.openProjectFromFile(path))) {
            qCritical() << "Could not open" << path;
            return 1;
        }
    }
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("store"), &store);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
