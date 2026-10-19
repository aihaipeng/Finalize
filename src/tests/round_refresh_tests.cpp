#include "capture/capture_overlay.h"
#include "shared/widgets.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QScreen>
#include "shared/settings.h"
#include <QSettings>
#include <QSlider>
#include <QThread>
#include <QToolButton>
#include <QVariant>
#include <QVariantAnimation>
#include <cstdio>
#include <cstdlib>

namespace {

QImage solid(const QSize &size, const QColor &color)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    return image;
}

void waitFor(const std::function<bool()> &done, int timeoutMs = 4000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QThread::msleep(10);
    }
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

} // namespace

int CaptureOverlay::testRoundRefresh(const QString &previewPath)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    };

    NamiSettings settings;
    const QVariant oldEnabled = settings.value(QStringLiteral("Capture/roundEnabled"));
    const QVariant oldRadius = settings.value(QStringLiteral("Capture/roundRadius"));

    const QRect screen = QGuiApplication::primaryScreen()->geometry();
    const QSize size(180, 120);
    const QRect logical(screen.topLeft() + QPoint(300, 200), size);
    const ScreenFrame whiteFrame{logical, QRect(QPoint(0, 0), size), solid(size, Qt::white)};
    CaptureOverlay overlay({whiteFrame});
    overlay.setAttribute(Qt::WA_DeleteOnClose, false);
    const BOOL affinity = SetWindowDisplayAffinity(reinterpret_cast<HWND>(overlay.winId()),
                                                    WDA_EXCLUDEFROMCAPTURE);
    SetWindowDisplayAffinity(reinterpret_cast<HWND>(overlay.winId()), WDA_NONE);
    check(affinity != FALSE, "the overlay can stay visible while excluded from the refresh capture");
    overlay.m_mode = Adjusting;
    overlay.m_sel = overlay.localScreenRect(logical);

    overlay.m_roundEnabled = true;
    overlay.m_roundRadius = 24;
    overlay.applyRoundSettings(false);
    const QImage rounded = overlay.cropped().toImage();
    check(rounded.size() == size, "rounded crop keeps the selection size");
    check(rounded.pixelColor(0, 0).alpha() == 0 && rounded.pixelColor(size.width() - 1, 0).alpha() == 0
              && rounded.pixelColor(size.width() / 2, size.height() / 2).alpha() == 255,
          "rounded crop clears the four corners and keeps the centre opaque");

    Annotation cover;
    cover.tool = ATool::Rect;
    cover.rect = overlay.m_sel.adjusted(2, 2, -2, -2);
    cover.color = Qt::black;
    cover.fill = true;
    overlay.m_anns.append(cover);
    const QImage annotated = overlay.cropped().toImage();
    check(annotated.pixelColor(0, 0).alpha() == 0
              && annotated.pixelColor(size.width() / 2, size.height() / 2).alpha() == 255,
          "rounded clip also applies to annotations");
    overlay.m_anns.clear();

    overlay.m_roundRadius = 0;
    const QImage square = overlay.cropped().toImage();
    check(square.pixelColor(0, 0).alpha() == 255,
          "radius zero keeps square corners");
    overlay.m_roundRadius = 24;

    auto *round = overlay.findChild<QToolButton *>(QStringLiteral("roundButton"));
    auto *refresh = overlay.findChild<QToolButton *>(QStringLiteral("refreshButton"));
    check(round && refresh, "capture toolbar exposes round and refresh actions");
    check(refresh->toolTip() == QStringLiteral("刷新截图（按住持续刷新）"),
          "refresh tooltip documents hold-to-refresh");
    check(round->isChecked(), "loaded round setting is reflected by the button");

    overlay.m_roundEnabled = false;
    overlay.applyRoundSettings(false);
    round->click();
    auto *slider = overlay.findChild<QSlider *>(QStringLiteral("roundRadiusSlider"));
    check(overlay.m_roundEnabled && slider && slider->minimum() == 0 && slider->maximum() == 60,
          "rounding button opens the 0..60 radius slider");
    slider->setValue(12);
    check(overlay.m_roundRadius == 12 && overlay.m_roundEnabled,
          "moving the slider stores the radius and enables rounding");
    slider->setValue(0);
    const QImage zeroRadius = overlay.cropped().toImage();
    check(zeroRadius.pixelColor(0, 0).alpha() == 255,
          "slider zero returns to square corners without losing the stored value");
    slider->setValue(12);
    if (!previewPath.isEmpty()) {
        overlay.placeToolbar();
        overlay.showToolBars();
        overlay.grab().save(previewPath + QStringLiteral("-round-toolbar.png"));
        overlay.cropped().save(previewPath + QStringLiteral("-round-crop.png"));
    }
    if (overlay.m_radiusPopup) overlay.m_radiusPopup->close();

    // Refresh replaces only the frozen frame: the committed selection, its
    // annotations and the armed tool must stay exactly as the user left them.
    overlay.m_roundEnabled = true;
    overlay.m_roundRadius = 12;
    overlay.m_anns.append(Annotation());
    overlay.m_undoStack.append(overlay.m_anns);
    const QRect keptSelection = overlay.m_sel;
    const QRect localSelection = overlay.localScreenRect(logical);
    overlay.m_hover = QRect(localSelection.topLeft(), QSize(10, 10));
    const ScreenFrame redFrame{logical, QRect(QPoint(0, 0), size), solid(size, Qt::red)};
    overlay.m_frameSource = [redFrame] { return QVector<ScreenFrame>{redFrame}; };
    overlay.refreshScreenshot();
    waitFor([&] { return !overlay.m_refreshing && overlay.isVisible(); });
    check(overlay.isVisible() && overlay.m_sel == keptSelection && overlay.m_anns.size() == 1
              && overlay.m_undoStack.size() == 1 && overlay.m_mode == Adjusting
              && overlay.m_hover.isNull() && overlay.m_roundEnabled && overlay.m_roundRadius == 12,
          "refresh keeps the selection/annotations, clears hover, keeps rounding, and returns visible");
    const qreal dpr = overlay.m_shot.devicePixelRatio();
    const QPoint shotCenter(qRound(localSelection.center().x() * dpr),
                            qRound(localSelection.center().y() * dpr));
    check(overlay.m_shot.toImage().pixelColor(shotCenter).red() > 200,
          "refresh replaces the frozen desktop pixels");

    const ScreenFrame greenFrame{logical, QRect(QPoint(0, 0), size), solid(size, Qt::green)};
    overlay.m_frameSource = [greenFrame] { return QVector<ScreenFrame>{greenFrame}; };
    QKeyEvent f5(QEvent::KeyPress, Qt::Key_F5, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &f5);
    waitFor([&] { return !overlay.m_refreshing && overlay.isVisible(); });
    check(overlay.isVisible() && overlay.m_sel == keptSelection && overlay.m_anns.size() == 1
              && overlay.m_shot.toImage().pixelColor(shotCenter).green() > 200,
          "F5 keeps the selection and replaces the frozen pixels");

    refresh->click();
    waitFor([&] { return !overlay.m_refreshing && overlay.isVisible(); });
    check(overlay.m_sel == keptSelection && overlay.m_anns.size() == 1 && !refresh->isChecked(),
          "a normal refresh click keeps the frame and releases the momentary button");

    overlay.beginDesktopPreview();
    check(overlay.m_desktopPreview && overlay.m_sel == keptSelection && refresh->isChecked()
              && overlay.m_refreshAnimation
              && overlay.m_refreshAnimation->state() == QAbstractAnimation::Running,
          "holding refresh keeps the frame and spins the icon over the live desktop");
    if (!previewPath.isEmpty())
        overlay.grab().save(previewPath + QStringLiteral("-hold-preview.png"));
    overlay.m_refreshAnimation->stop();
    overlay.m_desktopPreview = false;
    refresh->setChecked(false);

    overlay.close();
    if (oldEnabled.isValid()) settings.setValue(QStringLiteral("Capture/roundEnabled"), oldEnabled);
    else settings.remove(QStringLiteral("Capture/roundEnabled"));
    if (oldRadius.isValid()) settings.setValue(QStringLiteral("Capture/roundRadius"), oldRadius);
    else settings.remove(QStringLiteral("Capture/roundRadius"));
    settings.sync();
    std::puts("PASS: rounded crop, radius slider, selection-preserving refresh, hold preview and F5");
    return 0;
}
