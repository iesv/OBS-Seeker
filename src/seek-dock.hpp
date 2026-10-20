#pragma once

#include "seek-core.hpp"
#include "presentation-launch.hpp"
#include <QAbstractTableModel>
#include <QCollator>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QHash>
#include <QSortFilterProxyModel>
#include <QThread>
#include <QWidget>
#include <functional>

class QAction;
class QComboBox;
class QDialog;
class QFileSystemWatcher;
class QLabel;
class QInputEvent;
class QLineEdit;
class QListWidget;
class QProcess;
class QTableView;
class QTimer;
class QToolButton;

namespace obs_seeker {

class FileModel final : public QAbstractTableModel {
public:
    enum { FavoriteRole = Qt::UserRole + 4, PathRole, BaseNameRole, ExtensionRole, FavoriteOrderRole };
    explicit FileModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void setFiles(QList<FileEntry> files);
    const FileEntry *fileAt(int row) const;
    bool toggleFavorite(const QString &path);
    int removeFavorites(const QStringList &paths);
    bool moveFavoriteOrder(const QList<int> &rows, bool toEnd);
    bool isFavorite(const QString &path) const;
    qsizetype favoriteCount() const { return favorites_.size(); }
private:
    QList<FileEntry> files_;
    QStringList numberKeys_;
    QStringList baseNameKeys_;
    QStringList extensionKeys_;
    QList<quint64> favoriteOrders_;
    // 收藏只驻留内存，扫描更新和筛选不会清空。
    QHash<QString, quint64> favorites_;
    quint64 nextFavoriteOrder_ = 1;
};

class NameFilter final : public QSortFilterProxyModel {
public:
    explicit NameFilter(QObject *parent = nullptr);
    void setSourceModel(QAbstractItemModel *model) override;
    void setQuery(const QString &query);
    void setFolderScope(const QString &folder);
    void setFavoritesOnly(bool enabled);
    void setFavoritesFirst(bool enabled);
    void setExtensionOrder(const QStringList &extensions);
    void setFolderAliases(QHash<QString, QString> aliases);
    int scopedFileCount() const;
    const QStringList &tokens() const { return tokens_; }
    bool showsFavoriteSeparator() const { return tokens_.isEmpty() && favoritesFirst_ && !favoritesOnly_; }
protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;
private:
    bool acceptsFolder(const QModelIndex &index) const;
    bool acceptsScope(const QModelIndex &index) const;
    QStringList tokens_;
    QString numericQuery_;
    QString folderScope_;
    QHash<QString, QString> folderAliases_;
    QHash<QString, qsizetype> extensionOrder_;
    QCollator nameCollator_;
    QList<QMetaObject::Connection> countConnections_;
    mutable int scopeCount_ = -1;
    bool favoritesOnly_ = false;
    bool favoritesFirst_ = false;
};

class ScanThread final : public QThread {
public:
    ScanThread(Settings settings, QObject *parent);
    void cancel();
    ScanResult result;
protected:
    void run() override;
private:
    Settings settings_;
    std::atomic_bool cancelled_{false};
};

struct ShortcutSummary {
    QString search;
    QString refresh;
    QString favorite;
    QString favoritesView;
};

class PresentationEscape;

struct SceneChoice {
    QString id;
    QString name;
};

class SearchDock final : public QWidget {
public:
    explicit SearchDock(QString settingsPath, QWidget *parent = nullptr,
                        std::function<QStringList(const QString &)> command = {});
    ~SearchDock() override;
    void openSettings();
    void refresh();
    void shutdown();
    void focusSearch();
    void toggleSelectedFavorite(bool clearInput = false);
    void setFavoriteShortcutMatcher(std::function<bool(const QInputEvent &)> matcher);
    void setShortcutSummaryProvider(std::function<ShortcutSummary()> provider);
    void setSceneCallbacks(std::function<QList<SceneChoice>()> scenes, std::function<void(const QString &)> switchScene);
    void presentationEscape(std::function<void(bool)> complete);
    void cancelSceneSwitch();
    void toggleFavoritesView();
    void homeHotkey(bool pressed);
    void refreshScenes();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void scanFinished();
    void applyQuery();
    bool isSceneSearch() const;
    void syncWatchers(QStringList directories);
    void updateStatus();
    void showStatusNotice(const QString &text);
    void copySelectedName();
    void updateScopeChoices();
    void updateScopeCounts();
    void updateColumnVisibility();
    void rememberSearch();
    void showSearchHistory();
    void updateToolbarIcons();
    void updateFavoriteTooltip();
    void updateCompactLayout();
    void updateListMetrics();
    void clearSearch(const QPersistentModelIndex &selected = {});
    void openSelected();
    void openFile(const QString &path);
    bool openNormally(const QString &path);
    void scheduleSceneSwitch(const QString &path);
    void cancelPresentation();
    void finishPresentationEscape(bool send);
    QString homeSceneId() const;
    void revealFile(const QString &path);
    void showContextMenu(const QPoint &point);
    void toggleFavorite(const QString &path);
    bool canReorderFavorites() const;
    void moveFavorite(const QString &path, const QString &neighbor, int direction, bool toEdge = false);
    const FileEntry *selectedFile() const;

    QString settingsPath_;
    QPointer<QWidget> mainWindow_;
    Settings settings_;
    FileModel *model_ = nullptr;
    NameFilter *filter_ = nullptr;
    QLineEdit *search_ = nullptr;
    QAction *historyAction_ = nullptr;
    QWidget *scopeRow_ = nullptr;
    QComboBox *scope_ = nullptr;
    QToolButton *favoritesButton_ = nullptr;
    QToolButton *refreshButton_ = nullptr;
    QToolButton *settingsButton_ = nullptr;
    QTableView *table_ = nullptr;
    QListWidget *sceneResults_ = nullptr;
    QLabel *emptyResults_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *help_ = nullptr;
    QTimer *searchTimer_ = nullptr;
    QTimer *historyTimer_ = nullptr;
    QTimer *noticeTimer_ = nullptr;
    QFileSystemWatcher *watcher_ = nullptr;
    QTimer *watchTimer_ = nullptr;
    QTimer *pollTimer_ = nullptr;
    QTimer *sceneSwitchTimer_ = nullptr;
    QTimer *homeExitTimer_ = nullptr;
    PresentationEscape *escapeInterceptor_ = nullptr;
    QPointer<QProcess> presentationProcess_;
    std::function<QStringList(const QString &)> presentationCommand_;
    quint64 sceneSwitchGeneration_ = 0;
    QPointer<QDialog> settingsDialog_;
    ScanThread *worker_ = nullptr;
    QStringList warnings_;
    QStringList watchFailures_;
    QStringList searchHistory_;
    QHash<QString, int> folderCounts_;
    QString lastOpenedPath_;
    QString pendingPresentationPath_;
    QString favoritePressPath_;
    QString statusNotice_;
    std::function<bool(const QInputEvent &)> favoriteShortcutMatches_;
    std::function<ShortcutSummary()> shortcutSummaryProvider_;
    std::function<QList<SceneChoice>()> sceneChoices_;
    std::function<void(const QString &)> switchScene_;
    std::function<void(bool)> pendingEscape_;
    QElapsedTimer lastOpenTimer_;
    bool rescanRequested_ = false;
    bool historyCleared_ = false;
    bool homeEscapeBlocked_ = false;
    bool homeKeyPressed_ = false;
    bool shuttingDown_ = false;
};
} // namespace obs_seeker
