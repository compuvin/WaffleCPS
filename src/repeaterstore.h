#pragma once
#include "codeplugreader.h"

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
    Q_PROPERTY(QString selectedPower READ selectedPower NOTIFY selectedRepeaterChanged)
    Q_PROPERTY(QString radioName READ radioName NOTIFY radioSettingsChanged)
    Q_PROPERTY(int radioId READ radioId NOTIFY radioSettingsChanged)
    Q_PROPERTY(QString radioModel READ radioModel NOTIFY radioSettingsChanged)
    Q_PROPERTY(QString radioCpsVersion READ radioCpsVersion NOTIFY radioSettingsChanged)
    Q_PROPERTY(QVariantList selectedRepeaterChannels READ selectedRepeaterChannels NOTIFY talkgroupsChanged)
    Q_PROPERTY(QVariantList slot1Talkgroups READ slot1Talkgroups NOTIFY talkgroupsChanged)
    Q_PROPERTY(QVariantList slot2Talkgroups READ slot2Talkgroups NOTIFY talkgroupsChanged)
    Q_PROPERTY(QVariantList selectedSimplexFrequencies READ selectedSimplexFrequencies NOTIFY simplexFrequenciesChanged)
    Q_PROPERTY(QVariantList contacts READ contacts NOTIFY contactsChanged)
    Q_PROPERTY(QString projectDisplayName READ projectDisplayName NOTIFY projectChanged)
    Q_PROPERTY(QString sourceCodeplugName READ sourceCodeplugName NOTIFY projectChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY projectChanged)
    Q_PROPERTY(bool hasImportedCodeplug READ hasImportedCodeplug NOTIFY importedCodeplugChanged)
    Q_PROPERTY(QString importedCodeplugSummary READ importedCodeplugSummary NOTIFY importedCodeplugChanged)
    Q_PROPERTY(QStringList importedRepeaterNames READ importedRepeaterNames NOTIFY importedCodeplugChanged)
    Q_PROPERTY(bool editingAsZone READ editingAsZone NOTIFY importedSelectionChanged)
    Q_PROPERTY(QVariantList availableZoneChannels READ availableZoneChannels NOTIFY importedSelectionChanged)
    Q_PROPERTY(QStringList importedZoneNames READ importedZoneNames NOTIFY importedCodeplugChanged)
    Q_PROPERTY(QVariantMap selectedImportedGroup READ selectedImportedGroup NOTIFY importedSelectionChanged)
    Q_PROPERTY(int selectedImportedIndex READ selectedImportedIndex NOTIFY importedSelectionChanged)

public:
    explicit RepeaterStore(QObject *parent = nullptr);

    QStringList repeaterNames() const;
    int selectedIndex() const;
    QString selectedName() const;
    QString rxFrequency() const;
    QString txFrequency() const;
    int colorCode() const;
    QString selectedPower() const;
    QString radioName() const;
    int radioId() const;
    QString radioModel() const;
    QString radioCpsVersion() const;
    QVariantList selectedRepeaterChannels() const;
    Q_INVOKABLE bool reorderSelectedRepeaterChannels(const QVariantList &channelNumbers);
    QVariantList slot1Talkgroups() const;
    QVariantList slot2Talkgroups() const;
    QVariantList selectedSimplexFrequencies() const;
    QVariantList contacts() const;
    QString projectDisplayName() const;
    QString sourceCodeplugName() const;
    bool modified() const;
    bool hasImportedCodeplug() const;
    QString importedCodeplugSummary() const;
    QStringList importedRepeaterNames() const;
    QStringList importedZoneNames() const;
    QVariantMap selectedImportedGroup() const;
    int selectedImportedIndex() const;

    QVariantList availableZoneChannels() const;
    bool editingAsZone() const;
    Q_INVOKABLE bool editSelectedRepeaterAsZone();
    Q_INVOKABLE QVariantList zoneContactChoices() const;
    Q_INVOKABLE bool createCustomZone(const QString &name);
    Q_INVOKABLE bool renameSelectedZone(const QString &name);
    Q_INVOKABLE bool addExistingChannelToSelectedZone(int channelNumber);
    Q_INVOKABLE bool createChannelInSelectedZone(const QString &name, const QString &rx, const QString &tx,
        bool digital, int colorCode, int timeSlot, int contactIndex, bool highPower);
    Q_INVOKABLE bool updateSelectedZoneChannel(int zonePosition, const QString &name,
        const QString &rx, const QString &tx, bool digital, int colorCode, int timeSlot,
        int contactIndex, int powerChoice);
    Q_INVOKABLE bool removeSelectedZoneChannel(int zonePosition);
    Q_INVOKABLE bool updateSelectedZoneChannelFrequencies(int zonePosition, const QString &rx, const QString &tx);
    Q_INVOKABLE bool reorderSelectedZone(const QVariantList &channelNumbers);
    Q_INVOKABLE void leaveZoneEditor();
    Q_INVOKABLE void selectRepeater(int index);
    Q_INVOKABLE bool addRepeater(const QString &name, const QString &rxFrequency,
                                const QString &txFrequency, int colorCode, bool highPower = true);
    Q_INVOKABLE bool updateSelectedRepeater(const QString &name, const QString &rxFrequency,
                                            const QString &txFrequency, int colorCode,
                                            bool highPower = true);
    Q_INVOKABLE bool addTalkgroup(int timeSlot, const QString &name, int id,
                                 const QString &channelName = QString());
    Q_INVOKABLE QString suggestedChannelName(const QString &talkgroupName) const;
    Q_INVOKABLE QString availableChannelName(const QString &channelName) const;
    Q_INVOKABLE bool addContact(const QString &name, int id, bool groupCall);
    Q_INVOKABLE bool updateContact(int index, const QString &name, int id);
    Q_INVOKABLE void removeTalkgroup(int timeSlot, int id);
    Q_INVOKABLE bool addSimplexToSelectedRepeater(const QString &name, const QString &frequency,
                                                  bool digital, int colorCode, int timeSlot);
    Q_INVOKABLE bool removeSimplexFromSelectedRepeater(int index);
    Q_INVOKABLE void openProjectDialog();
    Q_INVOKABLE void saveProjectDialog(bool saveAs = false);
    Q_INVOKABLE void attachSourceDialog();
    Q_INVOKABLE void importCodeplugDialog();
    Q_INVOKABLE void importAttachedCodeplug();
    Q_INVOKABLE void selectImportedGroup(int index);
    Q_INVOKABLE bool updateSelectedImportedRepeater(const QString &name, const QString &rxFrequency,
                                                    const QString &txFrequency, int colorCode,
                                                    int powerChoice = -1);
    Q_INVOKABLE bool addImportedTalkgroup(int timeSlot, const QString &name, int id,
                                         const QString &channelName = QString());
    Q_INVOKABLE bool removeImportedTalkgroup(int timeSlot, int contactIndex, int zonePosition = 0);
    Q_INVOKABLE bool addSimplexToSelectedImportedRepeater(int zoneInGroup, const QString &name,
                                                         const QString &frequency, bool digital,
                                                         int colorCode, int timeSlot);
    Q_INVOKABLE bool removeSimplexFromSelectedImportedRepeater(int zoneInGroup, int zonePosition);
    Q_INVOKABLE bool reorderSelectedImportedZone(const QVariantList &channelNumbers);
    Q_INVOKABLE void newProject();
    Q_INVOKABLE void newProjectDialog();
    Q_INVOKABLE bool updateRadioSettings(const QString &name, const QString &idText);
    Q_INVOKABLE void exportCodeplugDialog();

    bool openProjectFromFile(const QString &path);
    bool saveProjectToFile(const QString &path);
    bool attachSourceCodeplugFromFile(const QString &path);
    bool importCodeplugFromFile(const QString &path);
    bool importCodeplugFromBytes(const QByteArray &bytes);
    bool importRadioCodeplug(const QByteArray &bytes);
    Q_INVOKABLE QByteArray exportCodeplugBytes();
    bool exportCodeplugToFile(const QString &path);

signals:
    void aboutToResetProject();
    void repeatersChanged();
    void selectedRepeaterChanged();
    void talkgroupsChanged();
    void projectChanged();
    void errorOccurred(const QString &message);
    void importedCodeplugChanged();
    void importedSelectionChanged();
    void simplexFrequenciesChanged();
    void contactsChanged();
    void radioSettingsChanged();

private:
    struct Talkgroup {
        QString name;
        int id;
        QString channelName;
        int zoneOrder = -1;
    };

    struct Repeater {
        QString name;
        qint64 rxHz;
        qint64 txHz;
        int colorCode;
        std::vector<Talkgroup> slot1;
        std::vector<Talkgroup> slot2;
        struct SimplexFrequency {
            QString name;
            qint64 hz;
            bool digital;
            int colorCode;
            int timeSlot;
            int zoneOrder = -1;
        };
        std::vector<SimplexFrequency> simplexFrequencies;
        bool highPower = true;
    };

    static int nextZoneOrder(const Repeater &repeater);
    static QVector<QPair<int, int>> orderedChannels(const Repeater &repeater);
    const Repeater &selected() const;
    Repeater &selected();
    const std::vector<Talkgroup> &talkgroups(int timeSlot) const;
    std::vector<Talkgroup> &talkgroups(int timeSlot);
    static QVariantList toVariantList(const std::vector<Talkgroup> &talkgroups);
    static QString formatFrequency(qint64 hz);
    static bool parseFrequency(const QString &text, qint64 *hz);
    void setModified(bool modified);
    bool fail(const QString &message);
    void notifyContactsChanged();
    void rebuildImportedGroups();
    bool buildManualCodeplug(ImportedCodeplug *result);
    bool ensureZoneRecords(int reservedZones = 0);
    int selectedZoneIndex() const;
    void selectZoneIndex(int zoneIndex);
    void zoneEdited();

    struct ImportedGroup {
        QString name;
        QString reason;
        QVector<int> zoneIndexes;
        bool repeater = false;
        quint32 frequencyAHz = 0;
        quint32 frequencyBHz = 0;
        int colorCode = 0;
    };

    std::vector<Repeater> m_repeaters;
    int m_selectedIndex = 0;
    QString m_projectPath;
    QString m_sourceCodeplugName;
    QByteArray m_sourceCodeplug;
    bool m_modified = false;
    bool m_generatedCodeplug = false;
    bool m_zoneClassificationPending = false;
    int m_advancedZoneIndex = -1;
    mutable bool m_zoneChoicesCached = false;
    mutable QVariantList m_zoneChoicesCache;
    mutable bool m_contactsCached = false;
    mutable QVariantList m_contactsCache;
    ImportedCodeplug m_importedCodeplug;
    QVector<ImportedContact> m_manualContacts;
    QString m_radioName;
    int m_radioId = 0;
    QVector<ImportedGroup> m_importedGroups;
    int m_selectedImportedIndex = 0;
};
