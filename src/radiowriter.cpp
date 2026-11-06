#include "radiowriter.h"
#include "codeplugdefinition.h"
#include "radiodefinition.h"
#ifdef __APPLE__
#include "macradiotransfer.h"
#endif
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>
#include <memory>
#include <cstdio>

RadioWriter::RadioWriter(QObject *parent, WriteOperation write)
    : QObject(parent), m_write(std::move(write)) {}

RadioWriter::~RadioWriter()
{
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
}

void RadioWriter::start(const QByteArray &encoded, const QString &definitionPath)
{
    if (busy() || encoded.isEmpty()) return;
    RadioDefinition radioDefinition;
    CodeplugDefinition codeplugDefinition;
    QByteArray decoded;
    QString error;
    if (definitionPath.isEmpty() || !RadioDefinition::load(definitionPath, &radioDefinition, &error)) {
        emit failed(error.isEmpty() ? tr("No radio definition selected.") : error);
        return;
    }
    std::fprintf(stdout, "[radio definitions] Write uses FILE: %s\n", qUtf8Printable(radioDefinition.sourcePath()));
    std::fflush(stdout);
    const QString wcpPath = QFileInfo(definitionPath).dir().filePath(
        QFileInfo(definitionPath).completeBaseName() + QStringLiteral(".wcp"));
    if (!CodeplugDefinition::load(wcpPath, &codeplugDefinition, &error)
        || !radioDefinition.compatibleWithCodeplug(codeplugDefinition.document(), &error)
        || !codeplugDefinition.decodedImage(encoded, &decoded, &error)) {
        emit failed(error);
        return;
    }
    ImportedCodeplug checked;
    if (!codeplugDefinition.read(decoded, &checked, &error)) {
        emit failed(error);
        return;
    }
    RadioExecutor::Bytes image(size_t(decoded.size()));
    for (qsizetype i = 0; i < decoded.size(); ++i) image[size_t(i)] = uint8_t(decoded[i]);
    m_status = tr("Connecting to radio…");
    m_progress = 0;
    auto result = std::make_shared<QString>();
    m_thread = QThread::create([this, image = std::move(image), radioDefinition,
                                codeplugDocument = codeplugDefinition.document(), result] {
        try {
            const auto progress = [this](const std::string &region, uint32_t done, uint32_t total) {
                QMetaObject::invokeMethod(this, [this, region, done, total] {
                    m_status = tr("Writing %1: block %2 of %3")
                        .arg(QString::fromStdString(region))
                        .arg(done).arg(total);
                    m_progress = total ? double(done) / total : 0;
                    emit progressChanged();
                }, Qt::QueuedConnection);
            };
            if (m_write) m_write(image, progress);
            else {
#ifdef __APPLE__
                MacRadioTransfer::write(radioDefinition.sourcePath(), image, progress);
#else
                RadioExecutor::write(radioDefinition, codeplugDocument, image, progress);
#endif
            }
        } catch (const std::exception &exception) {
            *result = QString::fromUtf8(exception.what());
        }
    });
    connect(m_thread, &QThread::finished, this, [this, result, definitionPath] {
        auto *finished = m_thread;
        m_thread = nullptr;
        finished->deleteLater();
        m_status = result->isEmpty() ? tr("Write complete") : tr("Write failed");
        emit progressChanged();
        if (result->isEmpty()) emit completed();
        else {
            std::fprintf(stderr, "[radio] Write failed using %s: %s\n",
                         qUtf8Printable(definitionPath), qUtf8Printable(*result));
            std::fflush(stderr);
            emit failed(tr("Write failed: %1").arg(*result));
        }
        emit stateChanged();
    });
    emit progressChanged();
    emit stateChanged();
    m_thread->start();
}
