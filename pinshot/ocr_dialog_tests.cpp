#include "ocr_dialog.h"
#include <QApplication>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextDocument>
#include <QThreadPool>
#include <QToolButton>
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
    return 0;
}
