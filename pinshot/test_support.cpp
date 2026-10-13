#include "test_support.h"
#include <QtMath>
#include <QTimer>
#include <QEventLoop>
#include <QThread>
#include <QElapsedTimer>
#include <qt_windows.h>

ClipboardBlocker::ClipboardBlocker()
{
    m_thread = QThread::create([this] {
        HWND window = CreateWindowExW(0, L"STATIC", L"clipboard regression", 0, 0, 0, 1, 1,
                                      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        QElapsedTimer retry;
        retry.start();
        while (window && !(m_locked = OpenClipboard(window)) && retry.elapsed() < 1000)
            QThread::msleep(15);
        m_ready.release();
        m_release.acquire();
        if (m_locked) CloseClipboard();
        if (window) DestroyWindow(window);
    });
    m_thread->start();
    while (!m_ready.tryAcquire()) {
        QEventLoop wait;
        QTimer::singleShot(15, &wait, &QEventLoop::quit);
        wait.exec(QEventLoop::ExcludeUserInputEvents);
    }
}

ClipboardBlocker::~ClipboardBlocker() { m_release.release(); m_thread->wait(); delete m_thread; }

bool ClipboardBlocker::locked() const { return m_locked; }
