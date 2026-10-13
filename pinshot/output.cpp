#include "output.h"
#include <QGuiApplication>
#include <QImage>
#include <QString>
#include <QByteArray>
#include <QtMath>
#include <QClipboard>
#include <QTimer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSaveFile>
#include <qt_windows.h>

bool copyImageToClipboard(const QImage &image)
{
    if (image.isNull()) return false;
    QClipboard *clipboard = QGuiApplication::clipboard();
    QElapsedTimer retry;
    retry.start();
    do {
        const DWORD sequence = GetClipboardSequenceNumber();
        clipboard->setImage(image);
        if (clipboard->ownsClipboard() && GetClipboardSequenceNumber() != sequence) return true;
        QEventLoop wait;
        QTimer::singleShot(15, &wait, &QEventLoop::quit);
        wait.exec(QEventLoop::ExcludeUserInputEvents); // Clipboard and IME owners need system events to release contention.
    } while (retry.elapsed() < 300);
    return false;
}

bool copyTextToClipboard(const QString &text)
{
    if (text.isEmpty()) return false;
    QClipboard *clipboard = QGuiApplication::clipboard();
    QElapsedTimer retry;
    retry.start();
    do {
        const DWORD sequence = GetClipboardSequenceNumber();
        clipboard->setText(text);
        if (clipboard->ownsClipboard() && GetClipboardSequenceNumber() != sequence) return true;
        QEventLoop wait;
        QTimer::singleShot(15, &wait, &QEventLoop::quit);
        wait.exec(QEventLoop::ExcludeUserInputEvents);
    } while (retry.elapsed() < 300);
    return false;
}

bool saveImageAtomic(const QImage &image, const QString &path)
{
    if (image.isNull()) return false;
    QSaveFile file(path);
    const QByteArray format = QFileInfo(path).suffix().toLatin1();
    if (format.isEmpty() || !file.open(QIODevice::WriteOnly)) return false;
    return image.save(&file, format.constData()) && file.commit();
}
