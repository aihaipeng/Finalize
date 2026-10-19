#pragma once

#include <QSettings>
#include <QString>

// The org/app constructor defaults to NativeFormat (the Windows registry), even when
// QSettings::setDefaultFormat() is set. Always use this explicit INI wrapper.
class NamiSettings : public QSettings
{
public:
    NamiSettings()
        : QSettings(QSettings::IniFormat, QSettings::UserScope,
                    QStringLiteral("Nami"), QStringLiteral("Nami"))
    {
    }
};
