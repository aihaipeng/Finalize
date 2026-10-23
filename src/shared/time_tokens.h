#pragma once

#include <QDateTime>
#include <QString>

// Shared {{...}} time-token engine for watermark text and export file names.
QString timeTokenValue(const QString &spec, const QDateTime &stamp);
QString resolveTimeTokens(const QString &text, const QDateTime &stamp);
