#include "capture/capture_overlay.h"
#include "shared/settings.h"
#include "shared/widgets.h"
#include "tests/test_support.h"
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QAccessible>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <qt_windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

int CaptureOverlay::testWatermark(const QString &previewPath)
{
    auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    };
    const qint64 stamp = QDateTime(QDate(2026, 5, 14), QTime(9, 8, 7)).toMSecsSinceEpoch();
    check(watermarkString(QStringLiteral("A{{yyyy-MM-dd}}B"), stamp) == QStringLiteral("A2026-05-14B"),
          "watermark time tokens use the stored capture time");
    check(watermarkString(QStringLiteral("A{{yyyy"), stamp) == QStringLiteral("A{{yyyy"),
          "an unclosed token leaves the whole watermark literal");
    check(watermarkString(QStringLiteral("A{B}C"), stamp) == QStringLiteral("A{B}C"),
          "single braces stay literal");
    check(watermarkString(QStringLiteral("{{HH:mm:ss}}"), stamp) == QStringLiteral("09:08:07"),
          "watermark supports time-only tokens");
    check(watermarkString(QStringLiteral("{{yyyy:MM:dd HH:mm:ss}}"), stamp)
              == QStringLiteral("2026:05:14 09:08:07"),
          "watermark supports the EXIF date-time form");
    check(QRegularExpression(QStringLiteral("^2026-05-14T09:08:07[+-]\\d{2}:\\d{2}$"))
              .match(watermarkString(QStringLiteral("{{@iso-offset}}"), stamp)).hasMatch(),
          "ISO 8601 offset tokens append the local timezone offset");
    check(watermarkString(QStringLiteral("{{@iso-utc}}"), stamp).endsWith(QLatin1Char('Z')),
          "ISO 8601 UTC tokens use the Z designator");
    check(QRegularExpression(QStringLiteral("^2026-05-14T09:08:07\\.\\d{3}[+-]\\d{2}:\\d{2}$"))
              .match(watermarkString(QStringLiteral("{{@rfc3339-ms}}"), stamp)).hasMatch(),
          "RFC 3339 milliseconds keep the numeric offset");
    check(QRegularExpression(QStringLiteral(
              "^[A-Z][a-z]{2}, \\d{2} [A-Z][a-z]{2} 2026 \\d{2}:\\d{2}:\\d{2} GMT$"))
              .match(watermarkString(QStringLiteral("{{@rfc1123}}"), stamp)).hasMatch(),
          "RFC 1123 tokens use English names and GMT");
    check(QRegularExpression(QStringLiteral(
              "^[A-Z][a-z]{2}, \\d{2} [A-Z][a-z]{2} 2026 \\d{2}:\\d{2}:\\d{2} [+-]\\d{4}$"))
              .match(watermarkString(QStringLiteral("{{@rfc2822}}"), stamp)).hasMatch(),
          "RFC 2822 tokens append a numeric offset");

    // The widget callbacks persist the last-used style; restore the real settings afterwards.
    LensSnipSettings settings;
    const QStringList keys = {QStringLiteral("Watermark/Text"), QStringLiteral("Watermark/Color"),
        QStringLiteral("Watermark/FontFamily"), QStringLiteral("Watermark/FontSize"),
        QStringLiteral("Watermark/Opacity"), QStringLiteral("Watermark/Gap"),
        QStringLiteral("Watermark/Position"), QStringLiteral("Watermark/Angle"),
        QStringLiteral("Watermark/AutoAdd"), QStringLiteral("Watermark/LastTimeFormat"),
        QStringLiteral("Watermark/DefaultsVersion")};
    QHash<QString, QVariant> saved;
    for (const QString &key : keys) saved.insert(key, settings.value(key));
    struct Restore {
        LensSnipSettings &s; const QStringList &keys; const QHash<QString, QVariant> &values;
        ~Restore() { for (const QString &key : keys) s.setValue(key, values.value(key)); }
    } restore{settings, keys, saved};

    const QRect screen = QGuiApplication::primaryScreen()->geometry();
    QImage image(720, 480, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#252a33")));
    settings.setValue(QStringLiteral("Watermark/Text"),
                      QStringLiteral("$yyyy-MM-dd$艾海鹏"));
    settings.setValue(QStringLiteral("Watermark/FontFamily"), QStringLiteral("仿宋"));
    settings.setValue(QStringLiteral("Watermark/FontSize"), 47);
    settings.setValue(QStringLiteral("Watermark/LastTimeFormat"), QStringLiteral("yyyy-MM-dd"));
    settings.setValue(QStringLiteral("Watermark/Opacity"), 51);
    settings.setValue(QStringLiteral("Watermark/Gap"), 0);
    settings.setValue(QStringLiteral("Watermark/Color"), QStringLiteral("#ff00ff00"));
    settings.remove(QStringLiteral("Watermark/DefaultsVersion"));
    CaptureOverlay overlay({ScreenFrame{QRect(screen.topLeft(), QSize(720, 480)),
                                        QRect(0, 0, image.width(), image.height()), image}});
    overlay.setAttribute(Qt::WA_DeleteOnClose, false);
    overlay.show();
    QApplication::processEvents();                 // Settle the native origin before creating local fixture coordinates.
    check(overlay.m_wmText == QStringLiteral("{{yyyy-MM-dd}}艾海鹏"),
          "legacy dollar tokens migrate to double braces");
    check(overlay.m_wmFamily == QStringLiteral("Microsoft YaHei"),
          "watermark defaults to Microsoft YaHei");
    check(overlay.m_wmSize == 20, "watermark defaults to 20pt");
    check(overlay.m_wmOpacity == 30, "watermark defaults to 30 percent opacity");
    check(overlay.m_wmGap == 30, "watermark defaults to 30 percent tile gap");
    check(overlay.m_wmColor == QColor(QStringLiteral("#ed1c24")),
          "watermark defaults to red");
    check(settings.value(QStringLiteral("Watermark/LastTimeFormat")).toString()
              == QStringLiteral("yyyy-MM-dd HH:mm:ss"),
          "watermark time format defaults to date and time");
    check(overlay.m_toolSide.value(QStringLiteral("watermark")) == QStringLiteral("hidden"),
          "watermark starts in the secondary toolbar area");
    overlay.selectTool(ATool::Watermark);
    auto *emptyEdit = overlay.m_toolOptions->findChild<QLineEdit *>(QStringLiteral("watermarkText"));
    check(emptyEdit && emptyEdit->text().isEmpty(),
          "a new watermark input starts empty");
    overlay.selectTool(ATool::None);
    const QPoint origin = overlay.localScreenRect(screen).topLeft();
    overlay.m_mode = Adjusting;
    overlay.m_sel = QRect(origin + QPoint(40, 40), QSize(560, 360));
    const QImage base = overlay.cropped().toImage();

    overlay.m_wmText = QStringLiteral("内部资料 {{yyyy-MM-dd}}");
    overlay.m_wmSize = 22;
    overlay.m_wmOpacity = 100;
    overlay.m_wmGap = 0;
    overlay.m_wmPosition = 0;
    Annotation *wm = overlay.ensureWatermarkItem();
    check(wm != nullptr && wm->tool == ATool::Watermark, "watermark creates one text overlay item");
    wm->text = overlay.m_wmText;
    wm->font = 22;
    wm->watermarkOpacity = 100;
    wm->watermarkGap = 0;
    wm->watermarkPosition = 0;
    wm->createdAt = stamp;
    auto changed = [](const QImage &a, const QImage &b, const QRect &area = QRect()) {
        const QRect r = area.isNull() ? a.rect() : area;
        int count = 0;
        for (int y = r.top(); y <= r.bottom(); ++y)
            for (int x = r.left(); x <= r.right(); ++x)
                if (a.pixel(x, y) != b.pixel(x, y)) ++count;
        return count;
    };
    QImage marked = overlay.cropped().toImage();
    check(changed(base, marked) > 500, "tile watermark paints text into the output");
    wm->watermarkOpacity = 0;
    check(changed(base, overlay.cropped().toImage()) == 0, "zero opacity leaves the output untouched");
    wm->watermarkOpacity = 100;
    wm->watermarkPosition = 4;                   // Top-left: one tile, no repeat in the center.
    marked = overlay.cropped().toImage();
    check(changed(base, marked, QRect(0, 0, 180, 80)) > 100,
          "top-left watermark paints one tile at the anchor");
    check(changed(base, marked, QRect(210, 140, 140, 80)) == 0,
          "anchored watermark does not repeat across the image");
    wm->watermarkPosition = 7;                   // Center.
    marked = overlay.cropped().toImage();
    check(changed(base, marked, QRect(220, 150, 120, 60)) > 50,
          "center watermark paints at the selection center");

    overlay.selectTool(ATool::Watermark);
    auto *edit = overlay.m_toolOptions->findChild<QLineEdit *>(QStringLiteral("watermarkText"));
    auto *position = overlay.m_toolOptions->findChild<QComboBox *>(QStringLiteral("watermarkPosition"));
    auto *style = overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("watermarkStyle"));
    auto *font = overlay.m_toolOptions->findChild<QComboBox *>(QStringLiteral("watermarkFont"));
    check(edit && position && style && font,
          "watermark strip exposes content, position, style and font controls");
    check(edit->focusPolicy() == Qt::StrongFocus && position->focusPolicy() == Qt::StrongFocus
              && style->focusPolicy() == Qt::StrongFocus && font->focusPolicy() == Qt::StrongFocus,
          "watermark strip controls are reachable from the keyboard");
    check(edit->width() == 230, "the watermark content field leaves room for a full token");
    check(!overlay.m_toolOptions->findChild<QToolButton *>(QStringLiteral("watermarkAddTime")),
          "the content field replaces the separate add-time button");
    check(edit->isReadOnly(), "the strip content field opens the dialog instead of inline editing");
    edit->setText(QStringLiteral("HELLO {{yyyy}}"));
    check(overlay.watermark() && overlay.watermark()->text == QStringLiteral("HELLO {{yyyy}}"),
          "typing in the watermark field updates the overlay");
    check(edit->toolTip() == QStringLiteral("HELLO {{yyyy}}"),
          "the content field tooltip exposes the full token");
    QMetaObject::invokeMethod(position, "activated", Q_ARG(int, 4));
    check(overlay.watermark()->watermarkPosition == 4, "position menu updates the watermark anchor");
    position->showPopup();
    for (int i = 0; i < 4; ++i) QApplication::processEvents();
    const QImage openRows = position->view()->window()->grab().toImage();
    position->hidePopup();
    const QImage closedCombo = position->grab().toImage();
    const auto firstInkX = [](const QImage &shot, const QColor &background) {
        for (int x = 0; x < shot.width(); ++x)
            for (int y = 10; y <= 17; ++y)
                if (shot.pixelColor(x, y) != background) return x;
        return -1;
    };
    const int rowInk = firstInkX(openRows, Bar::surface);
    const int comboInk = firstInkX(closedCombo, Bar::content);
    check(qAbs(rowInk - comboInk) <= 2,
          "position popup rows share the closed combo's content inset");
    if (!previewPath.isEmpty()) {
        openRows.save(previewPath + QStringLiteral(".position-menu.png"));
        closedCombo.save(previewPath + QStringLiteral(".position-combo.png"));
    }
    style->click();
    check(overlay.m_watermarkStylePopup != nullptr, "watermark style button opens the floating panel");
    auto *wmColors = overlay.m_watermarkStylePopup->findChild<ColorBoard *>(QStringLiteral("watermarkColors"));
    check(wmColors && wmColors->commonCount() == 8 && wmColors->width() == 239
              && ColorBoard::swatchRect(7, 8).isValid()
              && !ColorBoard::swatchRect(8, 8).isValid(),
          "the watermark color row exposes eight common colors");
    auto sliders = overlay.m_watermarkStylePopup->findChildren<QSlider *>();
    check(sliders.size() >= 3, "watermark style panel has opacity, size and gap sliders");
    check(wmColors->focusPolicy() == Qt::StrongFocus
              && std::all_of(sliders.cbegin(), sliders.cend(), [](QSlider *slider) {
                     return slider->focusPolicy() == Qt::StrongFocus;
                 }),
          "watermark style sliders and colors accept keyboard focus");
    sliders.first()->setFocus();
    QApplication::processEvents();
    const QImage focusedSlider = sliders.first()->grab().toImage();
    bool halo = false;
    for (int y = 0; y < focusedSlider.height() && !halo; ++y)
        for (int x = 0; x < focusedSlider.width(); ++x)
            if (focusedSlider.pixelColor(x, y) == Bar::focusRing) { halo = true; break; }
    check(!halo, "a focused style slider draws no focus halo");
    if (!previewPath.isEmpty())
        overlay.m_watermarkStylePopup->grab().save(previewPath + QStringLiteral(".style-popup.png"));
    // Clicking the canvas with the watermark tool must not reopen the input card.
    bool reopenedCard = false;
    QTimer::singleShot(0, &overlay, [&reopenedCard] {
        if (QWidget *modal = QApplication::activeModalWidget()) {
            reopenedCard = true;
            modal->close();
        }
    });
    const QPoint insideSelection = overlay.m_sel.center();
    QMouseEvent watermarkPress(QEvent::MouseButtonPress, QPointF(insideSelection),
                               overlay.mapToGlobal(insideSelection), Qt::LeftButton,
                               Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &watermarkPress);
    QApplication::processEvents();
    check(!reopenedCard, "clicking the canvas with the watermark tool does not reopen the card");
    // The selection frame keeps its resize cursors while the watermark tool is armed.
    const QPoint topEdge(overlay.m_sel.center().x(), overlay.m_sel.top());
    QMouseEvent edgeMove(QEvent::MouseMove, QPointF(topEdge), overlay.mapToGlobal(topEdge),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &edgeMove);
    check(overlay.cursor().shape() == Qt::SizeVerCursor,
          "the selection frame shows its resize cursor with the watermark tool");
    QMouseEvent innerMove(QEvent::MouseMove, QPointF(insideSelection),
                          overlay.mapToGlobal(insideSelection), Qt::NoButton,
                          Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&overlay, &innerMove);
    check(overlay.cursor().shape() == Qt::ArrowCursor,
          "the watermark tool keeps the arrow cursor inside the selection");
    auto *gapRow = overlay.m_watermarkStylePopup->findChild<QWidget *>(QStringLiteral("watermarkGapRow"));
    check(gapRow && !gapRow->isVisible(), "gap control is hidden for an anchored watermark");
    QMetaObject::invokeMethod(position, "activated", Q_ARG(int, 0));
    check(gapRow && gapRow->isVisible(), "gap control appears when switching to tile mode");
    sliders.first()->setValue(55);
    check(overlay.watermark()->watermarkOpacity == 55, "opacity slider updates the watermark");
    if (!previewPath.isEmpty())
        check(overlay.m_watermarkStylePopup->grab().save(previewPath + QStringLiteral(".style.png")),
              "save watermark style popup preview");
    overlay.m_watermarkStylePopup->close();
    // Clicking the read-only content field opens the input dialog: a multiline
    // editor, a standards menu with submenus and Apply/Cancel.
    settings.setValue(QStringLiteral("Watermark/LastTimeFormat"),
                      QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    QTimer::singleShot(80, [&check, &overlay, previewPath] {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QDialog *>(w);
            if (!dialog || !dialog->isVisible()) continue;
            auto *text = dialog->findChild<QPlainTextEdit *>(QStringLiteral("watermarkInputText"));
            auto *timeButton = dialog->findChild<QPushButton *>(QStringLiteral("watermarkTimeFormat"));
            if (!text || !timeButton) continue;
            check(dialog->windowFlags().testFlag(Qt::WindowStaysOnTopHint),
                  "the input dialog requests the topmost window layer");
            check((GetWindowLongPtr(reinterpret_cast<HWND>(dialog->winId()), GWL_EXSTYLE)
                   & WS_EX_TOPMOST) != 0,
                  "the input dialog is actually topmost above the capture overlay");
            check(dialog->windowFlags().testFlag(Qt::FramelessWindowHint)
                      && dialog->findChild<QWidget *>(QStringLiteral("watermarkDialogSurface")),
                  "the input dialog uses the app card chrome");
            check(dialog->width() == 488, "the editor uses a compact 440px card plus shadow margins");
            auto *captionAtOpen = dialog->findChild<QLabel *>(QStringLiteral("watermarkTimeLabel"));
            check(timeButton->mapTo(dialog, QPoint()).x()
                      > captionAtOpen->mapTo(dialog, QPoint()).x() + captionAtOpen->width()
                      && timeButton->mapTo(dialog, QPoint()).x() + timeButton->width()
                             < dialog->findChild<QPushButton *>(
                                          QStringLiteral("watermarkCancel"))
                                        ->mapTo(dialog, QPoint()).x(),
                  "the time button sits between its caption and the cancel action");
            auto *defaultAction = dialog->findChild<QAction *>(QStringLiteral("watermarkFmt_iso_datetime"));
            check(defaultAction && defaultAction->isCheckable() && defaultAction->isChecked(),
                  "the time menu marks the current format");
            check(!dialog->findChild<QLabel *>(QStringLiteral("watermarkTimePreview")),
                  "the time dialog has no preview line");
            QRect barRect;
            for (QWidget *bar : {overlay.m_tools, overlay.m_toolsTop,
                                 overlay.m_toolsLeft, overlay.m_toolsRight,
                                 overlay.m_toolOptions}) {
                if (!bar || !bar->isVisible()) continue;
                const QRect rect = overlay.globalScreenRect(QRect(bar->mapTo(&overlay, QPoint()), bar->size()));
                barRect = barRect.isNull() ? rect : barRect.united(rect);
            }
            if (!barRect.isNull()) {
                if (!dialog->frameGeometry().intersects(barRect))
                    std::fprintf(stderr, "card placement: dialog=%d,%d %dx%d toolbar=%d,%d %dx%d\n",
                                 dialog->x(), dialog->y(), dialog->width(), dialog->height(),
                                 barRect.x(), barRect.y(), barRect.width(), barRect.height());
                check(dialog->frameGeometry().intersects(barRect),
                      "the input dialog opens over the toolbar");
            }
            check(text->toPlainText() == QStringLiteral("HELLO {{yyyy}}"),
                  "the input dialog opens with the current watermark text");
            check(QAccessible::queryAccessibleInterface(text)->text(QAccessible::Name)
                      == QStringLiteral("水印内容")
                      && timeButton->accessibleName().contains(QStringLiteral("插入时间")),
                  "watermark controls expose their purpose to assistive technology");
            const auto settle = [] { for (int i = 0; i < 4; ++i) QApplication::processEvents(); };
            const QImage field = text->viewport()->grab().toImage();
            check(field.pixelColor(field.width() - 10, field.height() - 10) == DialogTheme::field,
                  "the editor renders its dark field color without placeholder substitution drift");
            dialog->activateWindow();
            text->setFocus();
            settle();
            QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
            QApplication::sendEvent(text, &tab);
            check(dialog->focusWidget() == timeButton && !text->toPlainText().contains(QLatin1Char('\t')),
                  "Tab leaves the watermark editor without inserting a tab");
            QKeyEvent backTab(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
            QApplication::sendEvent(timeButton, &backTab);
            check(dialog->focusWidget() == text, "Shift+Tab returns from time insertion to the editor");
            const int initialHeight = text->height();
            const int initialDialogHeight = dialog->height();
            check(initialHeight >= text->fontMetrics().lineSpacing() * 5,
                  "the editor opens enlarged with five visible lines");
            text->setPlainText(QStringLiteral("一\n二\n三\n四\n五\n六"));
            settle();
            const int sixLineHeight = text->height();
            check(sixLineHeight > initialHeight && text->verticalScrollBar()->maximum() == 0,
                  "six lines grow the editor and remain fully visible");
            text->setPlainText(QStringLiteral("1\n2\n3\n4\n5\n6\n7\n8"));
            settle();
            const int eightLineHeight = text->height();
            check(eightLineHeight > sixLineHeight && text->verticalScrollBar()->maximum() == 0,
                  "eight lines grow the editor without clipping the last line");
            text->setPlainText(QStringLiteral("中文自动折行内容 ").repeated(35));
            settle();
            check(text->document()->firstBlock().layout()->lineCount() > 3
                      && text->height() > initialHeight,
                  "a wrapped paragraph also grows the editor");
            text->setPlainText(QStringLiteral("line\n").repeated(20));
            settle();
            check(text->height() <= eightLineHeight && text->verticalScrollBar()->maximum() > 0,
                  "content beyond eight lines scrolls inside the editor");
            text->setPlainText(QStringLiteral("HELLO {{yyyy}}"));
            text->moveCursor(QTextCursor::End);
            settle();
            check(text->height() == initialHeight && dialog->height() == initialDialogHeight,
                  "shrinking content restores the compact card without blank header space");
            for (QPushButton *button : dialog->findChildren<QPushButton *>()) {
                check(button->height() == 32, "watermark buttons use a consistent 32px control height");
                const QImage normal = button->grab().toImage();
                if (button->objectName() == QStringLiteral("watermarkApply"))
                    check(normal.pixelColor(normal.width() / 2, normal.height() - 6) == Bar::primaryFill,
                          "Apply renders the inverted primary fill");
                button->setDown(true);
                const QImage pressed = button->grab().toImage();
                button->setDown(false);
                check(normal != pressed, "watermark buttons show immediate pressed feedback");
            }
            const int timeCenterY = timeButton->mapTo(dialog, QPoint()).y() + timeButton->height() / 2;
            auto *cancelButton = dialog->findChild<QPushButton *>(QStringLiteral("watermarkCancel"));
            auto *applyButton = dialog->findChild<QPushButton *>(QStringLiteral("watermarkApply"));
            const auto rowCenterY = [dialog](QPushButton *button) {
                return button->mapTo(dialog, QPoint()).y() + button->height() / 2;
            };
            check(qAbs(timeCenterY - rowCenterY(cancelButton)) <= 2
                      && qAbs(timeCenterY - rowCenterY(applyButton)) <= 2
                      && rowCenterY(applyButton) > text->mapTo(dialog, QPoint()).y() + text->height(),
                  "time insertion and confirm actions share one aligned bottom row");
            check(timeButton->text().contains(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                  "the time button defaults to the ISO date-time format");
            auto *menu = timeButton->menu();
            check(menu != nullptr, "the time button opens a format menu");
            int isoItems = 0;
            int rfcItems = 0;
            int exifItems = 0;
            int httpItems = 0;
            int mailItems = 0;
            bool hasSeparator = false;
            for (QAction *action : menu->actions()) {
                if (action->isSeparator()) hasSeparator = true;
                if (!action->menu()) continue;
                if (action->menu()->title() == QStringLiteral("ISO 8601"))
                    isoItems = action->menu()->actions().size();
                if (action->menu()->title() == QStringLiteral("RFC 3339"))
                    rfcItems = action->menu()->actions().size();
                if (action->menu()->title() == QStringLiteral("EXIF 2.3"))
                    exifItems = action->menu()->actions().size();
                if (action->menu()->title() == QStringLiteral("RFC 1123"))
                    httpItems = action->menu()->actions().size();
                if (action->menu()->title() == QStringLiteral("RFC 2822"))
                    mailItems = action->menu()->actions().size();
            }
            check(!hasSeparator, "the time menu has no separator line");
            check(isoItems == 6, "ISO 8601 exposes six forms in its submenu");
            check(rfcItems == 2, "RFC 3339 has its own submenu");
            check(exifItems == 1, "EXIF 2.3 has its own single-item submenu");
            check(httpItems == 1, "RFC 1123 has its own single-item submenu");
            check(mailItems == 1, "RFC 2822 has its own single-item submenu");
            if (!previewPath.isEmpty()) {
                dialog->grab().save(previewPath + QStringLiteral(".time-dialog.png"));
                const QString content = text->toPlainText();
                text->clear();
                settle();
                dialog->grab().save(previewPath + QStringLiteral(".empty-dialog.png"));
                text->setPlainText(content);
                text->moveCursor(QTextCursor::End);
                settle();
                QMenu *isoMenu = nullptr;
                for (QAction *action : menu->actions())
                    if (action->menu() && action->menu()->title() == QStringLiteral("ISO 8601"))
                        isoMenu = action->menu();
                menu->popup(timeButton->mapToGlobal(QPoint(0, timeButton->height())));
                QApplication::processEvents();
                menu->grab().save(previewPath + QStringLiteral(".time-menu.png"));
                menu->hide();
                if (isoMenu) {
                    isoMenu->popup(timeButton->mapToGlobal(QPoint(0, timeButton->height())));
                    QApplication::processEvents();
                    isoMenu->grab().save(previewPath + QStringLiteral(".time-menu-iso.png"));
                    isoMenu->hide();
                }
            }
            auto *rfc = dialog->findChild<QAction *>(QStringLiteral("watermarkFmt_rfc3339"));
            check(rfc != nullptr, "the standards menu exposes RFC 3339");
            rfc->trigger();
            check(text->toPlainText() == QStringLiteral("HELLO {{yyyy}}{{@rfc3339}}"),
                  "picking a time standard inserts its token at the caret");
            const QString chosenShown = timeButton->text();
            const QString chosenPrefix = chosenShown.endsWith(QChar(0x2026)) ? chosenShown.chopped(1)
                                                                             : chosenShown;
            check(rfc->text().startsWith(chosenPrefix) && timeButton->toolTip() == rfc->text(),
                  "the time button elides the chosen format and keeps the full one in its tooltip");
            check(!timeButton->text().contains(QStringLiteral("▾")),
                  "the time button has no dropdown arrow");
            check(rfc->text() == QStringLiteral("yyyy-MM-dd'T'HH:mm:ss±hh:mm"),
                  "the menu item contains only the time format");
            check(rfc->isChecked() && !defaultAction->isChecked(),
                  "the time menu moves the selected mark to the chosen format");
            const QString textBeforeFormats = text->toPlainText();
            const auto rowCenterYOf = [dialog](QWidget *widget) {
                return widget->mapTo(dialog, QPoint()).y() + widget->height() / 2;
            };
            auto *timeCaption = dialog->findChild<QLabel *>(QStringLiteral("watermarkTimeLabel"));
            bool formatsLoseChars = false;
            bool captionMisaligned = false;
            bool buttonOutsideRow = false;
            for (QAction *group : menu->actions()) {
                if (!group->menu()) continue;
                for (QAction *item : group->menu()->actions()) {
                    item->trigger();
                    settle();
                    const QString shown = timeButton->text();
                    const QString full = item->text();
                    formatsLoseChars = formatsLoseChars
                        || !full.startsWith(shown.endsWith(QChar(0x2026)) ? shown.chopped(1) : shown)
                        || timeButton->toolTip() != full
                        || timeButton->fontMetrics().horizontalAdvance(shown) + 18 > timeButton->width();
                    captionMisaligned = captionMisaligned
                        || qAbs(rowCenterYOf(timeCaption) - rowCenterYOf(cancelButton)) > 2;
                    const int buttonRight = timeButton->mapTo(dialog, QPoint()).x() + timeButton->width();
                    buttonOutsideRow = buttonOutsideRow
                        || timeButton->mapTo(dialog, QPoint()).x()
                               <= timeCaption->mapTo(dialog, QPoint()).x() + timeCaption->width()
                        || buttonRight >= cancelButton->mapTo(dialog, QPoint()).x();
                }
            }
            check(!formatsLoseChars,
                  "switching to any time format shows the full pattern without clipping");
            check(!captionMisaligned,
                  "the time caption stays aligned with the actions for every format");
            check(!buttonOutsideRow,
                  "the time button stays between its caption and Cancel for every format");
            if (!previewPath.isEmpty()) {
                auto *longFormat = dialog->findChild<QAction *>(QStringLiteral("watermarkFmt_rfc3339_ms"));
                longFormat->trigger();
                settle();
                dialog->grab().save(previewPath + QStringLiteral(".time-dialog-long.png"));
            }
            defaultAction->trigger();
            settle();
            check(qAbs(rowCenterYOf(timeButton) - rowCenterYOf(timeCaption)) <= 2,
                  "a short format returns the time button to the single bottom row");
            rfc->trigger();
            text->setPlainText(textBeforeFormats);
            text->moveCursor(QTextCursor::End);
            for (int i = 0; i < 3; ++i) {
                QKeyEvent next(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
                QApplication::sendEvent(dialog->focusWidget(), &next);
            }
            auto *apply = dialog->findChild<QPushButton *>(QStringLiteral("watermarkApply"));
            check(dialog->focusWidget() == apply, "Tab traverses time insertion and Cancel to Apply");
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
            QApplication::sendEvent(apply, &press);
            QApplication::sendEvent(apply, &release);
            return;
        }
    });
    overlay.show();
    overlay.placeToolBars();
    overlay.showToolBars();
    QApplication::processEvents();
    edit->setFocus();
    overlay.showWatermarkInputDialog();
    check(overlay.focusWidget() == edit, "closing the card returns focus to its content trigger");
    check(overlay.watermark() && overlay.watermark()->text.contains(QStringLiteral("{{@rfc3339}}")),
          "applying the input dialog commits the watermark text");
    check(edit->text() == overlay.watermark()->text,
          "applying the input dialog syncs the strip content field");

    const QString committedText = overlay.watermark()->text;
    for (const QSize viewport : {QSize(320, 640), QSize(640, 320), QSize(320, 320)}) {
        const QRect workArea(screen.topLeft(), viewport);
        QTimer::singleShot(80, [&check, workArea, previewPath] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            check(dialog != nullptr, "responsive watermark dialog opens");
            auto *text = dialog->findChild<QPlainTextEdit *>(QStringLiteral("watermarkInputText"));
            auto *time = dialog->findChild<QPushButton *>(QStringLiteral("watermarkTimeFormat"));
            auto *longFormat = dialog->findChild<QAction *>(QStringLiteral("watermarkFmt_rfc2822"));
            longFormat->trigger();
            text->setPlainText(QStringLiteral("中文内容 long line\n").repeated(12));
            for (int i = 0; i < 4; ++i) QApplication::processEvents();
            check(dialog->width() <= workArea.width() - 32
                      && dialog->height() <= workArea.height() - 32,
                  "the card fits narrow and short work areas");
            check(workArea.contains(dialog->frameGeometry()), "the entire card remains inside its work area");
            const QString displayed = time->text();
            const QString displayedPrefix = displayed.endsWith(QChar(0x2026)) ? displayed.chopped(1)
                                                                              : displayed;
            check(longFormat->text().startsWith(displayedPrefix)
                      && time->toolTip() == longFormat->text(),
                  "a long time format elides with an ellipsis and keeps the full tooltip");
            check(time->fontMetrics().horizontalAdvance(displayed) + 18 <= time->width(),
                  "the elided time format fits its button");
            auto *apply = dialog->findChild<QPushButton *>(QStringLiteral("watermarkApply"));
            auto *cancel = dialog->findChild<QPushButton *>(QStringLiteral("watermarkCancel"));
            check(dialog->rect().contains(QRect(apply->mapTo(dialog, QPoint()), apply->size()))
                      && dialog->rect().contains(QRect(cancel->mapTo(dialog, QPoint()), cancel->size())),
                  "Apply and Cancel remain reachable on short screens");
            if (!previewPath.isEmpty())
                dialog->grab().save(previewPath + QStringLiteral(".card-%1x%2.png")
                                   .arg(workArea.width()).arg(workArea.height()));
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(dialog, &escape);
        });
        overlay.showWatermarkInputDialog(workArea);
        check(overlay.watermark()->text == committedText, "canceling responsive editing preserves the watermark");
    }
    edit->clear();
    check(overlay.watermark() && overlay.watermark()->text.isEmpty(),
          "clearing the field hides the watermark text");
    check(changed(base, overlay.cropped().toImage()) == 0, "cleared watermark leaves no painted pixels");

    // A frame whose native pixels are twice its logical size exercises the
    // texture-DPR path used by real 150%/200% captures.
    CaptureOverlay hidpi({ScreenFrame{QRect(screen.topLeft(), QSize(360, 240)),
                                      QRect(0, 0, 720, 480), image}});
    hidpi.setAttribute(Qt::WA_DeleteOnClose, false);
    const QPoint hdOrigin = hidpi.localScreenRect(screen).topLeft();
    hidpi.m_mode = Adjusting;
    hidpi.m_sel = QRect(hdOrigin + QPoint(20, 20), QSize(280, 180));
    hidpi.m_wmText = QStringLiteral("HD {{yyyy}}");
    hidpi.m_wmSize = 20;
    hidpi.m_wmOpacity = 100;
    hidpi.m_wmPosition = 7;
    Annotation *hd = hidpi.ensureWatermarkItem();
    hd->text = hidpi.m_wmText; hd->font = 20; hd->watermarkOpacity = 100;
    hd->watermarkPosition = 7; hd->createdAt = stamp;
    const QImage hdOut = hidpi.cropped().toImage();
    check(hdOut.size() == QSize(560, 360), "high-DPI watermark crop keeps native pixels");
    int hdRed = 0;
    for (int y = 0; y < hdOut.height(); ++y)
        for (int x = 0; x < hdOut.width(); ++x) {
            const QColor c = hdOut.pixelColor(x, y);
            if (c.red() > c.green() + 10 && c.red() > c.blue() + 10) ++hdRed;
        }
    check(hdRed > 500, "high-DPI watermark rasterizes visible text");
    if (!previewPath.isEmpty())
        check(hdOut.save(previewPath + QStringLiteral(".hidpi.png")),
              "save high-DPI watermark preview");
    hd->watermarkPosition = 0;
    const QImage hdTile = hidpi.cropped().toImage();
    int hdTileRed = 0;
    for (int y = 0; y < hdTile.height(); ++y)
        for (int x = 0; x < hdTile.width(); ++x) {
            const QColor c = hdTile.pixelColor(x, y);
            if (c.red() > c.green() + 10 && c.red() > c.blue() + 10) ++hdTileRed;
        }
    check(hdTileRed > 1000, "high-DPI tile watermark repeats across the crop");
    if (!previewPath.isEmpty())
        check(hdTile.save(previewPath + QStringLiteral(".hidpi-tile.png")),
              "save high-DPI tile preview");

    overlay.m_autoWatermark = true;
    overlay.m_autoWatermarkAdded = false;
    overlay.m_anns.clear();
    overlay.m_wmText = QStringLiteral("AUTO");
    overlay.m_wmOpacity = 30;
    overlay.m_wmSize = 20;
    overlay.ensureAutoWatermark();
    check(overlay.watermark() && overlay.watermark()->text == QStringLiteral("AUTO"),
          "auto watermark appends one overlay on a fresh selection");
    overlay.ensureAutoWatermark();
    check(overlay.watermarkIndex() >= 0
              && std::count_if(overlay.m_anns.cbegin(), overlay.m_anns.cend(),
                               [](const Annotation &a) { return a.tool == ATool::Watermark; }) == 1,
          "auto watermark is idempotent within one capture");
    overlay.m_anns.clear();
    overlay.pushUndo();
    overlay.ensureWatermarkItem()->text = QStringLiteral("UNDO");
    overlay.undoAnnotation();
    check(overlay.watermarkIndex() < 0, "undo removes the watermark item");
    overlay.redoAnnotation();
    check(overlay.watermarkIndex() >= 0, "redo restores the watermark item");

    if (!previewPath.isEmpty()) {
        overlay.m_anns.clear();
        overlay.m_wmText = QStringLiteral("{{yyyy-MM-dd HH:mm:ss}}");
        overlay.m_wmSize = 20;
        overlay.m_wmOpacity = 30;
        overlay.m_wmGap = 0;
        overlay.m_wmPosition = 0;
        wm = overlay.ensureWatermarkItem();
        wm->text = overlay.m_wmText; wm->font = 20; wm->watermarkOpacity = 30;
        wm->watermarkGap = 0; wm->watermarkPosition = 0; wm->createdAt = stamp;
        check(overlay.cropped().save(previewPath + QStringLiteral(".tile.png")),
              "save tile watermark preview");
        wm->watermarkGap = 32;
        check(overlay.cropped().save(previewPath + QStringLiteral(".tile-gap32.png")),
              "save tile gap watermark preview");
        wm->watermarkGap = 0;
        for (int anchor = 1; anchor <= 7; ++anchor) {
            wm->watermarkPosition = anchor;
            check(overlay.cropped().save(previewPath + QStringLiteral(".anchor%1.png").arg(anchor)),
                  "save anchored watermark preview");
        }
        wm->watermarkPosition = 7;
        check(overlay.cropped().save(previewPath + QStringLiteral(".center.png")),
              "save center watermark preview");
        overlay.selectTool(ATool::Watermark);
        check(overlay.grab(overlay.m_tools->geometry().united(overlay.m_toolOptions->geometry()))
                  .save(previewPath + QStringLiteral(".strip.png")),
              "save watermark strip preview");
    }
    std::puts("PASS: watermark time tokens, tile/anchor rendering, opacity, controls, and auto add");
    return 0;
}
