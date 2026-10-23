#pragma once

#include <QDateTime>
#include <QDialog>
#include <QHash>
#include <QImage>
#include <QSize>
#include <QStringList>

struct ExportRequest
{
    QString path;
    QString format;
    int quality = -1;
    int scalePercent = 100;
};

class QButtonGroup;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QTimer;

// Compact save card shared by the capture overlay, pins, history and long screenshots.
class ExportDialog : public QDialog
{
    Q_OBJECT

public:
    ExportDialog(const QImage &image, const QStringList &formats,
                 const QString &defaultDirectory, const QString &defaultName,
                 QWidget *parent = nullptr);

    ExportRequest request() const;

    // Resolves the shared {{...}} time tokens and makes the stem safe for Windows file names.
    static QString resolvedName(const QString &pattern, const QDateTime &stamp);

private:
    void buildUi();
    void setFormat(const QString &format);
    void refreshQualityRow();
    void refreshEstimate();
    void scheduleEstimate();
    void updateNamePreview();
    void submit();
    QSize targetSize() const;
    QString suffixFor(const QString &format) const;
    QString composedPath() const;
    int qualityForSave() const;

    QImage m_image;
    QStringList m_formats;
    QHash<QString, int> m_qualities;
    QString m_format;
    QString m_defaultName;
    bool m_lossy = false;
    ExportRequest m_request;
    bool m_confirmed = false;

    int m_cardWidth = 440;
    int m_padding = 20;
    int m_shadow = 24;

    QButtonGroup *m_formatGroup = nullptr;
    QLabel *m_suffixLabel = nullptr;
    QLabel *m_qualityValue = nullptr;
    QLabel *m_scaleValue = nullptr;
    QLabel *m_estimate = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_path = nullptr;
    QSlider *m_quality = nullptr;
    QSlider *m_scale = nullptr;
    QPushButton *m_save = nullptr;
    QTimer *m_estimateTimer = nullptr;
};
