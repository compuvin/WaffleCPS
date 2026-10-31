#include "macradiotransfer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <stdexcept>

namespace {
void fail(const QString &message)
{
    throw std::runtime_error(message.toStdString());
}

void save(const QString &path, const RadioExecutor::Bytes &bytes)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()))
            != qsizetype(bytes.size())
        || !file.commit())
        fail(QStringLiteral("Could not prepare radio transfer image: %1").arg(file.errorString()));
}

void reportProgress(const QString &path, const RadioExecutor::Progress &progress,
                    QByteArray *previous)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QByteArray data = file.readAll();
    if (data == *previous) return;
    const QJsonObject update = QJsonDocument::fromJson(data).object();
    if (update.isEmpty()) return;
    *previous = data;
    if (progress)
        progress(update.value(QStringLiteral("region")).toString().toStdString(),
                 uint32_t(update.value(QStringLiteral("done")).toInt()),
                 uint32_t(update.value(QStringLiteral("total")).toInt()));
}

void run(const QString &mode, const QString &definitionPath, const QString &imagePath,
         const QString &progressPath, const QString &cancelPath,
         const RadioExecutor::Progress &progress, const std::atomic_bool *cancelled)
{
    const QString helper = QCoreApplication::applicationDirPath()
        + QStringLiteral("/radio-transfer-helper");
    if (!QFile::exists(helper)) fail(QStringLiteral("Mac radio transfer helper is missing from the app."));
    const QString script = QStringLiteral(
        "on run argv\n"
        "set commandText to quoted form of (item 1 of argv) & \" \" & quoted form of (item 2 of argv) & \" \" & quoted form of (item 3 of argv) & \" \" & quoted form of (item 4 of argv) & \" \" & quoted form of (item 5 of argv) & \" \" & quoted form of (item 6 of argv)\n"
        "do shell script commandText with administrator privileges\n"
        "end run");
    QProcess process;
    process.start(QStringLiteral("/usr/bin/osascript"),
                  {QStringLiteral("-e"), script, helper, mode, definitionPath,
                   imagePath, progressPath, cancelPath});
    if (!process.waitForStarted()) fail(QStringLiteral("Could not start macOS authorization prompt."));
    QByteArray previous;
    bool cancelSent = false;
    while (!process.waitForFinished(100)) {
        reportProgress(progressPath, progress, &previous);
        if (cancelled && cancelled->load() && !cancelSent) {
            QFile marker(cancelPath);
            if (marker.open(QIODevice::WriteOnly)) cancelSent = true;
        }
    }
    reportProgress(progressPath, progress, &previous);
    if (cancelled && cancelled->load()) throw RadioExecutor::Cancelled();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
        fail(detail.isEmpty() ? QStringLiteral("Mac radio transfer helper failed.") : detail);
    }
}

QTemporaryDir temporaryDirectory()
{
    QTemporaryDir dir(QDir::tempPath() + QStringLiteral("/wafflecps-radio-XXXXXX"));
    if (!dir.isValid()) fail(QStringLiteral("Could not create private radio transfer directory."));
    return dir;
}
}

RadioExecutor::Bytes MacRadioTransfer::read(const QString &definitionPath,
                                             const RadioExecutor::Progress &progress,
                                             const std::atomic_bool &cancelled)
{
    QTemporaryDir dir = temporaryDirectory();
    const QString imagePath = dir.filePath(QStringLiteral("image.bin"));
    run(QStringLiteral("read"), definitionPath, imagePath,
        dir.filePath(QStringLiteral("progress.json")), dir.filePath(QStringLiteral("cancel")),
        progress, &cancelled);
    QFile file(imagePath);
    if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("Could not open radio read image."));
    const QByteArray data = file.readAll();
    if (data.isEmpty()) fail(QStringLiteral("Radio helper returned an empty image."));
    return RadioExecutor::Bytes(data.begin(), data.end());
}

void MacRadioTransfer::write(const QString &definitionPath, const RadioExecutor::Bytes &image,
                             const RadioExecutor::Progress &progress)
{
    QTemporaryDir dir = temporaryDirectory();
    const QString imagePath = dir.filePath(QStringLiteral("image.bin"));
    save(imagePath, image);
    run(QStringLiteral("write"), definitionPath, imagePath,
        dir.filePath(QStringLiteral("progress.json")), QStringLiteral("-"), progress, nullptr);
}
