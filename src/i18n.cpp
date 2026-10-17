#include "i18n.hpp"

#include <QByteArray>
#include <QHash>
#include <QResource>
#include <QSettings>

static void initializeLocaleResources()
{
    Q_INIT_RESOURCE(seeker_locales);
}

namespace obs_seeker {
namespace {
class Catalog {
public:
    Catalog()
    {
        initializeLocaleResources();
        load(QStringLiteral("en-US"));
    }

    void load(const QString &requested)
    {
        messages.clear();
        read(QStringLiteral("en-US"));
        auto locale = requested;
        locale.replace(QLatin1Char('_'), QLatin1Char('-'));
        if (locale.startsWith(QStringLiteral("zh"), Qt::CaseInsensitive)) {
            const bool traditional = locale.contains(QStringLiteral("-TW"), Qt::CaseInsensitive)
                || locale.contains(QStringLiteral("-HK"), Qt::CaseInsensitive)
                || locale.contains(QStringLiteral("-MO"), Qt::CaseInsensitive)
                || locale.contains(QStringLiteral("-Hant"), Qt::CaseInsensitive);
            read(traditional ? QStringLiteral("zh-TW") : QStringLiteral("zh-CN"));
        }
    }

    QHash<QString, QByteArray> messages;

private:
    void read(const QString &locale)
    {
        QSettings file(QStringLiteral(":/OBS-Seeker/locale/%1.ini").arg(locale), QSettings::IniFormat);
        for (const auto &key : file.allKeys())
            messages.insert(key, file.value(key).toString().toUtf8());
    }
};

Catalog &catalog()
{
    static Catalog instance;
    return instance;
}
} // namespace

void setUiLocale(const char *locale)
{
    // 先加载美国英语作为回退，再覆盖所选中文语言。
    catalog().load(QString::fromUtf8(locale ? locale : "en-US"));
}

bool lookupText(const char *key, const char **translated)
{
    const auto &messages = catalog().messages;
    const auto it = messages.constFind(QString::fromLatin1(key));
    const bool found = it != messages.cend();
    if (translated)
        *translated = found ? it.value().constData() : key;
    return found;
}

const char *localizedText(const char *key)
{
    const char *translated = key;
    lookupText(key, &translated);
    return translated;
}

QString uiText(const char *key)
{
    return QString::fromUtf8(localizedText(key));
}
} // namespace obs_seeker
