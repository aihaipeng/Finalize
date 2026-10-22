#pragma once

#include "capture/capture_overlay.h"
#include "pin/pin_window.h"
#include "longshot/long_capture.h"
#include "history/history_dialog.h"
#include <QObject>
#include <QByteArray>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
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
    static int testThemeUi(const QString &previewPath);
public slots:
    void startCapture();
    void openHistory();
private:
    QMenu m_menu;                                    // Declared first so the tray releases it before destruction.
    QSystemTrayIcon m_tray;
    QPointer<CaptureOverlay> m_capture;
    QPointer<HistoryDialog> m_history;
    QAction *m_historyEnabled = nullptr;
    bool m_startingCapture = false;
};
