#include "controller.h"
#include <QGuiApplication>
#include <QPixmap>
#include <QVector>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QAction>
#include <QApplication>
#include <QScreen>
#include <QMenu>
#include <QtMath>
#include <QMessageBox>
#include <QSettings>
#include <QInputDialog>
#include <QPointer>
#include <qt_windows.h>

bool HotkeyFilter::nativeEventFilter(const QByteArray &, void *message, qintptr *)
{
    MSG *msg = static_cast<MSG *>(message);
    if (msg->message == WM_HOTKEY && msg->wParam == 1 && onHotkey)
        onHotkey();
    return false;
}

Controller::Controller()
{
    m_tray.setIcon(QApplication::windowIcon());
    m_tray.setToolTip(QStringLiteral("pinshot — F1 截图"));
    QMenu *m = &m_menu;
    m->addAction(QStringLiteral("截图 (F1)"), this, &Controller::startCapture);
    QMenu *detection = m->addMenu(QStringLiteral("窗口/UI 检测"));
    QSettings settings(QStringLiteral("pinshot"), QStringLiteral("pinshot"));
    for (const auto &entry : {qMakePair(QStringLiteral("自动检测窗口"), QStringLiteral("detect_windows")),
                              qMakePair(QStringLiteral("自动检测 UI 元素"), QStringLiteral("detect_ui_elements"))}) {
        QAction *action = detection->addAction(entry.first);
        action->setCheckable(true);
        action->setChecked(settings.value(QStringLiteral("Snip/") + entry.second, entry.second == QStringLiteral("detect_windows")).toBool());
        const QString key = entry.second;
        connect(action, &QAction::toggled, this, [key](bool enabled) {
            QSettings(QStringLiteral("pinshot"), QStringLiteral("pinshot")).setValue(QStringLiteral("Snip/") + key, enabled);
        });
    }
    detection->addAction(QStringLiteral("排除程序…"), this, [] {
        QSettings settings(QStringLiteral("pinshot"), QStringLiteral("pinshot"));
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(nullptr, QStringLiteral("排除窗口检测"),
            QStringLiteral("每行一个程序文件名，例如 example.exe（不区分大小写，下次截图生效）："),
            settings.value(QStringLiteral("Snip/exclude_from_detection")).toStringList().join(QLatin1Char('\n')), &ok);
        if (!ok) return;
        QStringList names;
        for (const QString &line : text.split(QLatin1Char('\n'))) {
            const QString name = line.trimmed().toLower();
            if (!name.isEmpty() && !names.contains(name)) names.append(name);
        }
        settings.setValue(QStringLiteral("Snip/exclude_from_detection"), names);
    });
    m->addSeparator();
    m->addAction(QStringLiteral("退出"), qApp, &QApplication::quit);
    m_tray.setContextMenu(m);
    m_tray.show();
}

void Controller::startCapture()
{
    if (m_capture && m_capture->isVisible()) { m_capture->raise(); m_capture->activateWindow(); return; }
    const QVector<ScreenFrame> frames = freezeScreens();   // Freeze before showing the selection overlay.
    if (frames.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("pinshot"), QStringLiteral("无法读取屏幕像素"));
        return;
    }
    QSettings settings(QStringLiteral("pinshot"), QStringLiteral("pinshot"));
    DetectionOptions options;
    options.windows = settings.value(QStringLiteral("Snip/detect_windows"), true).toBool();
    options.elements = settings.value(QStringLiteral("Snip/detect_ui_elements"), false).toBool();
    options.excludedPrograms = settings.value(QStringLiteral("Snip/exclude_from_detection")).toStringList();
    options.disabledUiPrograms = settings.value(QStringLiteral("Snip/crashes")).toStringList();
    auto *ov = new CaptureOverlay(frames, {}, options, true);
    m_capture = ov;
    connect(ov, &CaptureOverlay::grabbed, this, [](const QPixmap &pix, const QRect &region) {
        if (!pix.isNull() && !region.isEmpty()) new PinWindow(pix, region);
    });
    connect(ov, &CaptureOverlay::longRequested, this, [](const QRect &region) {
        auto *lc = new LongCapture(region);
        connect(lc, &LongCapture::finished, lc, [lc](const QPixmap &pix, bool edit) {
            const QRect region = lc->region();   // The selection may have moved along the scroll axis.
            const QScreen *scr = QGuiApplication::screenAt(region.center());
            if (!scr) scr = QGuiApplication::primaryScreen();
            // The pin and the editor share one centered, proportionally fitted placement.
            auto *pin = new PinWindow(pix, LongCapture::editorGeometry(pix.size(), scr->devicePixelRatio(),
                                                                       scr->availableGeometry()));
            if (!edit)
                return;
            // The editor can come back to the still-open capture.
            QPointer<LongCapture> session(lc);
            connect(pin, &PinWindow::editorLeft, lc, [lc](bool back) { back ? lc->resume() : lc->cancel(); });
            connect(pin, &QObject::destroyed, lc, [session] { if (session && session->suspended()) session->cancel(); });
            pin->openEditor(true);
        });
        lc->start();
    });
}
