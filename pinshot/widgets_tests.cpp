#include "widgets.h"
#include "output.h"
#include "test_support.h"
#include "pin_window.h"
#include "long_capture.h"
#include "ocr_dialog.h"
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QColor>
#include <QApplication>
#include <QMouseEvent>
#include <QtMath>
#include <qt_windows.h>
#include <cstdio>
#include <cstdlib>

int ColorBoard::testRendering()
{
    auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    };
    for (int i = 0; i <= int(Icon::MarkerRect); ++i) {     // Every glyph loads from the resource and draws ink.
        const QImage img = makeIcon(Icon(i)).pixmap(QSize(16, 16)).toImage();
        bool ink = false;
        for (int y = 0; y < img.height() && !ink; ++y)
            for (int x = 0; x < img.width() && !ink; ++x) ink = qAlpha(img.pixel(x, y)) > 128;
        check(ink, "every toolbar icon renders from its SVG resource");
    }
    ColorBoard board(QColor("#ed1c24"));
    board.show();
    QApplication::processEvents();
    check(board.currentRect() == QRect(1, 1, 24, 24) && board.focusPolicy() == Qt::NoFocus
              && board.cursor().shape() == Qt::PointingHandCursor && board.accessibleName() == QStringLiteral("颜色"),
          "current color square is 24x24 and the board never takes focus");
    for (int i = 0; i < PresetCount; ++i) {
        const QRect r = swatchRect(i);
        check(r.size() == QSize(11, 11) && board.rect().contains(r.adjusted(-1, -1, 1, 1))
                  && !r.intersects(board.currentRect()) && board.swatchAt(r.center()) == i,
              "20 preset cells are 11x11, inside the board, and hit-test to their own index");
        if (i % 10) check(r.left() - swatchRect(i - 1).right() == 3 && board.swatchAt(r.topLeft() - QPoint(1, 0)) == -1,
                          "preset columns have a 2px gap that picks nothing");
    }
    check(swatchRect(10).top() - swatchRect(0).bottom() == 3 && preset(0) == QColor("#000000")
              && preset(3) == QColor("#ed1c24") && preset(10) == QColor("#ffffff") && preset(19) == QColor("#c8bfe7"),
          "two rows of the expected preset colors");
    auto click = [&board](const QPoint &at) {
        for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, QPointF(at), QPointF(board.mapToGlobal(at)), Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&board, &event);
        }
    };
    QColor picked;
    int custom = 0;
    QObject::connect(&board, &ColorBoard::presetPicked, [&picked](const QColor &c) { picked = c; });
    QObject::connect(&board, &ColorBoard::customRequested, [&custom] { ++custom; });
    click(swatchRect(7).center());
    check(picked == QColor("#00a2e8") && custom == 0, "a preset click emits its color");
    picked = QColor();
    click(board.currentRect().center());
    check(custom == 1 && !picked.isValid(), "the current color square requests the full color dialog");
    click(swatchRect(1).topRight() + QPoint(2, 0));
    check(custom == 1 && !picked.isValid(), "clicking a gap does nothing");
    const qreal dpr = board.devicePixelRatioF();
    auto pixel = [&board, dpr](const QPoint &at) {
        const QImage image = board.grab().toImage();
        return image.pixelColor(qFloor((at.x() + 0.5) * dpr), qFloor((at.y() + 0.5) * dpr));
    };
    const QRect cell = swatchRect(4);
    check(pixel(cell.topLeft() - QPoint(1, 1)) != QColor(0x40, 0x40, 0x40), "an idle cell does not draw the hover frame");
    QMouseEvent hover(QEvent::MouseMove, QPointF(cell.center()), QPointF(board.mapToGlobal(cell.center())),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&board, &hover);
    check(pixel(cell.topLeft() - QPoint(1, 1)) == QColor(0x40, 0x40, 0x40),
          "the hovered cell grows by 1px with a darker frame");
    board.setCurrentColor(QColor("#3f48cc"));
    check(board.currentColor() == QColor("#3f48cc") && pixel(QPoint(6, 6)) == QColor("#3f48cc")
              && pixel(QPoint(1, 1)) == QColor(0x22, 0x22, 0x22),
          "the current square repaints the new color inside a dark frame");
    std::printf("PASS: color board geometry, hit testing, presets, custom request, and hover at DPR %.2f\n", double(dpr));
    return 0;
}
