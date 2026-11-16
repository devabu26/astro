#include "Backend.h"

extern "C" const char *astro_get_status(void);

Backend::Backend(QObject *parent)
    : QObject(parent)
{
}

QString Backend::status() const
{
    const char *rustStatus = astro_get_status();

    if (!rustStatus) {
        return QStringLiteral("Astro Rust backend returned no status!");
    }

    return QString::fromUtf8(rustStatus);
}