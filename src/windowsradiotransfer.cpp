#include "windowsradiotransfer.h"
#include "windowsmassstorage.h"

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <stdexcept>
#include <string>

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
        fail(QStringLiteral("Could not prepare Windows radio transfer image: %1").arg(file.errorString()));
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

QTemporaryDir temporaryDirectory()
{
    QTemporaryDir dir(QDir::tempPath() + QStringLiteral("/wafflecps-radio-XXXXXX"));
    if (!dir.isValid()) fail(QStringLiteral("Could not create Windows radio transfer directory."));
    return dir;
}

QString quoted(const QString &argument)
{
    if (argument.contains(QChar('"')) || argument.endsWith(QChar('\\')))
        fail(QStringLiteral("Invalid Windows radio transfer argument."));
    return QStringLiteral("\"") + argument + QStringLiteral("\"");
}

void run(const QString &mode, const QString &definitionPath, const QString &imagePath,
         const QString &progressPath, const QString &cancelPath, const QString &errorPath,
         const RadioExecutor::Progress &progress, const std::atomic_bool *cancelled)
{
    const QString helper = QCoreApplication::applicationDirPath()
        + QStringLiteral("/radio-transfer-helper.exe");
    if (!QFile::exists(helper)) fail(QStringLiteral("Windows radio transfer helper is missing from the installation."));
    const QString arguments = quoted(mode) + QChar(' ') + quoted(definitionPath)
        + QChar(' ') + quoted(imagePath) + QChar(' ') + quoted(progressPath)
        + QChar(' ') + quoted(cancelPath) + QChar(' ') + quoted(errorPath);
    const std::wstring helperWide = helper.toStdWString();
    const std::wstring argumentsWide = arguments.toStdWString();
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    launch.lpVerb = L"runas";
    launch.lpFile = helperWide.c_str();
    launch.lpParameters = argumentsWide.c_str();
    launch.nShow = SW_HIDE;
    if (!ShellExecuteExW(&launch)) {
        const DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) fail(QStringLiteral("Windows radio authorization was cancelled."));
        fail(QStringLiteral("Could not start Windows radio transfer helper: Windows error %1").arg(error));
    }
    if (!launch.hProcess) fail(QStringLiteral("Windows radio transfer helper did not return a process handle."));
    struct ProcessGuard {
        HANDLE handle;
        ~ProcessGuard() { CloseHandle(handle); }
    } process{launch.hProcess};
    QByteArray previous;
    bool cancelSent = false;
    for (;;) {
        const DWORD state = WaitForSingleObject(process.handle, 100);
        reportProgress(progressPath, progress, &previous);
        if (state == WAIT_OBJECT_0) break;
        if (state != WAIT_TIMEOUT)
            fail(QStringLiteral("Could not wait for Windows radio transfer helper: Windows error %1").arg(GetLastError()));
        if (cancelled && cancelled->load() && !cancelSent) {
            QFile marker(cancelPath);
            if (marker.open(QIODevice::WriteOnly)) cancelSent = true;
        }
    }
    if (cancelled && cancelled->load()) throw RadioExecutor::Cancelled();
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process.handle, &exitCode))
        fail(QStringLiteral("Could not inspect Windows radio transfer helper: Windows error %1").arg(GetLastError()));
    if (exitCode != 0) {
        QFile error(errorPath);
        const QString detail = error.open(QIODevice::ReadOnly)
            ? QString::fromUtf8(error.readAll()).trimmed() : QString();
        fail(detail.isEmpty() ? QStringLiteral("Windows radio transfer helper failed (exit %1).").arg(exitCode)
                              : detail);
    }
}

} // namespace

RadioExecutor::Bytes WindowsRadioTransfer::read(const RadioDefinition &definition,
                                                 const QString &definitionPath,
                                                 const RadioExecutor::Progress &progress,
                                                 const std::atomic_bool &cancelled)
{
    try {
        return RadioExecutor::read(definition, progress, cancelled);
    } catch (const WindowsElevationRequired &) {
        QTemporaryDir dir = temporaryDirectory();
        const QString imagePath = dir.filePath(QStringLiteral("image.bin"));
        run(QStringLiteral("read"), definitionPath, imagePath,
            dir.filePath(QStringLiteral("progress.json")), dir.filePath(QStringLiteral("cancel")),
            dir.filePath(QStringLiteral("error.txt")), progress, &cancelled);
        QFile image(imagePath);
        if (!image.open(QIODevice::ReadOnly)) fail(QStringLiteral("Could not open Windows radio read image."));
        const QByteArray data = image.readAll();
        if (data.isEmpty()) fail(QStringLiteral("Windows radio helper returned an empty image."));
        return RadioExecutor::Bytes(data.begin(), data.end());
    }
}

void WindowsRadioTransfer::write(const RadioDefinition &definition, const QString &definitionPath,
                                 const QJsonObject &codeplugDefinition,
                                 const RadioExecutor::Bytes &image,
                                 const RadioExecutor::Progress &progress)
{
    try {
        RadioExecutor::write(definition, codeplugDefinition, image, progress);
    } catch (const WindowsElevationRequired &) {
        QTemporaryDir dir = temporaryDirectory();
        const QString imagePath = dir.filePath(QStringLiteral("image.bin"));
        save(imagePath, image);
        run(QStringLiteral("write"), definitionPath, imagePath,
            dir.filePath(QStringLiteral("progress.json")), QStringLiteral("-"),
            dir.filePath(QStringLiteral("error.txt")), progress, nullptr);
    }
}
