#include "capture_overlay.h"
#include "output.h"
#include "test_support.h"
#include "widgets.h"
#include "pin_window.h"
#include "long_capture.h"
#include "ocr_dialog.h"
#include <QGuiApplication>
#include <QImage>
#include <QVector>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QColor>
#include <QFont>
#include <QPaintEvent>
#include <QApplication>
#include <QWidget>
#include <QScreen>
#include <QPainter>
#include <QKeyEvent>
#include <QLabel>
#include <QGraphicsView>
#include <QtMath>
#include <QToolButton>
#include <QComboBox>
#include <QRegularExpression>
#include <QClipboard>
#include <QMessageBox>
#include <QTimer>
#include <QMimeData>
#include <QEventLoop>
#include <QThread>
#include <QElapsedTimer>
#include <QPointer>
#include <QSplitter>
#include <QPlainTextEdit>
#include <QThreadPool>
#include <qt_windows.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <cmath>

int CaptureOverlay::testOcr(const QString &previewPath)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("OCR phase: start");
    auto check = [](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
    };
    OcrEngine::testAlgorithms();
    const QRect primary(0,0,1920,1040);
    const QRect target(240,160,640,190);
    check(OcrDialog::placement(target,primary).topLeft()==target.topLeft(),
          "OCR card starts at the capture origin instead of the virtual-desktop center");
    for (const QPoint offset : {QPoint(-1920,0),QPoint(1920,-1080)}) {
        const QRect available = primary.translated(offset);
        check(OcrDialog::placement(target.translated(offset),available)==OcrDialog::placement(target,primary).translated(offset)
                  && available.contains(OcrDialog::placement(QRect(available.bottomRight(),QSize(2,2)),available)),
              "OCR placement retains the selected secondary screen and clamps edge captures inside its work area");
    }
    for (const QRect pin : {target,QRect(1500,300,400,300),QRect(0,0,500,300)}) {
        const QRect card = OcrDialog::beside(pin,primary);
        check(primary.contains(card) && !card.intersects(pin),
              "OCR card opened from a pin sits beside the pin inside the work area");
    }
    const QRect bigPin(600,200,700,500);
    const QRect crowded = OcrDialog::beside(bigPin,primary), plain = OcrDialog::placement(bigPin,primary);
    const auto covered = [&](const QRect &r) { const QRect h = r.intersected(bigPin); return h.width()*h.height(); };
    check(primary.contains(crowded) && covered(crowded) < covered(plain),
          "OCR card covers a large pin less than the plain origin placement when no clear spot exists");
    const QString missing = OcrEngine::assetError(QCoreApplication::applicationDirPath());
    if (!missing.isEmpty()) {
        QImage image(640,180,QImage::Format_RGB32); image.fill(Qt::white);
        const QRect screen=QGuiApplication::primaryScreen()->geometry();
        CaptureOverlay overlay({ScreenFrame{QRect(screen.topLeft(),image.size()),QRect(QPoint(),image.size()),image}});
        overlay.setAttribute(Qt::WA_DeleteOnClose,false);
        auto *button=overlay.m_tools->findChild<QToolButton *>(QStringLiteral("ocrButton"));
        check(button && !button->isEnabled() && button->toolTip()==missing,
              "missing OCR assets disable the toolbar button with an actionable explanation");
        overlay.close();
        std::printf("UNAVAILABLE: %s\n",qPrintable(missing));
        return 0;
    }
    auto &engine = OcrEngine::instance();
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    auto fixture = [](qreal dpr, const QStringList &rows, const QVector<int> &sizes, int angle = 0) {
        QImage image(qRound(800*dpr),qRound(280*dpr),QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.scale(dpr,dpr);
        if (angle) { painter.translate(400,140); painter.rotate(angle); painter.translate(-400,-140); }
        painter.setPen(Qt::black);
        for (int i=0; i<rows.size(); ++i) {
            QFont font(QStringLiteral("Microsoft YaHei")); font.setPixelSize(sizes[i]); painter.setFont(font);
            painter.drawText(QPointF(36,rows.size()==1 ? 140 : 58+i*85),rows[i]);
        }
        painter.end();
        image.setDevicePixelRatio(dpr);
        return image;
    };
    const QStringList chinese{QStringLiteral("截图文字识别"),QStringLiteral("中文测试"),QStringLiteral("对照结果")};
    for (qreal dpr : {1.0,1.5,2.0}) {
        for (int scenario=0; scenario<4; ++scenario) {
            const QStringList rows = scenario==0 ? chinese : QStringList{QStringLiteral("Hello世界123")};
            const QImage image = fixture(dpr,rows,scenario==0 ? QVector<int>{16,24,40} : QVector<int>{36},
                                         scenario==2 ? 10 : scenario==3 ? -10 : 0);
            const OcrResult result = engine.recognize(image,cancel);
            std::printf("OCR dpr=%.1f case=%d rows=%lld elapsed=%lldms text=%s error=%s\n",dpr,scenario,
                        (long long)result.lines.size(),result.elapsed,qPrintable(result.text()),qPrintable(result.error));
            if (!previewPath.isEmpty()) image.save(previewPath+QStringLiteral("-%1-%2.png").arg(dpr).arg(scenario));
            check(result.error.isEmpty() && !result.cancelled,"real OCR pipeline finishes without an inference error");
            check(result.text() == rows.join(QLatin1Char('\n')),"real OCR output matches Chinese, mixed and rotated fixtures exactly");
        }
        QImage blank(qRound(800*dpr),qRound(280*dpr),QImage::Format_RGB32); blank.fill(Qt::white);
        const OcrResult empty = engine.recognize(blank,cancel);
        check(empty.error.isEmpty() && empty.lines.isEmpty(),"blank screenshots produce no recognized rows");
    }
    QImage dense = fixture(1,{QStringLiteral("中文测试中文测试中文测试")},{10});
    const OcrResult small = engine.recognize(dense,cancel);
    check(small.error.isEmpty(),"dense small text completes safely without assuming all characters are readable");
    QImage full(1920,1080,QImage::Format_RGB32); full.fill(Qt::white);
    {
        QPainter painter(&full); painter.setPen(Qt::black);
        QFont font(QStringLiteral("Microsoft YaHei")); font.setPixelSize(32); painter.setFont(font);
        for (int row=0; row<10; ++row) painter.drawText(QPoint(40,80+row*90),QStringLiteral("截图文字识别 HelloWorld123"));
    }
    const OcrResult performance=engine.recognize(full,cancel);
    check(performance.error.isEmpty() && performance.lines.size()==10,"1080p screenshot retains all ten text rows");
    std::printf("PERFORMANCE: 1920x1080 / ten rows / %lldms%s\n",performance.elapsed,
                performance.elapsed>3000 ? " / WARNING: exceeds the reference budget" : "");
    class Calibration : public QWidget {
    public:
        Calibration() : QWidget(nullptr,Qt::Tool|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint) {}
    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter painter(this); painter.fillRect(rect(),Qt::white); painter.setPen(Qt::black);
            QFont font(QStringLiteral("Microsoft YaHei")); font.setPixelSize(32); painter.setFont(font);
            painter.drawText(QPoint(24,62),QStringLiteral("截图文字识别"));
            painter.drawText(QPoint(24,132),QStringLiteral("HelloWorld123"));
        }
    } calibration;
    const QRect available=QGuiApplication::primaryScreen()->availableGeometry();
    calibration.setGeometry(QRect(available.topLeft()+QPoint(80,80),QSize(640,190)));
    calibration.show(); calibration.repaint();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QThread::msleep(100);
    RECT client{}; POINT origin{};
    const HWND hwnd=reinterpret_cast<HWND>(calibration.winId());
    check(GetClientRect(hwnd,&client) && ClientToScreen(hwnd,&origin),"live OCR fixture has valid native screen coordinates");
    const QRect liveNative(origin.x,origin.y,client.right,client.bottom);
    const QImage live=grabNative(liveNative);
    const OcrResult liveResult=engine.recognize(live,cancel);
    std::printf("LIVE OCR: %dx%d / %lldms / text=[%s] / error=[%s]\n",live.width(),live.height(),liveResult.elapsed,
                qPrintable(liveResult.text()),qPrintable(liveResult.error));
    for (const OcrLine &line : liveResult.lines) {
        std::printf("LIVE BOX: text=[%s]",qPrintable(line.text));
        for (const QPointF &point : line.box) std::printf(" (%.3f,%.3f)",point.x(),point.y());
        std::putchar('\n');
    }
    if (!previewPath.isEmpty()) check(live.save(previewPath+QStringLiteral("-live.png")),"save actual desktop OCR fixture");
    check(liveResult.error.isEmpty() && liveResult.text()==QStringLiteral("截图文字识别\nHelloWorld123"),
          "actual desktop pixels pass through detection and recognition with exact Chinese and English output");
    const QVector<ScreenFrame> liveFrames=freezeScreens();
    check(!liveFrames.isEmpty(),"freeze actual desktop frames for the OCR toolbar workflow");
    calibration.close();
    QImage invalid;
    check(!engine.recognize(invalid,cancel).error.isEmpty(),"empty images return an explicit OCR error");
    const QImage image = fixture(1,{QStringLiteral("截图文字识别")},{32});
    cancel->store(true);
    check(engine.recognize(image,cancel).cancelled,"pre-cancelled work does not load or infer");
    cancel->store(false);
    CaptureOverlay overlay(liveFrames);
    std::puts("OCR phase: overlay created");
    overlay.setAttribute(Qt::WA_DeleteOnClose,false);
    overlay.m_mode = Adjusting;
    overlay.m_sel = overlay.localScreenRect(logicalFromNative(liveFrames,liveNative));
    Annotation annotation;
    annotation.tool=ATool::Text; annotation.color=Qt::black; annotation.font=16;
    annotation.text=QStringLiteral("OCR2026");
    annotation.rect=QRect(overlay.m_sel.topLeft()+QPoint(24,153),QSize(300,35));
    overlay.m_anns.append(annotation);
    overlay.placeToolbar(); overlay.showToolBars();
    const QRect selection = overlay.m_sel;
    const QImage before = overlay.cropped().toImage();
    auto *button = overlay.m_tools->findChild<QToolButton *>(QStringLiteral("ocrButton"));
    check(button && button->isEnabled() && !button->toolTip().isEmpty(),"capture toolbar exposes a named usable OCR button");
    button->click();
    std::puts("OCR phase: card opened");
    QPointer<OcrDialog> dialog = overlay.m_ocrDialog;
    check(dialog && dialog->findChild<QSplitter *>(QStringLiteral("ocrSplit"))->count()==2,
          "OCR opens a two-pane screenshot and text comparison");
    check(dialog->findChild<QSplitter *>(QStringLiteral("ocrSplit"))->orientation()==Qt::Vertical
              && OcrDialog::prefersRows(QSize(800,100),QSize(1000,650)) && !OcrDialog::prefersRows(QSize(300,300),QSize(1000,650))
              && !OcrDialog::prefersRows(QSize(100,600),QSize(1000,650)),
          "wide strips open stacked while square and tall captures open side by side");
    dialog->findChild<QToolButton *>(QStringLiteral("ocrLayout"))->click();
    check(dialog->findChild<QSplitter *>(QStringLiteral("ocrSplit"))->orientation()==Qt::Horizontal,
          "the layout button switches the comparison back to side by side");
    QApplication::processEvents();
    check(!overlay.isVisible() && !overlay.m_tools->isVisible() && !overlay.m_toolOptions->isVisible()
              && !overlay.m_inspector->isVisible() && dialog->windowModality()==Qt::NonModal && !QApplication::activeModalWidget(),
          "OCR hides every capture overlay and does not block windows outside the card");
    const QRect captureRect=overlay.globalScreenRect(selection);
    const QScreen *captureScreen=QGuiApplication::screenAt(captureRect.center());
    check(captureScreen && dialog->screen()==captureScreen
              && dialog->geometry()==OcrDialog::placement(captureRect,captureScreen->availableGeometry())
              && dialog->windowTitle()==QStringLiteral("Finalize OCR"),
          "Finalize OCR opens at the selected capture on its own screen");
    const HWND cardHwnd=reinterpret_cast<HWND>(dialog->winId());
    check(GetWindow(cardHwnd,GW_OWNER)==nullptr && !(GetWindowLongPtrW(cardHwnd,GWL_EXSTYLE)&WS_EX_TOOLWINDOW),
          "OCR is an independent taskbar window rather than a tool owned by a hidden capture overlay");
    QWidget desktopProbe(nullptr,Qt::Tool|Qt::FramelessWindowHint);
    desktopProbe.setGeometry(QRect(captureScreen->availableGeometry().bottomRight()-QPoint(90,90),QSize(80,80)));
    desktopProbe.show(); desktopProbe.raise(); desktopProbe.activateWindow();
    QApplication::processEvents();
    const HWND probeHwnd=reinterpret_cast<HWND>(desktopProbe.winId());
    POINT probePoint{20,20};
    check(ClientToScreen(probeHwnd,&probePoint) && WindowFromPoint(probePoint)==probeHwnd && IsWindowEnabled(probeHwnd),
          "an independent desktop window stays enabled and receives native hit testing while OCR is open");
    desktopProbe.close(); dialog->raise(); dialog->activateWindow();
    auto *text = dialog->findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    auto *retry = dialog->findChild<QToolButton *>(QStringLiteral("ocrRetry"));
    check(text->isReadOnly() && !retry->isEnabled() && dialog->findChildren<QComboBox *>().isEmpty(),
          "unified recognition has no language selector and disables only busy controls without blocking the UI");
    auto wait = [&] {
        QElapsedTimer timer; timer.start();
        while (dialog && !retry->isEnabled() && timer.elapsed()<30000) {
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            QThread::msleep(10);
        }
        check(dialog && retry->isEnabled(),"asynchronous OCR finishes within the test timeout");
    };
    wait();
    const QString expectedText=QStringLiteral("截图文字识别\nHelloWorld123\nOCR2026");
    check(!text->isReadOnly() && text->toPlainText()==expectedText,"OCR recognizes the composed screenshot including annotations and allows editing");
    auto *copy=dialog->findChild<QToolButton *>(QStringLiteral("ocrCopy"));
    auto *back=dialog->findChild<QToolButton *>(QStringLiteral("ocrBack"));
    auto *status=dialog->findChild<QLabel *>(QStringLiteral("ocrStatus"));
    auto *zoomView=static_cast<OcrImageView *>(dialog->findChild<QGraphicsView *>());
    auto *zoomLevel=dialog->findChild<QLabel *>(QStringLiteral("ocrZoomLevel"));
    auto *zoomFit=dialog->findChild<QToolButton *>(QStringLiteral("ocrFit"));
    const bool fittedAtFirst=zoomView->fitted();
    zoomFit->click();                                     // Fitted: the toggle shows one-to-one.
    const bool actual=zoomView->percent()==100 && zoomLevel->text()==QStringLiteral("100%") && !zoomView->fitted();
    dialog->findChild<QToolButton *>(QStringLiteral("ocrZoomIn"))->click();
    const bool larger=zoomView->percent()==110 && zoomLevel->text()==QStringLiteral("110%");
    dialog->findChild<QToolButton *>(QStringLiteral("ocrZoomOut"))->click();
    dialog->findChild<QToolButton *>(QStringLiteral("ocrZoomOut"))->click();
    const bool smaller=zoomView->percent()==90;
    zoomFit->click();                                     // Zoomed: the same toggle fits again.
    check(fittedAtFirst && actual && larger && smaller && zoomView->fitted() && zoomFit->toolTip().contains(QStringLiteral("1:1")),
          "image block zooms in ten-point steps, shows the percentage and toggles between fit and one-to-one");
    check(dialog->findChild<QLabel *>(QStringLiteral("ocrCharacterCount"))->text()==QString::number(text->toPlainText().toUcs4().size()),
          "the text block counts the characters it holds");
    const int textBottom=text->mapTo(dialog,QPoint()).y()+text->height();
    check(retry->text().isEmpty() && copy->text().isEmpty() && !retry->icon().isNull() && !copy->icon().isNull()
              && copy->mapTo(dialog,QPoint()).x()+copy->width()<=retry->mapTo(dialog,QPoint()).x()
              && retry->mapTo(dialog,QPoint()).y()>=textBottom && copy->mapTo(dialog,QPoint()).y()>=textBottom
              && !back
              && status->alignment().testFlag(Qt::AlignRight) && status->mapTo(dialog,QPoint()).y()>=textBottom
              && QRegularExpression(QStringLiteral("^\\d+\\.\\d{2}秒 · 平均置信度\\d+%$")).match(status->text()).hasMatch(),
          "text actions and bottom-right timing run along the bottom edge of the text block");
    if (!previewPath.isEmpty()) check(dialog->grab().save(previewPath+QStringLiteral("-comparison.png")),"save comparison window preview");
    check(text->font().pointSizeF()>=12 && dialog->font().pointSizeF()>=10.5 && copy->size()==QSize(28,28)
              && retry->size()==copy->size() && copy->iconSize()==QSize(16,16),
          "OCR uses readable point-sized system fonts and consistent 16px glyphs on 28px action targets");
    // Recognized characters are selectable on the capture, and text-pane selections light them up.
    const QString wantedLine=QStringLiteral("HelloWorld123");
    const int glyphTotal=QString(expectedText).remove(QLatin1Char('\n')).size();
    bool ordered=zoomView->glyphCount()==glyphTotal;
    for (int i=1;ordered && i<zoomView->glyphCount();++i) {
        const QRectF a=zoomView->glyphPolygon(i-1).boundingRect(), b=zoomView->glyphPolygon(i).boundingRect();
        ordered = b.top()>a.center().y() || b.left()>=a.left();   // Left to right inside a row, rows top to bottom.
    }
    check(ordered,"every recognized character gets its own box on the capture, in reading order");
    {
        QTextCursor pick(text->document());
        pick.setPosition(text->toPlainText().indexOf(wantedLine));
        pick.setPosition(pick.position()+5,QTextCursor::KeepAnchor);   // "Hello"
        text->setTextCursor(pick);
    }
    const QVector<int> lit=zoomView->highlighted();
    const int helloFirst=QStringLiteral("截图文字识别").size();
    check(lit==QVector<int>({helloFirst,helloFirst+1,helloFirst+2,helloFirst+3,helloFirst+4}),
          "selecting text in the OCR pane highlights exactly those characters on the capture");
    text->moveCursor(QTextCursor::End);
    check(zoomView->highlighted().isEmpty(),"clearing the text selection clears the capture highlight");
    const auto viewMouse=[&](QEvent::Type type,int glyph,Qt::MouseButtons buttons) {
        const QPoint at=zoomView->mapFromScene(zoomView->glyphPolygon(glyph).boundingRect().center());
        QMouseEvent event(type,QPointF(at),QPointF(zoomView->viewport()->mapToGlobal(at)),
                          type==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,buttons,Qt::NoModifier);
        QApplication::sendEvent(zoomView->viewport(),&event);
    };
    viewMouse(QEvent::MouseMove,helloFirst+1,Qt::NoButton);
    const bool hoverBeam=zoomView->viewport()->hasMouseTracking() && zoomView->viewport()->cursor().shape()==Qt::IBeamCursor;
    const QPointF centreBefore=zoomView->mapToScene(zoomView->viewport()->rect().center());
    viewMouse(QEvent::MouseButtonPress,helloFirst+5,Qt::LeftButton);        // "W"
    viewMouse(QEvent::MouseMove,helloFirst+9,Qt::LeftButton);
    const bool dragBeam=zoomView->viewport()->cursor().shape()==Qt::IBeamCursor;
    viewMouse(QEvent::MouseMove,helloFirst+12,Qt::LeftButton);              // "3"
    viewMouse(QEvent::MouseButtonRelease,helloFirst+12,Qt::NoButton);
    check(hoverBeam && dragBeam,"hovering and dragging over capture text show the text cursor, not the hand");
    const bool dragged=zoomView->selectedText()==QStringLiteral("World123")
        && zoomView->mapToScene(zoomView->viewport()->rect().center())==centreBefore;
    QApplication::clipboard()->clear();
    zoomView->setFocus();
    QKeyEvent copyKey(QEvent::KeyPress,Qt::Key_C,Qt::ControlModifier);
    QApplication::sendEvent(zoomView,&copyKey);
    check(dragged && QApplication::clipboard()->text()==QStringLiteral("World123") && status->text()==QStringLiteral("已复制选中文字"),
          "dragging across capture text selects it without panning and Ctrl+C copies it");
    viewMouse(QEvent::MouseButtonPress,0,Qt::LeftButton);
    viewMouse(QEvent::MouseMove,glyphTotal-1,Qt::LeftButton);
    viewMouse(QEvent::MouseButtonRelease,glyphTotal-1,Qt::NoButton);
    check(zoomView->selectedText()==expectedText,"a selection across rows keeps the recognized line breaks");
    viewMouse(QEvent::MouseButtonPress,3,Qt::LeftButton);
    viewMouse(QEvent::MouseButtonRelease,3,Qt::NoButton);
    check(zoomView->selectedText().isEmpty(),"a click on text without dragging drops the capture selection");
    {
        const QPoint at=zoomView->mapFromScene(zoomView->glyphPolygon(helloFirst+2).boundingRect().center());
        QMouseEvent twice(QEvent::MouseButtonDblClick,QPointF(at),QPointF(zoomView->viewport()->mapToGlobal(at)),
                          Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(zoomView->viewport(),&twice);
    }
    check(zoomView->selectedText()==wantedLine && zoomView->fitted(),"double-clicking capture text selects its whole line instead of refitting");
    zoomView->selectRange(-1,-1);
    if (!previewPath.isEmpty()) {
        QTextCursor pick(text->document());
        pick.setPosition(text->toPlainText().indexOf(wantedLine));
        pick.setPosition(pick.position()+wantedLine.size(),QTextCursor::KeepAnchor);
        text->setTextCursor(pick);
        QApplication::processEvents();
        check(dialog->grab().save(previewPath+QStringLiteral("-highlight.png")),"save text highlight preview");
        text->moveCursor(QTextCursor::End);
    }
    const auto contrast = [](const QColor &a,const QColor &b) {
        const auto luminance = [](const QColor &color) {
            const auto linear=[](double value) { return value<=0.04045 ? value/12.92 : std::pow((value+0.055)/1.055,2.4); };
            return 0.2126*linear(color.redF())+0.7152*linear(color.greenF())+0.0722*linear(color.blueF());
        };
        const double first=luminance(a),second=luminance(b);
        return (qMax(first,second)+0.05)/(qMin(first,second)+0.05);
    };
    check(contrast(QColor("#323232"),QColor("#ffffff"))>=4.5
              && contrast(QColor("#5b5b5b"),QColor("#f2f2f2"))>=4.5
              && contrast(QColor("#2080f0"),QColor("#2f3236"))>=3
              && contrast(QColor("#5b5b5b"),QColor("#ffffff"))>=4.5
              && contrast(QColor("#323232"),QColor("#a3d6ff"))>=4.5
              && contrast(QColor("#858585"),QColor("#ffffff"))>=3
              && contrast(QColor("#2080f0"),QColor("#ffffff"))>=3,
          "OCR text, metadata, accent buttons, control outlines and focus cues meet their contrast budgets");
    const QList<QWidget *> tabOrder{dialog->findChild<QToolButton *>(QStringLiteral("ocrPin")),
        dialog->findChild<QToolButton *>(QStringLiteral("ocrMinimize")),dialog->findChild<QToolButton *>(QStringLiteral("ocrMaximize")),
        dialog->findChild<QToolButton *>(QStringLiteral("ocrClose")),dialog->findChild<QGraphicsView *>(),
        dialog->findChild<QToolButton *>(QStringLiteral("ocrCopyImage")),dialog->findChild<QToolButton *>(QStringLiteral("ocrZoomIn")),
        dialog->findChild<QToolButton *>(QStringLiteral("ocrZoomOut")),dialog->findChild<QToolButton *>(QStringLiteral("ocrFit")),
        text,copy,dialog->findChild<QToolButton *>(QStringLiteral("ocrLayout")),
        dialog->findChild<QToolButton *>(QStringLiteral("ocrFormat")),dialog->findChild<QToolButton *>(QStringLiteral("ocrSearch")),retry};
    tabOrder.first()->setFocus();
    for (int i=1;i<tabOrder.size();++i) {
        QKeyEvent tab(QEvent::KeyPress,Qt::Key_Tab,Qt::NoModifier);
        QApplication::sendEvent(QApplication::focusWidget(),&tab);
        check(QApplication::focusWidget()==tabOrder[i],"Tab follows the visual window-actions, image and text-actions order without a keyboard trap");
    }
    const QSize normalSize=dialog->size();
    dialog->resize(dialog->minimumSize()); QApplication::processEvents();
    const auto controlsFit = [&] {
        const QRect panelRect=dialog->findChild<QWidget *>(QStringLiteral("ocrTextPanel"))->rect();
        QWidget *panel=dialog->findChild<QWidget *>(QStringLiteral("ocrTextPanel"));
        return dialog->rect().contains(QRect(panel->mapTo(dialog,QPoint()),panel->size()))
            && panelRect.contains(QRect(retry->mapTo(panel,QPoint()),retry->size()))
            && panelRect.contains(QRect(copy->mapTo(panel,QPoint()),copy->size()))
            && copy->mapTo(dialog,QPoint()).x()+copy->width()<=retry->mapTo(dialog,QPoint()).x()
            && text->height()>100 && copy->width()>=copy->sizeHint().width();
    };
    if (dialog->size()!=dialog->minimumSize() || !controlsFit())
        std::fprintf(stderr,"MIN: copy=%d/%d dialog=%dx%d min=%dx%d hint=%dx%d text-height=%d retry=(%d,%d) copy=(%d,%d)\n",
            copy->width(),copy->sizeHint().width(),dialog->width(),dialog->height(),dialog->minimumWidth(),dialog->minimumHeight(),
            dialog->minimumSizeHint().width(),dialog->minimumSizeHint().height(),text->height(),
            retry->mapTo(dialog,QPoint()).x(),retry->mapTo(dialog,QPoint()).y(),copy->mapTo(dialog,QPoint()).x(),copy->mapTo(dialog,QPoint()).y());
    check(dialog->size()==dialog->minimumSize() && controlsFit(),"minimum OCR window retains both panes and every primary action without overlap");
    if (!previewPath.isEmpty()) check(dialog->grab().save(previewPath+QStringLiteral("-compact.png")),"save minimum-size OCR preview");
    QVector<QPair<QWidget *,QFont>> savedFonts;
    for (QWidget *widget:dialog->findChildren<QWidget *>())
        if (qobject_cast<QLabel *>(widget) || qobject_cast<QToolButton *>(widget)
            || qobject_cast<QPlainTextEdit *>(widget))
            savedFonts.append({widget,widget->font()});
    for (const auto &entry:savedFonts) {
        QFont largeFont=entry.second; largeFont.setPointSizeF(largeFont.pointSizeF()*1.25);
        entry.first->setFont(largeFont);
    }
    for (int pass=0;pass<3;++pass) QApplication::processEvents();
    check(retry->font().pointSizeF()>=13 && text->font().pointSizeF()>=15,
          "the enlarged-font preview actually increases both interface and reading fonts");
    if (!controlsFit()) std::fprintf(stderr,"LAYOUT: dialog=%dx%d panel=%dx%d retry=(%d,%d %dx%d) copy=(%d,%d %dx%d) text-height=%d\n",
        dialog->width(),dialog->height(),retry->parentWidget()->width(),retry->parentWidget()->height(),
        retry->x(),retry->y(),retry->width(),retry->height(),
        copy->x(),copy->y(),copy->width(),copy->height(),text->height());
    check(controlsFit(),"OCR controls tolerate an application-level twenty-five percent font enlargement at minimum size");
    if (!previewPath.isEmpty()) check(dialog->grab().save(previewPath+QStringLiteral("-large-font.png")),"save enlarged-interface-font OCR preview");
    for (const auto &entry:savedFonts) entry.first->setFont(entry.second);
    dialog->resize(normalSize); QApplication::processEvents();
    text->selectAll(); text->insertPlainText(QStringLiteral("校对后的文本"));
    auto *saved = new QMimeData;
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (const QMimeData *data=clipboard->mimeData()) for (const QString &format:data->formats()) saved->setData(format,data->data(format));
    copy->click();
    const bool copied = clipboard->text()==QStringLiteral("校对后的文本");
    clipboard->setMimeData(saved);
    check(copied,"copy-all outputs the edited text and leaves the comparison open");
    QTimer::singleShot(0,dialog,[dialog] {
        for (QWidget *widget:QApplication::topLevelWidgets())
            if (auto *question=qobject_cast<QMessageBox *>(widget))
                if (question->parentWidget()==dialog) question->button(QMessageBox::No)->click();
    });
    retry->click();
    check(text->toPlainText()==QStringLiteral("校对后的文本") && !text->isReadOnly() && retry->isEnabled(),
          "declining retry preserves manual corrections and does not start inference");
    QTimer::singleShot(0,dialog,[dialog] {
        for (QWidget *widget:QApplication::topLevelWidgets())
            if (auto *question=qobject_cast<QMessageBox *>(widget))
                if (question->parentWidget()==dialog) question->button(QMessageBox::Yes)->click();
    });
    retry->click(); wait();
    check(text->toPlainText()==expectedText,"retry recognizes the same retained screenshot");
    auto *maximize=dialog->findChild<QToolButton *>(QStringLiteral("ocrMaximize"));
    maximize->click(); QApplication::processEvents();
    QElapsedTimer maximizeTimer; maximizeTimer.start();
    while (dialog->geometry()!=dialog->screen()->availableGeometry() && maximizeTimer.elapsed()<2000) {
        QApplication::processEvents(); QThread::msleep(10);
    }
    check(dialog->m_maximized && maximize->toolTip()==QStringLiteral("还原")
              && dialog->geometry()==dialog->screen()->availableGeometry(),"maximize fills the work area and changes to a restore control");
    // Maximized by geometry: the edges still resize and empty chrome still drags.
    const auto mouse = [&](QEvent::Type type,const QPoint &local,Qt::MouseButtons buttons) {
        QMouseEvent event(type,QPointF(local),QPointF(dialog->mapToGlobal(local)),
                          type==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,buttons,Qt::NoModifier);
        QApplication::sendEvent(dialog,&event);
    };
    const QRect maxed=dialog->geometry();
    const QPoint rightEdge(dialog->width()-3,dialog->height()/2);
    mouse(QEvent::MouseMove,rightEdge,Qt::NoButton);
    const bool edgeCursor=dialog->cursor().shape()==Qt::SizeHorCursor;
    mouse(QEvent::MouseButtonPress,rightEdge,Qt::LeftButton);
    mouse(QEvent::MouseMove,rightEdge-QPoint(120,0),Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease,rightEdge-QPoint(120,0),Qt::NoButton);
    QApplication::processEvents();
    check(edgeCursor && !dialog->m_maximized && dialog->width()==maxed.width()-120 && dialog->x()==maxed.x()
              && maximize->toolTip()==QStringLiteral("最大化"),
          "a maximized card still resizes from its edge and drops the maximized state");
    const QPoint titleSpot=dialog->m_header->geometry().center();
    const QPoint startPos=dialog->pos();
    mouse(QEvent::MouseButtonPress,titleSpot,Qt::LeftButton);
    mouse(QEvent::MouseMove,titleSpot+QPoint(40,30),Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease,titleSpot+QPoint(40,30),Qt::NoButton);
    check(dialog->pos()==startPos+QPoint(40,30),"dragging the empty title bar moves the card");
    QMouseEvent twice(QEvent::MouseButtonDblClick,QPointF(titleSpot),QPointF(dialog->mapToGlobal(titleSpot)),
                      Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(dialog,&twice);
    check(dialog->m_maximized && dialog->geometry()==maxed,"double-clicking the title bar maximizes again");
    const QPoint maxSpot=dialog->m_header->geometry().center();
    mouse(QEvent::MouseButtonPress,maxSpot,Qt::LeftButton);
    mouse(QEvent::MouseMove,maxSpot+QPoint(25,0),Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease,maxSpot+QPoint(25,0),Qt::NoButton);
    check(dialog->pos()==maxed.topLeft()+QPoint(25,0),"a maximized card can still be dragged");
    const QPoint inner(dialog->width()/2,dialog->height()-3), deep(dialog->width()/2,dialog->height()-12);
    check(dialog->resizeEdges(inner)==Qt::BottomEdge && !dialog->resizeEdges(deep)
              && dialog->resizeEdges(QPoint(2,2))==(Qt::LeftEdge|Qt::TopEdge),
          "the resize band covers the outer half of the edge padding and combines corners");
    maximize->click(); QApplication::processEvents();
    check(!dialog->m_maximized && text->toPlainText()==expectedText,"restoring the card keeps the recognized text");
    dialog->findChild<QToolButton *>(QStringLiteral("ocrMinimize"))->click(); QApplication::processEvents();
    check(dialog->isMinimized() && !overlay.isVisible(),"minimizing OCR does not restore the global frozen overlay");
    dialog->showNormal(); QApplication::processEvents();
    const auto existingWindows=QApplication::topLevelWidgets();
    auto *pin=dialog->findChild<QToolButton *>(QStringLiteral("ocrPin"));
    const QRect cardGeometry=dialog->geometry();
    check(pin->isCheckable() && !pin->isChecked() && !(GetWindowLongPtrW(cardHwnd,GWL_EXSTYLE)&WS_EX_TOPMOST),
          "OCR starts unpinned with an explicit topmost toggle");
    pin->click(); QApplication::processEvents();
    check(pin->isChecked() && (GetWindowLongPtrW(cardHwnd,GWL_EXSTYLE)&WS_EX_TOPMOST)
              && pin->toolTip()==QStringLiteral("取消置顶 OCR 卡片") && pin->accessibleName()==pin->toolTip()
              && dialog->geometry()==cardGeometry && reinterpret_cast<HWND>(dialog->winId())==cardHwnd
              && text->toPlainText()==expectedText && dialog->isVisible() && !overlay.isVisible(),
          "pin makes the entire OCR card topmost without changing its handle, geometry, text or capture state");
    check(QApplication::topLevelWidgets().size()==existingWindows.size(),"OCR pin does not create an image window");
    dialog->findChild<QToolButton *>(QStringLiteral("ocrMinimize"))->click(); QApplication::processEvents();
    dialog->showNormal(); QApplication::processEvents();
    maximize->click(); QApplication::processEvents();
    maximize->click(); QApplication::processEvents();
    check(pin->isChecked() && (GetWindowLongPtrW(cardHwnd,GWL_EXSTYLE)&WS_EX_TOPMOST) && text->toPlainText()==expectedText,
          "topmost survives minimize restore and maximize restore without discarding the OCR result");
    pin->click(); QApplication::processEvents();
    check(!pin->isChecked() && !(GetWindowLongPtrW(cardHwnd,GWL_EXSTYLE)&WS_EX_TOPMOST)
              && pin->toolTip()==QStringLiteral("置顶 OCR 卡片") && dialog->geometry()==cardGeometry,
          "clicking pin again returns the OCR card to normal window stacking");
    dialog->findChild<QToolButton *>(QStringLiteral("ocrClose"))->click();
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    check(!overlay.isVisible() && overlay.m_sel==selection && overlay.cropped().toImage()==before,
          "closing OCR ends the capture and leaves the selection and output untouched");
    overlay.show(); overlay.placeToolbar(); overlay.showToolBars();
    button->click();
    dialog = overlay.m_ocrDialog;
    QApplication::processEvents();
    dialog->findChild<QToolButton *>(QStringLiteral("ocrClose"))->click();
    QApplication::processEvents();                       // Runs the queued capture close before showing it again.
    QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    QThreadPool::globalInstance()->waitForDone();
    check(!engine.busy.load() && !overlay.isVisible(),"closing during recognition cancels cooperatively and releases the single-job slot");
    overlay.show(); overlay.placeToolbar(); overlay.showToolBars();
    for (QScreen *screen:QGuiApplication::screens()) {
        const QRect area=screen->availableGeometry();
        const QRect anchor(area.topLeft()+QPoint(30,30),QSize(100,80));
        OcrDialog positioned(image,anchor);
        positioned.show();
        check(positioned.screen()==screen && positioned.geometry()==OcrDialog::placement(anchor,area),
              "actual OCR windows select each connected monitor from the capture anchor rather than the parent overlay");
        positioned.reject();
        QApplication::processEvents();
    }
    const QString mixedText=QStringLiteral("截图 繁體 日本語 Hello été");
    OcrDialog foreign(fixture(1,{mixedText},{36}));
    foreign.show();
    QApplication::processEvents();
    QElapsedTimer foreignTimer; foreignTimer.start();
    auto *foreignRetry=foreign.findChild<QToolButton *>(QStringLiteral("ocrRetry"));
    while (!foreignRetry->isEnabled() && foreignTimer.elapsed()<30000) {
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents); QThread::msleep(10);
    }
    foreignRetry->click(); foreignTimer.restart();
    while (!foreignRetry->isEnabled() && foreignTimer.elapsed()<30000) {
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents); QThread::msleep(10);
    }
    check(foreignRetry->isEnabled() && foreign.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"))->toPlainText()==mixedText
              && foreign.findChildren<QComboBox *>().isEmpty(),
          "one model recognizes and retries mixed scripts without a language selector");
    foreign.reject();
    engine.busy.store(true);
    OcrDialog blocked(image); blocked.show();
    QApplication::processEvents();
    check(blocked.findChild<QLabel *>(QStringLiteral("ocrStatus"))->text().contains(QStringLiteral("上一项"))
              && blocked.findChild<QToolButton *>(QStringLiteral("ocrRetry"))->isEnabled(),
          "a second request does not queue another job while the process-wide slot is occupied");
    blocked.reject(); engine.busy.store(false);
    button->click();
    dialog=overlay.m_ocrDialog;
    dialog->findChild<QToolButton *>(QStringLiteral("ocrClose"))->click();
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    check(!overlay.isVisible(),"closing OCR ends the capture rather than locking the desktop again");
    QThreadPool::globalInstance()->waitForDone();
    overlay.close();
    std::puts("PASS: offline OCR, exact native-pixel fixtures and comparison/edit/copy/retry/cancel workflow");
    return 0;
}
