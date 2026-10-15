#pragma once

#include "capture_overlay.h"
#include "pin_window.h"
#include "long_capture.h"
#include <QObject>
#include <QByteArray>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QtMath>
#include <QAbstractNativeEventFilter>
#include <QPointer>
#include <qt_windows.h>
#include <functional>

// ---------- F1 global hotkey filter ----------
class HotkeyFilter : public QAbstractNativeEventFilter
{
public:
    std::function<void()> onHotkey;
    bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override;
};

class Controller : public QObject
{
    Q_OBJECT
public:
    Controller();
public slots:
    void startCapture();
private:
    QMenu m_menu;                                    // Declared first so the tray releases it before destruction.
    QSystemTrayIcon m_tray;
    QPointer<CaptureOverlay> m_capture;
};
