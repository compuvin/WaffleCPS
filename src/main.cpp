#include "repeaterstore.h"

#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("WaffleCPS"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/waffle-radio.png")));

    RepeaterStore store;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("store"), &store);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
