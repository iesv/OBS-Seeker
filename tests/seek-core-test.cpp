#include "seek-core.hpp"
#include "i18n.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
void touch(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot create fixture");
    file.write("fixture");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        obs_seeker::setUiLocale("zh-CN");
        require(obs_seeker::uiText("Plugin.Title") == QStringLiteral("寻求者"), "Simplified Chinese translation failed");
        require(obs_seeker::uiText("Settings.Save") == QStringLiteral("保存"), "Simplified Chinese controls failed");
        obs_seeker::setUiLocale("zh-TW");
        require(obs_seeker::uiText("Plugin.Title") == QStringLiteral("尋求者"), "Traditional Chinese translation failed");
        require(obs_seeker::uiText("Settings.Save") == QStringLiteral("儲存"), "Traditional Chinese controls failed");
        obs_seeker::setUiLocale("en-US");
        require(obs_seeker::uiText("Plugin.Title") == QStringLiteral("Seeker"), "US English translation failed");
        require(obs_seeker::uiText("Settings.Save") == QStringLiteral("Save"), "US English controls failed");
        obs_seeker::setUiLocale("fr-FR");
        require(obs_seeker::uiText("Column.Location") == QStringLiteral("Location"), "English fallback failed");
        const char *unknown = nullptr;
        require(!obs_seeker::lookupText("Unknown.Key", &unknown) && QString::fromUtf8(unknown) == QStringLiteral("Unknown.Key"), "Missing translation handling failed");
        QTemporaryDir directory;
        require(directory.isValid(), "Temporary directory failed");
        const auto root = directory.path() + QStringLiteral("/诗歌 库,一");
        const auto nested = root + QStringLiteral("/子文件夹");
        const auto outside = directory.path() + QStringLiteral("/不应搜索");
        const auto spacedFolder = directory.path() + QStringLiteral("/末尾空格 ");
        require(obs_seeker::normalizedFolders({spacedFolder}).first().endsWith(QLatin1Char(' ')), "Folder normalization removed a valid trailing space");
        require(QDir().mkpath(nested) && QDir().mkpath(outside), "Cannot create directories");
        touch(root + QStringLiteral("/奇异恩典.PPTX"));
        touch(root + QStringLiteral("/恩典 2026.ppt"));
        touch(root + QStringLiteral("/~$恩典.pptx"));
        touch(root + QStringLiteral("/恩典.txt"));
        touch(nested + QStringLiteral("/赞美.ppsx"));
        touch(outside + QStringLiteral("/恩典.pptx"));
        obs_seeker::Settings settings;
        settings.folders = {root, nested, root + QStringLiteral("/.")};
        std::atomic_bool cancel{false};
        const auto recursive = obs_seeker::scanFolders(settings, cancel);
        require(!recursive.cancelled && recursive.files.size() == 3, "Recursive scan/overlap/extension/temp exclusions failed");
        require(recursive.directories.size() == 2, "Watched directories missing or duplicated");
        require(recursive.rootAliases.value(root) == QFileInfo(root).canonicalFilePath(), "Configured root alias mapping missing");
        require(recursive.folderCounts.size() == 2 && recursive.folderCounts.value(root) == 3
            && recursive.folderCounts.value(nested) == 1, "Overlapping location counts are incorrect");
        for (const auto &file : recursive.files)
            require(file.path.startsWith(root), "Scan escaped configured folders");
        settings.folders = {root};
        settings.recursive = false;
        const auto shallowScan = obs_seeker::scanFolders(settings, cancel);
        require(shallowScan.files.size() == 2 && shallowScan.folderCounts.value(root) == 2, "Nonrecursive scan or count failed");
        settings.extensions = {QStringLiteral("ppsx")};
        require(obs_seeker::scanFolders(settings, cancel).files.isEmpty(), "Custom extension filter failed");
        settings.recursive = true;
        require(obs_seeker::scanFolders(settings, cancel).files.size() == 1, "Nested custom type failed");
        settings.folders.append(root + QStringLiteral("/missing"));
        const auto missingScan = obs_seeker::scanFolders(settings, cancel);
        require(missingScan.warnings.size() == 1 && missingScan.folderCounts.value(root) == 1
            && missingScan.folderCounts.contains(root + QStringLiteral("/missing"))
            && missingScan.folderCounts.value(root + QStringLiteral("/missing")) == 0, "Missing root warning or count failed");
        cancel.store(true);
        require(obs_seeker::scanFolders(settings, cancel).cancelled, "Cancellation failed");
        require(obs_seeker::matchesName(obs_seeker::normalizedText(QStringLiteral("奇异恩典.PPTX")), obs_seeker::queryTokens(QStringLiteral("恩典 pptx"))), "Chinese multi-token matching failed");
        require(obs_seeker::matchesName(obs_seeker::normalizedText(QStringLiteral("ＡＢＣ.PPTX")), obs_seeker::queryTokens(QStringLiteral("abc"))), "Unicode normalization failed");
        require(!obs_seeker::matchesName(obs_seeker::normalizedText(QStringLiteral("赞美.pptx")), obs_seeker::queryTokens(QStringLiteral("恩典"))), "Wrong filename match");
        const auto numbered = obs_seeker::normalizedText(QStringLiteral("010.众圣颂扬.ppsx"));
        require(obs_seeker::matchesName(numbered, obs_seeker::queryTokens(QStringLiteral("010"))), "Number prefix search failed");
        require(obs_seeker::matchesName(numbered, obs_seeker::queryTokens(QStringLiteral("10"))), "Partial number search failed");
        require(!obs_seeker::matchesName(numbered, obs_seeker::queryTokens(QStringLiteral("029"))), "Wrong number matched");
        const auto ten = obs_seeker::queryTokens(QStringLiteral("10"));
        require(obs_seeker::numberKey(QStringLiteral("00010")) == QStringLiteral("10"), "Numeric ranking key did not remove leading zeros");
        require(obs_seeker::numberKey(QStringLiteral("10+")).isEmpty(), "Non-numeric query received numeric ranking");
        require(obs_seeker::leadingNumberKey(QStringLiteral("010.诗歌.pptx")) == QStringLiteral("10"), "Leading number ranking failed");
        require(obs_seeker::folderContains(root, nested), "Folder scope excluded a child directory");
        require(!obs_seeker::folderContains(root, root + QStringLiteral("2")), "Folder scope crossed a sibling path boundary");
#ifdef Q_OS_WIN
        require(obs_seeker::folderContains(QStringLiteral("C:/Songs"), QStringLiteral("c:/songs/Sub")), "Windows scope matching is case-sensitive");
#endif
        for (const auto &name : {QStringLiteral("010.pptx"), QStringLiteral("110.pptx"), QStringLiteral("210.pptx"),
                                 QStringLiteral("10+.pptx"), QStringLiteral("10-.pptx")}) {
            require(obs_seeker::matchesName(obs_seeker::normalizedText(name), ten), "Numeric substring omitted a related filename");
        }
        auto ranges = obs_seeker::highlightRanges(QStringLiteral("210.奇异恩典.ppsx"), ten);
        require(ranges.size() == 1 && ranges.first().start == 1 && ranges.first().length == 2, "Numeric substring highlighting failed");
        ranges = obs_seeker::highlightRanges(QStringLiteral("ＡＢＣ恩典"), {QStringLiteral("abc"), QStringLiteral("恩典")});
        require(ranges.size() == 1 && ranges.first().length == 5, "Unicode highlighting failed");
        ranges = obs_seeker::highlightRanges(QStringLiteral("a\uFB03z"), {QStringLiteral("ffi")});
        require(ranges.size() == 1 && ranges.first().start == 1 && ranges.first().length == 1, "Expanded compatibility-character highlighting failed");
        ranges = obs_seeker::highlightRanges(QStringLiteral("e\u0301歌"), {QStringLiteral("é")});
        require(ranges.size() == 1 && ranges.first().start == 0 && ranges.first().length == 2, "Combining-character highlighting failed");
        require(obs_seeker::parseExtensions(QStringLiteral("*.PPT; .pptx，PPT")).size() == 2, "Extension normalization failed");
        require(obs_seeker::parseExtensions(QStringLiteral("ppt;../exe")).isEmpty(), "Invalid extensions accepted");
        require(obs_seeker::normalizedFolders({root, root + QStringLiteral("/."), QStringLiteral("relative"), QString()}).size() == 1, "Folder normalization failed");

        const auto config = directory.path() + QStringLiteral("/config/设置.json");
        settings.folders = {root, nested};
        settings.autoRefresh = false;
        settings.pollIntervalSeconds = 15;
        settings.favoritesFirst = false;
        settings.switchSceneOnOpen = true;
        settings.escapeReturnsHome = false;
        settings.targetSceneId = QStringLiteral("scene-诗歌");
        settings.sceneDelayMs = 1500;
        QString error;
        require(obs_seeker::saveSettings(config, settings, &error), "Settings save failed");
        const auto loaded = obs_seeker::loadSettings(config, &error);
        require(loaded.pollIntervalSeconds == 15, "Refresh interval not persisted");
        require(!loaded.favoritesFirst, "Favorites priority setting not persisted");
        require(!loaded.escapeReturnsHome, "Disabled Esc HOME setting not persisted");
        require(loaded.switchSceneOnOpen && loaded.targetSceneId == settings.targetSceneId && loaded.sceneDelayMs == 1500,
                "Scene switching settings not persisted");
        require(error.isEmpty() && loaded.folders == settings.folders && loaded.extensions == settings.extensions && loaded.recursive == settings.recursive && !loaded.autoRefresh, "Settings roundtrip failed");
        QFile malformed(config);
        require(malformed.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot create bad config");
        malformed.write("invalid json");
        malformed.close();
        require(obs_seeker::loadSettings(config, &error).folders.isEmpty() && !error.isEmpty(), "Malformed settings not handled");
        const auto defaults = obs_seeker::loadSettings(directory.path() + QStringLiteral("/absent.json"), &error);
        require(defaults.pollIntervalSeconds == 30, "Default refresh interval failed");
        require(defaults.favoritesFirst, "Favorites priority should default to enabled");
        require(defaults.escapeReturnsHome, "Esc HOME should default to enabled");
        require(defaults.switchSceneOnOpen && defaults.targetSceneId.isEmpty() && defaults.sceneDelayMs == 500,
                "Scene switching defaults failed");
        QFile intervalConfig(config);
        require(intervalConfig.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write interval fixture");
        intervalConfig.write("{\"autoRefresh\":true}");
        intervalConfig.close();
        require(obs_seeker::loadSettings(config).pollIntervalSeconds == 30, "Missing interval default failed");
        require(obs_seeker::loadSettings(config).favoritesFirst, "Missing favorites priority should default to enabled");
        require(obs_seeker::loadSettings(config).escapeReturnsHome, "Missing Esc HOME option should default to enabled");
        require(obs_seeker::loadSettings(config).switchSceneOnOpen && obs_seeker::loadSettings(config).sceneDelayMs == 500,
                "Missing presentation option or default delay failed");
        require(intervalConfig.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write disabled presentation fixture");
        intervalConfig.write("{\"switchSceneOnOpen\":false,\"escapeReturnsHome\":false}");
        intervalConfig.close();
        const auto disabled = obs_seeker::loadSettings(config);
        require(!disabled.switchSceneOnOpen && !disabled.escapeReturnsHome, "Explicitly disabled options were lost");
        require(intervalConfig.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write enabled options fixture");
        intervalConfig.write("{\"switchSceneOnOpen\":true,\"escapeReturnsHome\":true}");
        intervalConfig.close();
        const auto enabled = obs_seeker::loadSettings(config);
        require(enabled.switchSceneOnOpen && enabled.escapeReturnsHome, "Saved enabled options were lost");

        require(intervalConfig.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write interval bound fixture");
        intervalConfig.write("{\"pollIntervalSeconds\":0}");
        intervalConfig.close();
        require(obs_seeker::loadSettings(config).pollIntervalSeconds == 5, "Minimum interval guard failed");
        require(intervalConfig.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write interval bound fixture");
        intervalConfig.write("{\"pollIntervalSeconds\":86400}");
        intervalConfig.close();
        require(obs_seeker::loadSettings(config).pollIntervalSeconds == 3600, "Maximum interval guard failed");
        for (const auto value : {-500, 20000}) {
            require(intervalConfig.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write scene delay fixture");
            intervalConfig.write(QByteArray("{\"sceneDelayMs\":") + QByteArray::number(value) + '}');
            intervalConfig.close();
            require(obs_seeker::loadSettings(config).sceneDelayMs == (value < 500 ? 500 : 3000), "Scene delay load bounds failed");
            settings.sceneDelayMs = value;
            require(obs_seeker::saveSettings(config, settings) && obs_seeker::loadSettings(config).sceneDelayMs == (value < 500 ? 500 : 3000),
                    "Scene delay save bounds failed");
        }
        require(error.isEmpty() && defaults.recursive && defaults.autoRefresh && defaults.extensions.contains(QStringLiteral("pptx")), "Missing settings defaults failed");
        require(defaults.extensions.join(QLatin1Char(';')) == QStringLiteral("pps;ppsx;ppt;pptx;key"), "Default extension order is incorrect");
        require(!obs_seeker::saveSettings(root, settings, &error) && !error.isEmpty(), "Settings write failure not surfaced");
        const auto largeRoot = directory.path() + QStringLiteral("/大目录");
        require(QDir().mkpath(largeRoot), "Cannot create large scan fixture");
        for (int index = 0; index < 2000; ++index)
            touch(largeRoot + QStringLiteral("/%1.诗歌.pptx").arg(index, 4, 10, QLatin1Char('0')));
        obs_seeker::Settings largeSettings;
        largeSettings.folders = {largeRoot};
        std::atomic_bool largeCancel{false};
        QElapsedTimer scanTime;
        scanTime.start();
        const auto largeScan = obs_seeker::scanFolders(largeSettings, largeCancel);
        require(!largeScan.cancelled && largeScan.files.size() == 2000 && largeScan.directories.size() == 1 && largeScan.warnings.isEmpty(), "Large directory scan lost files or failed");
        require(largeScan.folderCounts.value(largeRoot) == 2000, "Large location count lost files");
        std::cout << "PERF: scan 2000 presentation files " << scanTime.elapsed() << " ms\n";
        const auto manyRoot = directory.path() + QStringLiteral("/多个位置");
        obs_seeker::Settings manySettings;
        manySettings.folders.append(manyRoot);
        for (int index = 0; index < 40; ++index) {
            const auto folder = manyRoot + QStringLiteral("/位置%1").arg(index);
            require(QDir().mkpath(folder), "Cannot create many-location fixture");
            manySettings.folders.append(folder);
            for (int file = 0; file < 5; ++file)
                touch(folder + QStringLiteral("/%1.pptx").arg(file));
        }
        scanTime.restart();
        const auto manyScan = obs_seeker::scanFolders(manySettings, largeCancel);
        require(manyScan.files.size() == 200 && manyScan.folderCounts.value(manyRoot) == 200, "Many locations duplicated files or lost totals");
        for (const auto &folder : manySettings.folders.mid(1))
            require(manyScan.folderCounts.value(folder) == 5, "Many-location count incorrect");
        std::cout << "PERF: scan/count 41 overlapping locations, 200 files " << scanTime.elapsed() << " ms\n";
        const auto keyRoot = directory.path() + QStringLiteral("/Keynote");
        const auto keyPackage = keyRoot + QStringLiteral("/001.Presentation.KEY");
        require(QDir().mkpath(keyPackage + QStringLiteral("/Data")), "Cannot create Keynote package fixture");
        touch(keyPackage + QStringLiteral("/Data/internal.pptx"));
        touch(keyRoot + QStringLiteral("/002.Presentation.key"));
        obs_seeker::Settings keySettings;
        keySettings.folders = {keyRoot};
        const auto keyScan = obs_seeker::scanFolders(keySettings, largeCancel);
        require(keyScan.files.size() == 2 && keyScan.directories.size() == 1
            && keyScan.folderCounts.value(keyRoot) == 2, "Keynote files/packages were omitted or scanned internally");
        keySettings.extensions = {QStringLiteral("pptx")};
        require(obs_seeker::scanFolders(keySettings, largeCancel).files.isEmpty(), "Excluded Keynote package exposed its internal files");
        std::cout << "PASS: scanning, scope, duplicates, recursion, filtering, cancellation, Unicode, settings\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
