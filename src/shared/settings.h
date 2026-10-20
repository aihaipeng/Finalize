#pragma once

#include <QSettings>
#include <QString>

// The org/app constructor defaults to NativeFormat (the Windows registry), even when
// QSettings::setDefaultFormat() is set. Always use this explicit INI wrapper.
class LensSnipSettings : public QSettings
{
public:
    LensSnipSettings()
        : QSettings(QSettings::IniFormat, QSettings::UserScope,
                    QStringLiteral("LensSnip"), QStringLiteral("LensSnip"))
    {
    }
};
