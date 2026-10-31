#pragma once
#include "radioexecutor.h"
#include <QObject>
#include <QByteArray>
#include <QString>
#include <functional>

class QThread;

class RadioWriter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY progressChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
public:
    using WriteOperation = std::function<void(const RadioExecutor::Bytes &, const RadioExecutor::Progress &)>;
    explicit RadioWriter(QObject *parent = nullptr, WriteOperation write = {});
    ~RadioWriter() override;
    bool busy() const { return m_thread != nullptr; }
    QString status() const { return m_status; }
    double progress() const { return m_progress; }
    Q_INVOKABLE void start(const QByteArray &encoded, const QString &definitionPath = {});
signals:
    void stateChanged();
    void progressChanged();
    void completed();
    void failed(const QString &message);
private:
    WriteOperation m_write;
    QThread *m_thread = nullptr;
    QString m_status;
    double m_progress = 0;
};
