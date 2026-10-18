#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

struct ImportedChannel {
    QString name;
    quint32 frequencyAHz = 0;
    quint32 frequencyBHz = 0;
    bool digital = false;
    int colorCode = 0;
    int timeSlot = 0;
    int contactIndex = 0; // One-based contact table index; zero means no contact.
};

struct ImportedContact {
    QString name;
    int id = 0;
    bool groupCall = false;
};

struct ImportedZone {
    QString name;
    QVector<int> channelIndexes;
};

struct ImportedCodeplug {
    QString model;
    QString cpsVersion;
    QVector<ImportedChannel> channels;
    QVector<ImportedContact> contacts;
    QVector<ImportedZone> zones;
};

class CodeplugReader
{
public:
    // Reads the CS7000-M17 Plus V1.02.20.00 layout verified against the sample.
    static bool read(const QByteArray &bytes, ImportedCodeplug *result, QString *error);
};
