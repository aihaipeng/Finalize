#pragma once

#include "output.h"
#include <QtMath>
#include <QThread>
#include <QSemaphore>
#include <qt_windows.h>

// Native clipboard contention fixture; the lock must belong to a different thread.
class ClipboardBlocker
{
public:
    ClipboardBlocker();
    ~ClipboardBlocker();
    bool locked() const;
private:
    QThread *m_thread = nullptr;
    QSemaphore m_ready, m_release;
    bool m_locked = false;
};
