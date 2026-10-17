#include "ocr_dialog.h"
#include "translate_gateway.h"
#include <QApplication>
#include <QEventLoop>
#include <QMessageBox>
#include <QPainter>
#include <QElapsedTimer>
#include <QComboBox>
#include <QLabel>
#include <QSettings>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextDocument>
#include <QThreadPool>
#include <QToolButton>
#include <QMenu>
#include <QAction>
#include <QLineF>
#include <QNetworkProxy>
#include <cstdio>
#include <algorithm>

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

int OcrDialog::testTranslate()
{
    // Part 1: live smoke of the aggregating gateway as wired into the card -
    // two lines in, two aligned non-empty translations out (auto, target zh).
    TranslateGateway gateway;
    QEventLoop loop;
    TranslateResult out;
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    gateway.translate({QStringLiteral("The quick brown fox"), QStringLiteral("jumps over the lazy dog")},
                      QString(), QStringLiteral("zh"), TranslateGateway::Provider::Auto,
                      [&](const TranslateResult &result) { out = result; loop.quit(); });
    loop.exec();
    const bool aligned = out.code == TranslateResult::Error::None && out.lines.size() == 2
        && !out.lines.constFirst().isEmpty() && !out.lines.constLast().isEmpty();
    if (!aligned) {
        std::fprintf(stderr, "FAIL: gateway smoke: %s\n",
            qPrintable(out.code == TranslateResult::Error::None ? QStringLiteral("unaligned or empty output") : out.error));
        return 1;
    }
    std::printf("PASS: %s translated 2/2 lines aligned (%lldms)\n", qPrintable(out.provider), out.elapsed);

    // Part 2: the whole card chain - rendered text through recognition and the
    // gateway ends up as translated text in the pane.
    const QString loadError = OcrEngine::instance().load(QCoreApplication::applicationDirPath());
    if (!loadError.isEmpty()) { std::printf("SKIP: card chain: %s\n", qPrintable(loadError)); return 0; }
    QImage image(640, 140, QImage::Format_RGB32);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        QFont font = painter.font();
        font.setPointSize(26);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(image.rect(), Qt::AlignCenter, QStringLiteral("The quick brown fox jumps over the lazy dog"));
    }
    // The card reads the last choice from settings: start from defaults, restore after.
    QSettings settings(QStringLiteral("pinshot"), QStringLiteral("pinshot"));
    const QVariant savedProvider = settings.value(QStringLiteral("Translate/provider")),
                   savedSource = settings.value(QStringLiteral("Translate/source")),
                   savedTarget = settings.value(QStringLiteral("Translate/target"));
    settings.remove(QStringLiteral("Translate"));
    settings.setValue(QStringLiteral("Translate/provider"), int(TranslateGateway::Provider::Youdao));   // Old builds stored it.
    settings.sync();
    const auto restore = [&] {
        for (const auto &[key, value] : {std::pair(QStringLiteral("Translate/provider"), savedProvider),
                                         std::pair(QStringLiteral("Translate/source"), savedSource),
                                         std::pair(QStringLiteral("Translate/target"), savedTarget)}) {
            if (value.isValid()) settings.setValue(key, value);
            else settings.remove(key);
        }
    };
    OcrDialog dialog(image, {}, true);
    dialog.show();
    QElapsedTimer deadline;
    deadline.start();
    while (dialog.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"))->toPlainText().isEmpty()
           && deadline.elapsed() < 90000)
        QApplication::processEvents(QEventLoop::AllEvents, 200);
    auto *pane = dialog.findChild<QPlainTextEdit *>(QStringLiteral("ocrText"));
    const QString translated = pane->toPlainText();
    if (translated.isEmpty()) {
        restore();
        std::fprintf(stderr, "FAIL: card chain produced no translated text within 90s\n");
        return 1;
    }
    std::printf("PASS: card chain rendered %d chars of translated text\n", int(translated.size()));

    // Part 3: pickers. Engine list with Auto first, auto-detect source, zh target;
    // switching the engine re-translates with it, and the retry button reruns.
    auto *provider = dialog.findChild<QComboBox *>(QStringLiteral("translateProvider"));
    auto *source = dialog.findChild<QComboBox *>(QStringLiteral("translateSource"));
    auto *target = dialog.findChild<QComboBox *>(QStringLiteral("translateTarget"));
    auto *again = dialog.findChild<QToolButton *>(QStringLiteral("ocrRetry"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("ocrStatus"));
    const auto settle = [&] {
        QElapsedTimer wait;
        wait.start();
        while ((!again->isEnabled() || pane->toPlainText().isEmpty()) && wait.elapsed() < 60000)
            QApplication::processEvents(QEventLoop::AllEvents, 200);
    };
    const bool lists = provider && source && target && again && provider->count() == 8
        && provider->itemData(0).toInt() == int(TranslateGateway::Provider::Auto) && provider->currentIndex() == 0
        && source->itemData(0).toString().isEmpty() && target->currentData().toString() == QStringLiteral("zh")
        && again->isEnabled()
        && again->parentWidget() == source->parentWidget();   // Pickers and card actions share the top toolbar.
    bool switched = false, reran = false, retargeted = false;
    if (lists) {
        provider->setCurrentIndex(provider->findData(int(TranslateGateway::Provider::Transmart)));
        settle();
        switched = !pane->toPlainText().isEmpty() && status->text().contains(QStringLiteral("腾讯交互翻译"));
        // Transmart lacks Dutch: the target list follows the engine matrix.
        retargeted = target->findData(QStringLiteral("nl")) < 0;
        // Refresh recognizes again and translates with the engine still chosen.
        again->click();
        settle();
        reran = !pane->toPlainText().isEmpty() && status->text().contains(QStringLiteral("腾讯交互翻译"))
            && !QSettings(QStringLiteral("pinshot"), QStringLiteral("pinshot")).contains(QStringLiteral("Translate/provider"))
            && dialog.m_provider->displayText() == QStringLiteral("翻译引擎 (腾讯交互翻译)");
        provider->setCurrentIndex(0);   // Back to Auto: the picker names the engine that answered.
        settle();
        const QString shown = dialog.m_provider->displayText();
        reran = reran && shown.startsWith(QStringLiteral("翻译引擎 (")) && shown != QStringLiteral("翻译引擎 (自动选择)");
    }
    if (!(lists && switched && retargeted && reran)) {
        restore();
        std::fprintf(stderr, "FAIL: translate pickers lists %d engine switch %d matrix %d retry %d (%s)\n",
                     lists, switched, retargeted, reran, status ? qPrintable(status->text()) : "");
        return 1;
    }
    std::puts("PASS: engine/source/target pickers, engine switch re-translates, retry reruns");

    // Part 4: the recognized text sits in an editable source pane; auto-detect names the
    // language it found; typing a correction re-translates after the pause.
    const QString preview = qEnvironmentVariable("PINSHOT_TRANSLATE_PREVIEW");
    if (!preview.isEmpty()) dialog.grab().save(preview + QStringLiteral("-result.png"));
    auto *sourceText = dialog.findChild<QPlainTextEdit *>(QStringLiteral("translateSourceText"));
    const bool recognized = sourceText && sourceText->toPlainText().contains(QStringLiteral("fox"), Qt::CaseInsensitive)
        && !sourceText->isReadOnly() && !sourceText->document()->isModified();
    // Each text block carries its own copy button and character count along its bottom edge.
    const QList<QLabel *> counts = dialog.findChildren<QLabel *>(QStringLiteral("ocrCharacterCount"));
    auto *sourceCopy = dialog.findChild<QToolButton *>(QStringLiteral("translateSourceCopy"));
    const bool blocks = counts.size() == 2 && sourceCopy && sourceCopy->isEnabled()
        && dialog.findChild<QWidget *>(QStringLiteral("translateToolbar"))->isAncestorOf(source)
        && sourceText && sourceCopy->mapTo(&dialog, QPoint()).y() >= sourceText->mapTo(&dialog, QPoint()).y() + sourceText->height()
        && std::any_of(counts.begin(), counts.end(), [&](QLabel *l) { return l->text() == QString::number(sourceText->toPlainText().toUcs4().size()); })
        && std::any_of(counts.begin(), counts.end(), [&](QLabel *l) { return l->text() == QString::number(pane->toPlainText().toUcs4().size()); });
    if (!blocks) {
        restore();
        std::fprintf(stderr, "FAIL: source/translation blocks lack their own copy button and character count\n");
        return 1;
    }
    const bool named = dialog.m_source->displayText() == QStringLiteral("语言检测 (英语)");
    bool edited = false, hazed = false, quiet = false;
    auto *mask = dialog.findChild<QWidget *>(QStringLiteral("retranslateMask"));
    auto *maskButton = dialog.findChild<QPushButton *>(QStringLiteral("retranslateButton"));
    if (recognized && mask && maskButton) {
        const QString before = pane->toPlainText();
        const bool clearAtFirst = !mask->isVisible();
        sourceText->setFocus();
        sourceText->selectAll();
        // Same path as typing; switching to Japanese must re-detect the language on the next run.
        sourceText->insertPlainText(QStringLiteral("おはようございます。また明日会いましょう。"));
        // Typing alone never calls the engine: the old translation stays under the haze.
        QElapsedTimer pause;
        pause.start();
        while (pause.elapsed() < 1500) QApplication::processEvents(QEventLoop::AllEvents, 100);
        quiet = pane->toPlainText() == before && again->isEnabled();
        hazed = clearAtFirst && mask->isVisible() && mask->geometry() == pane->rect();
        if (!preview.isEmpty()) dialog.grab().save(preview + QStringLiteral("-hazed.png"));
        maskButton->click();
        QElapsedTimer wait;
        wait.start();
        while ((pane->toPlainText() == before || pane->toPlainText().isEmpty() || !again->isEnabled())
               && wait.elapsed() < 60000)
            QApplication::processEvents(QEventLoop::AllEvents, 100);
        edited = !pane->toPlainText().isEmpty() && pane->toPlainText() != before && sourceText->hasFocus()
            && !mask->isVisible() && dialog.m_source->displayText() == QStringLiteral("语言检测 (日语)");
    }
    if (!preview.isEmpty()) dialog.grab().save(preview + QStringLiteral("-edited.png"));
    restore();
    if (!(recognized && named && quiet && hazed && edited)) {
        std::fprintf(stderr, "FAIL: source pane recognized %d detected-name %d typing quiet %d haze %d button re-translates %d (%s | %s)\n",
                     recognized, named, quiet, hazed, edited, qPrintable(dialog.m_source->displayText()), qPrintable(status->text()));
        return 1;
    }
    std::puts("PASS: editable source pane, detected language named, edits haze the translation until 重新翻译");

    // Part 5: no back/search/format buttons; display modes; find scoped to the pane it runs in;
    // failures read as plain advice, never the raw engine error.
    auto *view = dialog.findChild<QToolButton *>(QStringLiteral("translateView"));
    auto *imagePanel = dialog.findChild<QWidget *>(QStringLiteral("ocrImagePanel"));
    const bool trimmed = !dialog.findChild<QToolButton *>(QStringLiteral("ocrBack"))
        && !dialog.findChild<QToolButton *>(QStringLiteral("ocrSearch"))
        && !dialog.findChild<QToolButton *>(QStringLiteral("ocrFormat"))->isVisible();
    bool modes = view && view->menu() && view->menu()->actions().size() == 3;
    if (modes) {
        const QList<QAction *> picks = view->menu()->actions();
        picks[1]->trigger();
        modes = !imagePanel->isVisible() && sourceText->isVisible() && pane->isVisible();
        // Original-and-translation still splits: original first (top, then left), translation after.
        auto *split = dialog.findChild<QToolButton *>(QStringLiteral("ocrLayout"));
        const bool imageRows = dialog.m_rows;
        modes = modes && split->isVisible() && dialog.m_texts->orientation() == Qt::Vertical;
        split->click();
        QApplication::processEvents();
        modes = modes && dialog.m_texts->orientation() == Qt::Horizontal && dialog.m_rows == imageRows
            && sourceText->mapTo(&dialog, QPoint()).x() < pane->mapTo(&dialog, QPoint()).x();
        split->click();
        modes = modes && dialog.m_texts->orientation() == Qt::Vertical;
        picks[2]->trigger();
        modes = modes && !split->isVisible();
        modes = modes && !imagePanel->isVisible() && !sourceText->isVisible() && pane->isVisible()
            && QSettings(QStringLiteral("pinshot"), QStringLiteral("pinshot")).value(QStringLiteral("Translate/view")).toInt() == 2;
        picks[0]->trigger();
        modes = modes && imagePanel->isVisible() && sourceText->isVisible();
    }
    dialog.showFind(false, sourceText);
    dialog.m_findEdit->setText(QStringLiteral("明日"));
    const bool scoped = dialog.m_find->parentWidget()->isAncestorOf(sourceText)
        && !dialog.m_find->parentWidget()->isAncestorOf(pane)
        && sourceText->extraSelections().size() == 1 && pane->extraSelections().isEmpty();
    // Opening replace from the strip (its toggle, or Ctrl+H typed inside it) keeps it in the original pane.
    dialog.m_replaceToggle->click();
    const bool stays = dialog.m_find->parentWidget()->isAncestorOf(sourceText) && dialog.m_replaceRow->isVisible()
        && dialog.m_findTarget == sourceText;
    dialog.showFind(false, pane);
    const bool moved = dialog.m_find->parentWidget()->isAncestorOf(pane) && sourceText->extraSelections().isEmpty();
    dialog.hideFind();
    auto *retranslate = dialog.findChild<QToolButton *>(QStringLiteral("translateAgain"));
    // A dead proxy fails every engine at once: a chosen engine and Auto each get their own advice.
    const QNetworkProxy savedProxy = QNetworkProxy::applicationProxy();
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 1));
    const auto failWith = [&](TranslateGateway::Provider engine) {
        provider->setCurrentIndex(provider->findData(int(engine)));
        if (provider->currentData().toInt() == int(engine)) retranslate->click();   // Already selected: re-translate.
        QElapsedTimer wait;
        wait.start();
        while (!again->isEnabled() && wait.elapsed() < 120000) QApplication::processEvents(QEventLoop::AllEvents, 100);
        return status->text();
    };
    const QString chosen = failWith(TranslateGateway::Provider::Edge);
    const QString automatic = failWith(TranslateGateway::Provider::Auto);
    QNetworkProxy::setApplicationProxy(savedProxy);
    const bool friendly = chosen == QStringLiteral("Edge暂时无法翻译，请稍后重试或切换为其他翻译引擎")
        && automatic == QStringLiteral("翻译服务暂时不可用，请检查网络后重试") && !pane->toPlainText().isEmpty();
    restore();
    QSettings(QStringLiteral("pinshot"), QStringLiteral("pinshot")).remove(QStringLiteral("Translate/view"));
    if (!(trimmed && retranslate && modes && scoped && stays && moved && friendly)) {
        std::fprintf(stderr, "FAIL: trimmed %d retranslate %d modes %d find scoped %d replace stays %d moved %d friendly %d (%s | %s)\n",
                     trimmed, retranslate != nullptr, modes, scoped, stays, moved, friendly, qPrintable(chosen), qPrintable(automatic));
        return 1;
    }
    std::puts("PASS: trimmed toolbar, display modes with text split, per-pane find/replace, friendly failure advice");
    return 0;
}
