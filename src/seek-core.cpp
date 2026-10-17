#include "seek-core.hpp"
#include "i18n.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QDateTime>
#include <QBitArray>
#include <QTextBoundaryFinder>
#include <algorithm>

namespace obs_seeker {
namespace {
QString pathKey(const QString &path)
{
#ifdef Q_OS_WIN
    return path.toCaseFolded();
#else
    return path;
#endif
}

QStringList stringsFromJson(const QJsonArray &array)
{
    QStringList result;
    for (const QJsonValue value : array) {
        if (value.isString())
            result.append(value.toString());
    }
    return result;
}
} // namespace

QString normalizedText(const QString &text)
{
    // 统一大小写和全角字符。
    return text.normalized(QString::NormalizationForm_KC).toCaseFolded();
}

QStringList queryTokens(const QString &query)
{
    return normalizedText(query).simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

bool matchesName(const QString &normalizedName, const QStringList &tokens)
{
    for (const auto &token : tokens) {
        if (!normalizedName.contains(token))
            return false;
    }
    return true;
}

QString numberKey(const QString &digits)
{
    if (digits.isEmpty())
        return {};
    for (const auto digit : digits) {
        if (digit < QLatin1Char('0') || digit > QLatin1Char('9'))
            return {};
    }
    qsizetype first = 0;
    while (first + 1 < digits.size() && digits.at(first) == QLatin1Char('0'))
        ++first;
    return digits.mid(first);
}

QString leadingNumberKey(const QString &name)
{
    qsizetype length = 0;
    while (length < name.size() && name.at(length) >= QLatin1Char('0') && name.at(length) <= QLatin1Char('9'))
        ++length;
    return numberKey(name.left(length));
}

bool folderContains(const QString &root, const QString &folder)
{
    if (root.isEmpty() || folder.isEmpty())
        return false;
    const auto rootKey = pathKey(QDir::cleanPath(QDir::fromNativeSeparators(root)));
    const auto folderKey = pathKey(QDir::cleanPath(QDir::fromNativeSeparators(folder)));
    const auto prefix = rootKey.endsWith(QLatin1Char('/')) ? rootKey : rootKey + QLatin1Char('/');
    // 按目录边界筛选，避免混入同名前缀目录。
    return folderKey == rootKey || folderKey.startsWith(prefix);
}

QList<TextRange> highlightRanges(const QString &name, const QStringList &tokens)
{
    if (name.isEmpty() || tokens.isEmpty())
        return {};
    // 按字素映射高亮，兼容全角及组合字符。
    QString normalized;
    QList<qsizetype> starts, ends;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, name);
    qsizetype start = 0;
    for (auto end = finder.toNextBoundary(); end >= 0; end = finder.toNextBoundary()) {
        const auto part = normalizedText(name.mid(start, end - start));
        normalized.append(part);
        for (qsizetype index = 0; index < part.size(); ++index) {
            starts.append(start);
            ends.append(end);
        }
        start = end;
    }
    QBitArray marked(name.size());
    for (const auto &token : tokens) {
        if (token.isEmpty())
            continue;
        qsizetype position = 0;
        while ((position = normalized.indexOf(token, position)) >= 0) {
            marked.fill(true, starts.at(position), ends.at(position + token.size() - 1));
            ++position;
        }
    }
    QList<TextRange> ranges;
    for (qsizetype index = 0; index < marked.size();) {
        if (!marked.testBit(index)) { ++index; continue; }
        const auto begin = index;
        while (index < marked.size() && marked.testBit(index))
            ++index;
        ranges.append({static_cast<int>(begin), static_cast<int>(index - begin)});
    }
    return ranges;
}

QStringList normalizedFolders(const QStringList &folders)
{
    QStringList result;
    QSet<QString> seen;
    for (const auto &folder : folders) {
        const auto value = QDir::fromNativeSeparators(folder);
        if (value.trimmed().isEmpty() || !QDir::isAbsolutePath(value))
            continue;
        const auto path = QDir::cleanPath(value);
        const auto key = pathKey(path);
        if (!seen.contains(key)) {
            seen.insert(key);
            result.append(path);
        }
    }
    return result;
}

QStringList parseExtensions(const QString &text)
{
    QStringList result;
    const auto values = text.toLower().split(QRegularExpression(QStringLiteral("[\\s;,，；]+")), Qt::SkipEmptyParts);
    const QRegularExpression valid(QStringLiteral("^[a-z0-9]+$"));
    for (auto value : values) {
        if (value.startsWith(QStringLiteral("*.")))
            value.remove(0, 2);
        else if (value.startsWith(QLatin1Char('.')))
            value.remove(0, 1);
        if (!valid.match(value).hasMatch())
            return {}; // 拒绝非法扩展名。
        if (!result.contains(value))
            result.append(value);
    }
    return result;
}

bool isKeynotePackage(const QFileInfo &file)
{
    return file.isDir() && file.suffix().compare(QStringLiteral("key"), Qt::CaseInsensitive) == 0;
}

Settings loadSettings(const QString &path, QString *error)
{
    if (error)
        error->clear();
    Settings settings;
    QFile file(path);
    if (!file.exists())
        return settings;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return settings;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = uiText("Error.ConfigInvalid");
        return settings;
    }
    const auto object = document.object();
    settings.folders = normalizedFolders(stringsFromJson(object.value(QStringLiteral("folders")).toArray()));
    settings.recursive = object.value(QStringLiteral("recursive")).toBool(true);
    settings.autoRefresh = object.value(QStringLiteral("autoRefresh")).toBool(true);
    settings.favoritesFirst = object.value(QStringLiteral("favoritesFirst")).toBool(true);
    settings.switchSceneOnOpen = object.value(QStringLiteral("switchSceneOnOpen")).toBool(settings.switchSceneOnOpen);
    settings.escapeReturnsHome = object.value(QStringLiteral("escapeReturnsHome")).toBool(settings.escapeReturnsHome);
    settings.targetSceneId = object.value(QStringLiteral("targetSceneId")).toString();
    settings.sceneDelayMs = std::clamp(object.value(QStringLiteral("sceneDelayMs")).toInt(DefaultSceneDelayMs),
                                      MinSceneDelayMs, MaxSceneDelayMs);
    // 限制间隔，避免频繁扫描或计时溢出。
    settings.pollIntervalSeconds = std::clamp(object.value(QStringLiteral("pollIntervalSeconds")).toInt(DefaultPollIntervalSeconds),
                                             MinPollIntervalSeconds, MaxPollIntervalSeconds);
    if (object.value(QStringLiteral("extensions")).isArray()) {
        const auto extensions = parseExtensions(stringsFromJson(object.value(QStringLiteral("extensions")).toArray()).join(QLatin1Char(';')));
        if (!extensions.isEmpty())
            settings.extensions = extensions;
    }
    return settings;
}

bool saveSettings(const QString &path, const Settings &settings, QString *error)
{
    if (error)
        error->clear();
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = uiText("Error.CreateConfigDirectory");
        return false;
    }
    QJsonObject object;
    object.insert(QStringLiteral("version"), 1);
    object.insert(QStringLiteral("folders"), QJsonArray::fromStringList(normalizedFolders(settings.folders)));
    object.insert(QStringLiteral("extensions"), QJsonArray::fromStringList(settings.extensions));
    object.insert(QStringLiteral("recursive"), settings.recursive);
    object.insert(QStringLiteral("autoRefresh"), settings.autoRefresh);
    object.insert(QStringLiteral("favoritesFirst"), settings.favoritesFirst);
    object.insert(QStringLiteral("switchSceneOnOpen"), settings.switchSceneOnOpen);
    object.insert(QStringLiteral("escapeReturnsHome"), settings.escapeReturnsHome);
    object.insert(QStringLiteral("targetSceneId"), settings.targetSceneId);
    object.insert(QStringLiteral("sceneDelayMs"), std::clamp(settings.sceneDelayMs, MinSceneDelayMs, MaxSceneDelayMs));
    object.insert(QStringLiteral("pollIntervalSeconds"), std::clamp(settings.pollIntervalSeconds, MinPollIntervalSeconds, MaxPollIntervalSeconds));
    // 原子写入，避免中断时留下半份配置。
    QSaveFile file(path);
    const auto content = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

ScanResult scanFolders(const Settings &settings, const std::atomic_bool &cancelled)
{
    ScanResult result;
    QSet<QString> extensions;
    for (const auto &extension : settings.extensions)
        extensions.insert(extension.toCaseFolded());
    QSet<QString> visitedFolders;
    QHash<QString, int> directoryCounts;
    const auto roots = normalizedFolders(settings.folders);
    QStringList pending = roots;
    while (!pending.isEmpty()) {
        if (cancelled.load(std::memory_order_relaxed)) {
            result.cancelled = true;
            return result;
        }
        const auto path = pending.takeLast();
        const QFileInfo root(path);
        if (!root.isDir() || !root.isReadable()) {
            result.warnings.append(QDir::toNativeSeparators(path));
            continue;
        }
        const auto canonical = root.canonicalFilePath();
        const auto canonicalFolder = canonical.isEmpty() ? root.absoluteFilePath() : canonical;
        if (roots.contains(path))
            result.rootAliases.insert(path, canonicalFolder);
        // 去重目录并收集监控路径。
        const auto folderKey = pathKey(canonicalFolder);
        if (visitedFolders.contains(folderKey))
            continue;
        visitedFolders.insert(folderKey);
        result.directories.append(root.absoluteFilePath());

        int fileCount = 0;
        QDirIterator entries(path, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
        while (entries.hasNext()) {
            if (cancelled.load(std::memory_order_relaxed)) {
                result.cancelled = true;
                return result;
            }
            entries.next();
            const auto file = entries.fileInfo();
            // 不跟随链接或目录联接，避免越界和循环。
            if (file.isSymLink() || file.isJunction())
                continue;
            const bool keynotePackage = isKeynotePackage(file);
            if (file.isDir() && !keynotePackage) {
                if (settings.recursive)
                    pending.append(file.absoluteFilePath());
                continue;
            }
            // Keynote 包作为文稿列出，不扫描包内文件。
            if ((!file.isFile() && !keynotePackage) || file.fileName().startsWith(QStringLiteral("~$")) || !extensions.contains(file.suffix().toCaseFolded()))
                continue;
            const auto fullPath = QDir::cleanPath(file.absoluteFilePath());
            const auto searchable = normalizedText(file.fileName());
            result.files.append({file.fileName(), file.absolutePath(), fullPath,
                                 searchable, file.lastModified().toSecsSinceEpoch(), canonicalFolder});
            ++fileCount;
        }
        directoryCounts.insert(folderKey, fileCount);
    }
    QHash<QString, QStringList> rootPaths;
    for (const auto &root : roots) {
        result.folderCounts.insert(root, 0);
        rootPaths[pathKey(result.rootAliases.value(root, root))].append(root);
    }
    // 按目录汇总数量，再累加到已配置的父目录；重叠位置不重复扫描。
    for (auto it = directoryCounts.cbegin(); it != directoryCounts.cend(); ++it) {
        if (cancelled.load(std::memory_order_relaxed)) {
            result.cancelled = true;
            return result;
        }
        if (it.value() == 0)
            continue;
        auto path = it.key();
        while (!path.isEmpty()) {
            for (const auto &root : rootPaths.value(path))
                result.folderCounts[root] += it.value();
            const auto slash = path.lastIndexOf(QLatin1Char('/'));
            if (slash < 0)
                break;
            const auto length = slash == 0 || (slash == 2 && path.at(1) == QLatin1Char(':')) ? slash + 1 : slash;
            if (length == path.size())
                break;
            path.truncate(length);
        }
    }
    result.cancelled = cancelled.load(std::memory_order_relaxed);
    return result;
}
} // namespace obs_seeker
