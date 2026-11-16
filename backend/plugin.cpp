#include "plugin.h"
#include "Backend.h"

#include <QtQml/qqml.h>
#include <QtCore/QtGlobal>

void AstroBackendPlugin::registerTypes(const char *uri)
{
    Q_ASSERT(QLatin1String(uri) == QLatin1String("Astro.Backend"));

    qmlRegisterType<Backend>(
        uri,
        1,
        0,
        "Backend"
    );
}