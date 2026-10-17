#pragma once

#include <QList>
#include <QHash>
#include <QStringList>
#include <atomic>

class QFileInfo;

namespace obs_seeker {

inline constexpr int MinPollIntervalSeconds = 5;
inline constexpr int MaxPollIntervalSeconds = 3600;
inline constexpr int DefaultPollIntervalSeconds = 30;
inline constexpr int MinSceneDelayMs = 500;
inline constexpr int MaxSceneDelayMs = 3000;
inline constexpr int DefaultSceneDelayMs = 500;

struct Settings {
    QStringList folders;
    QStringList extensions{QStringLiteral("pps"), QStringLiteral("ppsx"),
                           QStringLiteral("ppt"), QStringLiteral("pptx"), QStringLiteral("key")};
    bool recursive = true;
    bool autoRefresh = true;
    bool favoritesFirst = true;
    int pollIntervalSeconds = DefaultPollIntervalSeconds;
    bool switchSceneOnOpen = true;
    bool escapeReturnsHome = true;
    QString targetSceneId;
    int sceneDelayMs = DefaultSceneDelayMs;
};

struct FileEntry {
    QString name;
    QString folder;
    QString path;
    QString searchableName;
    qint64 modified = 0;
    QString canonicalFolder;
};

struct TextRange {
    int start = 0;
    int length = 0;
};

struct ScanResult {
    QList<FileEntry> files;
    QStringList warnings;
    QStringList directories;
    QHash<QString, QString> rootAliases;
    QHash<QString, int> folderCounts;
    bool cancelled = false;
};

QString normalizedText(const QString &text);
QStringList queryTokens(const QString &query);
bool matchesName(const QString &normalizedName, const QStringList &tokens);
QString numberKey(const QString &digits);
QString leadingNumberKey(const QString &name);
bool folderContains(const QString &root, const QString &folder);
QList<TextRange> highlightRanges(const QString &name, const QStringList &tokens);
QStringList normalizedFolders(const QStringList &folders);
QStringList parseExtensions(const QString &text);
bool isKeynotePackage(const QFileInfo &file);
Settings loadSettings(const QString &path, QString *error = nullptr);
bool saveSettings(const QString &path, const Settings &settings, QString *error = nullptr);
ScanResult scanFolders(const Settings &settings, const std::atomic_bool &cancelled);

} // namespace obs_seeker
