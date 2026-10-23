#include "shared/time_tokens.h"

#include <QLocale>
#include <QStringList>

namespace {
QString timeOffset(const QDateTime &dt, bool colon)
{
    const int seconds = dt.offsetFromUtc();
    const int absolute = qAbs(seconds);
    return QStringLiteral("%1%2%3%4")
        .arg(seconds < 0 ? QLatin1Char('-') : QLatin1Char('+'))
        .arg(absolute / 3600, 2, 10, QLatin1Char('0'))
        .arg(colon ? QStringLiteral(":") : QString())
        .arg((absolute % 3600) / 60, 2, 10, QLatin1Char('0'));
}
}

QString timeTokenValue(const QString &spec, const QDateTime &stamp)
{
    if (spec == QLatin1String("@iso-offset"))
        return stamp.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss")) + timeOffset(stamp, true);
    if (spec == QLatin1String("@iso-utc"))
        return stamp.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss")) + QLatin1Char('Z');
    if (spec == QLatin1String("@rfc3339"))
        return stamp.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss")) + timeOffset(stamp, true);
    if (spec == QLatin1String("@rfc3339-ms"))
        return stamp.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz")) + timeOffset(stamp, true);
    if (spec == QLatin1String("@rfc1123"))
        return QLocale::c().toString(stamp.toUTC(), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss"))
             + QStringLiteral(" GMT");
    if (spec == QLatin1String("@rfc2822"))
        return QLocale::c().toString(stamp, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss"))
             + QLatin1Char(' ') + timeOffset(stamp, false);
    return stamp.toString(spec);
}

QString resolveTimeTokens(const QString &text, const QDateTime &stamp)
{
    if (!text.contains(QStringLiteral("{{"))) return text;
    QString out;
    int cursor = 0;
    while (cursor < text.size()) {
        const int open = text.indexOf(QStringLiteral("{{"), cursor);
        if (open < 0) {
            out += text.mid(cursor);
            break;
        }
        out += text.mid(cursor, open - cursor);
        const int close = text.indexOf(QStringLiteral("}}"), open + 2);
        if (close < 0) return text;               // An unclosed token leaves the whole text literal.
        out += timeTokenValue(text.mid(open + 2, close - open - 2), stamp);
        cursor = close + 2;
    }
    return out;
}
