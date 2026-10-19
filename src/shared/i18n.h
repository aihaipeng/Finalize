#pragma once

#include <QString>
#include <QStringList>

class QTranslator;

// Runtime language packs live in <application dir>/language.
// The source language is Simplified Chinese, so a missing pack falls back to the
// strings compiled into the application.
namespace I18n {
QString translationDir();
QStringList availableLocales();
QString displayName(const QString &locale);
QString configuredLocale();                  // "system" or a locale such as "en_US"
QString effectiveLocale(const QStringList &arguments);
bool isTestMode(const QStringList &arguments);
bool install(QTranslator *applicationTranslator, QTranslator *qtTranslator,
             const QString &locale, bool loadApplication = true);
void saveLocale(const QString &locale);
}
