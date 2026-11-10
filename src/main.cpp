#include "radiowriter.h"
#include "repeaterstore.h"
#include "radioreader.h"
#include "codeplugdefinitions.h"

#include <QApplication>
#include <QIcon>
#include <QFileInfo>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QQuickStyle>
#include <QUrl>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

class RadioIconProvider : public QQuickImageProvider
{
public:
    RadioIconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

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
#ifdef Q_OS_WIN
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
        qputenv("QT_FORCE_STDERR_LOGGING", "1");
    }
#endif
    QApplication app(argc, argv);
#ifdef __APPLE__
    QQuickStyle::setStyle(QStringLiteral("Basic"));
#endif
    QApplication::setApplicationName(QStringLiteral("WaffleCPS"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/waffle-radio.png")));

    RepeaterStore store;
    RadioReader radioReader;
    QObject::connect(&radioReader, &RadioReader::completed,
                     &store, &RepeaterStore::importRadioCodeplug);
    QStringList arguments = app.arguments();
    arguments.removeFirst();
    if (arguments.size() == 1) {
        const QString path = arguments.first();
        const QString extension = QFileInfo(path).suffix().toLower();
        const bool project = extension == QStringLiteral("waffle") || extension == QStringLiteral("zip");
        if (!(project ? store.openProjectFromFile(path) : store.importCodeplugFromFile(path))) {
            qCritical() << "Could not open" << path;
            return 1;
        }
    }
    RadioWriter radioWriter;
    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("radioicons"), new RadioIconProvider);
    engine.rootContext()->setContextProperty(QStringLiteral("store"), &store);
    engine.rootContext()->setContextProperty(QStringLiteral("radioReader"), &radioReader);
    engine.rootContext()->setContextProperty(QStringLiteral("radioWriter"), &radioWriter);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
