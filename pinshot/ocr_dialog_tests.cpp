#include "ocr_dialog.h"
#include <QApplication>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextDocument>
#include <QThreadPool>
#include <QToolButton>
#include <QMenu>
#include <QAction>
#include <QLineF>
#include <cstdio>

int OcrDialog::testLifetime()
{
    auto &engine = OcrEngine::instance();
    engine.busy.store(true);
    QImage image(320, 120, QImage::Format_RGB32);
    image.fill(Qt::white);
    OcrDialog dialog(image);
    dialog.show();
    QApplication::processEvents();
    engine.busy.store(false);
    auto *text = dialog.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    auto *retry = dialog.findChild<QToolButton *>(QStringLiteral("ocrRetry"));
    const QString corrected = QStringLiteral("Retain these corrections");
    text->setReadOnly(false);
    text->setPlainText(corrected);
    text->document()->setModified(true);
    bool confirmed = false;
    QTimer::singleShot(0, &dialog, [&] {
        auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!question || question->parentWidget() != &dialog) return;
        confirmed = true;
        question->button(QMessageBox::Yes)->click();
        dialog.reject();
    });
    dialog.start();
    const bool passed = confirmed && !dialog.isVisible() && !engine.busy.load()
        && retry->isEnabled() && !text->isReadOnly() && text->toPlainText() == corrected;
    QThreadPool::globalInstance()->waitForDone();
    if (!passed) {
        std::fprintf(stderr, "FAIL: closing OCR during retry confirmation must not start work or alter corrections\n");
        return 1;
    }
    std::puts("PASS: closed OCR cards cannot restart recognition after retry confirmation");
    const bool formats =
        formatText(QStringLiteral("截图\n文字\nHello\nWorld"), TextFormat::JoinLines) == QStringLiteral("截图文字Hello World")
        && formatText(QStringLiteral("a b\tc\nd e"), TextFormat::RemoveSpaces) == QStringLiteral("abc\nde")
        && formatText(QStringLiteral("你好,世界!3.14"), TextFormat::FullWidth) == QStringLiteral("你好，世界！3.14")
        && formatText(QStringLiteral("你好，世界！。"), TextFormat::HalfWidth) == QStringLiteral("你好,世界!.");
    const bool layouts = prefersRows(QSize(1600, 100), QSize(1000, 650))
        && !prefersRows(QSize(100, 800), QSize(1000, 650)) && !prefersRows(QSize(400, 400), QSize(1000, 650));
    OcrDialog finder(image);
    finder.show();
    QApplication::processEvents();
    QThreadPool::globalInstance()->waitForDone();
    QApplication::processEvents();
    auto *edit = finder.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    edit->setReadOnly(false);
    edit->setPlainText(QStringLiteral("ab ab ab"));
    finder.showFind(true);
    finder.m_findEdit->setText(QStringLiteral("ab"));
    finder.m_replaceEdit->setText(QStringLiteral("x"));
    const bool counted = finder.matches().size() == 3 && edit->extraSelections().size() == 3;
    finder.replaceOne();
    const bool one = edit->toPlainText() == QStringLiteral("x ab ab");
    finder.replaceAll();
    const bool all = edit->toPlainText() == QStringLiteral("x x x") && finder.matches().isEmpty();
    edit->undo();
    const bool undo = edit->toPlainText() == QStringLiteral("x ab ab");
    finder.hideFind();
    const bool hidden = !finder.m_find->isVisible() && edit->extraSelections().isEmpty();
    // Format menu: Auto first and checked by default, a chosen mode becomes the only check, Auto restores.
    finder.m_raw = QStringLiteral("a b\nc");
    edit->setPlainText(finder.m_raw);
    const QList<QAction *> modes = finder.m_format->menu()->actions();
    modes.at(2)->trigger();
    const bool chosen = edit->toPlainText() == QStringLiteral("ab\nc") && modes.at(2)->isChecked()
        && !modes.at(0)->isChecked();
    modes.at(0)->trigger();
    const bool restored = modes.at(0)->text() == QStringLiteral("自动") && modes.at(0)->isChecked()
        && edit->toPlainText() == finder.m_raw;
    // Wheel zoom without any prior mouse move keeps the point under the cursor in place.
    auto *view = finder.findChild<QGraphicsView *>();
    const QPoint at(view->viewport()->width() / 3, view->viewport()->height() / 3);
    const QPointF before = view->mapToScene(at);
    QWheelEvent wheel(at, view->viewport()->mapToGlobal(at), {}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(view->viewport(), &wheel);
    const bool anchored = QLineF(view->mapToScene(at), before).length() < 2 / view->transform().m11();
    // Mouse clicks must not focus (and outline) any card button.
    bool noClickFocus = true;
    for (auto *button : finder.findChildren<QToolButton *>())
        noClickFocus = noClickFocus && !(button->focusPolicy() & Qt::ClickFocus);
    finder.close();
    QThreadPool::globalInstance()->waitForDone();
    if (!(formats && layouts && counted && one && all && undo && hidden && chosen && restored && anchored && noClickFocus)) {
        std::fprintf(stderr, "FAIL: text formats %d, split choice %d, find/replace %d%d%d%d%d, format menu %d%d, wheel anchor %d, click focus %d\n",
                     formats, layouts, counted, one, all, undo, hidden, chosen, restored, anchored, noClickFocus);
        return 1;
    }
    std::puts("PASS: text formats, format menu checks, cursor-anchored wheel zoom, split choice and find/replace with undo");
    return 0;
}
