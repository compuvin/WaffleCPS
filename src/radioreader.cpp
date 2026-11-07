#include "radioreader.h"
#include "codeplugdefinition.h"
#include "codeplugdefinitions.h"
#include "radioimage.h"
#ifdef __APPLE__
#include "macradiotransfer.h"
#elif defined(_WIN32)
#include "windowsradiotransfer.h"
#endif

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>
#include <memory>
#include <cstdio>

RadioReader::RadioReader(QObject *parent, ReadOperation read)
    : QObject(parent), m_read(std::move(read)) {}

RadioReader::~RadioReader()
{
    m_cancelled.store(true);
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
}

void RadioReader::start(const QString &definitionPath)
{
    if (busy()) return;
    RadioDefinition definition;
    QByteArray radioTemplate;
    if (!m_read) {
        QString error;
        if (definitionPath.isEmpty() || !RadioDefinition::load(definitionPath, &definition, &error)) {
            emit failed(error.isEmpty() ? tr("No radio definition selected.") : error);
            return;
        }
        const QString wcpPath = QFileInfo(definitionPath).dir().filePath(
            QFileInfo(definitionPath).completeBaseName() + QStringLiteral(".wcp"));
        CodeplugDefinition codeplug;
        if (!CodeplugDefinition::load(wcpPath, &codeplug, &error)
            || !definition.compatibleWithCodeplug(codeplug.document(), &error)) {
            emit failed(error);
            return;
        }
        if (definition.usesDfu() || definition.usesSerial()) radioTemplate = codeplug.defaultCodeplug();
        std::fprintf(stdout, "[radio definitions] Read uses FILE: %s\n", qUtf8Printable(definition.sourcePath()));
        std::fflush(stdout);
    }
    m_cancelled.store(false);
    m_progress = 0;
    m_status = tr("Connecting to radio…");
    struct Result { QByteArray encoded; QString error; };
    const auto result = std::make_shared<Result>();
    m_thread = QThread::create([this, result, definition, definitionPath, radioTemplate] {
        try {
            const auto progress = [this](const std::string &region, uint32_t completed, uint32_t total) {
                QMetaObject::invokeMethod(this, [this, region, completed, total] {
                    if (m_cancelled.load()) return;
                    m_status = tr("Reading %1: block %2 of %3")
                        .arg(QString::fromStdString(region))
                        .arg(completed).arg(total);
                    // Each declared region reports its own block total.
                    m_progress = total ? double(completed) / total : 0;
                    emit progressChanged();
                }, Qt::QueuedConnection);
            };
            const auto bytes = m_read ? m_read(progress, m_cancelled)
#ifdef __APPLE__
                : MacRadioTransfer::read(definition, definitionPath, progress, m_cancelled);
#elif defined(_WIN32)
                : WindowsRadioTransfer::read(definition, definitionPath, progress, m_cancelled);
#else
                : RadioExecutor::read(definition, progress, m_cancelled);
#endif
            if (m_cancelled.load()) return;
            result->encoded = assembleRadioImage(definition, radioTemplate,
                QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())));
            ImportedCodeplug checked;
            const QString wcpName = definitionPath.isEmpty() ? QString()
                : QFileInfo(definitionPath).completeBaseName() + QStringLiteral(".wcp");
            if (!CodeplugDefinitions::read(result->encoded, &checked, &result->error, wcpName))
                result->encoded.clear();
        } catch (const RadioExecutor::Cancelled &) {
            // Cancellation leaves the currently open project intact.
        } catch (const std::exception &error) {
            result->error = QString::fromUtf8(error.what());
        }
    });
    connect(m_thread, &QThread::finished, this, [this, result, definitionPath] {
        auto *finished = m_thread;
        m_thread = nullptr;
        finished->deleteLater();
        const bool cancelled = m_cancelled.load();
        m_status = cancelled ? tr("Read cancelled")
                 : result->error.isEmpty() ? tr("Read complete") : tr("Read failed");
        emit progressChanged();
        // Deliver the result while the UI is still blocked, then close the progress dialog.
        if (!cancelled) {
            if (!result->error.isEmpty()) {
                std::fprintf(stderr, "[radio] Read failed using %s: %s\n",
                             qUtf8Printable(definitionPath), qUtf8Printable(result->error));
                std::fflush(stderr);
                emit failed(result->error);
            }
            else emit completed(result->encoded);
        }
        emit stateChanged();
    });
    m_thread->start();
    emit progressChanged();
    emit stateChanged();
}

void RadioReader::cancel()
{
    if (!busy()) return;
    m_cancelled.store(true);
    m_status = tr("Cancelling after the current radio response…");
    emit progressChanged();
}
