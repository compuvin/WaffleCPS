#include "radioreader.h"
#include "radiocodeplug.h"

#include <QMetaObject>
#include <QThread>
#include <memory>

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

void RadioReader::start()
{
    if (busy()) return;
    m_cancelled.store(false);
    m_progress = 0;
    m_status = tr("Connecting to radio…");
    struct Result { QByteArray encoded; QString error; };
    const auto result = std::make_shared<Result>();
    m_thread = QThread::create([this, result] {
        try {
            const auto progress = [this](const std::string &region, uint32_t completed, uint32_t total) {
                QMetaObject::invokeMethod(this, [this, region, completed, total] {
                    if (m_cancelled.load()) return;
                    m_status = tr("Reading %1: block %2 of %3")
                        .arg(region == "main" ? tr("radio settings") : tr("contacts"))
                        .arg(completed).arg(total);
                    // Main and contact regions are separate phases with separate totals.
                    m_progress = total ? double(completed) / total : 0;
                    emit progressChanged();
                }, Qt::QueuedConnection);
            };
            const auto bytes = m_read(progress, m_cancelled);
            if (m_cancelled.load()) return;
            const QByteArray raw(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()));
            radioCodeplugFromRaw(raw, &result->encoded, &result->error);
        } catch (const Cs7000Usb::Cancelled &) {
            // Cancellation leaves the currently open project intact.
        } catch (const std::exception &error) {
            result->error = QString::fromUtf8(error.what());
        }
    });
    connect(m_thread, &QThread::finished, this, [this, result] {
        auto *finished = m_thread;
        m_thread = nullptr;
        finished->deleteLater();
        const bool cancelled = m_cancelled.load();
        m_status = cancelled ? tr("Read cancelled")
                 : result->error.isEmpty() ? tr("Read complete") : tr("Read failed");
        emit progressChanged();
        // Deliver the result while the UI is still blocked, then close the progress dialog.
        if (!cancelled) {
            if (!result->error.isEmpty()) emit failed(result->error);
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
