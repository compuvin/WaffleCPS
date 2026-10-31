#pragma once

#include "radioexecutor.h"
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
    using ReadOperation = std::function<RadioExecutor::Bytes(
        const RadioExecutor::Progress &, const std::atomic_bool &)>;
    explicit RadioReader(QObject *parent = nullptr, ReadOperation read = {});
    ~RadioReader() override;
    bool busy() const { return m_thread != nullptr; }
    QString status() const { return m_status; }
    double progress() const { return m_progress; }
    Q_INVOKABLE void start(const QString &definitionPath = {});
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
