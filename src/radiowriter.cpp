#include "radiowriter.h"
#include "codeplugreader.h"
#include <QMetaObject>
#include <QThread>
#include <memory>

RadioWriter::RadioWriter(QObject *parent, WriteOperation write)
    : QObject(parent), m_write(std::move(write)) {}

RadioWriter::~RadioWriter()
{
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
}

void RadioWriter::start(const QByteArray &encoded)
{
    if (busy() || encoded.isEmpty()) return;
    // Freeze and validate the complete exported image before entering programming mode.
    ImportedCodeplug checked;
    QString error;
    if (!CodeplugReader::read(encoded, &checked, &error)) {
        emit failed(error);
        return;
    }
    Cs7000Usb::Bytes image(size_t(encoded.size()));
    for (qsizetype i = 0; i < encoded.size(); ++i)
        image[size_t(i)] = uint8_t(encoded[i]) ^ uint8_t(encoded[0x300 + i % 256]);
    try { Cs7000Usb::planWrite(image); }
    catch (const std::exception &exception) {
        emit failed(QString::fromUtf8(exception.what()));
        return;
    }
    m_status = tr("Connecting to radio…");
    m_progress = 0;
    auto result = std::make_shared<QString>();
    m_thread = QThread::create([this, image = std::move(image), result] {
        try {
            m_write(image, [this](const std::string &region, uint32_t done, uint32_t total) {
                QMetaObject::invokeMethod(this, [this, region, done, total] {
                    m_status = tr("Writing %1: block %2 of %3")
                        .arg(region == "main" ? tr("radio settings") : tr("contacts"))
                        .arg(done).arg(total);
                    m_progress = total ? double(done) / total : 0;
                    emit progressChanged();
                }, Qt::QueuedConnection);
            });
        } catch (const std::exception &exception) {
            *result = QString::fromUtf8(exception.what());
        }
    });
    connect(m_thread, &QThread::finished, this, [this, result] {
        auto *finished = m_thread;
        m_thread = nullptr;
        finished->deleteLater();
        m_status = result->isEmpty() ? tr("Write complete") : tr("Write failed");
        emit progressChanged();
        if (result->isEmpty()) emit completed();
        else emit failed(tr("Write failed: %1").arg(*result));
        emit stateChanged();
    });
    emit progressChanged();
    emit stateChanged();
    m_thread->start();
}
