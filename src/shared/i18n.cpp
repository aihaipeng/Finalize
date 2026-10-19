#include "shared/i18n.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>
#include <QXmlStreamReader>

namespace {
QString languageOnly(const QString &locale)
{
    const int separator = locale.indexOf(QLatin1Char('_'));
    return separator > 0 ? locale.left(separator) : locale;
}

QString qtTranslationDir()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/translations");
}

// Direct TS loading keeps hand-edited language files usable without lrelease.
// QM is still the normal release format; TS entries missing or unfinished fall
// through to the QM translator and then to the Chinese source strings.
class TsTranslator : public QTranslator
{
public:
    explicit TsTranslator(QObject *parent = nullptr) : QTranslator(parent) {}

    bool loadFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return false;
        QXmlStreamReader xml(&file);
        QString context, source, translation, disambiguation;
        bool inContext = false;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                const auto name = xml.name();
                if (name == QLatin1String("context")) { inContext = true; context.clear(); }
                else if (inContext && name == QLatin1String("name")) context = xml.readElementText();
                else if (name == QLatin1String("message")) { source.clear(); translation.clear(); disambiguation.clear(); }
                else if (name == QLatin1String("source")) source = xml.readElementText();
                else if (name == QLatin1String("comment")) disambiguation = xml.readElementText();
                else if (name == QLatin1String("translation")) {
                    const auto attributes = xml.attributes();
                    // shortcut: numerus messages are QM-only until a plural string is added.
                    if (attributes.value(QLatin1String("type")) == QLatin1String("unfinished")
                        || attributes.value(QLatin1String("numerus")) == QLatin1String("yes")) {
                        xml.skipCurrentElement();
                    } else {
                        translation = xml.readElementText();
                    }
                }
            } else if (xml.isEndElement()) {
                if (xml.name() == QLatin1String("context")) inContext = false;
                else if (xml.name() == QLatin1String("message") && !context.isEmpty()
                         && !source.isEmpty() && !translation.isEmpty()) {
                    m_map.insert(key(context, source, disambiguation), translation);
                }
            }
        }
        return !xml.hasError();
    }

    QString translate(const char *context, const char *sourceText, const char *disambiguation = nullptr,
                      int n = -1) const override
    {
        Q_UNUSED(n);
        if (!context || !sourceText) return {};
        return m_map.value(key(QString::fromUtf8(context), QString::fromUtf8(sourceText),
                               disambiguation ? QString::fromUtf8(disambiguation) : QString()));
    }

private:
    static QString key(const QString &context, const QString &source, const QString &disambiguation)
    {
        return context + QChar(0x1f) + source + QChar(0x1f) + disambiguation;
    }

    QHash<QString, QString> m_map;
};
}

namespace I18n {
QString translationDir()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/language");
}

QStringList availableLocales()
{
    QStringList locales;
    const QDir dir(translationDir());
    for (const QFileInfo &info : dir.entryInfoList({QStringLiteral("nami_*.qm"), QStringLiteral("nami_*.ts")},
                                                   QDir::Files, QDir::Name)) {
        const QString base = info.completeBaseName();   // nami_en_US
        const QString locale = base.mid(QStringLiteral("nami_").size());
        if (!locale.isEmpty() && !locales.contains(locale)) locales.append(locale);
    }
    if (!locales.contains(QStringLiteral("zh_CN"))) locales.append(QStringLiteral("zh_CN"));
    locales.sort(Qt::CaseInsensitive);
    return locales;
}

QString displayName(const QString &locale)
{
    if (locale == QLatin1String("system")) return QCoreApplication::translate("I18n", "跟随系统");
    if (locale == QLatin1String("zh_CN")) return QCoreApplication::translate("I18n", "简体中文");
    const QLocale parsed(locale);
    const QString native = parsed.nativeLanguageName();
    return native.isEmpty() ? locale : native;
}

QString configuredLocale()
{
    const QString value = QSettings(QStringLiteral("Nami"), QStringLiteral("Nami"))
                              .value(QStringLiteral("UI/language")).toString();
    return value.isEmpty() ? QStringLiteral("system") : value;
}

bool isTestMode(const QStringList &arguments)
{
    for (const QString &argument : arguments) {
        if (argument.startsWith(QLatin1String("--test-")) || argument == QLatin1String("--probe-uia")
            || argument == QLatin1String("--dump-windows"))
            return true;
    }
    return false;
}

QString effectiveLocale(const QStringList &arguments)
{
    const int flag = arguments.indexOf(QStringLiteral("--lang"));
    if (flag >= 0 && flag + 1 < arguments.size() && !arguments.at(flag + 1).startsWith(QLatin1Char('-'))) {
        const QString requested = arguments.at(flag + 1);
        return requested == QLatin1String("system") ? QLocale::system().name() : requested;
    }
    // Regression tests assert the Chinese source strings regardless of the host locale.
    if (isTestMode(arguments)) return QStringLiteral("zh_CN");
    const QString configured = configuredLocale();
    return configured == QLatin1String("system") ? QLocale::system().name() : configured;
}

bool install(QTranslator *applicationTranslator, QTranslator *qtTranslator, const QString &locale,
             bool loadApplication)
{
    if (!applicationTranslator || !qtTranslator) return false;
    const QLocale parsed(locale);
    bool applicationLoaded = false;
    if (loadApplication) {
        applicationLoaded = applicationTranslator->load(parsed, QStringLiteral("nami"),
                                                         QStringLiteral("_"), translationDir());
        if (!applicationLoaded && !languageOnly(locale).isEmpty() && languageOnly(locale) != locale)
            applicationLoaded = applicationTranslator->load(QStringLiteral("nami_") + languageOnly(locale),
                                                            translationDir());
        if (applicationLoaded) QCoreApplication::installTranslator(applicationTranslator);

        const auto loadTs = [&](const QString &target) {
            const QFileInfo info(translationDir() + QStringLiteral("/nami_") + target + QStringLiteral(".ts"));
            if (!info.isFile() || info.size() > 16 * 1024 * 1024) return false;
            auto *translator = new TsTranslator(QCoreApplication::instance());
            if (!translator->loadFile(info.absoluteFilePath())) { delete translator; return false; }
            QCoreApplication::installTranslator(translator);
            return true;
        };
        bool tsLoaded = loadTs(locale);
        if (!tsLoaded && languageOnly(locale) != locale) tsLoaded = loadTs(languageOnly(locale));
        applicationLoaded = applicationLoaded || tsLoaded;
    }

    bool qtLoaded = qtTranslator->load(parsed, QStringLiteral("qtbase"), QStringLiteral("_"),
                                       qtTranslationDir());
    if (!qtLoaded) qtLoaded = qtTranslator->load(QStringLiteral("qtbase_") + languageOnly(locale),
                                                 qtTranslationDir());
    if (!qtLoaded)
        qtLoaded = qtTranslator->load(parsed, QStringLiteral("qtbase"), QStringLiteral("_"),
                                      QLibraryInfo::path(QLibraryInfo::TranslationsPath));
    if (qtLoaded) QCoreApplication::installTranslator(qtTranslator);
    return applicationLoaded;
}

void saveLocale(const QString &locale)
{
    QSettings(QStringLiteral("Nami"), QStringLiteral("Nami"))
        .setValue(QStringLiteral("UI/language"), locale);
}
}
