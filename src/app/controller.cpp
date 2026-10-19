#include "app/controller.h"
#include "shared/dialog_chrome.h"
#include "shared/i18n.h"
#include "shared/settings.h"
#include "shared/widgets.h"
#include "history/history_store.h"
#include <QGuiApplication>
#include <QCursor>
#include <QPixmap>
#include <QSizeF>
#include <QSignalBlocker>
#include <QVector>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QAction>
#include <QActionGroup>
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
    m_tray.setToolTip(tr("Nami · F1 截图"));
    QMenu *m = &m_menu;
    m->setStyleSheet(darkMenuQss);
    m->addAction(tr("截图 (F1)"), this, &Controller::startCapture);
    m->addAction(tr("截图历史…"), this, &Controller::openHistory);
    m_historyEnabled = m->addAction(tr("记录截图历史"));
    m_historyEnabled->setCheckable(true);
    m_historyEnabled->setChecked(HistoryStore::enabled());
    m_historyEnabled->setToolTip(tr("默认关闭。开启后，未保存的截图也会复制到本机历史。"));
    connect(m_historyEnabled, &QAction::toggled, this, [this](bool enabled) {
        HistoryStore::setEnabled(enabled);
        if (m_history) m_history->setRecordingEnabled(enabled);
    });
    QMenu *detection = m->addMenu(tr("窗口/UI 检测"));
    NamiSettings settings;
    for (const auto &entry : {qMakePair(tr("自动检测窗口"), QStringLiteral("detect_windows")),
                              qMakePair(tr("自动检测 UI 元素"), QStringLiteral("detect_ui_elements"))}) {
        QAction *action = detection->addAction(entry.first);
        action->setCheckable(true);
        action->setChecked(settings.value(QStringLiteral("Snip/") + entry.second, entry.second == QStringLiteral("detect_windows")).toBool());
        const QString key = entry.second;
        connect(action, &QAction::toggled, this, [key](bool enabled) {
            NamiSettings().setValue(QStringLiteral("Snip/") + key, enabled);
        });
    }
    detection->addAction(tr("排除程序…"), this, [] {
        NamiSettings settings;
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(nullptr, Controller::tr("排除窗口检测"),
            Controller::tr("每行一个程序文件名，例如 example.exe（不区分大小写，下次截图生效）："),
            settings.value(QStringLiteral("Snip/exclude_from_detection")).toStringList().join(QLatin1Char('\n')), &ok);
        if (!ok) return;
        QStringList names;
        for (const QString &line : text.split(QLatin1Char('\n'))) {
            const QString name = line.trimmed().toLower();
            if (!name.isEmpty() && !names.contains(name)) names.append(name);
        }
        settings.setValue(QStringLiteral("Snip/exclude_from_detection"), names);
    });
    QMenu *language = m->addMenu(tr("语言"));
    QActionGroup *languageGroup = new QActionGroup(language);
    languageGroup->setExclusive(true);
    const QString configured = I18n::configuredLocale();
    const auto addLanguage = [&](const QString &locale) {
        QAction *action = language->addAction(I18n::displayName(locale));
        action->setCheckable(true);
        action->setChecked(configured == locale);
        languageGroup->addAction(action);
        connect(action, &QAction::triggered, this, [locale] {
            I18n::saveLocale(locale);
            showNotice(nullptr, QCoreApplication::translate("Controller", "语言已保存"),
                       QCoreApplication::translate("Controller", "将在下次启动 Nami 时生效。"));
        });
    };
    addLanguage(QStringLiteral("system"));
    for (const QString &locale : I18n::availableLocales())
        if (locale != QLatin1String("system")) addLanguage(locale);
    m->addSeparator();
    m->addAction(tr("退出"), qApp, &QApplication::quit);
    const QList<QAction *> actions=m->actions();
    actions[0]->setIcon(barIcon(Icon::Crop));actions[0]->setText(tr("截图")+QStringLiteral("\tF1"));
    actions[1]->setIcon(barIcon(Icon::LongShot));
    detection->setIcon(barIcon(Icon::View));language->setIcon(barIcon(Icon::Translate));
    actions.last()->setIcon(barIcon(Icon::Cancel));
    m_tray.setContextMenu(m);
    m_tray.show();
}

void Controller::startCapture()
{
    if (m_capture && m_capture->isVisible()) { m_capture->raise(); m_capture->activateWindow(); return; }
    const QVector<ScreenFrame> frames = freezeScreens();   // Freeze before showing the selection overlay.
    if (frames.isEmpty()) {
        QMessageBox::warning(nullptr, QStringLiteral("Nami"), tr("无法读取屏幕像素"));
        return;
    }
    NamiSettings settings;
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
    connect(ov, &CaptureOverlay::captured, this, [](const QPixmap &pix, const QRect &) {
        if (!pix.isNull()) HistoryStore::record(pix.toImage(), QStringLiteral("capture"));
    });
    connect(ov, &CaptureOverlay::longRequested, this, [](const QRect &region) {
        auto *lc = new LongCapture(region);
        connect(lc, &LongCapture::captured, lc, [](const QImage &image) {
            HistoryStore::record(image, QStringLiteral("longshot"));
        });
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

void Controller::openHistory()
{
    if (m_history) {
        m_history->raise();
        m_history->activateWindow();
        return;
    }

    auto *dialog = new HistoryDialog;
    m_history = dialog;
    connect(dialog, &HistoryDialog::pinRequested, this, [](const QImage &image) {
        if (image.isNull()) return;
        QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (!screen) return;
        const QRect available = screen->availableGeometry();
        const qreal dpr = image.devicePixelRatio() > 0 ? image.devicePixelRatio() : 1.0;
        QSizeF logical(image.width() / dpr, image.height() / dpr);
        const QSizeF limit(available.width() * 0.8, available.height() * 0.8);
        if (logical.width() > limit.width() || logical.height() > limit.height())
            logical.scale(limit, Qt::KeepAspectRatio);
        QRect rect(QPoint(0, 0), logical.toSize());
        rect.moveCenter(available.center());
        new PinWindow(QPixmap::fromImage(image), rect);
    });
    connect(dialog, &HistoryDialog::enabledChanged, this, [this](bool enabled) {
        if (!m_historyEnabled || m_historyEnabled->isChecked() == enabled) return;
        const QSignalBlocker blocker(m_historyEnabled);
        m_historyEnabled->setChecked(enabled);
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
