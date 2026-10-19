#include "platform/output.h"
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

template <typename Set>
static bool setClipboardWithRetry(Set set)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    QElapsedTimer retry;
    retry.start();
    do {
        const DWORD sequence = GetClipboardSequenceNumber();
        set(clipboard);
        if (clipboard->ownsClipboard() && GetClipboardSequenceNumber() != sequence) return true;
        QEventLoop wait;
        QTimer::singleShot(15, &wait, &QEventLoop::quit);
        wait.exec(QEventLoop::ExcludeUserInputEvents); // Clipboard and IME owners need system events to release contention.
    } while (retry.elapsed() < 300);
    return false;
}

bool copyImageToClipboard(const QImage &image)
{
    return !image.isNull() && setClipboardWithRetry([&](QClipboard *c) { c->setImage(image); });
}

bool copyTextToClipboard(const QString &text)
{
    return !text.isEmpty() && setClipboardWithRetry([&](QClipboard *c) { c->setText(text); });
}

bool saveImageAtomic(const QImage &image, const QString &path)
{
    if (image.isNull()) return false;
    QSaveFile file(path);
    const QByteArray format = QFileInfo(path).suffix().toLatin1();
    if (format.isEmpty() || !file.open(QIODevice::WriteOnly)) return false;
    return image.save(&file, format.constData()) && file.commit();
}
