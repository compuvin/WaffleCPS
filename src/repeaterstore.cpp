#include "repeaterstore.h"
#include "codeplugdefinitions.h"
#include "codeplugconversion.h"
#include "ziparchive.h"
#include "satelliteorbits.h"
#include "satellitedata.h"
#include "serialexecutor.h"
#include "radiodefinition.h"
#include <QThread>
#include <memory>
#include <cstdio>

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <QClipboard>
#include <QGuiApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSet>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSettings>
#include <QDir>
#include <QUrl>
#include <QTimer>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

const QString projectFormat = QStringLiteral("org.wafflecps.project");
const QString sourceEntry = QStringLiteral("source/original-codeplug.bin");
constexpr qint64 maximumSourceSize = 256ll * 1024ll * 1024ll;

bool integerValue(const QJsonValue &value, qint64 minimum, qint64 maximum, qint64 *result)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < minimum || number > maximum || std::floor(number) != number)
        return false;
    *result = static_cast<qint64>(number);
    return true;
}

void makeExportChannelNamesUnique(ImportedCodeplug *codeplug)
{
    QSet<QString> reserved;
    QSet<QString> used;
    for (const auto &channel : codeplug->channels)
        reserved.insert(channel.name.toCaseFolded());
    for (auto &channel : codeplug->channels) {
        const QString key = channel.name.toCaseFolded();
        if (!used.contains(key)) {
            used.insert(key);
            continue;
        }
        for (int number = 2; ; ++number) {
            const QString suffix = QStringLiteral(" %1").arg(number);
            const QString candidate = channel.name.left(16 - suffix.size()) + suffix;
            const QString candidateKey = candidate.toCaseFolded();
            if (!reserved.contains(candidateKey) && !used.contains(candidateKey)) {
                channel.name = candidate;
                used.insert(candidateKey);
                break;
            }
        }
    }
}

} // namespace

RepeaterStore::RepeaterStore(QObject *parent)
    : QObject(parent)
    , m_repeaters{
          {QStringLiteral("North Ridge"), 444500000, 449500000, 1,
           {{QStringLiteral("Local"), 9, QStringLiteral("North Local")},
            {QStringLiteral("Statewide"), 123, QStringLiteral("North Statewide")}},
           {{QStringLiteral("Regional"), 3100, QStringLiteral("North Regional")}}},
          {QStringLiteral("Riverbend"), 442125000, 447125000, 3,
           {{QStringLiteral("Local"), 9, QStringLiteral("Riverbend Local")}},
           {{QStringLiteral("Worldwide"), 91, QStringLiteral("Riverbend Worldw")},
            {QStringLiteral("Regional"), 3100, QStringLiteral("Riverbend Regio")}}}}
{
    connect(this, &RepeaterStore::importedSelectionChanged, this, [this] { m_zoneChoicesCached = false; });
    for (auto signal : {&RepeaterStore::projectChanged, &RepeaterStore::repeatersChanged,
                        &RepeaterStore::contactsChanged, &RepeaterStore::talkgroupsChanged,
                        &RepeaterStore::simplexFrequenciesChanged, &RepeaterStore::radioSettingsChanged,
                        &RepeaterStore::importedCodeplugChanged, &RepeaterStore::selectedRepeaterChanged,
                        &RepeaterStore::importedSelectionChanged})
        connect(this, signal, this, [this] { ++m_conversionRevision; });
}

RepeaterStore::~RepeaterStore()
{
    if (m_satelliteThread) {
        m_satelliteThread->wait();
        delete m_satelliteThread;
    }
}

void RepeaterStore::notifyContactsChanged()
{
    // QML may reevaluate bindings before ordinary signal handlers run.
    m_contactsCached = false;
    m_zoneChoicesCached = false;
    emit contactsChanged();
}

QStringList RepeaterStore::repeaterNames() const
{
    QStringList names;
    for (const auto &repeater : m_repeaters)
        names.append(repeater.name);
    return names;
}

int RepeaterStore::selectedIndex() const { return m_selectedIndex; }
QString RepeaterStore::selectedName() const { return m_repeaters.empty() ? QString() : selected().name; }
QString RepeaterStore::rxFrequency() const { return m_repeaters.empty() ? QString() : formatFrequency(selected().rxHz); }
QString RepeaterStore::txFrequency() const { return m_repeaters.empty() ? QString() : formatFrequency(selected().txHz); }
int RepeaterStore::colorCode() const { return m_repeaters.empty() ? 0 : selected().colorCode; }
QStringList RepeaterStore::powerChoices() const
{
    const auto values = hasImportedCodeplug() ? m_importedCodeplug.powerSettings : QStringList{"low", "high"};
    QStringList labels;
    // Preserve the familiar display order, while choices come from the definition.
    if (values.contains("high")) labels.append("High");
    for (const auto &value : values) {
        if (value == "high" || value.isEmpty()) continue;
        QString label = value; label[0] = label[0].toUpper(); labels.append(label);
    }
    return labels;
}
int RepeaterStore::powerChoice(const QString &label) const
{
    const auto values = hasImportedCodeplug() ? m_importedCodeplug.powerSettings : QStringList{"low", "high"};
    for (int i = 0; i < values.size(); ++i) if (values[i].compare(label, Qt::CaseInsensitive) == 0) return i;
    return -1;
}

QString RepeaterStore::selectedPower() const
{
    return m_repeaters.empty() ? QString() : (selected().highPower ? QStringLiteral("High") : QStringLiteral("Low"));
}
QString RepeaterStore::radioName() const { return m_radioName; }
int RepeaterStore::radioId() const { return m_radioId; }
QString RepeaterStore::radioModel() const { return m_importedCodeplug.model; }
QString RepeaterStore::radioCpsVersion() const { return m_importedCodeplug.cpsVersion; }

QString RepeaterStore::availableChannelName(const QString &channelName) const
{
    const QString base = channelName.trimmed().left(16);
    if (base.isEmpty())
        return {};
    QSet<QString> names;
    if (hasImportedCodeplug()) {
        for (const auto &channel : m_importedCodeplug.channels)
            names.insert(channel.name.toCaseFolded());
    } else {
        for (const auto &repeater : m_repeaters) {
            for (const auto &group : repeater.slot1)
                names.insert((group.channelName.isEmpty() ? group.name : group.channelName).toCaseFolded());
            for (const auto &group : repeater.slot2)
                names.insert((group.channelName.isEmpty() ? group.name : group.channelName).toCaseFolded());
            for (const auto &simplex : repeater.simplexFrequencies)
                names.insert(simplex.name.toCaseFolded());
        }
    }
    if (!names.contains(base.toCaseFolded()))
        return base;
    for (int number = 2; ; ++number) {
        const QString suffix = QStringLiteral(" %1").arg(number);
        const QString candidate = base.left(16 - suffix.size()) + suffix;
        if (!names.contains(candidate.toCaseFolded()))
            return candidate;
    }
}

QString RepeaterStore::suggestedChannelName(const QString &talkgroupName) const
{
    const QString repeaterName = hasImportedCodeplug()
        ? selectedImportedGroup().value(QStringLiteral("name")).toString() : selectedName();
    QString firstWord = repeaterName.trimmed().split(QRegularExpression(QStringLiteral("\\s+")),
                                                    Qt::SkipEmptyParts).value(0);
    if (firstWord.size() > 14)
        firstWord = firstWord.left(8);
    const QString name = firstWord.isEmpty() ? talkgroupName.trimmed()
                                             : firstWord + QLatin1Char(' ') + talkgroupName.trimmed();
    return availableChannelName(name);
}

bool RepeaterStore::updateRadioSettings(const QString &name, const QString &idText)
{
    const QString cleanName = name.trimmed();
    const QString cleanId = idText.trimmed();
    bool validId = false;
    const int id = cleanId.toInt(&validId);
    if (cleanName.isEmpty() || cleanName.size() > 16)
        return fail(QStringLiteral("Radio name or call sign must be 1–16 characters."));
    if (!validId || id < 1 || id > 16777215 || cleanId != QString::number(id))
        return fail(QStringLiteral("Radio ID must be a number from 1 to 16777215."));
    if (m_radioName == cleanName && m_radioId == id)
        return true;
    m_radioName = cleanName;
    m_radioId = id;
    emit radioSettingsChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::saveNewRadioSettings(const QString &name, const QString &idText)
{
    const QString cleanName = name.trimmed();
    const QString cleanId = idText.trimmed();
    bool validId = false;
    const int id = cleanId.isEmpty() ? 0 : cleanId.toInt(&validId);

    if (cleanName.size() > 16)
        return fail(QStringLiteral("Radio name or call sign must be 1–16 characters."));
    if (!cleanId.isEmpty()
        && (!validId || id < 1 || id > 16777215 || cleanId != QString::number(id)))
        return fail(QStringLiteral("Radio ID must be a number from 1 to 16777215."));

    if (m_radioName != cleanName || m_radioId != id) {
        m_radioName = cleanName;
        m_radioId = id;
        emit radioSettingsChanged();
        setModified(true);
    }
    return true;
}
void RepeaterStore::copyApplicationVersion() const
{
    QGuiApplication::clipboard()->setText(QCoreApplication::applicationVersion());
}

QString RepeaterStore::projectDisplayName() const
{
    return m_projectPath.isEmpty() ? QStringLiteral("Untitled project") : QFileInfo(m_projectPath).fileName();
}
QString RepeaterStore::sourceCodeplugName() const { return m_sourceCodeplugName; }
bool RepeaterStore::modified() const { return m_modified; }
bool RepeaterStore::hasImportedCodeplug() const { return !m_importedCodeplug.model.isEmpty(); }
bool RepeaterStore::radioTransferAvailable() const
{
    return hasImportedCodeplug()
        && !CodeplugDefinitions::radioDefinitionSource(m_importedCodeplug.definitionFile,
                                                       m_importedCodeplug.definitionVersion).isEmpty();
}
bool RepeaterStore::satelliteSupportAvailable() const
{
    return hasImportedCodeplug() && radioTransferAvailable()
        && !CodeplugDefinitions::satelliteDefinition(m_importedCodeplug.definitionFile,
                                                     m_importedCodeplug.definitionVersion).isEmpty();
}
namespace {
const QString satelliteUrl = QStringLiteral("https://celestrak.org/NORAD/elements/gp.php?GROUP=amateur&FORMAT=tle");
QString satelliteFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/satellites.txt";
}
QStringList satelliteColumns(const QVariant &value)
{
    QStringList result;
    for (const auto &item : value.toList()) result.append(item.toString().trimmed());
    return result;
}
}

QVariantMap RepeaterStore::loadSatelliteSettings()
{
    if (!satelliteSupportAvailable()) {
        fail(tr("This radio definition does not support satellites."));
        return {};
    }
    const QString path = satelliteFilePath();
    if (!QFileInfo::exists(path)) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            fail(tr("Could not create the satellite settings folder."));
            return {};
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            fail(tr("Could not create %1: %2").arg(path, file.errorString()));
            return {};
        }
        const QByteArray defaults = R"SATELLITES(Cat  #,Name    ,    Rx1,    Tx1, CTCSS, ArmCTCSS,    Rx2,    Tx2,    Rx3,    Tx3,APRS Config
43017U,AO-91   ,145.960,435.250,     67,       0,      0,      0,145.960,      0,
61781U,AO-123  ,435.400,145.850,     67,       0,      0,      0,436.210,      0,
40931U,IO-86   ,435.880,145.880,   88.5,       0,      0,      0,437.425,      0,
25544U,ISS     ,437.800,145.990,     67,       0,145.825,145.825,145.800,      0,RS0ISS
40908U,LilacSat,437.200,144.350,      0,       0,      0,      0,437.200,      0,
43678U,PO-101  ,145.900,437.500,  141.3,       0,      0,      0,145.900,      0,
27607U,SO-50   ,436.795,145.850,     67,    74.4,      0,      0,436.795,      0,
59112U,SONATE-2,145.880,      0,      0,       0,145.825,145.825,145.840,      0,DP0SNX
57172U,UmKA-1  ,437.625,      0,      0,       0,      0,      0,437.625,      0,
67291U,RS95S   ,436.950,145.920,     67,       0,      0,      0,436.950,      0,
)SATELLITES";
        if (file.write(defaults) != defaults.size() || !file.flush()) {
            fail(tr("Could not write satellite defaults: %1").arg(file.errorString()));
            return {};
        }
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
        fail(tr("Could not read satellite list: %1").arg(path));
        return {};
    }
    const QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
    QVariantList rows;
    for (int i = 1; i < lines.size(); ++i) {
        if (lines[i].trimmed().isEmpty()) continue;
        const QStringList columns = lines[i].split(',');
        if (columns.size() != 11) {
            fail(tr("Satellite list line %1 must contain 11 columns. The file was not changed.").arg(i + 1));
            return {};
        }
        QVariantList row;
        for (const auto &column : columns) row.append(column.trimmed());
        rows.append(QVariant(row));
    }
    const auto definition = CodeplugDefinitions::satelliteDefinition(m_importedCodeplug.definitionFile,
                                                                    m_importedCodeplug.definitionVersion);
    QSettings settings(QStringLiteral("WaffleCPS"), QStringLiteral("WaffleCPS"));
    return {{"rows", rows}, {"path", path}, {"capacity", definition.value("capacity").toInt()},
            {"url", settings.value("satellites/orbitalUrl", satelliteUrl).toString()}};
}

bool RepeaterStore::saveSatelliteSettings(const QVariantList &rows, const QString &url)
{
    if (!satelliteSupportAvailable()) return fail(tr("This radio definition does not support satellites."));
    const auto definition = CodeplugDefinitions::satelliteDefinition(m_importedCodeplug.definitionFile,
                                                                    m_importedCodeplug.definitionVersion);
    if (rows.size() > definition.value("capacity").toInt())
        return fail(tr("This radio supports at most %1 satellites.").arg(definition.value("capacity").toInt()));
    const QUrl parsedUrl(url.trimmed());
    if (!parsedUrl.isValid() || parsedUrl.host().isEmpty()
        || (parsedUrl.scheme() != "https" && parsedUrl.scheme() != "http"))
        return fail(tr("Enter an HTTP or HTTPS URL for Keplerian data."));
    QString text = "Cat  #,Name,Rx1,Tx1,CTCSS,ArmCTCSS,Rx2,Tx2,Rx3,Tx3,APRS Config\n";
    QSet<QString> catalogIds;
    for (int i = 0; i < rows.size(); ++i) {
        auto columns = satelliteColumns(rows[i]);
        if (columns.size() != 11) return fail(tr("Satellite %1 has invalid fields.").arg(i + 1));
        if (!QRegularExpression("^[0-9]{1,5}U?$").match(columns[0]).hasMatch()
            || columns[0].remove('U').toInt() <= 0)
            return fail(tr("Satellite %1 needs a catalog ID from 1 to 99999.").arg(i + 1));
        if (catalogIds.contains(columns[0])) return fail(tr("Duplicate satellite catalog ID: %1").arg(columns[0]));
        catalogIds.insert(columns[0]);
        columns[0] += 'U';
        if (columns[1].isEmpty() || columns[1].size() > 8)
            return fail(tr("Satellite %1 needs a name of 1 to 8 characters.").arg(i + 1));
        for (const auto &column : columns)
            for (const QChar ch : column)
                if (ch.unicode() < 32 || ch.unicode() > 126 || ch == ',' || ch == '"')
                    return fail(tr("Satellite fields must use plain text without commas or quotation marks."));
        for (int field = 2; field <= 9; ++field) {
            bool ok = false;
            const double value = columns[field].toDouble(&ok);
            const bool tone = field == 4 || field == 5;
            if (!ok || !std::isfinite(value) || value < 0 || value > (tone ? 300.0 : 4294.967295))
                return fail(tr("Satellite %1 has an invalid frequency or tone. Use 0 for unused fields.").arg(i + 1));
        }
        text += columns.join(',') + '\n';
    }
    QSaveFile file(satelliteFilePath());
    if (!file.open(QIODevice::WriteOnly)) return fail(tr("Could not open satellite list: %1").arg(file.errorString()));
    const QByteArray bytes = text.toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit())
        return fail(tr("Could not save satellite list: %1").arg(file.errorString()));
    QSettings settings(QStringLiteral("WaffleCPS"), QStringLiteral("WaffleCPS"));
    settings.setValue("satellites/orbitalUrl", url.trimmed());
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return fail(tr("The satellite list was saved, but the Keplerian URL could not be saved."));
    return true;
}

bool RepeaterStore::updateSatelliteRadio(const QVariantList &rows, const QString &url)
{
    if (m_satelliteUpdateBusy) return false;
    if (!satelliteSupportAvailable()) return fail(tr("This radio definition does not support satellites."));
    const QUrl address(url.trimmed());
    if (!address.isValid() || address.host().isEmpty()
        || (address.scheme() != "https" && address.scheme() != "http"))
        return fail(tr("Enter an HTTP or HTTPS URL for Keplerian data."));
    QSet<int> wanted;
    for (const auto &row : rows) {
        const QStringList columns = satelliteColumns(row);
        QString id = columns.value(0);
        if (id.endsWith('U')) id.chop(1);
        if (columns.size() != 11 || !QRegularExpression("^[0-9]{1,5}$").match(id).hasMatch()
            || id.toInt() <= 0 || wanted.contains(id.toInt()))
            return fail(tr("Each satellite must have a unique catalog ID from 1 to 99999."));
        wanted.insert(id.toInt());
    }
    if (wanted.isEmpty()) return fail(tr("Add a satellite before downloading orbital data."));
    const QString source = radioDefinitionPath();
    auto *network = new QNetworkAccessManager(this);
    QNetworkRequest request(address);
    request.setRawHeader("User-Agent", "WaffleCPS/" + QCoreApplication::applicationVersion().toUtf8());
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto *reply = network->get(request);
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    deadline->setInterval(30000);
    connect(deadline, &QTimer::timeout, reply, [reply] {
        reply->setProperty("satelliteDownloadError", QStringLiteral("The orbital-data download timed out."));
        reply->abort();
    });
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64 total) {
        if (received > 4 * 1024 * 1024 || total > 4 * 1024 * 1024) {
            reply->setProperty("satelliteDownloadError", QStringLiteral("The orbital-data download exceeds 4 MiB."));
            reply->abort();
        }
    });
    m_satelliteUpdateBusy = true;
    m_satelliteStatus = tr("Downloading orbital data…");
    m_satelliteProgress = 0;
    emit satelliteProgressChanged();
    m_satelliteOrbitData.clear();
    emit satelliteUpdateBusyChanged();
    connect(reply, &QNetworkReply::finished, this, [this, reply, network, deadline, wanted, source, rows] {
        deadline->stop();
        QString error = reply->property("satelliteDownloadError").toString();
        if (error.isEmpty() && reply->error() != QNetworkReply::NoError)
            error = tr("Orbital-data download failed: %1").arg(reply->errorString());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (error.isEmpty() && (status < 200 || status >= 300))
            error = tr("Orbital-data server returned HTTP %1.").arg(status);
        const QByteArray bytes = reply->readAll();
        if (error.isEmpty() && bytes.size() > 4 * 1024 * 1024)
            error = tr("The orbital-data download exceeds 4 MiB.");
        if (error.isEmpty() && source != radioDefinitionPath())
            error = tr("The selected radio changed during the orbital-data download. Please try again.");
        SatelliteOrbits::Records records;
        if (error.isEmpty()) SatelliteOrbits::parse(bytes, &records, &error);
        if (error.isEmpty()) {
            for (int id : wanted) {
                if (!records.contains(id)) continue;
                const auto pair = records.value(id);
                m_satelliteOrbitData.append(QVariantMap{{"catalogId", id}, {"line1", pair.first}, {"line2", pair.second}});
            }
            try {
                startSatelliteTransfer(source, rows);
            } catch (const std::exception &exception) {
                error = QString::fromUtf8(exception.what());
            }
        }
        if (!error.isEmpty()) {
            m_satelliteUpdateBusy = false;
            m_satelliteOrbitData.clear();
            m_satelliteStatus = tr("Satellite update failed.");
            emit satelliteUpdateBusyChanged();
            emit satelliteProgressChanged();
            emit satelliteUpdateFailed();
            fail(error);
        }
        network->deleteLater();
    });
    deadline->start();
    return true;
}

void RepeaterStore::startSatelliteTransfer(const QString &source, const QVariantList &rows)
{
    const QJsonObject satelliteDefinition = CodeplugDefinitions::satelliteDefinition(
        m_importedCodeplug.definitionFile, m_importedCodeplug.definitionVersion);
    QVariantList writtenIds;
    const QByteArray payload = SatelliteData::encode(satelliteDefinition, rows, m_satelliteOrbitData, &writtenIds);
    m_satelliteOrbitData.clear();
    RadioDefinition radio;
    CodeplugDefinition codeplug;
    QString error;
    if (!RadioDefinition::loadProfile(source, &radio, &codeplug, nullptr, &error))
        throw std::runtime_error(error.toStdString());
    m_satelliteStatus = tr("Connecting to radio…");
    emit satelliteProgressChanged();
    auto result = std::make_shared<QString>();
    m_satelliteThread = QThread::create([this, radio, satelliteDefinition, payload, result] {
        try {
            const auto progress = [this](const std::string &stage, uint32_t done, uint32_t total) {
                QMetaObject::invokeMethod(this, [this, stage, done, total] {
                    m_satelliteStatus = QString::fromStdString(stage);
                    m_satelliteProgress = total ? double(done) / total : 0;
                    emit satelliteProgressChanged();
                }, Qt::QueuedConnection);
            };
            SerialExecutor::writeSatellites(radio, satelliteDefinition, payload, progress);
        } catch (const std::exception &exception) {
            *result = QString::fromUtf8(exception.what());
        }
    });
    connect(m_satelliteThread, &QThread::finished, this, [this, result, writtenIds, source] {
        auto *finished = m_satelliteThread;
        m_satelliteThread = nullptr;
        finished->deleteLater();
        m_satelliteUpdateBusy = false;
        emit satelliteUpdateBusyChanged();
        if (result->isEmpty()) {
            m_satelliteStatus = tr("Satellite update complete. The radio clock was set to UTC.");
            m_satelliteProgress = 1;
            emit satelliteUpdateCompleted(writtenIds);
        } else {
            m_satelliteStatus = tr("Satellite update failed.");
            std::fprintf(stderr, "[satellites] Update failed using %s: %s\n", qUtf8Printable(source), qUtf8Printable(*result));
            std::fflush(stderr);
            emit satelliteUpdateFailed();
            fail(tr("Satellite update failed: %1").arg(*result));
        }
        emit satelliteProgressChanged();
    });
    m_satelliteThread->start();
}

QString RepeaterStore::radioDefinitionPath() const
{
    return hasImportedCodeplug()
        ? CodeplugDefinitions::radioDefinitionSource(m_importedCodeplug.definitionFile,
                                                      m_importedCodeplug.definitionVersion) : QString();
}
QString RepeaterStore::radioDefinitionPathFor(const QString &definitionFile) const
{
    return CodeplugDefinitions::radioDefinitionSource(definitionFile);
}
QString RepeaterStore::radioTransferTestingStatus(const QString &source, const QString &operation) const
{
    if (operation != QStringLiteral("read") && operation != QStringLiteral("write"))
        return QStringLiteral("unknown");
    RadioProfileData profile;
    QString error;
    if (!CodeplugDefinitions::loadRadioProfile(source, &profile, &error))
        return QStringLiteral("unknown");
    const QString status = QJsonDocument::fromJson(profile.json).object()
        .value(QStringLiteral("hardware_testing")).toObject().value(operation).toString();
    return status == QStringLiteral("tested") || status == QStringLiteral("untested")
        ? status : QStringLiteral("unknown");
}

QVariantList RepeaterStore::newProjectOptions() const
{
    return CodeplugDefinitions::newProjectOptions();
}
QVariantList RepeaterStore::radioReadOptions() const
{
    return CodeplugDefinitions::radioReadOptions();
}
QString RepeaterStore::importedCodeplugSummary() const
{
    if (!hasImportedCodeplug())
        return {};
    const QString catalogModel = CodeplugDefinitions::modelName(m_importedCodeplug.definitionFile);
    const QString model = catalogModel.isEmpty() ? m_importedCodeplug.model : catalogModel;
    return QStringLiteral("%1 · %2 · %3 channels · %4 zones")
        .arg(model, m_importedCodeplug.cpsVersion)
        .arg(m_importedCodeplug.channels.size())
        .arg(m_importedCodeplug.zones.size());
}

QStringList RepeaterStore::importedRepeaterNames() const
{
    QStringList names;
    for (const auto &group : m_importedGroups)
        if (group.repeater)
            names.append(group.name);
    return names;
}

QStringList RepeaterStore::importedZoneNames() const
{
    QStringList names;
    for (const auto &group : m_importedGroups)
        if (!group.repeater)
            names.append(group.name);
    return names;
}

int RepeaterStore::selectedImportedIndex() const { return m_selectedImportedIndex; }

QVariantList RepeaterStore::ctcssChoices() const
{
    QVariantList result;
    for (int tone : m_importedCodeplug.ctcssTones)
        result.append(QVariantMap{{"value", tone}, {"label", tone == 0 ? tr("None")
            : QString::number(tone / 10.0, 'f', 1) + QStringLiteral(" Hz")}});
    return result;
}

QVector<int> RepeaterStore::zoneMembers(const ImportedZone &zone) const
{
    auto members = zone.channelIndexes;
    if (separateDisplayLists())
        for (int index : zone.bottomChannelIndexes)
            if (!members.contains(index)) members.append(index);
    return members;
}

bool RepeaterStore::zoneHasRoom(const ImportedZone &zone, int displays) const
{
    if (displays < 1 || displays > 3) return false;
    const int capacity = m_importedCodeplug.zoneMemberCapacity;
    return (!separateDisplayLists() || (displays & 1) ? zone.channelIndexes.size() < capacity : true)
        && (!separateDisplayLists() || !(displays & 2) || zone.bottomChannelIndexes.size() < capacity);
}

void RepeaterStore::applyZoneMembers(ImportedZone &zone, const QVector<int> &members)
{
    if (!separateDisplayLists()) { zone.channelIndexes = members; return; }
    const auto previous = zoneMembers(zone);
    // Channel edits may make a private copy for this zone. Preserve its display assignment.
    QMap<int, int> replacements;
    if (members.size() == previous.size())
        for (int i = 0; i < members.size(); ++i)
            if (members[i] != previous[i]) replacements.insert(previous[i], members[i]);
    for (auto *list : {&zone.channelIndexes, &zone.bottomChannelIndexes}) {
        QVector<int> updated;
        for (int index : *list) {
            index = replacements.value(index, index);
            if (members.contains(index)) updated.append(index);
        }
        for (int index : members)
            if (!previous.contains(index) && !replacements.values().contains(index)) updated.append(index);
        *list = updated;
    }
}

void RepeaterStore::shiftZoneChannelIndexes(int removedIndex)
{
    for (auto &scan : m_importedCodeplug.scanLists) {
        scan.channelIndexes.removeAll(removedIndex);
        for (int &index : scan.channelIndexes) if (index > removedIndex) --index;
    }
    for (auto &zone : m_importedCodeplug.zones)
        for (auto *list : {&zone.channelIndexes, &zone.bottomChannelIndexes})
            for (int &index : *list)
                if (index > removedIndex) --index;
}

bool RepeaterStore::setSelectedZoneDisplays(const QVariantList &assignments)
{
    const int index = selectedZoneIndex();
    if (!separateDisplayLists() || index < 0) return false;
    auto &zone = m_importedCodeplug.zones[index];
    const auto members = zoneMembers(zone);
    if (assignments.size() != members.size()) return false;
    QVector<int> top, bottom;
    for (int i = 0; i < assignments.size(); ++i) {
        bool ok = false;
        const int mask = assignments[i].toInt(&ok);
        if (!ok || assignments[i].toDouble() != mask || mask < 1 || mask > 3) return false;
        if (mask & 1) top.append(members[i]);
        if (mask & 2) bottom.append(members[i]);
    }
    if (top.size() > m_importedCodeplug.zoneMemberCapacity || bottom.size() > m_importedCodeplug.zoneMemberCapacity)
        return fail(tr("A Top or Bottom channel list exceeds the definition's membership limit."));
    // Keep the independent order of retained entries; append newly assigned entries.
    auto retainOrder = [](const QVector<int> &old, QVector<int> desired) {
        QVector<int> result;
        for (int member : old) {
            const int at = desired.indexOf(member);
            if (at >= 0) { result.append(member); desired.removeAt(at); }
        }
        result += desired;
        return result;
    };
    zone.channelIndexes = retainOrder(zone.channelIndexes, top);
    zone.bottomChannelIndexes = retainOrder(zone.bottomChannelIndexes, bottom);
    zoneEdited();
    return true;
}

bool RepeaterStore::setSelectedChannelDisplays(int zonePosition, int displays)
{
    const int index = selectedZoneIndex();
    if (index < 0) return false;
    const auto &zone = m_importedCodeplug.zones[index];
    const auto members = zoneMembers(zone);
    if (zonePosition < 1 || zonePosition > members.size()) return false;
    QVariantList assignments;
    for (int member : members)
        assignments.append((zone.channelIndexes.contains(member) ? 1 : 0)
                           | (zone.bottomChannelIndexes.contains(member) ? 2 : 0));
    assignments[zonePosition - 1] = displays;
    return setSelectedZoneDisplays(assignments);
}

QVariantMap RepeaterStore::selectedImportedGroup() const
{
    if (m_selectedImportedIndex < 0 || m_selectedImportedIndex >= m_importedGroups.size())
        return {};
    const auto &group = m_importedGroups.at(m_selectedImportedIndex);
    QVariantList zones;
    QVariantList additionalSimplex;
    QVariantList slot1Channels;
    QVariantList slot2Channels;
    QVariantList slot1Talkgroups;
    QVariantList slot2Talkgroups;
    int zoneChannelCount = 0;
    QMap<QString, int> powerCounts;
    for (int zoneInGroup = 0; zoneInGroup < group.zoneIndexes.size(); ++zoneInGroup) {
        const int zoneIndex = group.zoneIndexes.at(zoneInGroup);
        const auto &zone = m_importedCodeplug.zones.at(zoneIndex);
        const auto members = zoneMembers(zone);
        zoneChannelCount += members.size();
        if (zoneMembers(zone).isEmpty() && zone.repeaterTemplate.has_value()) {
            powerCounts[zone.repeaterTemplate->powerValue()]++;
        }
        QVariantList channels;
        QVariantList repeaterChannels;
        for (int zonePosition = 0; zonePosition < members.size(); ++zonePosition) {
            const int channelIndex = members.at(zonePosition);
            const auto &channel = m_importedCodeplug.channels.at(channelIndex);
            QVariantMap item{
                {QStringLiteral("name"), channel.name},
                {QStringLiteral("number"), channelIndex + 1},
                {QStringLiteral("zonePosition"), zonePosition + 1},
                {QStringLiteral("displays"), separateDisplayLists()
                     ? (zone.channelIndexes.contains(channelIndex) ? 1 : 0) | (zone.bottomChannelIndexes.contains(channelIndex) ? 2 : 0) : 3},
                {QStringLiteral("contactIndex"), channel.contactIndex},
                {QStringLiteral("receiveCtcss"), channel.receiveCtcss.value_or(-1)},
                {QStringLiteral("transmitCtcss"), channel.transmitCtcss.value_or(-1)},
                {QStringLiteral("receiveFrequency"), formatFrequency(channel.frequencyBHz)},
                {QStringLiteral("transmitFrequency"), formatFrequency(channel.frequencyAHz)},
                {QStringLiteral("mode"), channel.digital ? QStringLiteral("Digital") : QStringLiteral("Analog / other")},
                {QStringLiteral("colorCode"), channel.digital ? QVariant(channel.colorCode) : QVariant()},
                {QStringLiteral("timeSlot"), channel.digital ? QVariant(channel.timeSlot) : QVariant()},
                {QStringLiteral("power"), channel.powerValue().isEmpty() ? QStringLiteral("Unknown")
                     : channel.powerValue().left(1).toUpper() + channel.powerValue().mid(1)}};
            if (channel.digital && channel.contactIndex > 0
                && channel.contactIndex <= m_importedCodeplug.contacts.size()) {
                const auto &contact = m_importedCodeplug.contacts.at(channel.contactIndex - 1);
                item.insert(QStringLiteral("contactName"), contact.name);
                item.insert(QStringLiteral("contactId"), contact.id);
                item.insert(QStringLiteral("contactGroupCall"), contact.groupCall);
            }
            channels.append(item);
            if (group.repeater && channel.frequencyAHz == channel.frequencyBHz) {
                item.insert(QStringLiteral("zoneName"), zone.name);
                item.insert(QStringLiteral("zoneInGroup"), zoneInGroup);
                additionalSimplex.append(item);
            } else {
                repeaterChannels.append(item);
                if (group.repeater) {
                    powerCounts[channel.powerValue()]++;
                }
                if (group.repeater && channel.timeSlot == 1)
                    slot1Channels.append(item);
                else if (group.repeater && channel.timeSlot == 2)
                    slot2Channels.append(item);
            }
        }
        zones.append(QVariantMap{{QStringLiteral("name"), zone.name},
                                 {QStringLiteral("channels"), channels},
                                 {QStringLiteral("repeaterChannels"), repeaterChannels},
                                 {QStringLiteral("topNumbers"), [&]() { QVariantList v; for (int i : zone.channelIndexes) v.append(i + 1); return v; }()},
                                 {QStringLiteral("bottomNumbers"), [&]() { QVariantList v; for (int i : zone.bottomChannelIndexes) v.append(i + 1); return v; }()},
                                 {QStringLiteral("canAddChannel"), zoneHasRoom(zone, 1) || zoneHasRoom(zone, 2)}});
    }
    auto makeTalkgroups = [this](const QVariantList &channelItems) {
        QVariantList groups;
        QMap<int, int> indexes;
        for (const QVariant &value : channelItems) {
            const QVariantMap channel = value.toMap();
            const int contactIndex = m_importedCodeplug.channels.at(channel.value(QStringLiteral("number")).toInt() - 1).contactIndex;
            const bool valid = contactIndex > 0 && contactIndex <= m_importedCodeplug.contacts.size();
            const int key = valid ? contactIndex : 0;
            int groupIndex = indexes.value(key, -1);
            if (groupIndex < 0) {
                groupIndex = groups.size();
                indexes.insert(key, groupIndex);
                groups.append(QVariantMap{{QStringLiteral("name"), valid ? m_importedCodeplug.contacts.at(contactIndex - 1).name
                                                                : QStringLiteral("No contact assigned")},
                                          {QStringLiteral("contactIndex"), key},
                                          {QStringLiteral("id"), valid ? m_importedCodeplug.contacts.at(contactIndex - 1).id : 0},
                                          {QStringLiteral("groupCall"), valid && m_importedCodeplug.contacts.at(contactIndex - 1).groupCall},
                                          {QStringLiteral("channels"), QVariantList{}}});
            }
            QVariantMap groupItem = groups.at(groupIndex).toMap();
            QVariantList children = groupItem.value(QStringLiteral("channels")).toList();
            children.append(channel);
            groupItem.insert(QStringLiteral("channels"), children);
            groups[groupIndex] = groupItem;
        }
        return groups;
    };
    slot1Talkgroups = makeTalkgroups(slot1Channels);
    slot2Talkgroups = makeTalkgroups(slot2Channels);
    return {{QStringLiteral("name"), group.name},
            {QStringLiteral("classification"), m_zoneClassificationPending
                 ? tr("Classification will be updated when you leave this zone") : group.reason},
            {QStringLiteral("isRepeater"), group.repeater},
            {QStringLiteral("receiveFrequency"), group.repeater ? formatFrequency(group.frequencyBHz) : QString()},
            {QStringLiteral("transmitFrequency"), group.repeater ? formatFrequency(group.frequencyAHz) : QString()},
            {QStringLiteral("colorCode"), group.colorCode},
            {QStringLiteral("power"), powerCounts.isEmpty() || powerCounts.contains(QString()) ? QStringLiteral("Unknown")
                 : powerCounts.size() > 1 ? QStringLiteral("Mixed")
                 : powerCounts.firstKey().left(1).toUpper() + powerCounts.firstKey().mid(1)},
            {QStringLiteral("newChannelPower"), [&]() {
                QString best = "high"; int count = -1;
                for (auto it = powerCounts.begin(); it != powerCounts.end(); ++it)
                    if (!it.key().isEmpty() && it.value() > count) { best = it.key(); count = it.value(); }
                return best.left(1).toUpper() + best.mid(1);
            }()},
            {QStringLiteral("simplexChannels"), additionalSimplex},
            {QStringLiteral("slot1Channels"), slot1Channels},
            {QStringLiteral("slot2Channels"), slot2Channels},
            {QStringLiteral("slot1Talkgroups"), slot1Talkgroups},
            {QStringLiteral("slot2Talkgroups"), slot2Talkgroups},
            {QStringLiteral("zoneChannelCount"), zoneChannelCount},
            {QStringLiteral("canAddBoth"), selectedZoneIndex() >= 0 && zoneHasRoom(m_importedCodeplug.zones[selectedZoneIndex()])},
            {QStringLiteral("zoneMemberCapacity"), m_importedCodeplug.zoneMemberCapacity},
            {QStringLiteral("zones"), zones}};
}

int RepeaterStore::selectedZoneIndex() const
{
    if (m_selectedImportedIndex < 0 || m_selectedImportedIndex >= m_importedGroups.size()
        || m_importedGroups[m_selectedImportedIndex].zoneIndexes.size() != 1) return -1;
    return m_importedGroups[m_selectedImportedIndex].zoneIndexes.constFirst();
}

void RepeaterStore::selectZoneIndex(int zoneIndex)
{
    for (int i = 0; i < m_importedGroups.size(); ++i)
        if (m_importedGroups[i].zoneIndexes.contains(zoneIndex)) {
            m_selectedImportedIndex = i;
            emit importedSelectionChanged();
            return;
        }
}

void RepeaterStore::leaveZoneEditor()
{
    const bool wasAdvanced = editingAsZone();
    m_advancedZoneIndex = -1;
    if (!m_zoneClassificationPending) {
        if (wasAdvanced) emit importedSelectionChanged();
        return;
    }
    const int zoneIndex = selectedZoneIndex();
    rebuildImportedGroups();
    selectZoneIndex(zoneIndex);
}

void RepeaterStore::zoneEdited()
{
    m_zoneClassificationPending = true;
    setModified(true);
    emit importedSelectionChanged();
}

bool RepeaterStore::scanListsAvailable() const
{
    return hasImportedCodeplug() && m_importedCodeplug.scanListsSupported;
}

QStringList RepeaterStore::scanListNames() const
{
    QStringList names;
    for (const auto &list : m_importedCodeplug.scanLists) names.append(list.name);
    return names;
}

QVariantMap RepeaterStore::scanListEditorData(int index) const
{
    QVariantMap result{{"capacity", m_importedCodeplug.scanListCapacity},
        {"memberCapacity", m_importedCodeplug.scanMemberCapacity}, {"valid", false}};
    if (!scanListsAvailable()) return result;
    QVariantList channels, repeaters;
    for (int i = 0; i < m_importedCodeplug.channels.size(); ++i) {
        const auto &channel = m_importedCodeplug.channels[i];
        channels.append(QVariantMap{{"number", i + 1}, {"name", channel.name},
            {"receiveFrequency", formatFrequency(channel.frequencyBHz)},
            {"scanListIndex", channel.scanListIndex - 1},
            {"scanListName", channel.scanListIndex > 0 && channel.scanListIndex <= m_importedCodeplug.scanLists.size()
                ? m_importedCodeplug.scanLists[channel.scanListIndex - 1].name : QString()}});
    }
    for (const auto &group : m_importedGroups) {
        if (!group.repeater) continue;
        QVariantList numbers;
        QSet<int> seen;
        for (int zone : group.zoneIndexes)
            for (int channel : zoneMembers(m_importedCodeplug.zones[zone]))
                if (!seen.contains(channel)) { seen.insert(channel); numbers.append(channel + 1); }
        repeaters.append(QVariantMap{{"name", group.name}, {"numbers", numbers}});
    }
    result.insert("channels", channels);
    result.insert("repeaters", repeaters);
    if (index < 0 || index >= m_importedCodeplug.scanLists.size()) return result;
    const auto &list = m_importedCodeplug.scanLists[index];
    QVariantList members;
    int position = 0;
    for (int channel : list.channelIndexes) {
        members.append(QVariantMap{{"position", position++}, {"number", channel + 1},
            {"name", channel < 0 ? tr("Current channel") : m_importedCodeplug.channels[channel].name}});
    }
    result.insert("valid", true);
    result.insert("name", list.name);
    result.insert("members", members);
    return result;
}

bool RepeaterStore::saveScanListName(int index, const QString &name)
{
    if (!scanListsAvailable() || index < -1 || index >= m_importedCodeplug.scanLists.size()) return false;
    const QString clean = name.trimmed();
    QJsonObject metadata;
    QString error;
    if (!CodeplugDefinitions::conversionMetadata(m_sourceCodeplug, m_importedCodeplug.definitionFile, &metadata, &error))
        return fail(error);
    const auto rules = metadata.value("limits").toObject().value("names").toObject();
    auto rule = rules.value("scan_name").toObject();
    for (int depth = 0; rule.contains("same_as") && depth < 16; ++depth)
        rule = rules.value(rule.value("same_as").toString()).toObject();
    if (rule.isEmpty() || clean.size() < rule.value("minimum_characters").toInt()
        || clean.size() > rule.value("maximum_characters").toInt())
        return fail(tr("The scan-list name does not fit the selected radio's limits."));
    for (QChar ch : clean)
        if (!rule.value("allowed_characters").toString().contains(ch))
            return fail(tr("The scan-list name contains an unsupported character."));
    for (int i = 0; i < m_importedCodeplug.scanLists.size(); ++i)
        if (i != index && m_importedCodeplug.scanLists[i].name.compare(clean, Qt::CaseInsensitive) == 0)
            return fail(tr("A scan list already has this name."));
    if (index == -1) {
        if (m_importedCodeplug.scanLists.size() >= m_importedCodeplug.scanListCapacity)
            return fail(tr("The selected radio's scan-list limit has been reached."));
        m_importedCodeplug.scanLists.append({clean, {}, -1});
    } else m_importedCodeplug.scanLists[index].name = clean;
    setModified(true);
    emit importedCodeplugChanged();
    return true;
}

bool RepeaterStore::deleteScanList(int index)
{
    if (!scanListsAvailable() || index < 0 || index >= m_importedCodeplug.scanLists.size()) return false;
    m_importedCodeplug.scanLists.removeAt(index);
    for (auto &channel : m_importedCodeplug.channels) {
        if (channel.scanListIndex == index + 1) channel.scanListIndex = 0;
        else if (channel.scanListIndex > index + 1) --channel.scanListIndex;
    }
    setModified(true);
    emit importedCodeplugChanged();
    return true;
}

bool RepeaterStore::addScanListChannels(int index, const QVariantList &channelNumbers)
{
    if (!scanListsAvailable() || index < 0 || index >= m_importedCodeplug.scanLists.size()) return false;
    auto members = m_importedCodeplug.scanLists[index].channelIndexes;
    for (const auto &value : channelNumbers) {
        bool ok;
        const double number = value.toDouble(&ok);
        if (!ok || !std::isfinite(number) || number != std::floor(number)
            || number < 0 || number > m_importedCodeplug.channels.size())
            return fail(tr("Unknown channel in scan list."));
        const int channel = int(number) - 1;
        if (!members.contains(channel)) members.append(channel);
    }
    if (members.size() > m_importedCodeplug.scanMemberCapacity)
        return fail(tr("These channels will not fit. This radio allows %1 entries per scan list.")
                    .arg(m_importedCodeplug.scanMemberCapacity));
    m_importedCodeplug.scanLists[index].channelIndexes = members;
    setModified(true);
    emit importedCodeplugChanged();
    return true;
}

bool RepeaterStore::removeScanListMember(int index, int position)
{
    if (!scanListsAvailable() || index < 0 || index >= m_importedCodeplug.scanLists.size()) return false;
    auto &members = m_importedCodeplug.scanLists[index].channelIndexes;
    if (position < 0 || position >= members.size()) return false;
    members.removeAt(position);
    setModified(true);
    emit importedCodeplugChanged();
    return true;
}

bool RepeaterStore::moveScanListMember(int index, int from, int to)
{
    if (!scanListsAvailable() || index < 0 || index >= m_importedCodeplug.scanLists.size()) return false;
    auto &members = m_importedCodeplug.scanLists[index].channelIndexes;
    if (from < 0 || from >= members.size() || to < 0 || to >= members.size()) return false;
    members.move(from, to);
    setModified(true);
    emit importedCodeplugChanged();
    return true;
}

bool RepeaterStore::assignChannelScanList(int channelNumber, int index, bool replace)
{
    if (!scanListsAvailable() || channelNumber < 1 || channelNumber > m_importedCodeplug.channels.size()
        || index < -1 || index >= m_importedCodeplug.scanLists.size()) return false;
    auto &channel = m_importedCodeplug.channels[channelNumber - 1];
    if (!replace && channel.scanListIndex && index >= 0 && channel.scanListIndex != index + 1)
        return fail(tr("This channel already uses another scan list. Confirm replacement first."));
    channel.scanListIndex = index + 1;
    setModified(true);
    emit importedCodeplugChanged();
    return true;
}

QVariantList RepeaterStore::availableZoneChannels() const
{
    // QML may reread a sequence property for each field while filtering it.
    // Reuse the catalogue until selection, channel data, or contacts change.
    if (m_zoneChoicesCached) return m_zoneChoicesCache;
    QVariantList result;
    const int zoneIndex = selectedZoneIndex();
    if (zoneIndex < 0) return result;
    const auto members = zoneMembers(m_importedCodeplug.zones[zoneIndex]);
    for (int i = 0; i < m_importedCodeplug.channels.size(); ++i) {
        if (members.contains(i)) continue;
        const auto &channel = m_importedCodeplug.channels[i];
        QVariantMap item{{QStringLiteral("name"), channel.name}, {QStringLiteral("number"), i + 1},
            {QStringLiteral("receiveFrequency"), formatFrequency(channel.frequencyBHz)},
            {QStringLiteral("transmitFrequency"), formatFrequency(channel.frequencyAHz)},
            {QStringLiteral("mode"), channel.digital ? QStringLiteral("Digital") : QStringLiteral("Analog / other")},
            {QStringLiteral("colorCode"), channel.colorCode}, {QStringLiteral("timeSlot"), channel.timeSlot}};
        if (channel.contactIndex > 0 && channel.contactIndex <= m_importedCodeplug.contacts.size()) {
            const auto &contact = m_importedCodeplug.contacts[channel.contactIndex - 1];
            item.insert(QStringLiteral("contactName"), contact.name);
            item.insert(QStringLiteral("contactId"), contact.id);
        }
        result.append(item);
    }
    m_zoneChoicesCache = result;
    m_zoneChoicesCached = true;
    return result;
}

bool RepeaterStore::ensureZoneRecords(int reservedZones)
{
    if (hasImportedCodeplug()) return true;
    ImportedCodeplug generated;
    if (!buildManualCodeplug(&generated)) return false;
    if (m_repeaters.empty()) {
        generated.channels.clear();
        generated.zones.clear();
        generated.contacts = m_manualContacts;
    }
    for (const auto &channel : generated.channels)
        if (channel.frequencyAHz < 100000000 || channel.frequencyBHz < 100000000)
            return fail(tr("An existing channel frequency is below 100 MHz."));
    for (const auto &contact : generated.contacts)
        if (contact.name.size() > 16)
            return fail(tr("An existing contact name exceeds 16 characters."));
    for (const auto &zone : generated.zones) {
        if (zone.repeaterTemplate.has_value()
            && (zone.repeaterTemplate->frequencyAHz < 100000000 || zone.repeaterTemplate->frequencyBHz < 100000000))
            return fail(tr("An existing repeater frequency is below 100 MHz."));
        if (zone.name.size() > 16 || zone.channelIndexes.size() > 16)
            return fail(tr("An existing repeater exceeds the 16-character zone name or 16-channel limit."));
    }
    if (generated.channels.size() > 3000 || generated.zones.size() + reservedZones > 250)
        return fail(tr("The default codeplug limit of 3000 channels or 250 zones has been reached."));
    // Generated records use the embedded base, not any archived user attachment.
    for (auto &channel : generated.channels) channel.sourceRecord = -1;
    emit aboutToResetProject();
    m_importedCodeplug = std::move(generated);
    m_generatedCodeplug = true;
    m_repeaters.clear();
    m_selectedIndex = 0;
    rebuildImportedGroups();
    emit repeatersChanged();
    notifyContactsChanged();
    emit radioSettingsChanged();
    setModified(true);
    emit projectChanged();
    return true;
}

bool RepeaterStore::editingAsZone() const
{
    return m_advancedZoneIndex >= 0 && m_advancedZoneIndex == selectedZoneIndex();
}

bool RepeaterStore::editSelectedRepeaterAsZone()
{
    const bool imported = hasImportedCodeplug();
    const int zoneIndex = imported ? selectedZoneIndex() : m_selectedIndex;
    if (zoneIndex < 0 || (imported && !selectedImportedGroup().value(QStringLiteral("isRepeater")).toBool())
        || (!imported && m_repeaters.empty())) return false;
    if (!ensureZoneRecords()) return false;
    m_advancedZoneIndex = zoneIndex;
    selectZoneIndex(zoneIndex);
    return true;
}

QVariantList RepeaterStore::zoneContactChoices() const
{
    // A method returns a value snapshot, avoiding repeated QML property reads
    // while filtering a large contact catalogue.
    QVariantList result;
    result.reserve(m_importedCodeplug.contacts.size());
    for (int i = 0; i < m_importedCodeplug.contacts.size(); ++i) {
        const auto &contact = m_importedCodeplug.contacts[i];
        result.append(QVariantMap{{QStringLiteral("number"), i + 1}, {QStringLiteral("name"), contact.name},
            {QStringLiteral("id"), contact.id}, {QStringLiteral("groupCall"), contact.groupCall}});
    }
    return result;
}

bool RepeaterStore::createCustomZone(const QString &name)
{
    const QString cleanName = name.trimmed();
    if (cleanName.isEmpty() || cleanName.size() > 16) return fail(tr("Enter a zone name of 1–16 characters."));
    for (const auto &zone : m_importedCodeplug.zones)
        if (zone.name.compare(cleanName, Qt::CaseInsensitive) == 0) return fail(tr("A zone already has this name."));
    if (!hasImportedCodeplug())
        for (const auto &repeater : m_repeaters)
            if (repeater.name.compare(cleanName, Qt::CaseInsensitive) == 0) return fail(tr("A zone already has this name."));
    if (!ensureZoneRecords(1)) return false;
    if (m_importedCodeplug.zones.size() >= m_importedCodeplug.zoneCapacity) return fail(tr("The selected codeplug's zone limit has been reached."));
    leaveZoneEditor();
    const int zoneIndex = m_importedCodeplug.zones.size();
    m_importedCodeplug.zones.append({cleanName, {}});
    rebuildImportedGroups();
    selectZoneIndex(zoneIndex);
    setModified(true);
    return true;
}

bool RepeaterStore::renameSelectedZone(const QString &name)
{
    const int zoneIndex = selectedZoneIndex();
    const auto cleanName = name.trimmed();
    if (zoneIndex < 0 || cleanName.isEmpty() || cleanName.size() > 16) return false;
    for (int i = 0; i < m_importedCodeplug.zones.size(); ++i)
        if (i != zoneIndex && m_importedCodeplug.zones[i].name.compare(cleanName, Qt::CaseInsensitive) == 0)
            return fail(tr("A zone already has this name."));
    if (m_importedCodeplug.zones[zoneIndex].name == cleanName) return true;
    m_importedCodeplug.zones[zoneIndex].name = cleanName;
    m_importedGroups[m_selectedImportedIndex].name = cleanName;
    setModified(true);
    emit importedCodeplugChanged();
    emit importedSelectionChanged();
    return true;
}

bool RepeaterStore::addExistingChannelToSelectedZone(int channelNumber, int displays)
{
    const int zoneIndex = selectedZoneIndex();
    if (zoneIndex < 0 || channelNumber < 1 || channelNumber > m_importedCodeplug.channels.size()) return false;
    auto members = zoneMembers(m_importedCodeplug.zones[zoneIndex]);
    if (!zoneHasRoom(m_importedCodeplug.zones[zoneIndex], displays)) return fail(tr("The definition zone membership limit has been reached."));
    if (members.contains(channelNumber - 1)) return false;
    auto &zone = m_importedCodeplug.zones[zoneIndex];
    if (!separateDisplayLists() || (displays & 1)) zone.channelIndexes.append(channelNumber - 1);
    if (separateDisplayLists() && (displays & 2)) zone.bottomChannelIndexes.append(channelNumber - 1);
    zoneEdited();
    return true;
}

bool RepeaterStore::createChannelInSelectedZone(const QString &name, const QString &rx, const QString &tx,
    bool digital, int colorCode, int timeSlot, int contactIndex, int powerChoice, int displays,
    int receiveCtcss, int transmitCtcss)
{
    const int zoneIndex = selectedZoneIndex();
    if (digital && (contactIndex < 1 || contactIndex > m_importedCodeplug.contacts.size()))
        return fail(tr("Select a contact for this digital channel."));
    if (!digital && ((!m_importedCodeplug.ctcssTones.isEmpty()
            && (!m_importedCodeplug.ctcssTones.contains(receiveCtcss) || !m_importedCodeplug.ctcssTones.contains(transmitCtcss)))
        || (m_importedCodeplug.ctcssTones.isEmpty() && (receiveCtcss != 0 || transmitCtcss != 0))))
        return fail(tr("Select CTCSS tones supported by this definition."));
    qint64 rxHz = 0, txHz = 0;
    const auto cleanName = name.trimmed();
    if (powerChoice < 0 || powerChoice >= m_importedCodeplug.powerSettings.size() || zoneIndex < 0 || cleanName.isEmpty() || cleanName.size() > 16
        || !parseFrequency(rx, &rxHz) || !parseFrequency(tx, &txHz) || rxHz < 100000000 || txHz < 100000000
        || (digital && (colorCode < 0 || colorCode > 15 || timeSlot < 1 || timeSlot > 2)))
        return fail(tr("Enter a channel name, valid RX/TX frequencies, and valid digital settings."));
    if (availableChannelName(cleanName) != cleanName) return fail(tr("A channel already has this name."));
    if (!zoneHasRoom(m_importedCodeplug.zones[zoneIndex], displays) || m_importedCodeplug.channels.size() >= m_importedCodeplug.channelCapacity)
        return fail(tr("The definition channel or zone membership limit has been reached."));
    auto &zone = m_importedCodeplug.zones[zoneIndex];
    if (!separateDisplayLists() || (displays & 1)) zone.channelIndexes.append(m_importedCodeplug.channels.size());
    if (separateDisplayLists() && (displays & 2)) zone.bottomChannelIndexes.append(m_importedCodeplug.channels.size());
    m_importedCodeplug.channels.append({cleanName, quint32(txHz), quint32(rxHz), digital,
        digital ? colorCode : 0, digital ? timeSlot : 0, digital ? contactIndex : 0, std::nullopt});
    m_importedCodeplug.channels.last().setPowerValue(m_importedCodeplug.powerSettings.at(powerChoice));
    if (!digital && !m_importedCodeplug.ctcssTones.isEmpty()) {
        m_importedCodeplug.channels.last().receiveCtcss = receiveCtcss;
        m_importedCodeplug.channels.last().transmitCtcss = transmitCtcss;
    }
    zoneEdited();
    return true;
}

bool RepeaterStore::removeSelectedZoneChannel(int zonePosition)
{
    const int zoneIndex = selectedZoneIndex();
    if (zoneIndex < 0) return false;
    auto members = zoneMembers(m_importedCodeplug.zones[zoneIndex]);
    if (zonePosition < 1 || zonePosition > members.size()) return false;
    // Retain the global channel for other zones and the channel picker.
    members.removeAt(zonePosition - 1);
    applyZoneMembers(m_importedCodeplug.zones[zoneIndex], members);
    zoneEdited();
    return true;
}

bool RepeaterStore::updateSelectedZoneChannelFrequencies(int zonePosition, const QString &rx, const QString &tx)
{
    const int zoneIndex = selectedZoneIndex();
    if (zoneIndex < 0) return false;
    const auto members = zoneMembers(m_importedCodeplug.zones[zoneIndex]);
    if (zonePosition < 1 || zonePosition > members.size()) return false;
    const auto channel = m_importedCodeplug.channels[members[zonePosition - 1]];
    return updateSelectedZoneChannel(zonePosition, channel.name, rx, tx, channel.digital,
        channel.colorCode, channel.timeSlot, channel.contactIndex, -1);
}

bool RepeaterStore::updateSelectedZoneChannel(int zonePosition, const QString &name,
    const QString &rx, const QString &tx, bool digital, int colorCode, int timeSlot,
    int contactIndex, int powerChoice, int displays, int receiveCtcss, int transmitCtcss)
{
    const int zoneIndex = selectedZoneIndex();
    if (digital && (contactIndex < 1 || contactIndex > m_importedCodeplug.contacts.size()))
        return fail(tr("Select a contact for this digital channel."));
    if (!digital && (receiveCtcss < -1 || transmitCtcss < -1
        || (receiveCtcss >= 0 && !m_importedCodeplug.ctcssTones.contains(receiveCtcss))
        || (transmitCtcss >= 0 && !m_importedCodeplug.ctcssTones.contains(transmitCtcss))))
        return fail(tr("Select CTCSS tones supported by this definition."));
    qint64 rxHz = 0, txHz = 0;
    const QString cleanName = name.trimmed();
    if (zoneIndex < 0 || cleanName.isEmpty() || cleanName.size() > 16
        || !parseFrequency(rx, &rxHz) || !parseFrequency(tx, &txHz)
        || rxHz < 100000000 || txHz < 100000000 || powerChoice < -1 || powerChoice >= m_importedCodeplug.powerSettings.size()
        || (digital && (colorCode < 0 || colorCode > 15 || timeSlot < 1 || timeSlot > 2)))
        return fail(tr("Enter a channel name, valid RX/TX frequencies, and valid digital settings."));
    auto members = zoneMembers(m_importedCodeplug.zones[zoneIndex]);
    if (zonePosition < 1 || zonePosition > members.size()) return false;
    const int channelIndex = members[zonePosition - 1];
    const auto &zone = m_importedCodeplug.zones[zoneIndex];
    if (displays != -1) {
        if (!separateDisplayLists() || displays < 1 || displays > 3) return false;
        if (((displays & 1) && !zone.channelIndexes.contains(channelIndex)
             && zone.channelIndexes.size() >= m_importedCodeplug.zoneMemberCapacity)
            || ((displays & 2) && !zone.bottomChannelIndexes.contains(channelIndex)
                && zone.bottomChannelIndexes.size() >= m_importedCodeplug.zoneMemberCapacity))
            return fail(tr("The selected display's channel list is full."));
    }
    const auto original = m_importedCodeplug.channels[channelIndex];
    for (int i = 0; i < m_importedCodeplug.channels.size(); ++i)
        if (i != channelIndex && cleanName.compare(original.name, Qt::CaseInsensitive) != 0
            && m_importedCodeplug.channels[i].name.compare(cleanName, Qt::CaseInsensitive) == 0)
            return fail(tr("A channel already has this name."));
    auto corrected = original;
    corrected.name = cleanName;
    corrected.frequencyBHz = quint32(rxHz);
    corrected.frequencyAHz = quint32(txHz);
    corrected.digital = digital;
    corrected.colorCode = digital ? colorCode : 0;
    corrected.timeSlot = digital ? timeSlot : 0;
    corrected.contactIndex = digital ? contactIndex : 0;
    if (digital) {
        corrected.receiveCtcss.reset();
        corrected.transmitCtcss.reset();
    } else {
        if (original.digital && !m_importedCodeplug.ctcssTones.isEmpty()) {
            corrected.receiveCtcss = 0;
            corrected.transmitCtcss = 0;
        }
        if (receiveCtcss >= 0) corrected.receiveCtcss = receiveCtcss;
        if (transmitCtcss >= 0) corrected.transmitCtcss = transmitCtcss;
    }
    if (powerChoice >= 0) corrected.setPowerValue(m_importedCodeplug.powerSettings.at(powerChoice));
    if (corrected.name == original.name && corrected.frequencyBHz == original.frequencyBHz
        && corrected.frequencyAHz == original.frequencyAHz && corrected.digital == original.digital
        && corrected.colorCode == original.colorCode && corrected.timeSlot == original.timeSlot
        && corrected.contactIndex == original.contactIndex && corrected.powerValue() == original.powerValue()
        && corrected.receiveCtcss == original.receiveCtcss && corrected.transmitCtcss == original.transmitCtcss)
        return displays == -1 || setSelectedChannelDisplays(zonePosition, displays);
    if (digital != original.digital) {
        corrected.sourceRecord = -1;
        corrected.useDefaultTemplate = true;
    }
    bool shared = members.count(channelIndex) > 1;
    for (int i = 0; i < m_importedCodeplug.zones.size(); ++i)
        if (i != zoneIndex && zoneMembers(m_importedCodeplug.zones[i]).contains(channelIndex)) shared = true;
    if (shared) {
        if (m_importedCodeplug.channels.size() >= m_importedCodeplug.channelCapacity) return fail(tr("No channel slot is available for this zone's copy."));
        if (cleanName.compare(original.name, Qt::CaseInsensitive) == 0)
            corrected.name = availableChannelName(cleanName);
        members[zonePosition - 1] = m_importedCodeplug.channels.size();
        m_importedCodeplug.channels.append(corrected);
    } else {
        m_importedCodeplug.channels[channelIndex] = corrected;
    }
    applyZoneMembers(m_importedCodeplug.zones[zoneIndex], members);
    if (displays != -1) return setSelectedChannelDisplays(zonePosition, displays);
    zoneEdited();
    return true;
}


void RepeaterStore::selectImportedGroup(int index)
{
    if (index < 0 || index >= m_importedGroups.size()) return;
    if (index == m_selectedImportedIndex) {
        if (editingAsZone()) leaveZoneEditor();
        return;
    }
    const int targetZone = m_importedGroups[index].zoneIndexes.constFirst();
    leaveZoneEditor();
    selectZoneIndex(targetZone);
}

bool RepeaterStore::updateSelectedImportedRepeater(const QString &name, const QString &rxFrequency,
                                                   const QString &txFrequency, int colorCode,
                                                   int powerChoice)
{
    if (m_selectedImportedIndex < 0 || m_selectedImportedIndex >= m_importedGroups.size())
        return false;
    const auto &group = m_importedGroups.at(m_selectedImportedIndex);
    const QString cleanName = name.trimmed();
    qint64 rxHz = 0, txHz = 0;
    if (!group.repeater || group.zoneIndexes.size() != 1 || cleanName.isEmpty() || cleanName.size() > 16
        || !parseFrequency(rxFrequency, &rxHz) || !parseFrequency(txFrequency, &txHz)
        || rxHz < 100000000 || txHz < 100000000 || rxHz == txHz
        || colorCode < 0 || colorCode > 15 || powerChoice < -1 || powerChoice >= m_importedCodeplug.powerSettings.size())
        return false;
    const int zoneIndex = group.zoneIndexes.constFirst();
    auto &zone = m_importedCodeplug.zones[zoneIndex];
    auto members = zoneMembers(zone);
    if (zoneMembers(zone).isEmpty() && zone.repeaterTemplate.has_value()) {
        auto &draft = *zone.repeaterTemplate;
        draft.frequencyAHz = quint32(txHz);
        draft.frequencyBHz = quint32(rxHz);
        draft.colorCode = colorCode;
        if (powerChoice >= 0) draft.setPowerValue(m_importedCodeplug.powerSettings.at(powerChoice));
        zone.name = cleanName;
        rebuildImportedGroups();
        selectZoneIndex(zoneIndex);
        setModified(true);
        return true;
    }
    auto needsChange = [this, txHz, rxHz, colorCode, powerChoice](const ImportedChannel &channel) {
        return channel.frequencyAHz != channel.frequencyBHz
            && (channel.frequencyAHz != txHz || channel.frequencyBHz != rxHz
                || channel.colorCode != colorCode
                || (powerChoice >= 0 && channel.powerValue() != m_importedCodeplug.powerSettings.at(powerChoice)));
    };
    const bool changeChannels = std::any_of(members.cbegin(), members.cend(),
        [this, &needsChange](int index) { return needsChange(m_importedCodeplug.channels.at(index)); });
    if (!changeChannels && zone.name == cleanName)
        return true;
    if (changeChannels) {
        QSet<int> sharedChannels;
        for (int channelIndex : zoneMembers(zone)) {
            const auto &channel = m_importedCodeplug.channels.at(channelIndex);
            if (!needsChange(channel) || sharedChannels.contains(channelIndex))
                continue;
            for (int otherZone = 0; otherZone < m_importedCodeplug.zones.size(); ++otherZone)
                if (otherZone != zoneIndex
                    && zoneMembers(m_importedCodeplug.zones.at(otherZone)).contains(channelIndex)) {
                    sharedChannels.insert(channelIndex);
                    break;
                }
        }
        if (m_importedCodeplug.channels.size() + sharedChannels.size() > 3000)
            return false;
    }
    QMap<int, int> clones;
    for (int &channelIndex : members) {
        const ImportedChannel channel = m_importedCodeplug.channels.at(channelIndex);
        if (!needsChange(channel))
            continue;
        if (clones.contains(channelIndex)) {
            channelIndex = clones.value(channelIndex);
            continue;
        }
        const int originalIndex = channelIndex;
        bool shared = false;
        for (int otherZone = 0; otherZone < m_importedCodeplug.zones.size() && !shared; ++otherZone)
            if (otherZone != zoneIndex)
                shared = zoneMembers(m_importedCodeplug.zones.at(otherZone)).contains(originalIndex);
        if (shared) {
            if (m_importedCodeplug.channels.size() >= m_importedCodeplug.channelCapacity)
                return false;
            m_importedCodeplug.channels.append(channel);
            channelIndex = m_importedCodeplug.channels.size() - 1;
        }
        clones.insert(originalIndex, channelIndex);
        auto &editable = m_importedCodeplug.channels[channelIndex];
        editable.frequencyAHz = static_cast<quint32>(txHz);
        editable.frequencyBHz = static_cast<quint32>(rxHz);
        editable.colorCode = colorCode;
        if (powerChoice >= 0)
            editable.setPowerValue(m_importedCodeplug.powerSettings.at(powerChoice));
    }
    applyZoneMembers(zone, members);
    zone.name = cleanName;
    rebuildImportedGroups();
    for (int index = 0; index < m_importedGroups.size(); ++index)
        if (m_importedGroups.at(index).zoneIndexes.contains(zoneIndex)) {
            m_selectedImportedIndex = index;
            break;
        }
    emit importedSelectionChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::addImportedTalkgroup(int timeSlot, const QString &name, int id,
                                         const QString &channelName)
{
    if (m_selectedImportedIndex < 0 || m_selectedImportedIndex >= m_importedGroups.size())
        return false;
    const auto &group = m_importedGroups.at(m_selectedImportedIndex);
    if (!group.repeater || group.zoneIndexes.size() != 1 || (timeSlot != 1 && timeSlot != 2)
        || id < 1 || id > 16777215 || m_importedCodeplug.channels.size() >= m_importedCodeplug.channelCapacity)
        return false;
    const int zoneIndex = group.zoneIndexes.constFirst();
    auto &zone = m_importedCodeplug.zones[zoneIndex];
    const QString cleanName = name.trimmed();
    const QString cleanChannelName = channelName.trimmed().isEmpty() ? cleanName : channelName.trimmed();
    if (!zoneHasRoom(zone) || cleanName.isEmpty() || cleanName.size() > 16
        || cleanChannelName.isEmpty() || cleanChannelName.size() > 16
        || availableChannelName(cleanChannelName) != cleanChannelName)
        return false;
    const QVariantMap powerSummary = selectedImportedGroup();
    if (powerSummary.value(QStringLiteral("power")).toString() == QStringLiteral("Unknown"))
        return false;
    const bool highPower = powerSummary.value(QStringLiteral("newChannelPower")).toString() == QStringLiteral("High");
    int contactIndex = 0;
    for (int index = 0; index < m_importedCodeplug.contacts.size(); ++index) {
        const auto &contact = m_importedCodeplug.contacts.at(index);
        if (contact.id == id) {
            if (!contact.groupCall || contact.name.simplified() != cleanName.simplified())
                return false;
            contactIndex = index + 1;
            break;
        }
    }
    for (int channelIndex : zoneMembers(zone)) {
        const auto &channel = m_importedCodeplug.channels.at(channelIndex);
        if (channel.digital && channel.frequencyAHz == group.frequencyAHz
            && channel.frequencyBHz == group.frequencyBHz && channel.colorCode == group.colorCode
            && channel.timeSlot == timeSlot && channel.contactIndex == contactIndex
            && contactIndex != 0)
            return false;
    }
    if (contactIndex == 0) {
        if (m_importedCodeplug.contacts.size() >= m_importedCodeplug.contactCapacity)
            return false;
        m_importedCodeplug.contacts.append({cleanName, id, true});
        contactIndex = m_importedCodeplug.contacts.size();
    }
    m_importedCodeplug.channels.append({cleanChannelName, group.frequencyAHz, group.frequencyBHz,
                                       true, group.colorCode, timeSlot, contactIndex, highPower});
    m_importedCodeplug.channels.last().setPowerValue(powerSummary.value("newChannelPower").toString().toLower());
    auto members = zoneMembers(zone);
    members.append(m_importedCodeplug.channels.size() - 1);
    applyZoneMembers(zone, members);
    rebuildImportedGroups();
    notifyContactsChanged();
    for (int index = 0; index < m_importedGroups.size(); ++index)
        if (m_importedGroups.at(index).zoneIndexes.contains(zoneIndex)) {
            m_selectedImportedIndex = index;
            break;
        }
    emit importedSelectionChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::removeImportedTalkgroup(int timeSlot, int contactIndex, int zonePosition)
{
    if (m_selectedImportedIndex < 0 || m_selectedImportedIndex >= m_importedGroups.size())
        return false;
    const auto &group = m_importedGroups.at(m_selectedImportedIndex);
    if (!group.repeater || group.zoneIndexes.size() != 1 || (timeSlot != 1 && timeSlot != 2)
        || contactIndex < 0 || contactIndex > m_importedCodeplug.contacts.size()
        || (contactIndex == 0 && zonePosition == 0))
        return false;
    const int zoneIndex = group.zoneIndexes.constFirst();
    auto members = zoneMembers(m_importedCodeplug.zones[zoneIndex]);
    if (zonePosition < 0 || zonePosition > members.size()) return false;
    bool removed = false;
    QVector<int> removedChannelIndexes;
    for (int position = members.size() - 1; position >= 0; --position) {
        const int channelIndex = members.at(position);
        const auto &channel = m_importedCodeplug.channels.at(channelIndex);
        if (channel.digital && channel.frequencyAHz == group.frequencyAHz
            && channel.frequencyBHz == group.frequencyBHz && channel.colorCode == group.colorCode
            && channel.timeSlot == timeSlot && channel.contactIndex == contactIndex
            && (zonePosition == 0 || position == zonePosition - 1)) {
            members.removeAt(position);
            removedChannelIndexes.append(channelIndex);
            removed = true;
        }
    }
    if (!removed)
        return false;
    applyZoneMembers(m_importedCodeplug.zones[zoneIndex], members);
    std::sort(removedChannelIndexes.begin(), removedChannelIndexes.end(), std::greater<int>());
    removedChannelIndexes.erase(std::unique(removedChannelIndexes.begin(), removedChannelIndexes.end()),
                                removedChannelIndexes.end());
    for (int channelIndex : removedChannelIndexes) {
        bool usedElsewhere = false;
        for (const auto &zone : m_importedCodeplug.zones) {
            if (zoneMembers(zone).contains(channelIndex)) {
                usedElsewhere = true;
                break;
            }
        }
        if (usedElsewhere)
            continue;
        m_importedCodeplug.channels.removeAt(channelIndex);
        shiftZoneChannelIndexes(channelIndex);
    }
    rebuildImportedGroups();
    for (int index = 0; index < m_importedGroups.size(); ++index)
        if (m_importedGroups.at(index).zoneIndexes.contains(zoneIndex)) {
            m_selectedImportedIndex = index;
            break;
        }
    emit importedSelectionChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::addSimplexToSelectedImportedRepeater(int zoneInGroup, const QString &name,
                                                        const QString &frequency, bool digital,
                                                        int colorCode, int timeSlot)
{
    if (m_selectedImportedIndex < 0 || m_selectedImportedIndex >= m_importedGroups.size())
        return false;
    const auto &group = m_importedGroups.at(m_selectedImportedIndex);
    if (!group.repeater || zoneInGroup < 0 || zoneInGroup >= group.zoneIndexes.size()
        || m_importedCodeplug.channels.size() >= m_importedCodeplug.channelCapacity)
        return false;
    const int zoneIndex = group.zoneIndexes.at(zoneInGroup);
    if (!zoneHasRoom(m_importedCodeplug.zones.at(zoneIndex)))
        return false;
    qint64 hz = 0;
    const QString cleanName = name.trimmed();
    if (cleanName.isEmpty() || cleanName.size() > 16 || !parseFrequency(frequency, &hz)
        || hz < 100000000
        || (digital && (colorCode < 0 || colorCode > 15 || timeSlot < 1 || timeSlot > 2)))
        return false;
    const int channelIndex = m_importedCodeplug.channels.size();
    m_importedCodeplug.channels.append({cleanName, static_cast<quint32>(hz),
                                       static_cast<quint32>(hz), digital,
                                       digital ? colorCode : 0, digital ? timeSlot : 0});
    if (!digital && !m_importedCodeplug.ctcssTones.isEmpty()) {
        m_importedCodeplug.channels.last().receiveCtcss = 0;
        m_importedCodeplug.channels.last().transmitCtcss = 0;
    }
    auto members = zoneMembers(m_importedCodeplug.zones[zoneIndex]);
    members.append(channelIndex);
    applyZoneMembers(m_importedCodeplug.zones[zoneIndex], members);
    const int previousIndex = m_selectedImportedIndex;
    rebuildImportedGroups();
    m_selectedImportedIndex = previousIndex;
    emit importedSelectionChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::removeSimplexFromSelectedImportedRepeater(int zoneInGroup, int zonePosition)
{
    if (m_selectedImportedIndex < 0 || m_selectedImportedIndex >= m_importedGroups.size())
        return false;
    const auto &group = m_importedGroups.at(m_selectedImportedIndex);
    if (!group.repeater || zoneInGroup < 0 || zoneInGroup >= group.zoneIndexes.size())
        return false;
    const int zoneIndex = group.zoneIndexes.at(zoneInGroup);
    auto members = zoneMembers(m_importedCodeplug.zones[zoneIndex]);
    const int position = zonePosition - 1;
    if (position < 0 || position >= members.size())
        return false;
    const int channelIndex = members.at(position);
    const auto &channel = m_importedCodeplug.channels.at(channelIndex);
    if (channel.frequencyAHz != channel.frequencyBHz)
        return false;

    members.removeAt(position);
    applyZoneMembers(m_importedCodeplug.zones[zoneIndex], members);
    bool usedElsewhere = false;
    for (const auto &zone : m_importedCodeplug.zones) {
        if (zoneMembers(zone).contains(channelIndex)) {
            usedElsewhere = true;
            break;
        }
    }
    if (!usedElsewhere) {
        m_importedCodeplug.channels.removeAt(channelIndex);
        shiftZoneChannelIndexes(channelIndex);
    }
    rebuildImportedGroups();
    for (int index = 0; index < m_importedGroups.size(); ++index) {
        if (m_importedGroups.at(index).zoneIndexes.contains(zoneIndex)) {
            m_selectedImportedIndex = index;
            break;
        }
    }
    emit importedSelectionChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::reorderSelectedImportedZone(const QVariantList &channelNumbers)
{
    if (!selectedImportedGroup().value(QStringLiteral("isRepeater")).toBool()) return false;
    return reorderSelectedZone(channelNumbers);
}

bool RepeaterStore::reorderSelectedZone(const QVariantList &channelNumbers, int display)
{
    if (m_selectedImportedIndex < 0 || m_selectedImportedIndex >= m_importedGroups.size())
        return false;
    const auto &group = m_importedGroups.at(m_selectedImportedIndex);
    if (group.zoneIndexes.size() != 1)
        return false;
    auto &zone = m_importedCodeplug.zones[group.zoneIndexes.constFirst()];
    if (display != 1 && (display != 2 || !separateDisplayLists())) return false;
    auto &members = display == 2 ? zone.bottomChannelIndexes : zone.channelIndexes;
    if (channelNumbers.size() != members.size())
        return false;
    QVector<int> reordered;
    for (const QVariant &value : channelNumbers) {
        bool ok = false;
        const int number = value.toInt(&ok);
        if (!ok || value.toDouble() != number
            || number < 1 || number > m_importedCodeplug.channels.size())
            return false;
        reordered.append(number - 1);
    }
    auto expected = members;
    auto supplied = reordered;
    std::sort(expected.begin(), expected.end());
    std::sort(supplied.begin(), supplied.end());
    if (expected != supplied)
        return false;
    if (members == reordered)
        return true;
    members = std::move(reordered);
    setModified(true);
    emit importedSelectionChanged();
    return true;
}

QVariantList RepeaterStore::zoneOrderEntries() const
{
    QVariantList result;
    if (!hasImportedCodeplug()) {
        for (int i = 0; i < int(m_repeaters.size()); ++i)
            result.append(QVariantMap{{QStringLiteral("number"), i + 1},
                {QStringLiteral("name"), m_repeaters[i].name},
                {QStringLiteral("kind"), QStringLiteral("Repeater")}});
        return result;
    }
    QSet<int> repeaterZones;
    for (const auto &group : m_importedGroups)
        if (group.repeater)
            for (int index : group.zoneIndexes) repeaterZones.insert(index);
    for (int i = 0; i < m_importedCodeplug.zones.size(); ++i)
        result.append(QVariantMap{{QStringLiteral("number"), i + 1},
            {QStringLiteral("name"), m_importedCodeplug.zones[i].name},
            {QStringLiteral("kind"), repeaterZones.contains(i)
                ? QStringLiteral("Repeater") : QStringLiteral("Zone")}});
    return result;
}

bool RepeaterStore::reorderZones(const QVariantList &zoneNumbers)
{
    const bool imported = hasImportedCodeplug();
    const int count = imported ? m_importedCodeplug.zones.size() : int(m_repeaters.size());
    if (zoneNumbers.size() != count) return false;
    QVector<int> order;
    QSet<int> seen;
    bool changed = false;
    for (const auto &value : zoneNumbers) {
        bool ok = false;
        const int number = value.toInt(&ok);
        if (!ok || value.toDouble() != number || number < 1 || number > count
            || seen.contains(number)) return false;
        changed |= number != order.size() + 1;
        seen.insert(number);
        order.append(number - 1);
    }
    if (!changed) return true;
    if (imported) {
        leaveZoneEditor();
        const int selectedZone = selectedZoneIndex();
        const auto original = m_importedCodeplug.zones;
        for (int i = 0; i < count; ++i)
            m_importedCodeplug.zones[i] = original[order[i]];
        rebuildImportedGroups();
        selectZoneIndex(order.indexOf(selectedZone));
    } else {
        const int selectedRepeater = m_selectedIndex;
        const auto original = m_repeaters;
        for (int i = 0; i < count; ++i) m_repeaters[i] = original[order[i]];
        m_selectedIndex = order.indexOf(selectedRepeater);
        emit repeatersChanged();
        emit selectedRepeaterChanged();
        emit talkgroupsChanged();
        emit simplexFrequenciesChanged();
    }
    setModified(true);
    return true;
}

void RepeaterStore::rebuildImportedGroups()
{
    m_zoneClassificationPending = false;
    m_advancedZoneIndex = -1;
    m_importedGroups.clear();
    QVector<ImportedGroup> remainingZones;
    for (int zoneIndex = 0; zoneIndex < m_importedCodeplug.zones.size(); ++zoneIndex) {
        const auto &zone = m_importedCodeplug.zones.at(zoneIndex);
        bool repeater = true;
        bool hasRepeaterChannel = false;
        QString reason;
        ImportedChannel first;
        for (int channelIndex : zoneMembers(zone)) {
            const auto &channel = m_importedCodeplug.channels.at(channelIndex);
            if (channel.frequencyAHz == channel.frequencyBHz) {
                continue;
            }
            if (!channel.digital) {
                repeater = false;
                reason = QStringLiteral("Contains an analog or unrecognized non-simplex channel");
            } else if (!hasRepeaterChannel) {
                first = channel;
                hasRepeaterChannel = true;
            } else if (channel.frequencyAHz != first.frequencyAHz
                       || channel.frequencyBHz != first.frequencyBHz) {
                repeater = false;
                reason = QStringLiteral("Channels use different repeater frequency pairs");
            } else if (channel.colorCode != first.colorCode) {
                repeater = false;
                reason = QStringLiteral("Repeater channels use different color codes");
            }
        }
        if (zoneMembers(zone).isEmpty() && zone.repeaterTemplate.has_value()) {
            first = *zone.repeaterTemplate;
            hasRepeaterChannel = true;
        }
        repeater = repeater && hasRepeaterChannel;
        if (!hasRepeaterChannel)
            reason = zoneMembers(zone).isEmpty()
                ? QStringLiteral("Empty zone")
                : QStringLiteral("Only same-frequency channels; may be simplex or a hotspot");
        if (repeater) {
            ImportedGroup group;
            group.name = zone.name;
            group.reason = QStringLiteral("One repeater frequency pair and color code; simplex channels shown separately");
            group.zoneIndexes.append(zoneIndex);
            group.repeater = true;
            group.frequencyAHz = first.frequencyAHz;
            group.frequencyBHz = first.frequencyBHz;
            group.colorCode = first.colorCode;
            m_importedGroups.append(group);
        } else {
            ImportedGroup group;
            group.name = zone.name;
            group.reason = reason;
            group.zoneIndexes.append(zoneIndex);
            remainingZones.append(group);
        }
    }
    m_importedGroups += remainingZones;
    m_selectedImportedIndex = 0;
    emit importedCodeplugChanged();
    emit importedSelectionChanged();
}

int RepeaterStore::nextZoneOrder(const Repeater &repeater)
{
    int maximum = -1;
    for (const auto *slot : {&repeater.slot1, &repeater.slot2})
        for (const auto &group : *slot) maximum = std::max(maximum, group.zoneOrder);
    for (const auto &simplex : repeater.simplexFrequencies)
        maximum = std::max(maximum, simplex.zoneOrder);
    return maximum < 0 ? -1 : maximum + 1;
}

QVector<QPair<int, int>> RepeaterStore::orderedChannels(const Repeater &repeater)
{
    QVector<QPair<int, int>> result;
    for (int i = 0; i < int(repeater.slot2.size()); ++i) result.append({2, i});
    for (int i = 0; i < int(repeater.slot1.size()); ++i) result.append({1, i});
    for (int i = 0; i < int(repeater.simplexFrequencies.size()); ++i) result.append({0, i});
    auto rank = [&](const QPair<int, int> &entry) {
        const int value = entry.first == 0 ? repeater.simplexFrequencies[entry.second].zoneOrder
            : (entry.first == 1 ? repeater.slot1 : repeater.slot2)[entry.second].zoneOrder;
        return value < 0 ? std::numeric_limits<int>::max() : value;
    };
    std::stable_sort(result.begin(), result.end(), [&](const auto &a, const auto &b) { return rank(a) < rank(b); });
    return result;
}

QVariantList RepeaterStore::selectedRepeaterChannels() const
{
    QVariantList result;
    if (m_repeaters.empty()) return result;
    const auto &repeater = selected();
    for (const auto &entry : orderedChannels(repeater)) {
        QString name;
        int slot = entry.first;
        int number = 0;
        if (slot == 0) {
            const auto &simplex = repeater.simplexFrequencies[entry.second];
            name = simplex.name;
            slot = simplex.digital ? simplex.timeSlot : 0;
            number = int(repeater.slot2.size() + repeater.slot1.size()) + entry.second + 1;
        } else {
            const auto &group = (slot == 1 ? repeater.slot1 : repeater.slot2)[entry.second];
            name = group.channelName.isEmpty() ? group.name : group.channelName;
            number = (slot == 1 ? int(repeater.slot2.size()) : 0) + entry.second + 1;
        }
        result.append(QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("number"), number},
            {QStringLiteral("timeSlot"), slot}, {QStringLiteral("simplex"), entry.first == 0}});
    }
    return result;
}

bool RepeaterStore::reorderSelectedRepeaterChannels(const QVariantList &channelNumbers)
{
    if (hasImportedCodeplug() || m_repeaters.empty()) return false;
    const auto current = selectedRepeaterChannels();
    if (channelNumbers.size() != current.size()) return false;
    QSet<int> seen;
    for (const auto &value : channelNumbers) {
        bool ok = false;
        const int number = value.toInt(&ok);
        if (!ok || value.toDouble() != number || number < 1 || number > current.size() || seen.contains(number))
            return false;
        seen.insert(number);
    }
    bool changed = false;
    for (int i = 0; i < current.size(); ++i)
        changed |= current[i].toMap().value(QStringLiteral("number")).toInt() != channelNumbers[i].toInt();
    if (!changed) return true;
    auto &repeater = selected();
    for (int i = 0; i < channelNumbers.size(); ++i) {
        int index = channelNumbers[i].toInt() - 1;
        if (index < int(repeater.slot2.size())) repeater.slot2[index].zoneOrder = i;
        else if ((index -= int(repeater.slot2.size())) < int(repeater.slot1.size())) repeater.slot1[index].zoneOrder = i;
        else repeater.simplexFrequencies[index - int(repeater.slot1.size())].zoneOrder = i;
    }
    emit talkgroupsChanged();
    emit simplexFrequenciesChanged();
    setModified(true);
    return true;
}

QVariantList RepeaterStore::slot1Talkgroups() const { return m_repeaters.empty() ? QVariantList() : toVariantList(selected().slot1); }
QVariantList RepeaterStore::slot2Talkgroups() const { return m_repeaters.empty() ? QVariantList() : toVariantList(selected().slot2); }
QVariantList RepeaterStore::selectedSimplexFrequencies() const
{
    QVariantList result;
    if (m_repeaters.empty())
        return result;
    for (const auto &item : selected().simplexFrequencies)
        result.append(QVariantMap{{QStringLiteral("name"), item.name},
                                  {QStringLiteral("frequency"), formatFrequency(item.hz)},
                                  {QStringLiteral("mode"), item.digital ? QStringLiteral("Digital") : QStringLiteral("Analog / other")},
                                  {QStringLiteral("colorCode"), item.digital ? QVariant(item.colorCode) : QVariant()},
                                  {QStringLiteral("timeSlot"), item.digital ? QVariant(item.timeSlot) : QVariant()}});
    return result;
}

QVariantList RepeaterStore::contacts() const
{
    // QML sequence filtering can read this property once per row or field.
    if (m_contactsCached) return m_contactsCache;
    QVariantList result;
    if (hasImportedCodeplug()) {
        for (const auto &contact : m_importedCodeplug.contacts) {
            result.append(QVariantMap{{QStringLiteral("name"), contact.name.simplified()},
                                      {QStringLiteral("id"), contact.id},
                                      {QStringLiteral("groupCall"), contact.groupCall},
                                      {QStringLiteral("index"), result.size()}});
        }
        m_contactsCache = result;
        m_contactsCached = true;
        return result;
    }
    QSet<QString> seen;
    for (const auto &contact : m_manualContacts) {
        if (contact.groupCall)
            seen.insert(QString::number(contact.id) + QLatin1Char(':') + contact.name);
        result.append(QVariantMap{{QStringLiteral("name"), contact.name},
                                  {QStringLiteral("id"), contact.id},
                                  {QStringLiteral("groupCall"), contact.groupCall},
                                  {QStringLiteral("index"), result.size()}});
    }
    for (const auto &repeater : m_repeaters) {
        for (const auto *slot : {&repeater.slot1, &repeater.slot2}) {
            for (const auto &talkgroup : *slot) {
                const QString key = QString::number(talkgroup.id) + QLatin1Char(':') + talkgroup.name;
                if (seen.contains(key))
                    continue;
                seen.insert(key);
                result.append(QVariantMap{{QStringLiteral("name"), talkgroup.name},
                                          {QStringLiteral("id"), talkgroup.id},
                                          {QStringLiteral("groupCall"), true},
                                          {QStringLiteral("index"), result.size()}});
            }
        }
    }
    m_contactsCache = result;
    m_contactsCached = true;
    return result;
}

bool RepeaterStore::addContact(const QString &name, int id, bool groupCall)
{
    const QString cleanName = name.trimmed();
    const int nameLimit = 16;
    if (cleanName.isEmpty() || cleanName.size() > nameLimit || id < 1 || id > 16777215)
        return fail(tr("Enter a name of at most %1 characters and a DMR ID from 1 to 16777215.").arg(nameLimit));
    const QVariantList existing = contacts();
    for (const QVariant &value : existing) {
        const QVariantMap contact = value.toMap();
        if (contact.value(QStringLiteral("id")).toInt() == id)
            return fail(tr("A person or talkgroup already uses that DMR ID. Choose a different ID."));
    }
    auto &items = hasImportedCodeplug() ? m_importedCodeplug.contacts : m_manualContacts;
    if (items.size() >= (hasImportedCodeplug() ? m_importedCodeplug.contactCapacity : 10000))
        return fail(tr("The contact list is full."));
    items.append({cleanName, id, groupCall});
    notifyContactsChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::updateContact(int index, const QString &name, int id)
{
    const QVariantList existing = contacts();
    if (index < 0 || index >= existing.size())
        return fail(tr("Select a contact to edit."));
    const QVariantMap old = existing.at(index).toMap();
    const QString cleanName = name.trimmed();
    const int nameLimit = 16;
    if (cleanName.isEmpty() || cleanName.size() > nameLimit || id < 1 || id > 16777215)
        return fail(tr("Enter a name of at most %1 characters and a DMR ID from 1 to 16777215.").arg(nameLimit));
    for (int i = 0; i < existing.size(); ++i) {
        const QVariantMap other = existing.at(i).toMap();
        if (i != index && other.value(QStringLiteral("id")).toInt() == id
            && old.value(QStringLiteral("id")).toInt() != id)
            return fail(tr("A person or talkgroup already uses that DMR ID. Choose a different ID."));
    }
    if (hasImportedCodeplug()) {
        auto &contact = m_importedCodeplug.contacts[index];
        if (contact.name.simplified() == cleanName && contact.id == id)
            return true;
        if (contact.name.simplified() != cleanName)
            contact.name = cleanName;
        contact.id = id;
        emit importedSelectionChanged();
    } else {
        const int oldId = old.value(QStringLiteral("id")).toInt();
        const QString oldName = old.value(QStringLiteral("name")).toString();
        const bool groupCall = old.value(QStringLiteral("groupCall")).toBool();
        if (oldId == id && oldName == cleanName)
            return true;
        if (groupCall) {
            for (const auto &repeater : m_repeaters) {
                for (const auto *slot : {&repeater.slot1, &repeater.slot2}) {
                    const bool changingHere = std::any_of(slot->begin(), slot->end(), [&](const Talkgroup &item) {
                        return item.id == oldId && item.name == oldName;
                    });
                    if (changingHere && id != oldId && std::any_of(slot->begin(), slot->end(), [&](const Talkgroup &item) {
                            return item.id == id;
                        }))
                        return fail(tr("That DMR ID is already used in a repeater time slot."));
                }
            }
            for (auto &repeater : m_repeaters)
                for (auto *slot : {&repeater.slot1, &repeater.slot2})
                    for (auto &item : *slot)
                        if (item.id == oldId && item.name == oldName) {
                            item.name = cleanName;
                            item.id = id;
                        }
            emit talkgroupsChanged();
        }
        if (index < m_manualContacts.size()) {
            m_manualContacts[index].name = cleanName;
            m_manualContacts[index].id = id;
        } else {
            m_manualContacts.append({cleanName, id, groupCall});
        }
    }
    notifyContactsChanged();
    setModified(true);
    return true;
}

void RepeaterStore::newProject()
{
    emit aboutToResetProject();
    m_generatedCodeplug = false;
    m_zoneClassificationPending = false;
    m_advancedZoneIndex = -1;
    m_repeaters.clear();
    m_selectedIndex = 0;
    m_projectPath.clear();
    m_sourceCodeplugName.clear();
    m_sourceCodeplug.clear();
    m_importedCodeplug = {};
    m_manualContacts.clear();
    m_radioName.clear();
    m_radioId = 0;
    m_importedGroups.clear();
    m_selectedImportedIndex = 0;
    m_modified = false;
    emit repeatersChanged();
    emit selectedRepeaterChanged();
    emit talkgroupsChanged();
    emit simplexFrequenciesChanged();
    notifyContactsChanged();
    emit radioSettingsChanged();
    emit importedCodeplugChanged();
    emit importedSelectionChanged();
    emit projectChanged();
}

void RepeaterStore::newProjectDialog()
{
    if (m_modified && QMessageBox::question(nullptr, tr("Discard unsaved changes?"),
                                             tr("Creating a new project will discard your unsaved changes."))
                          != QMessageBox::Yes)
        return;
    emit aboutToResetProject();
    QTimer::singleShot(100, this, [this] { newProject(); });
}

ImportedCodeplug RepeaterStore::conversionSource() const
{
    if (hasImportedCodeplug()) {
        auto source = m_importedCodeplug;
        source.radioName = m_radioName;
        source.radioId = m_radioId;
        return source;
    }
    ImportedCodeplug source;
    source.radioName = m_radioName;
    source.radioId = m_radioId;
    source.contacts = m_manualContacts;
    auto contactFor = [&](const Talkgroup &group) {
        for (int i = 0; i < source.contacts.size(); ++i)
            if (source.contacts[i].id == group.id && source.contacts[i].callTypeValue() == "group") return i + 1;
        source.contacts.append({group.name, group.id, true});
        return int(source.contacts.size());
    };
    for (const auto &repeater : m_repeaters) {
        ImportedZone zone;
        zone.name = repeater.name;
        for (const auto &entry : orderedChannels(repeater)) {
            ImportedChannel channel;
            if (entry.first == 0) {
                const auto &simplex = repeater.simplexFrequencies[entry.second];
                channel.name = simplex.name;
                channel.frequencyAHz = channel.frequencyBHz = quint32(simplex.hz);
                channel.digital = simplex.digital;
                channel.colorCode = simplex.colorCode;
                channel.timeSlot = simplex.timeSlot;
                channel.highPower = true;
            } else {
                const auto &group = (entry.first == 1 ? repeater.slot1 : repeater.slot2)[entry.second];
                channel.name = group.channelName.isEmpty() ? group.name : group.channelName;
                channel.frequencyAHz = quint32(repeater.txHz);
                channel.frequencyBHz = quint32(repeater.rxHz);
                channel.digital = true;
                channel.colorCode = repeater.colorCode;
                channel.timeSlot = entry.first;
                channel.contactIndex = contactFor(group);
                channel.highPower = repeater.highPower;
            }
            channel.receiveCtcss = channel.transmitCtcss = 0;
            zone.channelIndexes.append(source.channels.size());
            source.channels.append(channel);
        }
        if (zone.channelIndexes.isEmpty()) {
            ImportedChannel draft;
            draft.name = repeater.name;
            draft.frequencyAHz = quint32(repeater.txHz);
            draft.frequencyBHz = quint32(repeater.rxHz);
            draft.digital = true;
            draft.colorCode = repeater.colorCode;
            draft.timeSlot = 1;
            draft.highPower = repeater.highPower;
            draft.receiveCtcss = draft.transmitCtcss = 0;
            zone.repeaterTemplate = draft;
        }
        source.zones.append(zone);
    }
    return source;
}

QVariantMap RepeaterStore::beginConversion(const QString &definitionFile, bool twoMeters, bool seventyCm)
{
    if (satelliteUpdateBusy()) { fail(tr("Wait for the radio update to finish before converting.")); return {}; }
    m_conversion.reset();
    try {
        auto session = std::make_unique<CodeplugConversion::Session>();
        QString error;
        session->targetBase = CodeplugDefinitions::defaultCodeplug(definitionFile, &error);
        if (session->targetBase.isEmpty()
            || !CodeplugDefinitions::read(session->targetBase, &session->baseline, &error, definitionFile)
            || !CodeplugDefinitions::conversionMetadata(session->targetBase, definitionFile, &session->metadata, &error)) {
            fail(error); return {};
        }
        session->source = conversionSource();
        session->twoMeters = twoMeters;
        session->seventyCm = seventyCm;
        session->sourceRevision = m_conversionRevision;
        session->contactUses.fill(0, session->source.contacts.size());
        if (hasImportedCodeplug() && !m_sourceCodeplug.isEmpty()) {
            QJsonObject sourceMetadata;
            if (!CodeplugDefinitions::conversionMetadata(m_sourceCodeplug, m_importedCodeplug.definitionFile, &sourceMetadata, &error)) {
                fail(error); return {};
            }
            QMap<int, int> counts;
            for (const auto &ref : sourceMetadata.value("contact_references").toArray()) ++counts[ref.toInt()];
            for (int i = 0; i < session->source.contacts.size(); ++i) {
                const int record = session->source.contacts[i].sourceRecord;
                if (record >= 0) session->contactUses[i] = counts.value(record);
            }
        }
        m_conversion = std::move(session);
        return refreshConversion(true);
    } catch (const std::exception &exception) { fail(QString::fromUtf8(exception.what())); return {}; }
}

QVariantMap RepeaterStore::refreshConversion(bool initialize)
{
    try {
        if (!m_conversion || m_conversion->sourceRevision != m_conversionRevision) {
            fail(tr("The source project changed. Please reopen Convert to review it again.")); return {};
        }
        m_conversion->prepared.clear();
        CodeplugConversion::rebuild(*m_conversion, initialize);
        if (m_conversion->review.value("canConvert").toBool()) {
            QString error;
            if (!CodeplugDefinitions::write(m_conversion->targetBase, m_conversion->converted,
                    m_conversion->converted.radioName, m_conversion->converted.radioId, &m_conversion->prepared, &error)) {
                auto blockers = m_conversion->review.value("blockers").toStringList();
                blockers.append(tr("Destination validation: %1").arg(error));
                m_conversion->review.insert("blockers", blockers);
                m_conversion->review.insert("canConvert", false);
            }
        }
        return m_conversion->review;
    } catch (const std::exception &exception) { fail(QString::fromUtf8(exception.what())); return {}; }
}

QVariantMap RepeaterStore::setConversionChoice(const QString &kind, int index, bool included)
{
    if (!m_conversion) return {};
    QSet<int> *selected = nullptr;
    int count = 0;
    if (kind == "contacts") { selected = &m_conversion->contacts; count = m_conversion->source.contacts.size(); }
    else if (kind == "channels") { selected = &m_conversion->channels; count = m_conversion->source.channels.size(); }
    else if (kind == "zones") { selected = &m_conversion->zones; count = m_conversion->source.zones.size(); }
    if (!selected || index < 0 || index >= count) return {};
    if (included) selected->insert(index); else selected->remove(index);
    return refreshConversion();
}

void RepeaterStore::cancelConversion() { m_conversion.reset(); }

bool RepeaterStore::applyConversion()
{
    if (!m_conversion || m_conversion->sourceRevision != m_conversionRevision)
        return fail(tr("The source project changed. Please reopen Convert to review it again."));
    if (!m_conversion->review.value("canConvert").toBool() || m_conversion->prepared.isEmpty())
        return fail(tr("Resolve the conversion review items before continuing."));
    ImportedCodeplug checked;
    QString error;
    const QByteArray bytes = m_conversion->prepared;
    const QString definitionFile = m_conversion->baseline.definitionFile;
    if (!CodeplugDefinitions::read(bytes, &checked, &error, definitionFile)) return fail(error);
    for (int i = 0; i < checked.zones.size() && i < m_conversion->converted.zones.size(); ++i)
        checked.zones[i].repeaterTemplate = m_conversion->converted.zones[i].repeaterTemplate;
    // All target construction and reimport validation finished before touching the workspace.
    emit aboutToResetProject();
    m_generatedCodeplug = false;
    m_zoneClassificationPending = false;
    m_advancedZoneIndex = -1;
    m_repeaters.clear();
    m_manualContacts.clear();
    m_selectedIndex = 0;
    m_selectedImportedIndex = 0;
    m_projectPath.clear();
    m_sourceCodeplugName.clear();
    m_sourceCodeplug = bytes;
    m_importedCodeplug = std::move(checked);
    m_radioName = m_importedCodeplug.radioName;
    m_radioId = m_importedCodeplug.radioId;
    m_conversion.reset();
    rebuildImportedGroups();
    emit repeatersChanged();
    emit selectedRepeaterChanged();
    emit talkgroupsChanged();
    emit simplexFrequenciesChanged();
    notifyContactsChanged();
    emit radioSettingsChanged();
    setModified(true);
    emit projectChanged();
    return true;
}

bool RepeaterStore::newProjectForDefinition(const QString &definitionFile)
{
    QString error;
    const QByteArray base = CodeplugDefinitions::defaultCodeplug(definitionFile, &error);
    if (base.isEmpty()) return fail(error);
    ImportedCodeplug validated;
    if (!CodeplugDefinitions::read(base, &validated, &error, definitionFile)) return fail(error);
    if (m_modified && QMessageBox::question(nullptr, tr("Discard unsaved changes?"),
                                             tr("Creating a new project will discard your unsaved changes."))
                          != QMessageBox::Yes)
        return false;
    validated.channels.clear();
    validated.contacts.clear();
    validated.zones.clear();
    validated.radioName.clear();
    validated.radioId = 0;
    if (validated.powerSettings.isEmpty()) validated.powerSettings = {"low", "high"};
    newProject();
    m_importedCodeplug = std::move(validated);
    m_sourceCodeplug = base;
    rebuildImportedGroups();
    emit radioSettingsChanged();
    emit projectChanged();
    return true;
}

bool RepeaterStore::addRepeater(const QString &name, const QString &rxFrequency,
                                const QString &txFrequency, int colorCode, bool highPower)
{
    const QString cleanName = name.trimmed();
    qint64 rxHz = 0, txHz = 0;
    if (cleanName.isEmpty() || cleanName.size() > 32 || m_repeaters.size() >= 10000
        || !parseFrequency(rxFrequency, &rxHz) || !parseFrequency(txFrequency, &txHz)
        || colorCode < 0 || colorCode > 15)
        return false;
    if (hasImportedCodeplug()) {
        if (m_importedCodeplug.zones.size() >= m_importedCodeplug.zoneCapacity || cleanName.size() > 16 || rxHz == txHz
            || rxHz < 100000000 || txHz < 100000000) return false;
        leaveZoneEditor();
        ImportedChannel draft;
        draft.frequencyAHz = quint32(txHz);
        draft.frequencyBHz = quint32(rxHz);
        draft.digital = true;
        draft.colorCode = colorCode;
        draft.timeSlot = 1;
        draft.highPower = highPower;
        m_importedCodeplug.zones.append({cleanName, {}, draft});
        ImportedGroup group;
        group.name = cleanName;
        group.zoneIndexes = {int(m_importedCodeplug.zones.size()) - 1};
        group.repeater = true;
        group.reason = QStringLiteral("New repeater");
        group.frequencyAHz = quint32(txHz);
        group.frequencyBHz = quint32(rxHz);
        group.colorCode = colorCode;
        const int index = importedRepeaterNames().size();
        m_importedGroups.insert(index, group);
        m_selectedImportedIndex = index;
        emit importedCodeplugChanged();
        emit importedSelectionChanged();
        setModified(true);
        return true;
    }
    m_repeaters.push_back({cleanName, rxHz, txHz, colorCode, {}, {}, {}, highPower});
    m_selectedIndex = static_cast<int>(m_repeaters.size()) - 1;
    emit repeatersChanged();
    emit selectedRepeaterChanged();
    emit talkgroupsChanged();
    emit simplexFrequenciesChanged();
    setModified(true);
    return true;
}

void RepeaterStore::selectRepeater(int index)
{
    if (index < 0 || index >= static_cast<int>(m_repeaters.size()) || index == m_selectedIndex)
        return;
    m_selectedIndex = index;
    emit selectedRepeaterChanged();
    emit talkgroupsChanged();
    emit simplexFrequenciesChanged();
}

bool RepeaterStore::addSimplexToSelectedRepeater(const QString &name, const QString &frequency,
                                                 bool digital, int colorCode, int timeSlot)
{
    qint64 hz = 0;
    const QString cleanName = name.trimmed();
    if (m_repeaters.empty() || cleanName.isEmpty() || cleanName.size() > 16
        || selected().simplexFrequencies.size() >= 64 || !parseFrequency(frequency, &hz)
        || (digital && (colorCode < 0 || colorCode > 15 || timeSlot < 1 || timeSlot > 2)))
        return false;
    selected().simplexFrequencies.push_back({cleanName, hz, digital,
                                             digital ? colorCode : 0, digital ? timeSlot : 0, nextZoneOrder(selected())});
    emit simplexFrequenciesChanged();
    emit talkgroupsChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::removeSimplexFromSelectedRepeater(int index)
{
    if (m_repeaters.empty() || index < 0
        || index >= static_cast<int>(selected().simplexFrequencies.size()))
        return false;
    auto &frequencies = selected().simplexFrequencies;
    frequencies.erase(frequencies.begin() + index);
    emit simplexFrequenciesChanged();
    emit talkgroupsChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::updateSelectedRepeater(const QString &name, const QString &rxFrequency,
                                            const QString &txFrequency, int colorCode,
                                            bool highPower)
{
    const QString cleanName = name.trimmed();
    qint64 rxHz = 0;
    qint64 txHz = 0;
    if (m_repeaters.empty() || cleanName.isEmpty() || cleanName.size() > 32 || !parseFrequency(rxFrequency, &rxHz)
        || !parseFrequency(txFrequency, &txHz) || colorCode < 0 || colorCode > 15)
        return false;

    auto &repeater = selected();
    if (repeater.name == cleanName && repeater.rxHz == rxHz && repeater.txHz == txHz
        && repeater.colorCode == colorCode && repeater.highPower == highPower)
        return true;
    repeater.name = cleanName;
    repeater.rxHz = rxHz;
    repeater.txHz = txHz;
    repeater.colorCode = colorCode;
    repeater.highPower = highPower;
    emit repeatersChanged();
    emit selectedRepeaterChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::addTalkgroup(int timeSlot, const QString &name, int id,
                                 const QString &channelName)
{
    const QString cleanName = name.trimmed();
    const QString cleanChannelName = channelName.trimmed().isEmpty() ? cleanName : channelName.trimmed();
    if (hasImportedCodeplug() || m_repeaters.empty() || (timeSlot != 1 && timeSlot != 2)
        || cleanName.isEmpty() || cleanName.size() > 32 || cleanChannelName.isEmpty()
        || cleanChannelName.size() > 16 || id < 1 || id > 16777215
        || availableChannelName(cleanChannelName) != cleanChannelName)
        return false;

    auto &items = talkgroups(timeSlot);
    if (std::any_of(items.begin(), items.end(), [id](const Talkgroup &item) { return item.id == id; }))
        return false;
    for (const QVariant &value : contacts()) {
        const QVariantMap contact = value.toMap();
        if (contact.value(QStringLiteral("id")).toInt() == id
            && (!contact.value(QStringLiteral("groupCall")).toBool()
                || contact.value(QStringLiteral("name")).toString().simplified() != cleanName.simplified()))
            return false;
    }
    const bool inLibrary = std::any_of(m_manualContacts.cbegin(), m_manualContacts.cend(),
                                       [id](const ImportedContact &contact) { return contact.id == id; });
    if (!inLibrary) {
        if (m_manualContacts.size() >= 10000)
            return false;
        m_manualContacts.append({cleanName, id, true});
    }
    items.push_back({cleanName, id, cleanChannelName, nextZoneOrder(selected())});
    emit talkgroupsChanged();
    notifyContactsChanged();
    setModified(true);
    return true;
}

void RepeaterStore::removeTalkgroup(int timeSlot, int id)
{
    if (m_repeaters.empty() || (timeSlot != 1 && timeSlot != 2))
        return;
    auto &items = talkgroups(timeSlot);
    const auto firstRemoved = std::remove_if(items.begin(), items.end(),
                                             [id](const Talkgroup &item) { return item.id == id; });
    if (firstRemoved == items.end())
        return;
    items.erase(firstRemoved, items.end());
    emit talkgroupsChanged();
    notifyContactsChanged();
    setModified(true);
}

bool RepeaterStore::saveProjectToFile(const QString &path)
{
    QJsonArray repeaters;
    for (const auto &repeater : m_repeaters) {
        auto convert = [](const std::vector<Talkgroup> &groups) {
            QJsonArray result;
            for (const auto &group : groups) {
                QJsonObject item{{QStringLiteral("name"), group.name},
                                 {QStringLiteral("id"), group.id},
                                 {QStringLiteral("zoneOrder"), group.zoneOrder}};
                if (!group.channelName.isEmpty()) {
                    item.insert(QStringLiteral("channelName"), group.channelName);
                }
                result.append(item);
            }
            return result;
        };
        QJsonObject item{
            {QStringLiteral("name"), repeater.name},
            {QStringLiteral("rxHz"), repeater.rxHz},
            {QStringLiteral("txHz"), repeater.txHz},
            {QStringLiteral("colorCode"), repeater.colorCode},
            {QStringLiteral("highPower"), repeater.highPower},
            {QStringLiteral("slots"), QJsonObject{{QStringLiteral("1"), convert(repeater.slot1)},
                                                   {QStringLiteral("2"), convert(repeater.slot2)}}}};
        if (!repeater.simplexFrequencies.empty()) {
            QJsonArray simplex;
            for (const auto &frequency : repeater.simplexFrequencies)
                simplex.append(QJsonObject{{QStringLiteral("name"), frequency.name},
                                           {QStringLiteral("frequencyHz"), frequency.hz},
                                           {QStringLiteral("digital"), frequency.digital},
                                           {QStringLiteral("colorCode"), frequency.colorCode},
                                           {QStringLiteral("timeSlot"), frequency.timeSlot},
                                           {QStringLiteral("zoneOrder"), frequency.zoneOrder}});
            item.insert(QStringLiteral("simplexFrequencies"), simplex);
        }
        repeaters.append(item);
    }

    QJsonObject root{{QStringLiteral("format"), projectFormat},
                     {QStringLiteral("schemaVersion"), 11},
                     {QStringLiteral("repeaters"), repeaters}};
    root.insert(QStringLiteral("generatedCodeplug"), m_generatedCodeplug);
    root.insert(QStringLiteral("radioSettings"),
                QJsonObject{{QStringLiteral("name"), m_radioName},
                            {QStringLiteral("id"), m_radioId}});
    if (!m_manualContacts.isEmpty()) {
        QJsonArray library;
        for (const auto &contact : m_manualContacts)
            library.append(QJsonObject{{QStringLiteral("name"), contact.name},
                                       {QStringLiteral("id"), contact.id},
                                       {QStringLiteral("groupCall"), contact.groupCall}});
        root.insert(QStringLiteral("contactLibrary"), library);
    }
    if (hasImportedCodeplug()) {
        QJsonArray channels;
        for (const auto &channel : m_importedCodeplug.channels)
            channels.append(QJsonObject{{QStringLiteral("name"), channel.name},
                                        {QStringLiteral("sourceRecord"), channel.sourceRecord},
                                        {QStringLiteral("useDefaultTemplate"), channel.useDefaultTemplate},
                                        {QStringLiteral("powerSetting"), channel.powerValue()},
                                        {QStringLiteral("frequencyAHz"), int(channel.frequencyAHz)},
                                        {QStringLiteral("frequencyBHz"), int(channel.frequencyBHz)},
                                        {QStringLiteral("receiveCtcss"), channel.receiveCtcss ? QJsonValue(*channel.receiveCtcss) : QJsonValue(QJsonValue::Null)},
                                        {QStringLiteral("transmitCtcss"), channel.transmitCtcss ? QJsonValue(*channel.transmitCtcss) : QJsonValue(QJsonValue::Null)},
                                        {QStringLiteral("digital"), channel.digital},
                                        {QStringLiteral("colorCode"), channel.colorCode},
                                        {QStringLiteral("timeSlot"), channel.timeSlot},
                                        {QStringLiteral("contactIndex"), channel.contactIndex},
                                        {QStringLiteral("scanListIndex"), channel.scanListIndex},
                                        {QStringLiteral("highPower"), channel.highPower.has_value()
                                             ? QJsonValue(*channel.highPower) : QJsonValue(QJsonValue::Null)}});
        QJsonArray scans;
        for (const auto &list : m_importedCodeplug.scanLists) {
            QJsonArray members;
            for (int index : list.channelIndexes) members.append(index);
            scans.append(QJsonObject{{"name", list.name}, {"sourceRecord", list.sourceRecord}, {"channelIndexes", members}});
        }
        QJsonArray contacts;
        for (const auto &contact : m_importedCodeplug.contacts)
            contacts.append(QJsonObject{{QStringLiteral("name"), contact.name},
                                        {QStringLiteral("id"), contact.id},
                                        {QStringLiteral("groupCall"), contact.groupCall},
                                        {QStringLiteral("sourceRecord"), contact.sourceRecord},
                                        {QStringLiteral("callType"), contact.callTypeValue()}});
        QJsonArray zones;
        for (const auto &zone : m_importedCodeplug.zones) {
            QJsonArray indexes;
            for (int index : zone.channelIndexes)
                indexes.append(index);
            QJsonObject zoneJson{{QStringLiteral("name"), zone.name},
                                 {QStringLiteral("sourceRecord"), zone.sourceRecord},
                                 {QStringLiteral("channelIndexes"), indexes}};
            if (separateDisplayLists()) {
                QJsonArray bottom;
                for (int index : zone.bottomChannelIndexes) bottom.append(index);
                zoneJson.insert(QStringLiteral("bottomChannelIndexes"), bottom);
            }
            if (zone.repeaterTemplate.has_value()) {
                const auto &draft = *zone.repeaterTemplate;
                zoneJson.insert(QStringLiteral("repeaterTemplate"), QJsonObject{
                    {QStringLiteral("rxHz"), int(draft.frequencyBHz)}, {QStringLiteral("txHz"), int(draft.frequencyAHz)},
                    {QStringLiteral("colorCode"), draft.colorCode}, {QStringLiteral("powerSetting"), draft.powerValue()}, {QStringLiteral("highPower"), draft.highPower.value_or(true)}});
            }
            zones.append(zoneJson);
        }
        root.insert(QStringLiteral("importedCodeplug"),
                    QJsonObject{{QStringLiteral("model"), m_importedCodeplug.model},
                                {QStringLiteral("separateDisplayLists"), separateDisplayLists()},
                                {QStringLiteral("definitionFile"), m_importedCodeplug.definitionFile},
                                {QStringLiteral("definitionVersion"), m_importedCodeplug.definitionVersion},
                                {QStringLiteral("cpsVersion"), m_importedCodeplug.cpsVersion},
                                {QStringLiteral("channels"), channels},
                                {QStringLiteral("contacts"), contacts},
                                {QStringLiteral("zones"), zones},
                                {QStringLiteral("scanLists"), scans}});
    }
    QMap<QString, QByteArray> entries;
    if (!m_sourceCodeplug.isEmpty()) {
        const QByteArray hash = QCryptographicHash::hash(m_sourceCodeplug, QCryptographicHash::Sha256).toHex();
        root.insert(QStringLiteral("sourceCodeplug"),
                    QJsonObject{{QStringLiteral("entry"), sourceEntry},
                                {QStringLiteral("fileName"), m_sourceCodeplugName},
                                {QStringLiteral("size"), m_sourceCodeplug.size()},
                                {QStringLiteral("sha256"), QString::fromLatin1(hash)}});
        entries.insert(sourceEntry, m_sourceCodeplug);
    }
    entries.insert(QStringLiteral("project.json"), QJsonDocument(root).toJson(QJsonDocument::Indented));

    QString error;
    if (!ZipArchive::write(path, entries, &error))
        return fail(QStringLiteral("Could not save project: %1").arg(error));
    m_projectPath = path;
    m_modified = false;
    emit projectChanged();
    return true;
}

bool RepeaterStore::openProjectFromFile(const QString &path)
{
    QMap<QString, QByteArray> entries;
    QString error;
    if (!ZipArchive::read(path, &entries, &error))
        return fail(QStringLiteral("Could not open project: %1").arg(error));
    if (!entries.contains(QStringLiteral("project.json")))
        return fail(QStringLiteral("Project is missing project.json."));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(entries.value(QStringLiteral("project.json")), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(QStringLiteral("Project JSON is invalid."));
    const QJsonObject root = document.object();
    qint64 version = 0;
    if (root.value(QStringLiteral("format")).toString() != projectFormat
        || !integerValue(root.value(QStringLiteral("schemaVersion")), 1, 11, &version))
        return fail(QStringLiteral("This WaffleCPS project version is not supported."));

    const QJsonValue repeatersValue = root.value(QStringLiteral("repeaters"));
    if (!repeatersValue.isArray() || repeatersValue.toArray().size() > 10000
        || (version == 1 && repeatersValue.toArray().isEmpty()))
        return fail(QStringLiteral("Project has no valid repeater list."));

    std::vector<Repeater> loaded;
    for (const QJsonValue &value : repeatersValue.toArray()) {
        if (!value.isObject())
            return fail(QStringLiteral("A repeater record is invalid."));
        const QJsonObject item = value.toObject();
        Repeater repeater;
        repeater.name = item.value(QStringLiteral("name")).toString().trimmed();
        qint64 colorCode = 0;
        if (repeater.name.isEmpty() || repeater.name.size() > 32
            || !integerValue(item.value(QStringLiteral("rxHz")), 1, 999999999, &repeater.rxHz)
            || !integerValue(item.value(QStringLiteral("txHz")), 1, 999999999, &repeater.txHz)
            || !integerValue(item.value(QStringLiteral("colorCode")), 0, 15, &colorCode))
            return fail(QStringLiteral("A repeater has invalid details."));
        repeater.colorCode = static_cast<int>(colorCode);
        if (version >= 5) {
            if (!item.value(QStringLiteral("highPower")).isBool())
                return fail(QStringLiteral("A repeater has invalid transmit power."));
            repeater.highPower = item.value(QStringLiteral("highPower")).toBool();
        }

        const QJsonValue slotsValue = item.value(QStringLiteral("slots"));
        if (!slotsValue.isObject())
            return fail(QStringLiteral("A repeater is missing time slots."));
        const QJsonObject slotObject = slotsValue.toObject();
        auto readGroups = [version](const QJsonValue &arrayValue, std::vector<Talkgroup> *groups) {
            if (!arrayValue.isArray() || arrayValue.toArray().size() > 10000)
                return false;
            QSet<int> ids;
            for (const QJsonValue &groupValue : arrayValue.toArray()) {
                if (!groupValue.isObject())
                    return false;
                const QJsonObject group = groupValue.toObject();
                const QString name = group.value(QStringLiteral("name")).toString().trimmed();
                const QJsonValue channelNameValue = group.value(QStringLiteral("channelName"));
                const QString channelName = channelNameValue.toString().trimmed();
                qint64 id = 0;
                if (name.isEmpty() || name.size() > 32
                    || !integerValue(group.value(QStringLiteral("id")), 1, 16777215, &id)
                    || ids.contains(static_cast<int>(id))
                    || (!channelNameValue.isUndefined()
                        && (version < 4 || !channelNameValue.isString()
                            || channelName.isEmpty() || channelName.size() > 32)))
                    return false;
                ids.insert(static_cast<int>(id));
                qint64 order = -1;
                if (version >= 7 && !integerValue(group.value(QStringLiteral("zoneOrder")), -1, 2147483646, &order))
                    return false;
                groups->push_back({name, static_cast<int>(id), channelName, int(order)});
            }
            return true;
        };
        if (!readGroups(slotObject.value(QStringLiteral("1")), &repeater.slot1)
            || !readGroups(slotObject.value(QStringLiteral("2")), &repeater.slot2))
            return fail(QStringLiteral("A repeater has invalid talkgroups."));
        const QJsonValue simplexValue = item.value(QStringLiteral("simplexFrequencies"));
        if (!simplexValue.isUndefined()) {
            if (version < 3 || !simplexValue.isArray() || simplexValue.toArray().size() > 64)
                return fail(QStringLiteral("A repeater has invalid simplex frequencies."));
            for (const QJsonValue &entry : simplexValue.toArray()) {
                if (!entry.isObject())
                    return fail(QStringLiteral("A simplex frequency is invalid."));
                const QJsonObject frequency = entry.toObject();
                Repeater::SimplexFrequency simplex;
                simplex.name = frequency.value(QStringLiteral("name")).toString();
                qint64 colorCode = 0, timeSlot = 0;
                if (simplex.name.isEmpty() || simplex.name.size() > 16
                    || !integerValue(frequency.value(QStringLiteral("frequencyHz")), 1, 999999999, &simplex.hz)
                    || !frequency.value(QStringLiteral("digital")).isBool()
                    || !integerValue(frequency.value(QStringLiteral("colorCode")), 0, 15, &colorCode)
                    || !integerValue(frequency.value(QStringLiteral("timeSlot")), 0, 2, &timeSlot))
                    return fail(QStringLiteral("A simplex frequency has invalid details."));
                qint64 order = -1;
                if (version >= 7 && !integerValue(frequency.value(QStringLiteral("zoneOrder")), -1, 2147483646, &order))
                    return fail(QStringLiteral("A simplex frequency has invalid channel order."));
                simplex.zoneOrder = int(order);
                simplex.digital = frequency.value(QStringLiteral("digital")).toBool();
                simplex.colorCode = colorCode;
                simplex.timeSlot = timeSlot;
                repeater.simplexFrequencies.push_back(simplex);
            }
        }
        loaded.push_back(std::move(repeater));
    }

    QVector<ImportedContact> manualContacts;
    const QJsonValue libraryValue = root.value(QStringLiteral("contactLibrary"));
    if (!libraryValue.isUndefined()) {
        if (version < 4 || !libraryValue.isArray() || libraryValue.toArray().size() > 10000)
            return fail(QStringLiteral("Contact library is invalid."));
        QSet<QString> usedIds;
        for (const QJsonValue &value : libraryValue.toArray()) {
            if (!value.isObject())
                return fail(QStringLiteral("A contact is invalid."));
            const QJsonObject item = value.toObject();
            ImportedContact contact;
            contact.name = item.value(QStringLiteral("name")).toString().trimmed();
            qint64 id = 0;
            if (contact.name.isEmpty() || contact.name.size() > 32
                || !integerValue(item.value(QStringLiteral("id")), 1, 16777215, &id)
                || !item.value(QStringLiteral("groupCall")).isBool())
                return fail(QStringLiteral("A contact has invalid details."));
            contact.id = static_cast<int>(id);
            contact.groupCall = item.value(QStringLiteral("groupCall")).toBool();
            const QString key = QString::number(contact.id);
            if (usedIds.contains(key))
                return fail(QStringLiteral("The contact library has duplicate DMR IDs."));
            usedIds.insert(key);
            manualContacts.append(contact);
        }
    }

    ImportedCodeplug imported;
    if (version == 2 || (version >= 3 && root.contains(QStringLiteral("importedCodeplug")))) {
        const QJsonValue importedValue = root.value(QStringLiteral("importedCodeplug"));
        if (!importedValue.isObject())
            return fail(QStringLiteral("Imported codeplug data is missing."));
        const QJsonObject data = importedValue.toObject();
        if (version >= 9) {
            if (!data.value("separateDisplayLists").isBool()) return fail(tr("Invalid display channel lists."));
            imported.separateDisplayLists = data.value("separateDisplayLists").toBool();
        }
        imported.model = data.value(QStringLiteral("model")).toString();
        const auto definitionValue = data.value(QStringLiteral("definitionFile"));
        if (!definitionValue.isUndefined()) {
            imported.definitionFile = definitionValue.toString();
            if (!definitionValue.isString() || imported.definitionFile.size() > 255
                || (!imported.definitionFile.isEmpty()
                    && (QFileInfo(imported.definitionFile).fileName() != imported.definitionFile
                        || imported.definitionFile.contains('\\') || imported.definitionFile.contains(':'))))
                return fail(QStringLiteral("Imported definition filename is invalid."));
        }
        const QJsonValue definitionVersion = data.value(QStringLiteral("definitionVersion"));
        if (!definitionVersion.isUndefined()) {
            const QString selectedVersion = definitionVersion.toString();
            if (!definitionVersion.isString() || selectedVersion.size() > 64
                || !std::all_of(selectedVersion.cbegin(), selectedVersion.cend(),
                                [](QChar c) { return c.isLetterOrNumber() || c == '-' || c == '_'; }))
                return fail(QStringLiteral("Imported definition version is invalid."));
            imported.definitionVersion = selectedVersion;
        }
        imported.cpsVersion = data.value(QStringLiteral("cpsVersion")).toString();
        const QJsonValue channelsValue = data.value(QStringLiteral("channels"));
        const QJsonValue zonesValue = data.value(QStringLiteral("zones"));
        if (imported.model.isEmpty() || imported.model.size() > 128
            || !std::all_of(imported.model.cbegin(), imported.model.cend(), [](QChar c) { return c.isPrint(); })
            || !data.value(QStringLiteral("cpsVersion")).isString()
            || imported.cpsVersion.size() > 64
            || !std::all_of(imported.cpsVersion.cbegin(), imported.cpsVersion.cend(),
                            [](QChar c) { return c.isPrint(); })
            || !channelsValue.isArray()
            || channelsValue.toArray().size() > 3000 || !zonesValue.isArray()
            || zonesValue.toArray().isEmpty() || zonesValue.toArray().size() > 500)
            return fail(QStringLiteral("Imported codeplug metadata is invalid."));
        const QJsonValue contactsValue = data.value(QStringLiteral("contacts"));
        if (!contactsValue.isUndefined()) {
            if (version < 3 || !contactsValue.isArray() || contactsValue.toArray().size() > 100000)
                return fail(QStringLiteral("Imported contacts are invalid."));
            for (const QJsonValue &value : contactsValue.toArray()) {
                if (!value.isObject())
                    return fail(QStringLiteral("An imported contact is invalid."));
                const QJsonObject item = value.toObject();
                ImportedContact contact;
                contact.name = item.value(QStringLiteral("name")).toString();
                qint64 id = 0;
                if (contact.name.isEmpty() || contact.name.size() > 16
                    || !integerValue(item.value(QStringLiteral("id")), 1, 16777215, &id)
                    || !item.value(QStringLiteral("groupCall")).isBool())
                    return fail(QStringLiteral("An imported contact has invalid details."));
                contact.id = static_cast<int>(id);
                contact.groupCall = item.value(QStringLiteral("groupCall")).toBool();
                contact.sourceRecord = item.value("sourceRecord").toInt(imported.contacts.size());
                contact.callType = item.value("callType").toString();
                imported.contacts.append(contact);
            }
        }
        for (const QJsonValue &value : channelsValue.toArray()) {
            if (!value.isObject())
                return fail(QStringLiteral("An imported channel is invalid."));
            const QJsonObject item = value.toObject();
            ImportedChannel channel;
            channel.name = item.value(QStringLiteral("name")).toString();
            const QJsonValue powerValue = item.value(QStringLiteral("highPower"));
            qint64 frequencyA = 0, frequencyB = 0, colorCode = 0, timeSlot = 0, contactIndex = 0;
            if (channel.name.isEmpty() || channel.name.size() > 16
                || !integerValue(item.value(QStringLiteral("frequencyAHz")), 100000000, 999999999, &frequencyA)
                || !integerValue(item.value(QStringLiteral("frequencyBHz")), 100000000, 999999999, &frequencyB)
                || !item.value(QStringLiteral("digital")).isBool()
                || !integerValue(item.value(QStringLiteral("colorCode")), 0, 15, &colorCode)
                || !integerValue(item.value(QStringLiteral("timeSlot")), 0, 2, &timeSlot)
                || (!item.value(QStringLiteral("contactIndex")).isUndefined()
                    && !integerValue(item.value(QStringLiteral("contactIndex")), 0, imported.contacts.size(), &contactIndex))
                || (version >= 5 && !powerValue.isBool() && !powerValue.isNull()))
                return fail(QStringLiteral("An imported channel has invalid details."));
            channel.frequencyAHz = frequencyA;
            channel.frequencyBHz = frequencyB;
            channel.digital = item.value(QStringLiteral("digital")).toBool();
            channel.colorCode = colorCode;
            channel.timeSlot = timeSlot;
            channel.contactIndex = contactIndex;
            if (powerValue.isBool())
                channel.highPower = powerValue.toBool();
            qint64 sourceRecord = -1;
            if (version >= 8 && !integerValue(item.value(QStringLiteral("sourceRecord")), -1, 2999, &sourceRecord))
                return fail(QStringLiteral("An imported channel has invalid source data."));
            channel.sourceRecord = int(sourceRecord);
            if (item.contains("powerSetting")) {
                if (!item.value("powerSetting").isString()) return fail(tr("Invalid power setting."));
                channel.setPowerValue(item.value("powerSetting").toString());
            }
            const auto defaultTemplate = item.value(QStringLiteral("useDefaultTemplate"));
            if (!defaultTemplate.isUndefined() && !defaultTemplate.isBool())
                return fail(QStringLiteral("An imported channel has invalid mode defaults."));
            channel.useDefaultTemplate = defaultTemplate.toBool();
            if (version >= 10) {
                for (auto entry : {qMakePair(QStringLiteral("receiveCtcss"), &channel.receiveCtcss),
                                   qMakePair(QStringLiteral("transmitCtcss"), &channel.transmitCtcss)}) {
                    const auto value = item.value(entry.first);
                    if (value.isNull()) continue;
                    qint64 tone = 0;
                    if (!integerValue(value, 0, 9999, &tone) || channel.digital)
                        return fail(tr("An imported channel has invalid CTCSS settings."));
                    *entry.second = int(tone);
                }
            }
            imported.channels.append(channel);
        }
        for (const QJsonValue &value : zonesValue.toArray()) {
            if (!value.isObject())
                return fail(QStringLiteral("An imported zone is invalid."));
            const QJsonObject item = value.toObject();
            ImportedZone zone;
            zone.sourceRecord = item.value("sourceRecord").toInt(imported.zones.size());
            zone.name = item.value(QStringLiteral("name")).toString();
            const QJsonValue indexesValue = item.value(QStringLiteral("channelIndexes"));
            if (zone.name.isEmpty() || zone.name.size() > 16 || !indexesValue.isArray()
                || indexesValue.toArray().size() > 1024)
                return fail(QStringLiteral("An imported zone has invalid details."));
            for (const QJsonValue &indexValue : indexesValue.toArray()) {
                qint64 channelIndex = 0;
                if (!integerValue(indexValue, 0, imported.channels.size() - 1, &channelIndex))
                    return fail(QStringLiteral("An imported zone references an unknown channel."));
                zone.channelIndexes.append(channelIndex);
            }
            if (imported.separateDisplayLists) {
                const auto bottom = item.value("bottomChannelIndexes");
                if (!bottom.isArray() || bottom.toArray().size() > 1024) return fail(tr("Invalid Bottom channel list."));
                for (const auto &value : bottom.toArray()) {
                    qint64 index;
                    if (!integerValue(value, 0, imported.channels.size() - 1, &index))
                        return fail(tr("A Bottom channel list references an unknown channel."));
                    zone.bottomChannelIndexes.append(int(index));
                }
            }
            if (version >= 8 && item.contains(QStringLiteral("repeaterTemplate"))) {
                const auto draft = item.value(QStringLiteral("repeaterTemplate")).toObject();
                qint64 rxHz = 0, txHz = 0, cc = 0;
                if (!integerValue(draft.value(QStringLiteral("rxHz")), 100000000, 999999999, &rxHz)
                    || !integerValue(draft.value(QStringLiteral("txHz")), 100000000, 999999999, &txHz)
                    || rxHz == txHz || !integerValue(draft.value(QStringLiteral("colorCode")), 0, 15, &cc)
                    || !draft.value(QStringLiteral("highPower")).isBool())
                    return fail(QStringLiteral("An empty repeater has invalid settings."));
                zone.repeaterTemplate = ImportedChannel{zone.name, quint32(txHz), quint32(rxHz), true,
                    int(cc), 1, 0, draft.value(QStringLiteral("highPower")).toBool()};
                if (draft.contains("powerSetting")) zone.repeaterTemplate->setPowerValue(draft.value("powerSetting").toString());
            }
            imported.zones.append(std::move(zone));
        }
    }

    if (!imported.model.isEmpty() && version >= 11) {
        const auto saved = root.value("importedCodeplug").toObject();
        const auto scans = saved.value("scanLists");
        if (!scans.isArray() || scans.toArray().size() > 1024) return fail(tr("Invalid saved scan lists."));
        for (const auto &entry : scans.toArray()) {
            const auto item = entry.toObject();
            ImportedScanList list;
            list.name = item.value("name").toString();
            qint64 sourceRecord;
            if (list.name.isEmpty() || list.name.size() > 256
                || !integerValue(item.value("sourceRecord"), -1, 100000, &sourceRecord)
                || !item.value("channelIndexes").isArray() || item.value("channelIndexes").toArray().size() > 100000)
                return fail(tr("Invalid saved scan list."));
            list.sourceRecord = int(sourceRecord);
            for (const auto &value : item.value("channelIndexes").toArray()) {
                qint64 index;
                if (!integerValue(value, -1, imported.channels.size() - 1, &index))
                    return fail(tr("Saved scan list references an unknown channel."));
                list.channelIndexes.append(int(index));
            }
            imported.scanLists.append(list);
        }
        const auto channels = saved.value("channels").toArray();
        for (int i = 0; i < imported.channels.size(); ++i) {
            qint64 index;
            if (!integerValue(channels[i].toObject().value("scanListIndex"), 0, imported.scanLists.size(), &index))
                return fail(tr("Saved channel references an unknown scan list."));
            imported.channels[i].scanListIndex = int(index);
        }
    }

    const bool generatedCodeplug = version >= 8 && root.value(QStringLiteral("generatedCodeplug")).toBool();
    QString sourceName;
    QByteArray sourceBytes;
    const QJsonValue sourceValue = root.value(QStringLiteral("sourceCodeplug"));
    if (!sourceValue.isUndefined()) {
        if (!sourceValue.isObject())
            return fail(QStringLiteral("Source codeplug metadata is invalid."));
        const QJsonObject source = sourceValue.toObject();
        sourceName = source.value(QStringLiteral("fileName")).toString();
        qint64 size = 0;
        if (source.value(QStringLiteral("entry")).toString() != sourceEntry
            || (sourceName.isEmpty() && version < 9)
            || QFileInfo(sourceName).fileName() != sourceName
            || !integerValue(source.value(QStringLiteral("size")), 0, maximumSourceSize, &size)
            || !entries.contains(sourceEntry))
            return fail(QStringLiteral("Source codeplug is missing or invalid."));
        sourceBytes = entries.value(sourceEntry);
        const QString actualHash = QString::fromLatin1(
            QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256).toHex());
        if (sourceBytes.size() != size || source.value(QStringLiteral("sha256")).toString() != actualHash)
            return fail(QStringLiteral("Source codeplug failed its integrity check."));
    }
    if (entries.size() != (sourceValue.isUndefined() ? 1 : 2))
        return fail(QStringLiteral("Project contains unsupported archive entries."));

    if (!imported.model.isEmpty() && imported.cpsVersion.isEmpty()) {
        ImportedCodeplug recognized;
        QString identityError;
        if (sourceBytes.isEmpty()
            || !CodeplugDefinitions::read(sourceBytes, &recognized, &identityError, imported.definitionFile)
            || recognized.model != imported.model || !recognized.cpsVersion.isEmpty())
            return fail(QStringLiteral("Imported version metadata does not match the source definition."));
    }

    QString loadedRadioName;
    int loadedRadioId = 0;
    if (version >= 6) {
        const QJsonValue settingsValue = root.value(QStringLiteral("radioSettings"));
        if (!settingsValue.isObject())
            return fail(QStringLiteral("Radio settings are invalid."));
        const QJsonObject settings = settingsValue.toObject();
        loadedRadioName = settings.value(QStringLiteral("name")).toString();
        qint64 id = 0;
        if (!settings.value(QStringLiteral("name")).isString()
            || loadedRadioName.size() > 16
            || !integerValue(settings.value(QStringLiteral("id")), 0, 16777215, &id)
            || (loadedRadioName.isEmpty() != (id == 0) && imported.model.isEmpty()))
            return fail(QStringLiteral("Radio settings are invalid."));
        loadedRadioId = int(id);
    } else if (!sourceBytes.isEmpty()) {
        ImportedCodeplug sourceParsed;
        QString sourceError;
        if (CodeplugDefinitions::read(sourceBytes, &sourceParsed, &sourceError, imported.definitionFile)) {
            loadedRadioName = sourceParsed.radioName;
            loadedRadioId = sourceParsed.radioId;
        }
    }

    if (!imported.model.isEmpty() && !sourceBytes.isEmpty()) {
        ImportedCodeplug metadata;
        QString error;
        if (!CodeplugDefinitions::read(sourceBytes, &metadata, &error, imported.definitionFile))
            return fail(tr("Cannot restore tone settings from the project definition: %1").arg(error));
        imported.ctcssTones = metadata.ctcssTones;
        imported.scanListsSupported = metadata.scanListsSupported;
        imported.scanListCapacity = metadata.scanListCapacity;
        imported.scanMemberCapacity = metadata.scanMemberCapacity;
        imported.scanListsNeedCleanup = metadata.scanListsNeedCleanup;
        if (version < 11 && metadata.scanListsSupported) {
            QMap<int, int> channels;
            for (int i = 0; i < imported.channels.size(); ++i) channels.insert(imported.channels[i].sourceRecord, i);
            imported.scanLists = metadata.scanLists;
            for (auto &list : imported.scanLists) {
                QVector<int> members;
                for (int index : list.channelIndexes) {
                    if (index == -1) members.append(-1);
                    else if (channels.contains(metadata.channels[index].sourceRecord)) members.append(channels.value(metadata.channels[index].sourceRecord));
                }
                list.channelIndexes = members;
            }
            for (auto &channel : imported.channels)
                for (const auto &original : metadata.channels)
                    if (channel.sourceRecord == original.sourceRecord) { channel.scanListIndex = original.scanListIndex; break; }
        }
        if ((!imported.scanListsSupported && !imported.scanLists.isEmpty())
            || imported.scanLists.size() > imported.scanListCapacity)
            return fail(tr("Saved scan lists exceed the definition limits."));
        for (const auto &list : imported.scanLists)
            if (list.channelIndexes.size() > imported.scanMemberCapacity)
                return fail(tr("Saved scan list exceeds the definition's channel limit."));
        QMap<int, ImportedChannel> originals;
        for (const auto &channel : metadata.channels) originals.insert(channel.sourceRecord, channel);
        for (auto &channel : imported.channels) {
            if (version < 10 && !channel.digital && !channel.useDefaultTemplate && originals.contains(channel.sourceRecord)) {
                const auto &original = originals[channel.sourceRecord];
                if (!original.digital) {
                    channel.receiveCtcss = original.receiveCtcss;
                    channel.transmitCtcss = original.transmitCtcss;
                }
            }
            for (const auto tone : {channel.receiveCtcss, channel.transmitCtcss})
                if (tone && !imported.ctcssTones.contains(*tone))
                    return fail(tr("A saved CTCSS tone is not supported by this definition."));
        }
    }

    if (imported.separateDisplayLists) {
        QString error;
        const QByteArray base = sourceBytes.isEmpty()
            ? CodeplugDefinitions::defaultCodeplug(imported.definitionFile, &error) : sourceBytes;
        ImportedCodeplug metadata;
        if (base.isEmpty() || !CodeplugDefinitions::read(base, &metadata, &error, imported.definitionFile)
            || !metadata.separateDisplayLists)
            return fail(tr("Cannot restore Top/Bottom channel limits from the project definition: %1").arg(error));
        imported.powerSettings = metadata.powerSettings;
        imported.channelCapacity = metadata.channelCapacity;
        imported.contactCapacity = metadata.contactCapacity;
        imported.zoneCapacity = metadata.zoneCapacity;
        imported.zoneMemberCapacity = metadata.zoneMemberCapacity;
        for (const auto &zone : imported.zones)
            if (zone.channelIndexes.size() > imported.zoneMemberCapacity || zone.bottomChannelIndexes.size() > imported.zoneMemberCapacity)
                return fail(tr("A project display list exceeds the definition's membership limit."));
    }

    // Recover older project details from the untouched source when available.
    if (!generatedCodeplug && !imported.channels.isEmpty() && !sourceBytes.isEmpty()) {
        ImportedCodeplug sourceParsed;
        QString sourceError;
        if (CodeplugDefinitions::read(sourceBytes, &sourceParsed, &sourceError, imported.definitionFile)) {
            if (imported.definitionFile.isEmpty()) imported.definitionFile = sourceParsed.definitionFile;
            if (imported.definitionVersion.isEmpty()) imported.definitionVersion = sourceParsed.definitionVersion;
            imported.powerSettings = sourceParsed.powerSettings;
            imported.channelCapacity = sourceParsed.channelCapacity;
            imported.contactCapacity = sourceParsed.contactCapacity;
            imported.zoneCapacity = sourceParsed.zoneCapacity;
            imported.zoneMemberCapacity = sourceParsed.zoneMemberCapacity;
            if (imported.separateDisplayLists != sourceParsed.separateDisplayLists)
                return fail(tr("Project display channel lists do not match the source definition."));
            QMap<QString, int> sourceByName;
            for (int index = 0; index < sourceParsed.channels.size(); ++index) {
                const QString &name = sourceParsed.channels.at(index).name;
                if (sourceByName.contains(name))
                    sourceByName[name] = -1;
                else
                    sourceByName.insert(name, index);
            }
            for (int index = 0; index < imported.channels.size(); ++index) {
                auto &channel = imported.channels[index];
                int sourceIndex = index < sourceParsed.channels.size()
                                      && sourceParsed.channels.at(index).name == channel.name
                    ? index : sourceByName.value(channel.name, -1);
                if (sourceIndex >= 0) {
                    if (version < 8) channel.sourceRecord = sourceParsed.channels.at(sourceIndex).sourceRecord;
                    if (channel.powerValue().isEmpty()) channel.setPowerValue(sourceParsed.channels.at(sourceIndex).powerValue());
                }
            }
            if (sourceParsed.channels.size() <= imported.channels.size()) {
                if (imported.contacts.isEmpty()) {
                    imported.contacts = sourceParsed.contacts;
                    for (int index = 0; index < sourceParsed.channels.size(); ++index)
                        imported.channels[index].contactIndex = sourceParsed.channels.at(index).contactIndex;
                } else {
                    const int count = std::min(imported.contacts.size(), sourceParsed.contacts.size());
                    for (int index = 0; index < count; ++index) {
                        auto &saved = imported.contacts[index];
                        const auto &original = sourceParsed.contacts.at(index);
                        if (saved.id == original.id && saved.groupCall == original.groupCall
                            && saved.name == original.name.left(11).trimmed())
                            saved.name = original.name;
                    }
                }
            }
        }
    }

    m_generatedCodeplug = generatedCodeplug;
    m_zoneClassificationPending = false;
    m_advancedZoneIndex = -1;
    m_repeaters = std::move(loaded);
    m_selectedIndex = 0;
    m_projectPath = path;
    m_sourceCodeplugName = sourceName;
    m_sourceCodeplug = sourceBytes;
    if (imported.powerSettings.isEmpty()) imported.powerSettings = {"low", "high"};
    m_importedCodeplug = std::move(imported);
    m_manualContacts = std::move(manualContacts);
    m_radioName = loadedRadioName;
    m_radioId = loadedRadioId;
    rebuildImportedGroups();
    m_modified = false;
    emit repeatersChanged();
    emit selectedRepeaterChanged();
    emit talkgroupsChanged();
    emit simplexFrequenciesChanged();
    notifyContactsChanged();
    emit radioSettingsChanged();
    emit projectChanged();
    return true;
}

bool RepeaterStore::attachSourceCodeplugFromFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Could not read codeplug: %1").arg(file.errorString()));
    if (file.size() > maximumSourceSize)
        return fail(QStringLiteral("Codeplug exceeds the project size limit."));
    const QByteArray bytes = file.readAll();
    if (bytes.size() != file.size())
        return fail(QStringLiteral("Could not read the complete codeplug."));
    m_sourceCodeplugName = QFileInfo(path).fileName();
    m_sourceCodeplug = bytes;
    setModified(true);
    emit projectChanged();
    return true;
}

bool RepeaterStore::importCodeplugFromBytes(const QByteArray &bytes, const QString &definitionFile)
{
    ImportedCodeplug imported;
    QString error;
    if (!CodeplugDefinitions::read(bytes, &imported, &error, definitionFile))
        return fail(QStringLiteral("Could not import codeplug: %1").arg(error));
    emit aboutToResetProject();
    m_generatedCodeplug = false;
    m_zoneClassificationPending = false;
    m_advancedZoneIndex = -1;
    if (imported.powerSettings.isEmpty()) imported.powerSettings = {"low", "high"};
    m_importedCodeplug = std::move(imported);
    m_radioName = m_importedCodeplug.radioName;
    m_radioId = m_importedCodeplug.radioId;
    rebuildImportedGroups();
    notifyContactsChanged();
    emit radioSettingsChanged();
    setModified(true);
    emit projectChanged();
    return true;
}

bool RepeaterStore::importCodeplugFromFile(const QString &path, const QString &definitionFile)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Could not read codeplug: %1").arg(file.errorString()));
    if (file.size() > maximumSourceSize)
        return fail(QStringLiteral("Codeplug exceeds the project size limit."));
    const QByteArray bytes = file.readAll();
    if (bytes.size() != file.size())
        return fail(QStringLiteral("Could not read the complete codeplug."));
    if (!importCodeplugFromBytes(bytes, definitionFile))
        return false;
    m_sourceCodeplugName = QFileInfo(path).fileName();
    m_sourceCodeplug = bytes;
    m_projectPath.clear();
    emit projectChanged();
    return true;
}

bool RepeaterStore::importRadioCodeplug(const QByteArray &bytes)
{
    // Validate before replacing any part of the currently open project.
    ImportedCodeplug imported;
    QString error;
    if (!CodeplugDefinitions::read(bytes, &imported, &error))
        return fail(tr("Could not import radio codeplug: %1").arg(error));
    QByteArray fileSource;
    QString fileExtension;
    if (!CodeplugDefinitions::encodeRadioImage(bytes, imported.definitionFile,
                                                &fileSource, &fileExtension, &error))
        return fail(tr("Could not prepare radio codeplug for file export: %1").arg(error));
    emit aboutToResetProject();
    m_generatedCodeplug = false;
    m_zoneClassificationPending = false;
    m_advancedZoneIndex = -1;
    m_repeaters.clear();
    m_manualContacts.clear();
    m_selectedIndex = 0;
    m_projectPath.clear();
    m_sourceCodeplug = fileSource;
    m_sourceCodeplugName = QStringLiteral("radio-read") + fileExtension;
    m_generatedCodeplug = false;
    m_zoneClassificationPending = false;
    m_advancedZoneIndex = -1;
    if (imported.powerSettings.isEmpty()) imported.powerSettings = {"low", "high"};
    m_importedCodeplug = std::move(imported);
    m_radioName = m_importedCodeplug.radioName;
    m_radioId = m_importedCodeplug.radioId;
    rebuildImportedGroups();
    setModified(true);
    emit repeatersChanged();
    emit selectedRepeaterChanged();
    emit talkgroupsChanged();
    emit simplexFrequenciesChanged();
    notifyContactsChanged();
    emit radioSettingsChanged();
    emit projectChanged();
    return true;
}

void RepeaterStore::importAttachedCodeplug()
{
    if (m_sourceCodeplug.isEmpty()) {
        fail(QStringLiteral("No source codeplug is attached."));
        return;
    }
    importCodeplugFromBytes(m_sourceCodeplug, m_importedCodeplug.definitionFile);
}

void RepeaterStore::importCodeplugDialog()
{
    QString error, warnings;
    const QString filter = CodeplugDefinitions::importFileFilter(&error, &warnings);
    if (filter.isEmpty()) {
        fail(error);
        return;
    }
    if (!warnings.isEmpty())
        QMessageBox::warning(nullptr, tr("Definitions that failed to load"), warnings);
    if (m_modified && QMessageBox::question(nullptr, tr("Discard unsaved changes?"),
                                             tr("Importing a codeplug will replace the current imported view and discard unsaved changes."))
                          != QMessageBox::Yes)
        return;
    QString selectedFilter;
    const QString path = QFileDialog::getOpenFileName(nullptr, tr("Import codeplug"), QString(), filter, &selectedFilter);
    if (!path.isEmpty()) {
        const QString definitionFile = CodeplugDefinitions::definitionForFilter(selectedFilter, &error);
        if (!error.isEmpty()) { fail(error); return; }
        importCodeplugFromFile(path, definitionFile);
    }
}

void RepeaterStore::openProjectDialog()
{
    if (m_modified && QMessageBox::question(nullptr, tr("Discard unsaved changes?"),
                                             tr("Opening another project will discard your unsaved changes."))
                          != QMessageBox::Yes)
        return;
    const QString path = QFileDialog::getOpenFileName(nullptr, tr("Open WaffleCPS project"), QString(),
                                                      tr("WaffleCPS projects (*.waffle *.zip)"));
    if (!path.isEmpty())
        openProjectFromFile(path);
}

void RepeaterStore::saveProjectDialog(bool saveAs)
{
    QString path = saveAs ? QString() : m_projectPath;
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(nullptr, tr("Save WaffleCPS project"),
                                            m_projectPath.isEmpty() ? QStringLiteral("project.waffle") : m_projectPath,
                                            tr("WaffleCPS projects (*.waffle)"));
        if (path.isEmpty())
            return;
        if (!path.endsWith(QStringLiteral(".waffle"), Qt::CaseInsensitive))
            path += QStringLiteral(".waffle");
    }
    saveProjectToFile(path);
}

bool RepeaterStore::buildManualCodeplug(ImportedCodeplug *result)
{
    ImportedCodeplug target;
    QString readError;
    const auto source = CodeplugDefinitions::defaultCodeplug(&readError);
    if (source.isEmpty() || !CodeplugDefinitions::read(source, &target, &readError))
        return fail(QStringLiteral("Could not read the definition's default codeplug: %1").arg(readError));
    if (!m_repeaters.empty()) {
        target.channels.clear();
        target.contacts = m_manualContacts;
        target.zones.clear();
        QMap<int, int> contactById;
        for (int i = 0; i < target.contacts.size(); ++i)
            contactById.insert(target.contacts.at(i).id, i + 1);
        auto contactFor = [&](const Talkgroup &group) {
            const int existing = contactById.value(group.id, 0);
            if (existing)
                return target.contacts.at(existing - 1).groupCall ? existing : 0;
            target.contacts.append({group.name, group.id, true});
            const int index = target.contacts.size();
            contactById.insert(group.id, index);
            return index;
        };
        for (const Repeater &repeater : m_repeaters) {
            ImportedZone zone;
            zone.name = repeater.name;
            auto addGroup = [&](const Talkgroup &group, int timeSlot) {
                const int contact = contactFor(group);
                if (contact == 0)
                    return false;
                ImportedChannel channel;
                channel.name = group.channelName.isEmpty() ? group.name : group.channelName;
                channel.frequencyAHz = quint32(repeater.txHz);
                channel.frequencyBHz = quint32(repeater.rxHz);
                channel.digital = true;
                channel.colorCode = repeater.colorCode;
                channel.timeSlot = timeSlot;
                channel.contactIndex = contact;
                channel.highPower = repeater.highPower;
                zone.channelIndexes.append(target.channels.size());
                target.channels.append(channel);
                return true;
            };
            // New manual zones default to TS2 first, then TS1.
            for (const Talkgroup &group : repeater.slot2)
                if (!addGroup(group, 2))
                    return fail(QStringLiteral("A talkgroup ID conflicts with a person contact."));
            for (const Talkgroup &group : repeater.slot1)
                if (!addGroup(group, 1))
                    return fail(QStringLiteral("A talkgroup ID conflicts with a person contact."));
            for (const auto &simplex : repeater.simplexFrequencies) {
                ImportedChannel channel;
                channel.name = simplex.name;
                channel.frequencyAHz = quint32(simplex.hz);
                channel.frequencyBHz = quint32(simplex.hz);
                channel.digital = simplex.digital;
                channel.colorCode = simplex.digital ? simplex.colorCode : 0;
                channel.timeSlot = simplex.digital ? simplex.timeSlot : 0;
                channel.highPower = true;
                zone.channelIndexes.append(target.channels.size());
                target.channels.append(channel);
            }
            const auto originalMembers = zone.channelIndexes;
            zone.channelIndexes.clear();
            for (const auto &entry : orderedChannels(repeater)) {
                const int index = entry.first == 2 ? entry.second
                    : entry.first == 1 ? int(repeater.slot2.size()) + entry.second
                    : int(repeater.slot2.size() + repeater.slot1.size()) + entry.second;
                zone.channelIndexes.append(originalMembers[index]);
            }
            if (zoneMembers(zone).isEmpty() && repeater.rxHz != repeater.txHz)
                zone.repeaterTemplate = ImportedChannel{repeater.name, quint32(repeater.txHz), quint32(repeater.rxHz),
                    true, repeater.colorCode, 1, 0, repeater.highPower};
            target.zones.append(std::move(zone));
        }
    } else if (!m_manualContacts.isEmpty()) {
        target.contacts += m_manualContacts;
    }
    makeExportChannelNamesUnique(&target);
    *result = std::move(target);
    return true;
}

bool RepeaterStore::exportCodeplugToFile(const QString &path)
{
    if (!m_sourceCodeplugName.isEmpty()
        && QFileInfo(path).fileName().compare(m_sourceCodeplugName, Qt::CaseInsensitive) == 0)
        return fail(QStringLiteral("Choose a different filename so the original codeplug stays untouched."));
    const QByteArray bytes = exportCodeplugBytes();
    if (bytes.isEmpty()) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(QStringLiteral("Could not write codeplug: %1").arg(file.errorString()));
    return true;
}

QByteArray RepeaterStore::exportCodeplugBytes()
{
    QString writeError;
    const QByteArray source = hasImportedCodeplug() && !m_generatedCodeplug
        ? m_sourceCodeplug
        : hasImportedCodeplug()
            ? CodeplugDefinitions::defaultCodeplug(m_importedCodeplug.definitionFile, &writeError)
            : CodeplugDefinitions::defaultCodeplug(&writeError);
    if (source.isEmpty()) {
        fail(QStringLiteral("The codeplug export base is unavailable: %1").arg(writeError));
        return {};
    }
    ImportedCodeplug target;
    if (hasImportedCodeplug()) {
        target = m_importedCodeplug;
    } else {
        if (!buildManualCodeplug(&target)) return {};
    }
    QByteArray bytes;
    if (!CodeplugDefinitions::write(source, target, m_radioName, m_radioId, &bytes, &writeError)) {
        fail(QStringLiteral("Could not export codeplug: %1").arg(writeError));
        return {};
    }
    return bytes;
}

void RepeaterStore::exportCodeplugDialog()
{
    if (!hasImportedCodeplug()) {
        fail(tr("Blank projects cannot be exported as codeplugs. Save this work as a WaffleCPS project. "
                "To export a codeplug, first use Codeplug > Convert to select a radio."));
        return;
    }
    QString title = tr("Export codeplug");
    QString extension;
    QString error;
    const QByteArray source = m_generatedCodeplug
        ? CodeplugDefinitions::defaultCodeplug(m_importedCodeplug.definitionFile, &error)
        : m_sourceCodeplug;
    if (source.isEmpty()) { fail(error); return; }
    const QString filter = CodeplugDefinitions::exportFileFilter(
        source, &extension, &error, m_importedCodeplug.definitionFile);
    if (filter.isEmpty()) {
        fail(error);
        return;
    }
    QString path = QFileDialog::getSaveFileName(
        nullptr, title, QStringLiteral("wafflecps-export") + extension, filter);
    if (path.isEmpty())
        return;
    if (!path.endsWith(extension, Qt::CaseInsensitive))
        path += extension;
    exportCodeplugToFile(path);
}

void RepeaterStore::attachSourceDialog()
{
    const QString path = QFileDialog::getOpenFileName(nullptr, tr("Attach original codeplug"), QString(),
                                                      tr("Codeplug files (*.rdbx *.rbdx);;All files (*)"));
    if (!path.isEmpty())
        attachSourceCodeplugFromFile(path);
}

const RepeaterStore::Repeater &RepeaterStore::selected() const
{
    return m_repeaters.at(static_cast<std::size_t>(m_selectedIndex));
}

RepeaterStore::Repeater &RepeaterStore::selected()
{
    return m_repeaters.at(static_cast<std::size_t>(m_selectedIndex));
}

std::vector<RepeaterStore::Talkgroup> &RepeaterStore::talkgroups(int timeSlot)
{
    return timeSlot == 1 ? selected().slot1 : selected().slot2;
}

QVariantList RepeaterStore::toVariantList(const std::vector<Talkgroup> &talkgroups)
{
    QVariantList result;
    auto ordered = talkgroups;
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
        return (a.zoneOrder < 0 ? std::numeric_limits<int>::max() : a.zoneOrder)
             < (b.zoneOrder < 0 ? std::numeric_limits<int>::max() : b.zoneOrder);
    });
    for (const auto &talkgroup : ordered) {
        const QString channelName = talkgroup.channelName.isEmpty() ? talkgroup.name : talkgroup.channelName;
        result.append(QVariantMap{{QStringLiteral("name"), talkgroup.name},
                                  {QStringLiteral("id"), talkgroup.id},
                                  {QStringLiteral("groupCall"), true},
                                  {QStringLiteral("channelName"), channelName},
                                  {QStringLiteral("channels"), QVariantList{QVariantMap{{QStringLiteral("name"), channelName}}}}});
    }
    return result;
}

QString RepeaterStore::formatFrequency(qint64 hz)
{
    QString fraction = QStringLiteral("%1").arg(hz % 1000000, 6, 10, QChar('0'));
    while (fraction.size() > 4 && fraction.endsWith(QLatin1Char('0')))
        fraction.chop(1);
    return QString::number(hz / 1000000) + QLatin1Char('.') + fraction;
}

bool RepeaterStore::parseFrequency(const QString &text, qint64 *hz)
{
    static const QRegularExpression pattern(QStringLiteral("^(\\d{1,3})(?:\\.(\\d{1,6}))?$"));
    const auto match = pattern.match(text.trimmed());
    if (!match.hasMatch())
        return false;
    QString fraction = match.captured(2);
    fraction = fraction.leftJustified(6, QLatin1Char('0'));
    *hz = match.captured(1).toLongLong() * 1000000 + fraction.toLongLong();
    return *hz > 0;
}

void RepeaterStore::setModified(bool modified)
{
    if (m_modified == modified)
        return;
    m_modified = modified;
    emit projectChanged();
}

bool RepeaterStore::fail(const QString &message)
{
    emit errorOccurred(message);
    return false;
}
