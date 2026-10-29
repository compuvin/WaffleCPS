#pragma once

#include "cs7000usb.h"
#include <QObject>
#include <QByteArray>
#include <QString>
#include <atomic>
#include <functional>

class QThread;

class RadioReader : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY progressChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)

public:
    using ReadOperation = std::function<Cs7000Usb::Bytes(
        const Cs7000Usb::Progress &, const std::atomic_bool &)>;
    explicit RadioReader(QObject *parent = nullptr, ReadOperation read = Cs7000Usb::read);
    ~RadioReader() override;
    bool busy() const { return m_thread != nullptr; }
    QString status() const { return m_status; }
    double progress() const { return m_progress; }
    Q_INVOKABLE void start();
    Q_INVOKABLE void cancel();
signals:
    void stateChanged();
    void progressChanged();
    void completed(const QByteArray &codeplug);
    void failed(const QString &message);

private:
    ReadOperation m_read;
    QThread *m_thread = nullptr;
    std::atomic_bool m_cancelled{false};
    QString m_status;
    double m_progress = 0;
};
