#include "radioexecutor.h"
#include "codeplugdefinition.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
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
    if (argc != 6) {
        std::fprintf(stderr, "Usage: radio-transfer-helper read|write definition.wrd image.bin progress.json cancel-marker|-\n");
        return 2;
    }
    const QString mode = QString::fromLocal8Bit(argv[1]);
    const QString wrdPath = QString::fromLocal8Bit(argv[2]);
    const QString imagePath = QString::fromLocal8Bit(argv[3]);
    const QString progressPath = QString::fromLocal8Bit(argv[4]);
    const QString cancelPath = QString::fromLocal8Bit(argv[5]);
    if (mode != QStringLiteral("read") && mode != QStringLiteral("write")) {
        std::fprintf(stderr, "Unknown radio transfer operation.\n");
        return 2;
    }

    RadioDefinition radio;
    CodeplugDefinition codeplug;
    QString error;
    const QString wcpPath = QFileInfo(wrdPath).dir().filePath(
        QFileInfo(wrdPath).completeBaseName() + QStringLiteral(".wcp"));
    if (!RadioDefinition::load(wrdPath, &radio, &error)
        || !CodeplugDefinition::load(wcpPath, &codeplug, &error)
        || !radio.compatibleWithCodeplug(codeplug.document(), &error)) {
        std::fprintf(stderr, "%s\n", qUtf8Printable(error));
        return 1;
    }
    {
        QFile progressFile(progressPath);
        if (!progressFile.open(QIODevice::WriteOnly)
            || !progressFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                            | QFileDevice::ReadGroup | QFileDevice::ReadOther)) {
            std::fprintf(stderr, "Could not prepare radio transfer progress file.\n");
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
        return 1;
    }
}
