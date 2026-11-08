#include "radioexecutor.h"
#include "codeplugdefinition.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <thread>

namespace {
void progress(const QString &path, const std::string &region, uint32_t done, uint32_t total)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                        | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    file.write(QJsonDocument(QJsonObject{
        {QStringLiteral("region"), QString::fromStdString(region)},
        {QStringLiteral("done"), int(done)},
        {QStringLiteral("total"), int(total)}
    }).toJson(QJsonDocument::Compact));
    file.flush();
}

void reportError(const QString &path, const QString &message)
{
    if (path.isEmpty()) return;
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(message.toUtf8());
        file.flush();
    }
}

class CancelWatcher {
    std::atomic_bool &cancelled;
    std::atomic_bool stopped{false};
    std::thread thread;
public:
    CancelWatcher(std::atomic_bool &cancel, const QString &path) : cancelled(cancel)
    {
        thread = std::thread([this, path] {
            while (!stopped.load()) {
                if (QFile::exists(path)) { cancelled.store(true); return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }
    ~CancelWatcher() { stopped.store(true); thread.join(); }
};
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 6 && args.size() != 7) {
        std::fprintf(stderr, "Usage: radio-transfer-helper read|write profile-source image.bin progress.json cancel-marker|- [error.txt]\n");
        return 2;
    }
    const QString mode = args[1];
    const QString wrdPath = args[2];
    const QString imagePath = args[3];
    const QString progressPath = args[4];
    const QString cancelPath = args[5];
    const QString errorPath = args.size() == 7 ? args[6] : QString();
    if (mode != QStringLiteral("read") && mode != QStringLiteral("write")) {
        std::fprintf(stderr, "Unknown radio transfer operation.\n");
        reportError(errorPath, QStringLiteral("Unknown radio transfer operation."));
        return 2;
    }

    RadioDefinition radio;
    CodeplugDefinition codeplug;
    QString error;
    if (!RadioDefinition::loadProfile(wrdPath, &radio, &codeplug, nullptr, &error)) {
        std::fprintf(stderr, "%s\n", qUtf8Printable(error));
        reportError(errorPath, error);
        return 1;
    }
    {
        QFile progressFile(progressPath);
        if (!progressFile.open(QIODevice::WriteOnly)
            || !progressFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                            | QFileDevice::ReadGroup | QFileDevice::ReadOther)) {
            std::fprintf(stderr, "Could not prepare radio transfer progress file.\n");
            reportError(errorPath, QStringLiteral("Could not prepare radio transfer progress file."));
            return 1;
        }
    }

    try {
        const auto update = [&progressPath](const std::string &region, uint32_t done, uint32_t total) {
            progress(progressPath, region, done, total);
        };
        if (mode == QStringLiteral("read")) {
            std::atomic_bool cancelled{false};
            CancelWatcher watcher(cancelled, cancelPath);
            const auto bytes = RadioExecutor::read(radio, update, cancelled);
            QSaveFile file(imagePath);
            if (!file.open(QIODevice::WriteOnly)
                || file.write(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()))
                    != qsizetype(bytes.size())
                || !file.commit()
                || !QFile::setPermissions(imagePath,
                    QFileDevice::ReadOwner | QFileDevice::WriteOwner
                    | QFileDevice::ReadGroup | QFileDevice::ReadOther))
                throw std::runtime_error("Could not deliver radio read image");
        } else {
            QFile file(imagePath);
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Could not open radio write image");
            const QByteArray data = file.readAll();
            if (data.isEmpty()) throw std::runtime_error("Radio write image is empty");
            ImportedCodeplug checked;
            if (!codeplug.read(data, &checked, &error))
                throw std::runtime_error(error.toStdString());
            RadioExecutor::write(radio, codeplug.document(),
                                 RadioExecutor::Bytes(data.begin(), data.end()), update);
        }
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "Radio transfer failed: %s\n", e.what());
        reportError(errorPath, QStringLiteral("Radio transfer failed: %1").arg(QString::fromUtf8(e.what())));
        return 1;
    }
}
