#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <vector>

class RepeaterStore : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList repeaterNames READ repeaterNames NOTIFY repeatersChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectedRepeaterChanged)
    Q_PROPERTY(QString selectedName READ selectedName NOTIFY selectedRepeaterChanged)
    Q_PROPERTY(QString rxFrequency READ rxFrequency NOTIFY selectedRepeaterChanged)
    Q_PROPERTY(QString txFrequency READ txFrequency NOTIFY selectedRepeaterChanged)
    Q_PROPERTY(int colorCode READ colorCode NOTIFY selectedRepeaterChanged)
    Q_PROPERTY(QVariantList slot1Talkgroups READ slot1Talkgroups NOTIFY talkgroupsChanged)
    Q_PROPERTY(QVariantList slot2Talkgroups READ slot2Talkgroups NOTIFY talkgroupsChanged)
    Q_PROPERTY(QString projectDisplayName READ projectDisplayName NOTIFY projectChanged)
    Q_PROPERTY(QString sourceCodeplugName READ sourceCodeplugName NOTIFY projectChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY projectChanged)

public:
    explicit RepeaterStore(QObject *parent = nullptr);

    QStringList repeaterNames() const;
    int selectedIndex() const;
    QString selectedName() const;
    QString rxFrequency() const;
    QString txFrequency() const;
    int colorCode() const;
    QVariantList slot1Talkgroups() const;
    QVariantList slot2Talkgroups() const;
    QString projectDisplayName() const;
    QString sourceCodeplugName() const;
    bool modified() const;

    Q_INVOKABLE void selectRepeater(int index);
    Q_INVOKABLE bool updateSelectedRepeater(const QString &name, const QString &rxFrequency,
                                            const QString &txFrequency, int colorCode);
    Q_INVOKABLE bool addTalkgroup(int timeSlot, const QString &name, int id);
    Q_INVOKABLE void removeTalkgroup(int timeSlot, int id);
    Q_INVOKABLE void openProjectDialog();
    Q_INVOKABLE void saveProjectDialog(bool saveAs = false);
    Q_INVOKABLE void attachSourceDialog();

    bool openProjectFromFile(const QString &path);
    bool saveProjectToFile(const QString &path);
    bool attachSourceCodeplugFromFile(const QString &path);

signals:
    void repeatersChanged();
    void selectedRepeaterChanged();
    void talkgroupsChanged();
    void projectChanged();
    void errorOccurred(const QString &message);

private:
    struct Talkgroup {
        QString name;
        int id;
    };

    struct Repeater {
        QString name;
        qint64 rxHz;
        qint64 txHz;
        int colorCode;
        std::vector<Talkgroup> slot1;
        std::vector<Talkgroup> slot2;
    };

    const Repeater &selected() const;
    Repeater &selected();
    const std::vector<Talkgroup> &talkgroups(int timeSlot) const;
    std::vector<Talkgroup> &talkgroups(int timeSlot);
    static QVariantList toVariantList(const std::vector<Talkgroup> &talkgroups);
    static QString formatFrequency(qint64 hz);
    static bool parseFrequency(const QString &text, qint64 *hz);
    void setModified(bool modified);
    bool fail(const QString &message);

    std::vector<Repeater> m_repeaters;
    int m_selectedIndex = 0;
    QString m_projectPath;
    QString m_sourceCodeplugName;
    QByteArray m_sourceCodeplug;
    bool m_modified = false;
};
