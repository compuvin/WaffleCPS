#include "../src/repeaterstore.h"

#include <QApplication>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    RepeaterStore store;
    if (argc > 1 && !store.importCodeplugFromFile(QString::fromLocal8Bit(argv[1]))) {
        qCritical() << "Could not import sample for UI test";
        return 1;
    }
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("store"), &store);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 2;
    QTimer::singleShot(700, &app, [&engine] {
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()))
            window->close();
    });
    return app.exec();
}
