#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QtGlobal>

struct RadioReadOperation {
    enum class Kind { Identify, ReadRegion } kind;
    QString region;
    quint8 command = 0;
    QString sizeField;
    bool checkFirstBlockSize = false;
    int sizeCheckOffset = 0;
    QString sizeCheckType;
};

class RadioDefinition {
public:
    static bool load(const QString &path, RadioDefinition *out, QString *error);
    static bool fromJson(const QByteArray &json, RadioDefinition *out, QString *error);
    bool canRead() const { return !m_readOperations.isEmpty(); }
    bool canWrite() const { return !m_writePlan.isEmpty(); }
    const QVector<RadioReadOperation> &readOperations() const { return m_readOperations; }
    const QJsonObject &writePlan() const { return m_writePlan; }
    const QJsonObject &document() const { return m_document; }
    QString sourcePath() const { return m_sourcePath; }
    bool compatibleWithCodeplug(const QJsonObject &codeplugDocument, QString *error) const;

private:
    QVector<RadioReadOperation> m_readOperations;
    QJsonObject m_writePlan;
    QJsonObject m_document;
    QString m_sourcePath;
};
