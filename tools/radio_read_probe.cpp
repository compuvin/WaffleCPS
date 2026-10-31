#include "radioexecutor.h"
#include "codeplugdefinition.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <atomic>
#include <cstdio>
#include <exception>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "Usage: radio-read-probe definition.wrd\n");
        return 2;
    }

    const QString wrdPath = QString::fromLocal8Bit(argv[1]);
    const QString wcpPath = QFileInfo(wrdPath).dir().filePath(
        QFileInfo(wrdPath).completeBaseName() + QStringLiteral(".wcp"));
    RadioDefinition radio;
    CodeplugDefinition codeplug;
    QString error;
    if (!RadioDefinition::load(wrdPath, &radio, &error)
        || !CodeplugDefinition::load(wcpPath, &codeplug, &error)
        || !radio.compatibleWithCodeplug(codeplug.document(), &error)) {
        std::fprintf(stderr, "%s\n", qUtf8Printable(error));
        return 1;
    }

    try {
        const std::atomic_bool cancelled{false};
        const auto bytes = RadioExecutor::read(
            radio,
            [](const std::string &region, uint32_t done, uint32_t total) {
                if (done == total)
                    std::fprintf(stderr, "Read %s: %u blocks\n", region.c_str(), total);
            },
            cancelled);
        const QByteArray data(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()));
        const QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
        std::printf("Read %lld bytes; SHA-256 %s\n",
                    static_cast<long long>(data.size()), hash.constData());
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "Read failed: %s\n", e.what());
        return 1;
    }
}
