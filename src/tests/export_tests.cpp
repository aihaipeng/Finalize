#include "platform/output.h"
#include "shared/export_dialog.h"
#include "shared/settings.h"

#include <QApplication>
#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFocusEvent>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPainter>
#include <QSlider>
#include <QStringList>
#include <QTemporaryDir>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <QWidget>
#include <cstdio>

namespace {

struct FailureCounter
{
    int count = 0;
    void check(bool ok, const char *message)
    {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++count;
        }
    }
};

QImage solidImage(const QSize &size, const QColor &color)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    return image;
}

QImage transparentImage(const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    return image;
}

void waitForEstimate()
{
    QEventLoop loop;
    QTimer::singleShot(250, &loop, &QEventLoop::quit);
    loop.exec();
}

QToolButton *segmentNamed(ExportDialog &dialog, const QString &text)
{
    for (QToolButton *button : dialog.findChildren<QToolButton *>(QStringLiteral("exportSegment")))
        if (button->text() == text) return button;
    return nullptr;
}

} // namespace

int runExportTests(const QString &previewPrefix)
{
    FailureCounter failures;

    const QDateTime stamp(QDate(2026, 10, 24), QTime(1, 3, 2));
    failures.check(ExportDialog::resolvedName(QStringLiteral("LensSnip_{{yyyy-MM-dd HH:mm:ss}}"), stamp)
                       == QStringLiteral("LensSnip_2026-10-24_01-03-02"),
                   "time token resolves to a filename-safe stem");
    failures.check(ExportDialog::resolvedName(QStringLiteral("shot.PNG"), stamp) == QStringLiteral("shot"),
                   "a typed image suffix is stripped");
    failures.check(ExportDialog::resolvedName(QStringLiteral("a:b/c d"), stamp) == QStringLiteral("a-b-c_d"),
                   "illegal characters and whitespace are normalized");
    failures.check(ExportDialog::resolvedName(QStringLiteral("A{{yyyy"), stamp) == QStringLiteral("A{{yyyy"),
                   "an unclosed token stays literal");
    failures.check(ExportDialog::resolvedName(QString(), stamp) == QStringLiteral("LensSnip_2026-10-24_01-03-02"),
                   "an empty pattern falls back to the default stem");

    QTemporaryDir directory;
    failures.check(directory.isValid(), "export regression has an isolated directory");

    // Pipeline: scaling, quality, alpha flattening and alpha removal.
    const QImage opaque = solidImage(QSize(64, 40), QColor("#24536b"));
    const QString scaledPath = directory.filePath(QStringLiteral("scaled.png"));
    failures.check(saveImageAtomic(opaque, scaledPath, {QStringLiteral("png"), -1, 50}),
                   "scaled export writes a PNG");
    failures.check(QImage(scaledPath).size() == QSize(32, 20),
                   "50% export uses the documented rounding rule");

    const QString q95Path = directory.filePath(QStringLiteral("q95.jpg"));
    const QString q30Path = directory.filePath(QStringLiteral("q30.jpg"));
    failures.check(saveImageAtomic(opaque, q95Path, {QStringLiteral("jpg"), 95, 100}),
                   "JPEG q95 export writes");
    failures.check(saveImageAtomic(opaque, q30Path, {QStringLiteral("jpg"), 30, 100}),
                   "JPEG q30 export writes");
    failures.check(QFileInfo(q30Path).size() < QFileInfo(q95Path).size(),
                   "lower JPEG quality produces a smaller file");
    failures.check(QImageReader(q95Path).format().toLower() == QByteArray("jpeg"),
                   "JPEG export reads back as jpeg");

    QImage corners = transparentImage(QSize(24, 24));
    {
        QPainter painter(&corners);
        painter.fillRect(QRect(6, 6, 12, 12), QColor("#d02020"));
    }
    const QString flatPath = directory.filePath(QStringLiteral("flat.jpg"));
    failures.check(saveImageAtomic(corners, flatPath, {QStringLiteral("jpg"), 90, 100}),
                   "JPEG export flattens transparency");
    const QImage flattened(flatPath);
    const QColor corner = flattened.pixelColor(0, 0);
    failures.check(corner.red() > 240 && corner.green() > 240 && corner.blue() > 240,
                   "transparent corners become white in JPEG");

    const QString opaquePng = directory.filePath(QStringLiteral("opaque.png"));
    const QString alphaPng = directory.filePath(QStringLiteral("alpha.png"));
    failures.check(saveImageAtomic(opaque, opaquePng, {QStringLiteral("png"), -1, 100}),
                   "opaque PNG export writes");
    failures.check(saveImageAtomic(corners, alphaPng, {QStringLiteral("png"), -1, 100}),
                   "alpha PNG export writes");
    failures.check(!QImage(opaquePng).hasAlphaChannel(),
                   "fully opaque PNG drops the alpha channel");
    failures.check(QImage(alphaPng).hasAlphaChannel(),
                   "transparent PNG keeps the alpha channel");

    if (writableImageFormats().contains(QStringLiteral("webp"))) {
        const QString webpPath = directory.filePath(QStringLiteral("alpha.webp"));
        failures.check(saveImageAtomic(opaque, webpPath, {QStringLiteral("webp"), 80, 100}),
                       "WebP export writes");
        failures.check(QImageReader(webpPath).format().toLower() == QByteArray("webp"),
                       "WebP export reads back as webp");
    } else {
        std::printf("SKIP: WebP encoder not installed\n");
    }

    // Card behavior: defaults, format switching, quality semantics, estimate and path composition.
    LensSnipSettings settings;
    const QStringList keys = {
        QStringLiteral("Output/LastFormat"), QStringLiteral("Output/Quality_jpg"),
        QStringLiteral("Output/Quality_webp"), QStringLiteral("Output/Scale"),
        QStringLiteral("Output/LastDir")};
    QHash<QString, QVariant> saved;
    for (const QString &key : keys) saved.insert(key, settings.value(key));
    settings.setValue(QStringLiteral("Output/LastFormat"), QStringLiteral("jpg"));
    settings.setValue(QStringLiteral("Output/Quality_jpg"), 72);
    settings.setValue(QStringLiteral("Output/Scale"), 50);
    settings.setValue(QStringLiteral("Output/LastDir"), directory.path());
    settings.sync();

    const QStringList formats = writableImageFormats();
    failures.check(formats.contains(QStringLiteral("png")), "PNG writer is deployed");
    failures.check(formats.contains(QStringLiteral("jpg")), "JPEG writer is deployed");

    QImage previewImage(320, 240, QImage::Format_RGB32);
    previewImage.fill(QColor("#243648"));
    QImage dialogImage = previewImage;
    {
        QPainter painter(&dialogImage);
        painter.fillRect(QRect(40, 40, 120, 80), QColor("#5e6ad2"));
        painter.fillRect(QRect(190, 130, 90, 60), QColor("#ed1c24"));
    }

    ExportDialog dialog(dialogImage, formats, directory.path(),
                        QStringLiteral("LensSnip_{{yyyy-MM-dd HH:mm:ss}}"));
    auto *quality = dialog.findChild<QSlider *>(QStringLiteral("qualitySlider"));
    auto *scale = dialog.findChild<QSlider *>(QStringLiteral("scaleSlider"));
    auto *qualityValue = dialog.findChild<QLabel *>(QStringLiteral("qualityValue"));
    auto *scaleValue = dialog.findChild<QLabel *>(QStringLiteral("scaleValue"));
    auto *name = dialog.findChild<QLineEdit *>(QStringLiteral("exportName"));
    auto *path = dialog.findChild<QLineEdit *>(QStringLiteral("exportPath"));
    auto *suffix = dialog.findChild<QLabel *>(QStringLiteral("exportSuffix"));
    auto *estimate = dialog.findChild<QLabel *>(QStringLiteral("exportEstimate"));
    auto *surface = dialog.findChild<QWidget *>(QStringLiteral("exportSurface"));
    const auto segments = dialog.findChildren<QToolButton *>(QStringLiteral("exportSegment"));

    failures.check(quality && scale && qualityValue && scaleValue && name && path && suffix
                       && estimate && surface && segments.size() == formats.size(),
                   "card exposes every expected element");
    if (!(quality && scale && qualityValue && scaleValue && name && path && suffix && estimate
          && surface && segments.size() == formats.size())) {
        for (const QString &key : keys) {
            if (saved.value(key).isValid()) settings.setValue(key, saved.value(key));
            else settings.remove(key);
        }
        settings.sync();
        return 1;
    }

    const bool hasJpg = formats.contains(QStringLiteral("jpg"));
    failures.check(hasJpg && quality->isEnabled() && quality->value() == 72,
                   "last format and per-format quality are restored");
    failures.check(suffix->text() == (hasJpg ? QStringLiteral(".jpg") : QStringLiteral(".png")),
                   "suffix matches the restored format");
    failures.check(scale->value() == 50 && scaleValue->text() == QStringLiteral("50%"),
                   "scale is restored and displays a percentage");
    waitForEstimate();
    failures.check(estimate->text().contains(QStringLiteral("160 × 120")),
                   "estimate reports the scaled dimensions");
    failures.check(path->text() == QDir::toNativeSeparators(directory.path()),
                   "the remembered directory is shown in the path field");

    if (auto *png = segmentNamed(dialog, QStringLiteral("PNG"))) png->click();
    failures.check(!quality->isEnabled() && quality->value() == 100
                       && qualityValue->text() == QStringLiteral("100")
                       && suffix->text() == QStringLiteral(".png"),
                   "PNG disables quality at 100 and switches the suffix");
    waitForEstimate();
    failures.check(estimate->text().contains(QStringLiteral("160 × 120")),
                   "lossless quality changes do not disturb the scaled estimate");

    if (hasJpg) {
        if (auto *jpg = segmentNamed(dialog, QStringLiteral("JPG"))) jpg->click();
        failures.check(quality->isEnabled() && quality->value() == 72,
                       "returning to JPG restores its own quality");
        failures.check(suffix->text() == QStringLiteral(".jpg"), "JPG suffix follows the format");
    }

    scale->setValue(100);
    waitForEstimate();
    failures.check(estimate->text().contains(QStringLiteral("320 × 240")),
                   "100% reports the source dimensions");
    name->setText(QStringLiteral("Shot_{{yyyy-MM-dd}}"));
    const QString expectedStem = QStringLiteral("Shot_")
        + QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));
    const ExportRequest request = dialog.request();
    failures.check(request.format == (hasJpg ? QStringLiteral("jpg") : QStringLiteral("png")),
                   "request carries the selected format");
    failures.check(request.scalePercent == 100, "request carries the selected scale");
    failures.check(QFileInfo(request.path).fileName()
                       == expectedStem + (hasJpg ? QStringLiteral(".jpg") : QStringLiteral(".png")),
                   "request composes the resolved name and suffix");
    failures.check(QFileInfo(request.path).absolutePath() == QFileInfo(directory.path()).absoluteFilePath(),
                   "request keeps the chosen directory");

    dialog.show();
    QApplication::processEvents();
    auto *titleRowWidget = dialog.findChild<QWidget *>(QStringLiteral("exportTitleRow"));
    std::printf("CARD dpr=%.2f surface=%dx%d titleRowY=%d\n", surface->devicePixelRatioF(),
                surface->width(), surface->height(), titleRowWidget ? titleRowWidget->y() : -1);
    const auto printRow = [&dialog](const char *name) {
        auto *row = dialog.findChild<QWidget *>(QString::fromLatin1(name));
        if (!row) return;
        auto *label = row->findChild<QLabel *>(QStringLiteral("exportLabel"));
        QWidget *control = nullptr;
        if (row->layout() && row->layout()->count() > 1) control = row->layout()->itemAt(1)->widget();
        if (!label || !control) return;
        std::printf("ROW %s y=%d h=%d labelX=%d labelW=%d ctrlX=%d ctrlW=%d ctrlH=%d\n",
                    name, row->y(), row->height(), label->x(), label->width(),
                    control->x(), control->width(), control->height());
    };
    for (const char *name : {"formatRow", "qualityRow", "scaleRow", "nameRow", "pathRow"})
        printRow(name);
    if (auto *footerWidget = dialog.findChild<QWidget *>(QStringLiteral("exportFooter")))
        std::printf("FOOTER y=%d h=%d\n", footerWidget->y(), footerWidget->height());
    auto *titleLabel = dialog.findChild<QLabel *>(QStringLiteral("exportTitle"));
    auto *labelSample = dialog.findChild<QLabel *>(QStringLiteral("exportLabel"));
    if (titleLabel && labelSample && estimate && qualityValue && suffix)
        std::printf("FONTS title=%d label=%d value=%d estimate=%d suffix=%d\n",
                    titleLabel->font().pixelSize(), labelSample->font().pixelSize(),
                    qualityValue->font().pixelSize(), estimate->font().pixelSize(),
                    suffix->font().pixelSize());

    if (!previewPrefix.isEmpty()) {
        if (auto *png = segmentNamed(dialog, QStringLiteral("PNG"))) png->click();
        QApplication::processEvents();
        surface->grab().save(previewPrefix + QStringLiteral("-export-png.png"));
        if (hasJpg) {
            if (auto *jpg = segmentNamed(dialog, QStringLiteral("JPG"))) jpg->click();
            QApplication::processEvents();
            surface->grab().save(previewPrefix + QStringLiteral("-export-jpg.png"));
        }
        name->setFocus(Qt::MouseFocusReason);
        {
            QFocusEvent mouseFocus(QEvent::FocusIn, Qt::MouseFocusReason);
            QApplication::sendEvent(name, &mouseFocus);
        }
        QApplication::processEvents();
        surface->grab().save(previewPrefix + QStringLiteral("-export-mouse.png"));
        name->setFocus(Qt::TabFocusReason);
        {
            QFocusEvent keyFocus(QEvent::FocusIn, Qt::TabFocusReason);
            QApplication::sendEvent(name, &keyFocus);
        }
        QApplication::processEvents();
        surface->grab().save(previewPrefix + QStringLiteral("-export-tab.png"));
    }
    dialog.close();

    for (const QString &key : keys) {
        if (saved.value(key).isValid()) settings.setValue(key, saved.value(key));
        else settings.remove(key);
    }
    settings.sync();

    if (failures.count == 0) std::printf("PASS: export card and pipeline\n");
    return failures.count == 0 ? 0 : 1;
}
