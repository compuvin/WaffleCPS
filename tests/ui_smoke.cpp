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
    if (argc > 1 && QByteArray(argv[1]) == "--new")
        store.newProject();
    else if (argc > 1 && !store.importCodeplugFromFile(QString::fromLocal8Bit(argv[1]))) {
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
    QTimer::singleShot(700, &app, [&engine] {
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()))
            window->close();
    });
    const int exitCode = app.exec();
    return qmlWarningCount == 0 ? exitCode : 3;
}
