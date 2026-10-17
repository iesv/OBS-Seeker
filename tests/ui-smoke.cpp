#include "seek-dock.hpp"
#include "i18n.hpp"
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QImage>
#include <QIcon>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QProcess>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QScrollBar>
#include <QSpinBox>
#include <QTableView>
#include <QTextDocument>
#include <QTextEdit>
#include <QToolButton>
#include <QTimer>
#include <QTemporaryDir>
#include <QUrl>
#include <QWindow>
#include <functional>
#include <cstring>
#include <iostream>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

QT_BEGIN_NAMESPACE
Q_GUI_EXPORT void qt_handleMouseEvent(QWindow *, const QPointF &, const QPointF &, Qt::MouseButtons,
                                     Qt::MouseButton, QEvent::Type, Qt::KeyboardModifiers, int);
QT_END_NAMESPACE

class UrlRecorder final : public QObject {
    Q_OBJECT
public:
    QUrl opened;
    int openRequests = 0;
public slots:
    void record(const QUrl &url) { opened = url; ++openRequests; }
};

class HostKeyFilter final : public QObject {
public:
    int swallowedEscapes = 0;
    int swallowedEnters = 0;
protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            ++swallowedEscapes;
            return true;
        }
        if (event->type() == QEvent::KeyPress && (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Return
            || static_cast<QKeyEvent *>(event)->key() == Qt::Key_Enter)) {
            ++swallowedEnters;
            return true;
        }
        return false;
    }
};

namespace {
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
void pumpEvents()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
bool scanIdle(const obs_seeker::SearchDock &dock)
{
    for (auto *child : dock.children())
        if (dynamic_cast<obs_seeker::ScanThread *>(child))
            return false;
    return true;
}
void waitFor(const std::function<bool()> &predicate, int timeoutMs = 8000,
             const char *message = "UI asynchronous operation timed out")
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        pumpEvents();
        QThread::msleep(5);
    }
    require(predicate(), message);
}
void touch(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot create UI fixture");
}
void sendMouse(QWidget *target, QEvent::Type type, const QPoint &point)
{
    QMouseEvent event(type, QPointF(point), QPointF(target->mapToGlobal(point)), Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &event);
}
void clickStar(QTableView *table, const QModelIndex &index)
{
    const auto cell = table->visualRect(index);
    const auto point = QPoint(cell.left() + 10, cell.center().y());
    sendMouse(table->viewport(), QEvent::MouseButtonPress, point);
    sendMouse(table->viewport(), QEvent::MouseButtonRelease, point);
}

bool dropFolders(QListWidget *list, const QList<QUrl> &urls)
{
    QMimeData mime;
    mime.setUrls(urls);
    const auto point = list->viewport()->rect().center();
    const auto actions = Qt::CopyAction | Qt::MoveAction;
    QDragEnterEvent enter(point, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    enter.ignore();
    QCoreApplication::sendEvent(list->viewport(), &enter);
    if (!enter.isAccepted())
        return false;
    QDragMoveEvent move(point, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    move.ignore();
    QCoreApplication::sendEvent(list->viewport(), &move);
    require(move.isAccepted() && move.dropAction() == Qt::CopyAction, "Folder drag offered a move operation");
    QDropEvent drop(QPointF(point), actions, &mime, Qt::LeftButton, Qt::NoModifier);
    drop.ignore();
    QCoreApplication::sendEvent(list->viewport(), &drop);
    require(!drop.isAccepted() || drop.dropAction() == Qt::CopyAction, "Folder drop changed its source action");
    return drop.isAccepted();
}
bool iconUsesColor(const QIcon &icon, const QColor &color)
{
    const auto image = icon.pixmap(QSize(16, 16)).toImage();
    bool visible = false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixelColor(x, y);
            if (pixel.alpha() < 180)
                continue;
            visible = true;
            if (qAbs(pixel.red() - color.red()) > 2 || qAbs(pixel.green() - color.green()) > 2 || qAbs(pixel.blue() - color.blue()) > 2)
                return false;
        }
    }
    return visible;
}
QDialog *openSettings(obs_seeker::SearchDock &panel)
{
    pumpEvents();
    panel.openSettings();
    auto *dialog = panel.findChild<QDialog *>(QStringLiteral("obsSeekerSettings"));
    require(dialog, "Settings dialog missing");
    auto *version = dialog->findChild<QLabel *>(QStringLiteral("pluginVersionLabel"));
    require(version, "Settings version label missing");
    QTextDocument versionText;
    versionText.setHtml(version->text());
    require(versionText.toPlainText() == obs_seeker::uiText("Settings.Version").arg(QStringLiteral(OBS_SEEKER_VERSION)),
            "Settings version is missing or incorrect");
    require(version->openExternalLinks() && version->textInteractionFlags().testFlag(Qt::LinksAccessibleByMouse)
        && version->text().contains(QStringLiteral("href=\"https://haliluya.net\"")), "Settings author link is not enabled");
    pumpEvents();
    auto *recursive = dialog->findChild<QCheckBox *>(QStringLiteral("recursiveCheck"));
    auto *autoRefresh = dialog->findChild<QCheckBox *>(QStringLiteral("autoRefreshCheck"));
    auto *favoritesFirst = dialog->findChild<QCheckBox *>(QStringLiteral("favoritesFirstCheck"));
    auto *folders = dialog->findChild<QListWidget *>(QStringLiteral("folderList"));
    auto *folderTitle = dialog->findChild<QLabel *>(QStringLiteral("folderCountLabel"));
    auto *add = dialog->findChild<QPushButton *>(QStringLiteral("addFolderButton"));
    auto *pollLabel = dialog->findChild<QLabel *>(QStringLiteral("pollIntervalLabel"));
    auto *typesLabel = dialog->findChild<QLabel *>(QStringLiteral("fileTypesLabel"));
    auto *typesEdit = dialog->findChild<QLineEdit *>(QStringLiteral("extensionsEdit"));
    auto *typesHint = dialog->findChild<QLabel *>(QStringLiteral("fileTypeOrderHintLabel"));
    require(recursive && autoRefresh && favoritesFirst && folders && add && pollLabel && typesLabel, "Settings layout controls missing");
    require(folderTitle && folderTitle->text() == obs_seeker::uiText("Settings.Folders").arg(folders->count()), "Configured folder count is incorrect");
    require(recursive->geometry().top() == autoRefresh->geometry().top()
        && autoRefresh->geometry().top() == favoritesFirst->geometry().top()
        && recursive->geometry().right() < autoRefresh->geometry().left()
        && autoRefresh->geometry().right() < favoritesFirst->geometry().left(), "Settings options are not aligned side by side");
    require(add->geometry().bottom() < folders->geometry().top() && pollLabel->geometry().left() == typesLabel->geometry().left(), "Settings header or form alignment is incorrect");
    require(typesEdit && typesHint && typesHint->text() == obs_seeker::uiText("Settings.FileTypeOrder")
        && typesHint->geometry().top() == typesLabel->geometry().top()
        && typesHint->geometry().left() > typesLabel->geometry().right()
        && typesEdit->geometry().top() > typesHint->geometry().bottom(), "File-type heading is not on one line above the input");
    require(typesEdit->geometry().left() == folders->geometry().left()
        && typesEdit->geometry().right() == folders->geometry().right(), "Extension input does not use the full content width");
    const auto *pollHint = dialog->findChild<QLabel *>(QStringLiteral("pollHintLabel"));
    require(pollHint && pollHint->foregroundRole() == QPalette::PlaceholderText
        && typesHint->foregroundRole() == pollHint->foregroundRole()
        && pollHint->palette().color(QPalette::Active, pollHint->foregroundRole())
            == typesHint->palette().color(QPalette::Active, typesHint->foregroundRole()), "Settings hints use different colors");
    for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        require(pollHint->palette().color(group, pollHint->foregroundRole()).rgb() == QColor(128, 128, 128).rgb()
            && typesHint->palette().color(group, typesHint->foregroundRole()).rgb() == QColor(128, 128, 128).rgb(),
                "Settings hint gray changed with its state");
    }
    for (const auto *name : {"searchShortcutValue", "refreshShortcutValue", "favoriteShortcutValue", "favoritesViewShortcutValue"}) {
        auto *keys = dialog->findChild<QLabel *>(QString::fromLatin1(name));
        require(keys && keys->textFormat() == Qt::PlainText && !keys->text().isEmpty()
            && keys->textInteractionFlags().testFlag(Qt::TextSelectableByMouse), "Shortcut summary is missing or not readable");
    }
    auto *shortcutTop = dialog->findChild<QFrame *>(QStringLiteral("shortcutTopSeparator"));
    auto *shortcutBottom = dialog->findChild<QFrame *>(QStringLiteral("shortcutBottomSeparator"));
    auto *sceneDivider = dialog->findChild<QFrame *>(QStringLiteral("sceneOptionsSeparator"));
    auto *switchScene = dialog->findChild<QCheckBox *>(QStringLiteral("switchSceneCheck"));
    auto *escapeHome = dialog->findChild<QCheckBox *>(QStringLiteral("escapeHomeCheck"));
    require(!dialog->findChild<QCheckBox *>(QStringLiteral("protectProjectorsCheck")), "Projector protection should have no setting");
    require(sceneDivider && shortcutTop && shortcutBottom && switchScene, "Settings section dividers missing");
    require(escapeHome && escapeHome->isEnabled() && escapeHome->text() == obs_seeker::uiText("Settings.EscapeHome")
        && escapeHome->geometry().top() == switchScene->geometry().top()
        && escapeHome->geometry().left() > switchScene->geometry().right(), "Esc HOME option is missing or overlaps presentation option");
    require(sceneDivider->geometry().top() > typesEdit->geometry().bottom()
        && sceneDivider->geometry().bottom() < switchScene->geometry().top(), "Scene settings divider overlaps content");
    const auto *sceneDelay = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("sceneDelaySpin"));
    require(sceneDelay && shortcutTop->geometry().top() > sceneDelay->parentWidget()->geometry().bottom(),
            "Shortcut divider overlaps scene settings");
    const auto *lastShortcut = dialog->findChild<QLabel *>(QStringLiteral("favoritesViewShortcutValue"));
    const auto *firstShortcut = dialog->findChild<QLabel *>(QStringLiteral("searchShortcutValue"));
    require(shortcutTop->geometry().bottom() < firstShortcut->geometry().top()
        && shortcutBottom->geometry().top() > lastShortcut->geometry().bottom()
        && shortcutBottom->geometry().bottom() < version->geometry().top(), "Shortcut dividers overlap content");
    return dialog;
}
void saveDialog(QDialog *dialog)
{
    auto *box = dialog->findChild<QDialogButtonBox *>();
    require(box, "Settings buttons missing");
    for (auto *button : box->buttons()) {
        if (box->buttonRole(button) == QDialogButtonBox::AcceptRole) {
            button->click();
            return;
        }
    }
    throw std::runtime_error("Save button missing");
}

void projectorEscapeTest()
{
    QTemporaryDir temp;
    require(temp.isValid(), "Cannot create projector protection fixture");
    const auto config = temp.path() + QStringLiteral("/settings.json");
    QFile legacy(config);
    require(legacy.open(QIODevice::WriteOnly), "Cannot create projector fixture");
    legacy.write("{\"autoRefresh\":false,\"protectFullscreenProjectors\":false}");
    legacy.close();
    obs_seeker::SearchDock dock(config);
    dock.show();
    pumpEvents();
    int resets = 0;
    auto *table = dock.findChild<QTableView *>(QStringLiteral("resultsTable"));
    QObject::connect(table->model(), &QAbstractItemModel::modelReset, &dock, [&]() { ++resets; });
    const auto send = [](QWidget *target, int key = Qt::Key_Escape, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                         QEvent::Type type = QEvent::KeyPress) {
        QKeyEvent event(type, key, modifiers);
        if (type == QEvent::ShortcutOverride)
            event.ignore();
        QCoreApplication::sendEvent(target, &event);
        return event.isAccepted();
    };
    QWidget projector;
    // 隔离系统全屏动画，验证 Qt 按键分发。
    projector.setAttribute(Qt::WA_DontShowOnScreen);
    require(projector.winId() != 0, "Cannot create projector fixture window");
    QLineEdit child(&projector);
    projector.showFullScreen();
    pumpEvents();
    require(projector.isFullScreen() && projector.windowHandle(), "Projector fullscreen fixture failed");
    projector.windowHandle()->setProperty("isOBSProjectorWindow", true);
    HostKeyFilter received;
    projector.installEventFilter(&received);
    child.installEventFilter(&received);
    send(&child);
    require(received.swallowedEscapes == 0, "An obsolete preference disabled projector protection");
    require(send(&child, Qt::Key_Escape, Qt::NoModifier, QEvent::ShortcutOverride), "Projector Esc shortcut was not overridden");
    send(&child);
    send(&projector);
    require(received.swallowedEscapes == 0, "Fullscreen projector received Esc");
    send(&child, Qt::Key_Return);
    send(&child, Qt::Key_Escape, Qt::ControlModifier);
    require(received.swallowedEnters == 1 && received.swallowedEscapes == 1, "Protection blocked another key or modified Esc");
    QWidget windowedProjector;
    windowedProjector.show();
    pumpEvents();
    windowedProjector.windowHandle()->setProperty("isOBSProjectorWindow", true);
    windowedProjector.installEventFilter(&received);
    send(&windowedProjector);
    require(received.swallowedEscapes == 2, "Windowed projector Esc was blocked");
    projector.windowHandle()->setProperty("isOBSProjectorWindow", false);
    send(&child);
    require(received.swallowedEscapes == 3, "Unrelated fullscreen window Esc was blocked");
    projector.windowHandle()->setProperty("isOBSProjectorWindow", true);
    QMenu popup(&projector);
    popup.addAction(QStringLiteral("Close"), &projector, &QWidget::close);
    popup.popup(QPoint(100, 100));
    pumpEvents();
    require(QApplication::activePopupWidget() == &popup, "Projector context menu fixture failed");
    send(&child);
    require(received.swallowedEscapes == 4, "Protection intercepted Esc while a context menu was open");
    send(&popup);
    waitFor([&]() { return !popup.isVisible() && !QApplication::activePopupWidget(); });
    QPointer<QDialog> dialog = openSettings(dock);
    saveDialog(dialog);
    waitFor([&]() { return dialog.isNull(); });
    require(legacy.open(QIODevice::ReadOnly) && !legacy.readAll().contains("protectFullscreenProjectors"),
            "Saving settings retained obsolete projector preference");
    legacy.close();
    dock.hide();
    require(projector.isFullScreen() && projector.windowHandle()->property("isOBSProjectorWindow").toBool(),
            "Fullscreen projector fixture lost its state");
    send(&child);
    require(received.swallowedEscapes == 4, "Hidden Seeker panel stopped projector protection");
    require(resets == 0 && dock.findChildren<QThread *>().isEmpty(), "Protection settings triggered a scan");
    dock.shutdown();
    send(&child);
    require(received.swallowedEscapes == 5, "Shutdown left projector protection installed");
    obs_seeker::SearchDock restored(config);
    send(&child);
    require(received.swallowedEscapes == 5, "Projector protection did not apply on startup");
    QWidget laterProjector;
    laterProjector.setAttribute(Qt::WA_DontShowOnScreen);
    require(laterProjector.winId() != 0, "Cannot create later projector fixture window");
    laterProjector.showFullScreen();
    pumpEvents();
    laterProjector.windowHandle()->setProperty("isOBSProjectorWindow", true);
    laterProjector.installEventFilter(&received);
    for (int press = 0; press < 10000; ++press) {
        require(send(&laterProjector, Qt::Key_Escape, Qt::NoModifier, QEvent::ShortcutOverride),
                "Repeated projector Esc shortcut was not overridden");
        send(&laterProjector);
    }
    require(received.swallowedEscapes == 5, "New fullscreen projector was not protected during repeated Esc input");
    popup.actions().first()->trigger();
    require(!projector.isVisible(), "Protection blocked the explicit close action");
    std::cout << "PASS: always-on projector Esc protection, window scope, menus, settings and shutdown\n";
}

#ifdef Q_OS_WIN
void preloadTest(bool stress)
{
    const auto command = QStringList{QCoreApplication::applicationFilePath(), QStringLiteral("--preload-fixture")};
    const int cycles = stress ? 100 : 2;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        auto *owner = new QObject;
        QPointer<QProcess> process = obs_seeker::preloadPowerPoint(owner, command);
        require(process, "Cannot create preload helper");
        waitFor([&] { return !process || !process->findChild<QTimer *>()->isActive(); });
        require(process && process->state() == QProcess::Running, "Preload did not stay resident after readiness");
        const auto eventName = process->arguments().at(2);
        delete owner;
        require(!process, "Preload survived its owner");
        const auto event = OpenEventW(SYNCHRONIZE, FALSE, reinterpret_cast<LPCWSTR>(eventName.utf16()));
        if (event)
            CloseHandle(event);
        require(!event, "Preload stop handle leaked");
    }
    for (const auto &failure : {QStringLiteral("--preload-fail"), QStringLiteral("--preload-stall")}) {
        QObject owner;
        QPointer<QProcess> process = obs_seeker::preloadPowerPoint(&owner,
            {QCoreApplication::applicationFilePath(), failure});
        require(process, "Cannot create preload failure fixture");
        if (failure.endsWith(QStringLiteral("stall"))) {
            waitFor([&] { return process && process->state() == QProcess::Running; });
            process->findChild<QTimer *>()->start(50);
        }
        waitFor([&] { return !process; });
        require(owner.findChildren<QProcess *>().isEmpty(), "Failed preload was retained or retried");
    }
    {
        QObject owner;
        QPointer<QProcess> process = obs_seeker::preloadPowerPoint(&owner, {QStringLiteral("Z:/missing/OBS-Seeker-present.exe")});
        waitFor([&] { return !process; });
    }
    // 预加载仍在初始化时关闭 OBS，也必须有界退出。
    auto *owner = new QObject;
    QPointer<QProcess> process = obs_seeker::preloadPowerPoint(owner,
        {QCoreApplication::applicationFilePath(), QStringLiteral("--preload-stall")});
    waitFor([&] { return process && process->state() == QProcess::Running; });
    QElapsedTimer elapsed;
    elapsed.start();
    delete owner;
    require(!process && elapsed.elapsed() < 2500, "Stalled preload blocked shutdown");
    std::cout << "STRESS: PowerPoint preload " << cycles
              << " ready/shutdown cycles, missing helper, failure, timeout and stalled shutdown passed\n";
}

#endif

void homeEscapeTest(bool stress)
{
    QTemporaryDir temp;
    require(temp.isValid(), "Cannot create HOME hotkey fixture");
    const auto config = temp.path() + QStringLiteral("/settings.json");
    obs_seeker::SearchDock dock(config);
    dock.show();
    dock.activateWindow();
    auto *table = dock.findChild<QTableView *>(QStringLiteral("resultsTable"));
    auto *input = dock.findChild<QLineEdit *>(QStringLiteral("searchInput"));
    auto *timer = dock.findChild<QTimer *>(QStringLiteral("sceneSwitchTimer"));
    table->setFocus();
    pumpEvents();
    int switches = 0;
    QString selected;
    QList<obs_seeker::SceneChoice> scenes{{QStringLiteral("lower"), QStringLiteral("home")},
                                         {QStringLiteral("home-id"), QStringLiteral("HOME")}};
    dock.setSceneCallbacks([&] { return scenes; }, [&](const QString &id) { ++switches; selected = id; });
    const auto trigger = [&] { dock.homeHotkey(true); dock.homeHotkey(false); };
    const auto shortcut = [](QWidget *target, bool repeat = false) {
        QKeyEvent event(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier, {}, repeat);
        QCoreApplication::sendEvent(target, &event);
    };
    QPointer<QDialog> dialog = openSettings(dock);
    require(dialog->findChild<QCheckBox *>(QStringLiteral("switchSceneCheck"))->isChecked()
        && dialog->findChild<QCheckBox *>(QStringLiteral("escapeHomeCheck"))->isChecked(), "Presentation options should default to checked");
    dialog->findChild<QCheckBox *>(QStringLiteral("switchSceneCheck"))->setChecked(false);
    dialog->findChild<QCheckBox *>(QStringLiteral("escapeHomeCheck"))->setChecked(false);
    saveDialog(dialog);
    waitFor([&] { return dialog.isNull(); });
    table->setFocus();
    trigger();
    require(switches == 0, "Disabled Esc HOME switched scenes");
    dialog = openSettings(dock);
    require(!dialog->findChild<QCheckBox *>(QStringLiteral("switchSceneCheck"))->isChecked()
        && !dialog->findChild<QCheckBox *>(QStringLiteral("escapeHomeCheck"))->isChecked(), "Disabled presentation options were not retained");
    dialog->findChild<QCheckBox *>(QStringLiteral("escapeHomeCheck"))->setChecked(true);
    saveDialog(dialog);
    waitFor([&] { return dialog.isNull(); });
    table->setFocus();
    pumpEvents();
    timer->start(5000);
    dock.homeHotkey(true);
    for (int repeat = 0; repeat < 100; ++repeat)
        dock.homeHotkey(true);
    dock.homeHotkey(false);
    require(switches == 1 && selected == QStringLiteral("home-id") && !timer->isActive(),
            "HOME hotkey repeated while held, used the wrong scene or left a pending scene switch");
    scenes.removeLast();
    trigger();
    require(switches == 1, "HOME hotkey matched a differently cased scene name");
    scenes.append({QStringLiteral("home-id"), QStringLiteral("HOME")});
    scenes.last().name = QStringLiteral("HOME renamed");
    trigger();
    require(switches == 1, "HOME lookup kept a renamed scene");
    scenes.last().name = QStringLiteral("HOME");
    shortcut(input);
    shortcut(table, true);
    trigger();
    require(switches == 1, "An input Esc or auto-repeat switched HOME");
    QPlainTextEdit plain;
    QTextEdit rich;
    QDoubleSpinBox spin;
    QComboBox combo;
    combo.setEditable(true);
    for (auto *editor : {static_cast<QWidget *>(&plain), static_cast<QWidget *>(&rich),
                         static_cast<QWidget *>(&spin), static_cast<QWidget *>(&combo)}) {
        shortcut(editor);
        trigger();
        require(switches == 1, "An editor Esc switched HOME");
    }
    QMenu popup(&dock);
    popup.addAction(QStringLiteral("Close"));
    popup.popup(QPoint(100, 100));
    pumpEvents();
    shortcut(&popup);
    popup.close();
    pumpEvents();
    trigger();
    require(switches == 1, "Esc closing a menu switched HOME through a late callback");
    dialog = openSettings(dock);
    require(dialog->findChild<QCheckBox *>(QStringLiteral("escapeHomeCheck"))->isChecked(), "Saved Esc HOME option was lost");
    trigger();
    require(switches == 1, "HOME hotkey ran while Settings was open");
    shortcut(dialog.data());
    dialog->reject();
    waitFor([&] { return dialog.isNull(); });
    trigger();
    require(switches == 1, "Esc closing Settings switched HOME through a late callback");
    table->setFocus();
    shortcut(table);
    trigger();
    require(switches == 2, "Releasing Esc did not restore HOME hotkey handling");
    dialog = openSettings(dock);
    dialog->findChild<QCheckBox *>(QStringLiteral("escapeHomeCheck"))->setChecked(false);
    dialog->reject();
    waitFor([&] { return dialog.isNull(); });
    table->setFocus();
    shortcut(table);
    trigger();
    require(switches == 3, "Cancel changed the Esc HOME preference");
    dialog = openSettings(dock);
    dialog->findChild<QCheckBox *>(QStringLiteral("escapeHomeCheck"))->setChecked(false);
    saveDialog(dialog);
    waitFor([&] { return dialog.isNull(); });
    require(!obs_seeker::loadSettings(config).escapeReturnsHome, "Disabled Esc HOME setting was not saved");
    trigger();
    require(switches == 3, "Disabled Esc HOME still switched scenes");
    dialog = openSettings(dock);
    dialog->findChild<QCheckBox *>(QStringLiteral("escapeHomeCheck"))->setChecked(true);
    saveDialog(dialog);
    waitFor([&] { return dialog.isNull(); });
    if (stress) {
        table->setFocus();
        pumpEvents();
        QElapsedTimer elapsed;
        elapsed.start();
        const auto queued = [&](bool pressed) {
            QMetaObject::invokeMethod(&dock, [&dock, pressed] { dock.homeHotkey(pressed); }, Qt::QueuedConnection);
        };
        for (int cycle = 0; cycle < 5000; ++cycle) {
            const auto before = switches;
            shortcut(input);
            queued(true);
            queued(false);
            QCoreApplication::sendPostedEvents(&dock, QEvent::MetaCall);
            require(switches == before, "Queued input Esc switched HOME under load");
            shortcut(table);
            queued(true);
            for (int repeat = 0; repeat < 5; ++repeat)
                queued(true);
            queued(false);
            QCoreApplication::sendPostedEvents(&dock, QEvent::MetaCall);
            require(switches == before + 1 && selected == QStringLiteral("home-id"),
                    "Queued HOME hotkey was lost or repeated under load");
        }
        const auto beforeMissing = switches;
        scenes.clear();
        for (int cycle = 0; cycle < 1000; ++cycle)
            trigger();
        require(switches == beforeMissing, "Missing HOME switched a different scene under load");
        scenes.append({QStringLiteral("home-id"), QStringLiteral("HOME")});
        std::cout << "STRESS: HOME 5000 actions, 5000 blocked inputs, 25000 held repeats, 1000 missing scenes; "
                  << elapsed.elapsed() << " ms\n";
    }
    const auto beforeShutdown = switches;
    dock.shutdown();
    trigger();
    require(switches == beforeShutdown, "Shutdown left the HOME hotkey active");
    std::cout << "PASS: HOME hotkey, exact name, input/menu guards, delayed callbacks, save/cancel and shutdown\n";
}

void presentationExitTest(bool stress)
{
    QTemporaryDir temp;
    const auto config = temp.path() + QStringLiteral("/settings.json");
    obs_seeker::Settings settings;
    settings.escapeReturnsHome = true;
    require(obs_seeker::saveSettings(config, settings), "Cannot enable presentation exit fixture");
    obs_seeker::SearchDock dock(config);
    bool hasHome = true;
    int switches = 0;
    int delivered = 0;
    int canceled = 0;
    dock.setSceneCallbacks([&] {
        return hasHome ? QList<obs_seeker::SceneChoice>{{QStringLiteral("home"), QStringLiteral("HOME")}}
                       : QList<obs_seeker::SceneChoice>{};
    }, [&](const QString &) { ++switches; });
    const auto completion = [&](bool send) { send ? ++delivered : ++canceled; };
    auto *timeout = dock.findChild<QTimer *>(QStringLiteral("homeExitTimer"));
    auto *sceneTimer = dock.findChild<QTimer *>(QStringLiteral("sceneSwitchTimer"));
    QElapsedTimer elapsed;
    elapsed.start();
    const int cycles = stress ? 10000 : 20;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        sceneTimer->start(5000);
        dock.presentationEscape(completion);
        require(delivered == cycle && switches == cycle + 1 && !sceneTimer->isActive()
                    && timeout->isActive() && timeout->interval() == 500,
                "HOME request did not start the configured exit delay or retained a pending scene switch");
        const auto timerId = timeout->timerId();
        dock.presentationEscape(completion);
        require(canceled == cycle + 1 && switches == cycle + 1 && timeout->timerId() == timerId,
                "Repeated presentation Esc restarted the exit delay or scene request");
        require(QMetaObject::invokeMethod(timeout, "timeout", Qt::DirectConnection), "Cannot trigger exit timer fixture");
        require(delivered == cycle + 1 && !timeout->isActive(), "Exit delay did not deliver Esc without a transition event");
    }
    dock.presentationEscape(completion);
    require(delivered == cycles && timeout->isActive() && timeout->interval() == 500,
            "Repeated HOME request skipped the shared delay");
    require(QMetaObject::invokeMethod(timeout, "timeout", Qt::DirectConnection), "Cannot trigger exit timer fixture");
    require(QMetaObject::invokeMethod(timeout, "timeout", Qt::DirectConnection), "Cannot repeat exit timer fixture");
    require(delivered == cycles + 1, "Exit timer delivered Esc more than once");
    hasHome = false;
    dock.presentationEscape(completion);
    require(delivered == cycles + 2, "Missing HOME swallowed Esc");
    hasHome = true;
    dock.presentationEscape(completion);
    QPointer<QDialog> dialog = openSettings(dock);
    dialog->findChild<QCheckBox *>(QStringLiteral("escapeHomeCheck"))->setChecked(false);
    saveDialog(dialog);
    waitFor([&] { return dialog.isNull(); });
    require(canceled == cycles + 1, "Saving Settings did not cancel the pending exit");
    dock.presentationEscape(completion);
    require(delivered == cycles + 3, "Disabled HOME intercepted presentation Esc");
    dialog = openSettings(dock);
    dialog->findChild<QCheckBox *>(QStringLiteral("escapeHomeCheck"))->setChecked(true);
    saveDialog(dialog);
    waitFor([&] { return dialog.isNull(); });
    for (const int delayMs : {500, 1500, 3000}) {
        dialog = openSettings(dock);
        dialog->findChild<QDoubleSpinBox *>(QStringLiteral("sceneDelaySpin"))->setValue(delayMs / 1000.0);
        saveDialog(dialog);
        waitFor([&] { return dialog.isNull(); });
        const auto before = delivered;
        QElapsedTimer delay;
        delay.start();
        dock.presentationEscape(completion);
        require(delivered == before && timeout->isActive() && timeout->interval() == delayMs,
                "Exit did not use the saved delay");
        waitFor([&] { return delivered == before + 1; }, delayMs + 2000);
        require(delay.elapsed() >= delayMs && !timeout->isActive(), "Presentation exited before the configured delay");
    }
    dock.presentationEscape(completion);
    dock.shutdown();
    require(canceled == cycles + 2 && !timeout->isActive(), "Shutdown left a pending presentation exit");
    require(QMetaObject::invokeMethod(timeout, "timeout", Qt::DirectConnection), "Cannot trigger stopped exit timer fixture");
    require(delivered == cycles + 6, "Shutdown delivered Esc late");
    std::cout << (stress ? "STRESS: " : "PASS: ") << "presentation exit without transition events, " << cycles
              << " HOME requests; 500/1500/3000 ms delays, repeats, cancellation and shutdown; " << elapsed.elapsed() << " ms\n";
}

void presentationTest(UrlRecorder &recorder, bool stress)
{
    require(obs_seeker::Settings{}.switchSceneOnOpen, "Presentation should default to enabled");
    for (const auto &extension : {"ppt", "PPTX"}) {
        const auto path = QStringLiteral("/诗歌 ' & %1.") + QString::fromLatin1(extension);
        const auto command = obs_seeker::presentationCommand(path);
        require(!command.isEmpty() && command.last() == path, "PowerPoint command lost the literal path");
    }
    for (const auto &extension : {"pps", "PPSX"}) {
        const auto path = QStringLiteral("/诗歌 ' & %1.") + QString::fromLatin1(extension);
        const auto command = obs_seeker::presentationCommand(path);
        require(command.isEmpty(), "Show files should use the default app");
    }
    require(obs_seeker::presentationCommand(QStringLiteral("/file.pdf")).isEmpty(), "PDF was sent to presentation automation");
#ifdef Q_OS_MACOS
    require(!obs_seeker::presentationCommand(QStringLiteral("/file.key")).isEmpty(), "Mac Keynote command is missing");
#else
    require(obs_seeker::presentationCommand(QStringLiteral("/file.key")).isEmpty(), "Windows tried to automate Keynote");
#endif
    QTemporaryDir temp;
    require(temp.isValid(), "Cannot create presentation fixture");
    const auto first = temp.path() + QStringLiteral("/1 诗歌 ' & %1.pptx");
    const auto second = temp.path() + QStringLiteral("/2 诗歌.ppsx");
    touch(first);
    touch(second);
    obs_seeker::Settings settings;
    settings.folders = {temp.path()};
    settings.autoRefresh = false;
    settings.switchSceneOnOpen = true;
    settings.targetSceneId = QStringLiteral("scene");
    settings.sceneDelayMs = 500;
    const auto config = temp.path() + QStringLiteral("/settings.json");
    require(obs_seeker::saveSettings(config, settings), "Cannot save presentation fixture");
    int pause = 30;
    int exitCode = 0;
    bool missingProgram = false;
    bool nativeShow = false;
    obs_seeker::SearchDock dock(config, nullptr, [&](const QString &path) {
        if (nativeShow && path == second)
            return obs_seeker::presentationCommand(path);
        if (missingProgram)
            return QStringList{temp.path() + QStringLiteral("/missing-program"), path};
        return QStringList{QCoreApplication::applicationFilePath(), QStringLiteral("--presentation-fixture"),
                           QString::number(pause), QString::number(exitCode), path};
    });
    int switches = 0;
    dock.setSceneCallbacks([] { return QList<obs_seeker::SceneChoice>{}; }, [&](const QString &) { ++switches; });
    auto *input = dock.findChild<QLineEdit *>(QStringLiteral("searchInput"));
    auto *table = dock.findChild<QTableView *>(QStringLiteral("resultsTable"));
    auto *timer = dock.findChild<QTimer *>(QStringLiteral("sceneSwitchTimer"));
    waitFor([&] { return table->model()->rowCount() == 2; });
    const auto open = [&](const char *number) {
        input->setText(QString::fromLatin1(number));
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(input, &enter);
    };
    const auto idle = [&] { return dock.findChildren<QProcess *>().isEmpty(); };
    open("1");
    require(!timer->isActive(), "Scene delay started before presentation succeeded");
    auto *initial = dock.findChild<QProcess *>();
    require(initial && initial->arguments().last() == first, "Presentation path was changed or interpreted as code");
    open("1");
    require(dock.findChild<QProcess *>() == initial, "Repeated Enter started another presentation");
    waitFor([&] { return timer->isActive() && idle(); });
    require(timer->remainingTime() >= 400, "Presentation did not receive the full scene delay");
    waitFor([&] { return switches == 1; });
    require(QFileInfo::exists(first + QStringLiteral(".opened")), "Presentation helper did not receive the Unicode path");
    open("2");
    dock.cancelSceneSwitch();
    waitFor(idle);
    require(!timer->isActive() && switches == 1, "A canceled presentation completion switched scenes");
    auto defaultOpens = recorder.openRequests;
    exitCode = 1;
    open("1");
    waitFor(idle);
    require(!timer->isActive() && switches == 1 && recorder.openRequests == defaultOpens,
            "An uncertain presentation failure reopened the file or switched scenes");
    exitCode = obs_seeker::PresentationNotOpened;
    open("2");
    waitFor(idle);
    require(recorder.openRequests == ++defaultOpens && recorder.opened.toLocalFile() == second && !timer->isActive(),
            "Safe presentation failure did not fall back once without switching scenes");
    open("2");
    require(idle() && recorder.openRequests == defaultOpens, "Repeated Enter reopened the fallback file");
    pause = 100;
    open("1");
    dock.cancelSceneSwitch();
    waitFor(idle);
    require(recorder.openRequests == defaultOpens && !timer->isActive(), "Canceled presentation opened a fallback file");
    open("2");
    open("1");
    waitFor(idle);
    require(recorder.openRequests == ++defaultOpens && recorder.opened.toLocalFile() == first && !timer->isActive(),
            "Queued presentation failure opened a stale file or switched scenes");
    pause = 30;
    missingProgram = true;
    open("2");
    waitFor(idle);
    require(recorder.openRequests == ++defaultOpens && recorder.opened.toLocalFile() == second && !timer->isActive(),
            "Missing presentation helper did not fall back once without switching scenes");
    missingProgram = false;
    exitCode = 0;
    pause = 5000;
    open("1");
    auto *deadline = dock.findChild<QTimer *>(QStringLiteral("presentationTimeout"));
    require(deadline && deadline->isSingleShot(), "Presentation timeout is missing");
    deadline->start(10);
    waitFor(idle);
    require(!timer->isActive() && recorder.openRequests == defaultOpens, "Timed out presentation reopened the file or switched scenes");
    pause = 30;
#ifdef Q_OS_MACOS
    exitCode = obs_seeker::PresentationPermissionDenied;
    open("2");
    waitFor(idle);
    require(!timer->isActive() && switches == 1 && recorder.openRequests == ++defaultOpens
        && recorder.opened.toLocalFile() == second, "Denied Keynote permission did not fall back once without switching scenes");
    require(dock.findChild<QLabel *>(QStringLiteral("statusLabel"))->text()
        == obs_seeker::uiText("Status.PresentationPermissionDenied"), "Denied presentation permission guidance is missing");
    open("2");
    require(idle() && recorder.openRequests == defaultOpens, "Denied Keynote fallback opened twice");
    exitCode = 0;
#endif
    const int rapidRequests = stress ? 1000 : 100;
    for (int request = 0; request < rapidRequests; ++request) {
        open(request % 2 ? "1" : "2");
        pumpEvents();
        int running = 0;
        for (const auto *process : dock.findChildren<QProcess *>())
            running += process->state() != QProcess::NotRunning;
        require(running <= 1, "Rapid requests started parallel presentation helpers");
    }
    waitFor([&] { return idle() && timer->isActive(); });
    dock.cancelSceneSwitch();
    require(switches == 1, "Rapid presentation requests switched to a stale scene");
    nativeShow = true;
    open("2");
    require(idle() && recorder.openRequests == ++defaultOpens && recorder.opened.toLocalFile() == second && timer->isActive(),
            "PPSX did not use the default app and normal scene delay");
    dock.cancelSceneSwitch();
    nativeShow = false;
    if (stress) {
        QElapsedTimer elapsed;
        elapsed.start();
        for (int request = 0; request < 150; ++request) {
            const auto kind = request % 5;
            missingProgram = kind == 0;
            exitCode = kind == 1 ? 1 : kind == 2 ? obs_seeker::PresentationNotOpened : 0;
            pause = kind == 3 ? 5000 : 0;
            open(request % 2 ? "2" : "1");
            const bool fallback = kind == 0 || kind == 2;
            if (kind == 3) {
                auto *deadline = dock.findChild<QTimer *>(QStringLiteral("presentationTimeout"));
                require(deadline, "Stress presentation timeout missing");
                deadline->start(10);
            } else if (kind == 4) {
                dock.cancelSceneSwitch();
            }
            waitFor(idle);
            defaultOpens += fallback;
            require(recorder.openRequests == defaultOpens && !timer->isActive() && switches == 1,
                    "Stress failure/cancellation reopened a file or switched scenes incorrectly");
        }
        missingProgram = false;
        exitCode = 0;
        std::cout << "STRESS: presentation " << rapidRequests << " rapid requests, 150 mixed failures/timeouts/cancellations; "
                  << elapsed.elapsed() << " ms\n";
    }
    pause = 5000;
    open("1");
    open("2");
    QPointer<QDialog> dialog = openSettings(dock);
    saveDialog(dialog);
    waitFor([&] { return dialog.isNull() && idle(); });
    require(!timer->isActive() && switches == 1 && recorder.openRequests == defaultOpens,
            "Saving settings left a queued presentation or scene switch");
    open("2");
    open("1");
    dock.shutdown();
    waitFor(idle);
    require(!timer->isActive() && switches == 1 && recorder.openRequests == defaultOpens,
            "Shutdown left a presentation, fallback or scene switch pending");
    std::cout << "STRESS: presentation success/failure, safe fallback, timeout, Unicode paths, 100 rapid opens, cancellation and shutdown passed\n";
}

void stressTest(UrlRecorder &recorder)
{
    QElapsedTimer total;
    total.start();
    QList<obs_seeker::FileEntry> files;
    files.reserve(100000);
    for (int row = 0; row < 100000; ++row) {
        const auto name = QStringLiteral("%1.song.pptx").arg(row, 6, 10, QLatin1Char('0'));
        files.append({name, QStringLiteral("/indexed"), QStringLiteral("/indexed/") + name, obs_seeker::normalizedText(name), 0});
    }
    obs_seeker::FileModel model;
    model.setFiles(std::move(files));
    obs_seeker::NameFilter filter;
    filter.setSourceModel(&model);
    filter.setExtensionOrder(obs_seeker::Settings{}.extensions);
    filter.sort(0, Qt::AscendingOrder);
    QElapsedTimer queryTime;
    queryTime.start();
    for (int query = 1000; query < 8400; query += 37) {
        filter.setQuery(QString::number(query));
        require(filter.rowCount() > 0 && filter.index(0, 0).data(obs_seeker::FileModel::PathRole).toString()
            == QStringLiteral("/indexed/%1.song.pptx").arg(query, 6, 10, QLatin1Char('0')), "Stress search ranking failed");
    }
    std::cout << "STRESS: 100000 files, 200 queries " << queryTime.elapsed() << " ms\n";
    filter.setQuery({});
    filter.setFavoritesOnly(true);
    QStringList favorites;
    QList<int> rows;
    for (int row = 0; row < 1000; ++row) {
        favorites.append(model.fileAt(row)->path);
        rows.append(row);
        model.toggleFavorite(favorites.last());
    }
    for (int move = 0; move < 200; ++move) {
        require(model.moveFavoriteOrder(rows, true), "Stress favorite reorder failed");
        rows.move(0, rows.size() - 1);
    }
    require(filter.rowCount() == 1000 && filter.scopedFileCount() == 1000 && model.removeFavorites(favorites) == 1000
        && filter.rowCount() == 0 && model.favoriteCount() == 0, "Stress favorite counts or batch removal failed");
    model.setFiles({});
    QTemporaryDir temp;
    require(temp.isValid(), "Cannot create stress directory");
    for (int row = 0; row < 3000; ++row)
        touch(temp.path() + QStringLiteral("/%1.song.pptx").arg(row, 4, 10, QLatin1Char('0')));
    obs_seeker::Settings settings;
    settings.folders = {temp.path()};
    settings.extensions.append(QStringLiteral("pdf"));
    settings.switchSceneOnOpen = true;
    settings.targetSceneId = QStringLiteral("stress-scene");
    settings.sceneDelayMs = 500;
    const auto config = temp.path() + QStringLiteral("/settings.json");
    require(obs_seeker::saveSettings(config, settings), "Cannot save stress settings");
    obs_seeker::SearchDock dock(config);
    int switches = 0;
    dock.setSceneCallbacks([]() { return QList<obs_seeker::SceneChoice>{{QStringLiteral("stress-scene"), QStringLiteral("Song")}}; },
                          [&](const QString &) { ++switches; });
    dock.resize(680, 420);
    dock.show();
    auto *table = dock.findChild<QTableView *>(QStringLiteral("resultsTable"));
    auto *input = dock.findChild<QLineEdit *>(QStringLiteral("searchInput"));
    auto *proxy = static_cast<QSortFilterProxyModel *>(table->model());
    waitFor([&]() { return proxy->rowCount() == 3000 && scanIdle(dock); });
    int resets = 0;
    QObject::connect(proxy->sourceModel(), &QAbstractItemModel::modelReset, &dock, [&]() { ++resets; });
    for (int batch = 0; batch < 3; ++batch) {
        const auto previousResets = resets;
        for (int row = batch * 30; row < (batch + 1) * 30; ++row) {
            require(QFile::rename(temp.path() + QStringLiteral("/%1.song.pptx").arg(row, 4, 10, QLatin1Char('0')),
                                  temp.path() + QStringLiteral("/%1.updated.pptx").arg(row, 4, 10, QLatin1Char('0'))), "Stress rename failed");
            require(QFile::remove(temp.path() + QStringLiteral("/%1.song.pptx").arg(row + 1000, 4, 10, QLatin1Char('0'))), "Stress delete failed");
            touch(temp.path() + QStringLiteral("/new-%1.pdf").arg(row));
        }
        waitFor([&]() { return resets > previousResets && scanIdle(dock); });
        require(proxy->rowCount() == 3000, "Stress refresh lost files");
    }
    const auto opens = recorder.openRequests;
    for (int request = 0; request < 200; ++request) {
        input->setText(QString::number(2998 + request % 2));
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(input, &enter);
    }
    require(recorder.openRequests == opens + 200, "Stress open requests were lost");
    require(switches == 0, "Stress requests switched early");
    waitFor([&]() { return switches == 1; });
    for (int cycle = 0; cycle < 20; ++cycle) {
        QPointer<QDialog> dialog = openSettings(dock);
        dialog->reject();
        waitFor([&]() { return dialog.isNull(); });
    }
    input->setText(QStringLiteral("2998"));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(input, &enter);
    dock.shutdown();
    require(!dock.findChild<QTimer *>(QStringLiteral("sceneSwitchTimer"))->isActive() && switches == 1,
            "Stress shutdown left a scene switch pending");
    std::cout << "STRESS: 1000 favorites, 200 reorders, 3000 watched files, 270 file changes, 200 opens, 20 dialogs; total "
              << total.elapsed() << " ms\n";
}
#ifdef Q_OS_MACOS
void macOpeningTest(UrlRecorder &recorder)
{
    QTemporaryDir temp;
    const auto config = temp.path() + QStringLiteral("/settings.json");
    for (const auto *name : {"1.pptx", "2.ppsx", "3.key"})
        touch(temp.path() + QLatin1Char('/') + QString::fromLatin1(name));
    obs_seeker::Settings settings;
    settings.folders = {temp.path()};
    settings.autoRefresh = false;
    settings.switchSceneOnOpen = false;
    settings.escapeReturnsHome = false;
    require(obs_seeker::saveSettings(config, settings), "Cannot save Mac open fixture");
    obs_seeker::SearchDock dock(config);
    auto *table = dock.findChild<QTableView *>(QStringLiteral("resultsTable"));
    auto *input = dock.findChild<QLineEdit *>(QStringLiteral("searchInput"));
    waitFor([&] { return table->model()->rowCount() == 3 && dock.findChildren<QThread *>().isEmpty(); });
    const auto before = recorder.openRequests;
    for (int number = 1; number <= 3; ++number) {
        input->setText(QString::number(number));
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(input, &enter);
        require(recorder.openRequests == before + number && recorder.opened.toLocalFile().startsWith(temp.path()),
                "Mac did not use the default file opener");
    }
    require(dock.findChildren<QProcess *>().isEmpty()
        && !dock.findChild<QTimer *>(QStringLiteral("sceneSwitchTimer"))->isActive()
        && !dock.findChild<QTimer *>(QStringLiteral("homeExitTimer"))->isActive(), "Disabled Mac features launched automation");
    QPointer<QDialog> dialog = openSettings(dock);
    require(!dialog->findChild<QCheckBox *>(QStringLiteral("switchSceneCheck"))->isChecked()
        && !dialog->findChild<QCheckBox *>(QStringLiteral("escapeHomeCheck"))->isChecked(), "Disabled Mac options were not retained");
    saveDialog(dialog);
    waitFor([&] { return dialog.isNull(); });
    std::cout << "PASS: Mac default opening for PPTX/PPSX/Keynote with both options disabled\n";
}
#endif
}

int main(int argc, char **argv)
{
#ifdef Q_OS_WIN
    if (argc == 5 && QString::fromUtf8(argv[1]).startsWith(QStringLiteral("--preload-"))) {
        if (strcmp(argv[1], "--preload-fail") == 0)
            return 1;
        if (strcmp(argv[1], "--preload-stall") == 0)
            Sleep(INFINITE);
        const auto eventName = QString::fromUtf8(argv[3]);
        const auto stop = OpenEventW(SYNCHRONIZE, FALSE, reinterpret_cast<LPCWSTR>(eventName.utf16()));
        const auto parent = OpenProcess(SYNCHRONIZE, FALSE, QString::fromUtf8(argv[4]).toULong());
        if (!stop || !parent)
            return 2;
        DWORD written;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "READY\n", 6, &written, nullptr);
        HANDLE handles[]{stop, parent};
        const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        CloseHandle(stop);
        CloseHandle(parent);
        return result == WAIT_OBJECT_0 ? 0 : 3;
    }
#endif
    if (argc == 5 && QString::fromUtf8(argv[1]) == QStringLiteral("--presentation-fixture")) {
        QCoreApplication fixture(argc, argv);
        QThread::msleep(fixture.arguments().at(2).toUInt());
        QFile marker(fixture.arguments().at(4) + QStringLiteral(".opened"));
        if (!marker.open(QIODevice::WriteOnly))
            return 3;
        marker.write("opened");
        return fixture.arguments().at(3).toInt();
    }
    QApplication app(argc, argv);
    obs_seeker::setUiLocale(argc > 1 ? argv[1] : "en-US");
    app.setStyle(QStringLiteral("Fusion"));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(43, 46, 56));
    palette.setColor(QPalette::WindowText, Qt::white);
    palette.setColor(QPalette::Base, QColor(30, 33, 41));
    palette.setColor(QPalette::AlternateBase, QColor(37, 40, 49));
    palette.setColor(QPalette::Text, Qt::white);
    palette.setColor(QPalette::Button, QColor(55, 59, 71));
    palette.setColor(QPalette::ButtonText, Qt::white);
    palette.setColor(QPalette::Highlight, QColor(55, 102, 193));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    app.setPalette(palette);
    UrlRecorder recorder;
    QDesktopServices::setUrlHandler(QStringLiteral("file"), &recorder, "record");
    try {
        const bool stress = argc > 2 && QString::fromUtf8(argv[2]) == QStringLiteral("--stress");
        projectorEscapeTest();
#ifdef Q_OS_WIN
        preloadTest(stress);
#endif
        homeEscapeTest(stress);
        presentationExitTest(stress);
        presentationTest(recorder, stress);
#ifdef Q_OS_MACOS
        macOpeningTest(recorder);
#endif
        recorder.openRequests = 0;
        recorder.opened.clear();
        if (stress) {
            stressTest(recorder);
            QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
            return 0;
        }
        QTemporaryDir temp;
        require(temp.isValid(), "Cannot create UI temporary directory");
        const auto first = temp.path() + QStringLiteral("/诗歌%1 %2库");
        const auto nested = first + QStringLiteral("/子文件夹");
        const auto second = temp.path() + QStringLiteral("/新诗歌");
        require(QDir().mkpath(nested) && QDir().mkpath(second), "Cannot create UI folders");
        const auto numbered = first + QStringLiteral("/010.奇异恩典.pptx");
        touch(numbered);
        touch(first + QStringLiteral("/恩典之路.ppt"));
        touch(first + QStringLiteral("/赞美之泉.ppsx"));
        touch(second + QStringLiteral("/数算恩典.pptx"));
        obs_seeker::FileModel numberModel;
        obs_seeker::NameFilter numberFilter;
        numberFilter.setSourceModel(&numberModel);
        QList<obs_seeker::FileEntry> numberedFiles;
        for (const auto &name : {QStringLiteral("010.圣歌.ppsx"), QStringLiteral("110.圣歌.ppsx"), QStringLiteral("210.圣歌.ppsx"),
                                 QStringLiteral("10+.圣歌.ppsx"), QStringLiteral("10-.圣歌.ppsx"), QStringLiteral("999.圣歌.ppsx")}) {
            const auto normalized = obs_seeker::normalizedText(name);
            numberedFiles.append({name, first, first + QLatin1Char('/') + name, normalized, 0});
        }
        numberModel.setFiles(numberedFiles);
        numberFilter.setQuery(QStringLiteral("10"));
        require(numberFilter.rowCount() == 5, "Numeric substring filtering omitted related filenames");
        for (const auto order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
            numberFilter.sort(0, order);
            for (int row = 0; row < 3; ++row)
                require(obs_seeker::leadingNumberKey(numberFilter.index(row, 0).data().toString()) == QStringLiteral("10"), "Same-number files were not ranked first");
        }
        numberFilter.sort(0, Qt::AscendingOrder);
        numberFilter.setQuery(QStringLiteral("10+"));
        require(numberFilter.rowCount() == 1 && numberFilter.index(0, 0).data().toString().startsWith(QStringLiteral("10+")), "Literal plus sign matching failed");
        numberFilter.setQuery(QStringLiteral("10-"));
        require(numberFilter.rowCount() == 1 && numberFilter.index(0, 0).data().toString().startsWith(QStringLiteral("10-")), "Literal minus sign matching failed");
        for (const auto &folder : {nested, second, first + QStringLiteral("2")}) {
            const auto name = QStringLiteral("210.目录范围.pptx");
            numberedFiles.append({name, folder, folder + QLatin1Char('/') + name, obs_seeker::normalizedText(name), 0});
        }
        numberModel.setFiles(numberedFiles);
        numberFilter.setQuery(QStringLiteral("10"));
        numberFilter.setFolderScope(first);
        require(numberFilter.rowCount() == 6 && numberFilter.scopedFileCount() == 7, "Folder scope included a sibling or excluded a subfolder");
        const auto aliasedRoot = first + QStringLiteral("-alias");
        numberFilter.setFolderAliases({{aliasedRoot, first}});
        numberFilter.setFolderScope(aliasedRoot);
        require(numberFilter.rowCount() == 6 && numberFilter.scopedFileCount() == 7, "Root alias scope lost results");
        numberFilter.setFolderScope(second);
        require(numberFilter.rowCount() == 1 && numberFilter.scopedFileCount() == 1, "Second folder scope failed");
        numberFilter.setFolderScope(QString());
        require(numberFilter.rowCount() == 8 && numberFilter.scopedFileCount() == 9, "All-locations scope failed");
        numberModel.toggleFavorite(numberedFiles.first().path);
        numberFilter.setFavoritesOnly(true);
        require(numberFilter.rowCount() == 1 && numberFilter.scopedFileCount() == 1, "Favorites filter or total failed");
        numberFilter.setFolderScope(second);
        require(numberFilter.rowCount() == 0 && numberFilter.scopedFileCount() == 0, "Favorites escaped the selected location");
        numberFilter.setFolderScope(QString());
        numberFilter.setQuery(QStringLiteral("999"));
        require(numberFilter.rowCount() == 0 && numberFilter.scopedFileCount() == 1, "Search did not filter favorites");
        numberModel.setFiles(numberedFiles);
        numberFilter.setQuery(QStringLiteral("10"));
        require(numberFilter.rowCount() == 1, "Refreshing the model lost favorites");
        numberModel.toggleFavorite(numberedFiles.first().path);
        require(numberFilter.rowCount() == 0, "Favorite removal did not update the proxy");
        numberFilter.setFavoritesOnly(false);
        require(numberFilter.rowCount() == 8, "Leaving favorites did not restore search results");
        const auto rankedFavorite = numberedFiles.at(2).path;
        const auto exactFavorite = numberedFiles.at(4).path;
        numberFilter.setFavoritesFirst(true);
        numberModel.toggleFavorite(rankedFavorite);
        numberModel.toggleFavorite(exactFavorite);
        for (const auto column : {0, 1, 2}) {
            for (const auto order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
                numberFilter.sort(column, order);
                for (int row = 0; row < 3; ++row)
                    require(obs_seeker::leadingNumberKey(numberFilter.index(row, 0).data().toString()) == QStringLiteral("10"),
                            "Favorite overrode exact-number priority");
                require(numberFilter.index(0, 0).data(obs_seeker::FileModel::PathRole).toString() == exactFavorite
                    && numberFilter.index(3, 0).data(obs_seeker::FileModel::PathRole).toString() == rankedFavorite,
                        "Favorites were not prioritized within each match group");
            }
        }
        numberFilter.setQuery({});
        require(numberFilter.index(0, 0).data(obs_seeker::FileModel::FavoriteRole).toBool()
            && numberFilter.index(1, 0).data(obs_seeker::FileModel::FavoriteRole).toBool(), "Empty query lost favorite priority");
        numberModel.toggleFavorite(exactFavorite);
        numberModel.toggleFavorite(rankedFavorite);
        numberFilter.setFavoritesFirst(false);
        numberFilter.sort(0, Qt::AscendingOrder);
        {
            obs_seeker::FileModel exactModel;
            QList<obs_seeker::FileEntry> files;
            for (const auto &name : {QStringLiteral("060.Hymn.pps"), QStringLiteral("60.Hymn.pptx"),
                    QStringLiteral("160.Hymn.ppsx"), QStringLiteral("260.Hymn.ppt"),
                    QStringLiteral("015.Hymn.pps"), QStringLiteral("15.Hymn.pptx"),
                    QStringLiteral("115.Hymn.ppsx"), QStringLiteral("215.Hymn.ppt")})
                files.append({name, first, first + QLatin1Char('/') + name, obs_seeker::normalizedText(name), 0});
            exactModel.setFiles(files);
            obs_seeker::NameFilter exactFilter;
            exactFilter.setSourceModel(&exactModel);
            exactFilter.setFavoritesFirst(true);
            for (const auto &name : {QStringLiteral("160.Hymn.ppsx"), QStringLiteral("115.Hymn.ppsx"), QStringLiteral("15.Hymn.pptx")})
                exactModel.toggleFavorite(first + QLatin1Char('/') + name);
            for (const auto &query : {QStringLiteral("60"), QStringLiteral("15")}) {
                exactFilter.setQuery(query);
                require(exactFilter.rowCount() == 4, "Exact priority omitted a substring match");
                for (const auto column : {0, 1, 2}) {
                    for (const auto order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
                        exactFilter.sort(column, order);
                        for (int row = 0; row < 2; ++row)
                            require(obs_seeker::leadingNumberKey(exactFilter.index(row, 0).data().toString()) == query,
                                    "Leading-zero number match was displaced by a favorite");
                        require(exactFilter.index(2, 0).data(obs_seeker::FileModel::FavoriteRole).toBool(),
                                "Partial-match group lost favorite priority");
                        if (query == QStringLiteral("15"))
                            require(exactFilter.index(0, 0).data(obs_seeker::FileModel::FavoriteRole).toBool(),
                                    "Exact-match group lost favorite priority");
                    }
                }
                exactFilter.setFavoritesFirst(false);
                require(obs_seeker::leadingNumberKey(exactFilter.index(0, 0).data().toString()) == query,
                        "Disabling favorites disabled exact-number priority");
                exactFilter.setFavoritesFirst(true);
            }
        }
        {
            obs_seeker::FileModel extensionModel;
            QList<obs_seeker::FileEntry> files;
            for (const auto &name : {QStringLiteral("001.Hymn.pdf"), QStringLiteral("001.Hymn.ppt"),
                    QStringLiteral("001.Hymn.notes.pptx"), QStringLiteral("001.Hymn.ppsx"),
                    QStringLiteral("001.Hymn.pptx"), QStringLiteral("001.Hymn.pps"), QStringLiteral("001.Hymn.zzz"),
                    QStringLiteral("010.Hymn.pdf"), QStringLiteral("010.Hymn.pptx"), QStringLiteral("110.Hymn.pdf")})
                files.append({name, first, first + QLatin1Char('/') + name, obs_seeker::normalizedText(name), 0});
            extensionModel.setFiles(files);
            obs_seeker::NameFilter extensions;
            extensions.setSourceModel(&extensionModel);
            extensions.setExtensionOrder(obs_seeker::Settings{}.extensions);
            extensions.sort(0, Qt::AscendingOrder);
            const QStringList expected{QStringLiteral("001.Hymn.pps"), QStringLiteral("001.Hymn.ppsx"),
                QStringLiteral("001.Hymn.ppt"), QStringLiteral("001.Hymn.pptx"), QStringLiteral("001.Hymn.pdf"),
                QStringLiteral("001.Hymn.zzz"), QStringLiteral("001.Hymn.notes.pptx")};
            for (int row = 0; row < expected.size(); ++row)
                require(extensions.index(row, 0).data().toString() == expected.at(row), "Extension preference separated a same-name group");
            extensions.setQuery(QStringLiteral("001.Hymn"));
            extensions.sort(0, Qt::DescendingOrder);
            for (int row = 0; row < 4; ++row)
                require(extensions.index(row + 1, 0).data().toString() == expected.at(row), "Descending names reversed extension priority");
            extensions.setExtensionOrder({QStringLiteral(".PDF"), QStringLiteral("*.PPTX"), QStringLiteral("pdf"), QStringLiteral("ppsx")});
            extensions.sort(0, Qt::AscendingOrder);
            require(extensions.index(0, 0).data().toString() == QStringLiteral("001.Hymn.pdf")
                && extensions.index(1, 0).data().toString() == QStringLiteral("001.Hymn.pptx"), "Custom extension order or normalization failed");
            extensions.setFavoritesFirst(true);
            extensionModel.toggleFavorite(first + QStringLiteral("/001.Hymn.pps"));
            require(extensions.index(0, 0).data().toString() == QStringLiteral("001.Hymn.pps"), "Extension priority overrode favorites");
            extensionModel.toggleFavorite(first + QStringLiteral("/001.Hymn.pps"));
            extensions.setQuery(QStringLiteral("10"));
            for (const auto order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
                extensions.sort(0, order);
                require(extensions.index(0, 0).data().toString() == QStringLiteral("010.Hymn.pdf")
                    && extensions.index(1, 0).data().toString() == QStringLiteral("010.Hymn.pptx"), "Extension order disrupted same-number priority");
            }
            extensions.setQuery({});
            extensionModel.setFiles(files);
            extensions.sort(0, Qt::AscendingOrder);
            require(extensions.index(0, 0).data().toString() == QStringLiteral("001.Hymn.pdf"), "Rescanning lost extension order");
        }
        {
            obs_seeker::FileModel countsModel;
            countsModel.setFiles({{QStringLiteral("a.pptx"), first, first + QStringLiteral("/a.pptx"), QStringLiteral("a.pptx"), 0},
                                 {QStringLiteral("b.pptx"), second, second + QStringLiteral("/b.pptx"), QStringLiteral("b.pptx"), 0}});
            obs_seeker::NameFilter counts;
            counts.setSourceModel(&countsModel);
            counts.setFolderScope(first);
            require(counts.scopedFileCount() == 1, "Initial scoped count failed");
            counts.setQuery(QStringLiteral("absent"));
            require(counts.rowCount() == 0 && counts.scopedFileCount() == 1, "Query changed the scoped total");
            counts.setFavoritesOnly(true);
            require(counts.scopedFileCount() == 0, "Favorite count cache kept nonfavorites");
            countsModel.toggleFavorite(first + QStringLiteral("/a.pptx"));
            require(counts.scopedFileCount() == 1, "Favorite addition did not invalidate the count");
            countsModel.toggleFavorite(first + QStringLiteral("/a.pptx"));
            require(counts.scopedFileCount() == 0, "Favorite removal did not invalidate the count");
            counts.setFavoritesOnly(false);
            const auto alias = temp.path() + QStringLiteral("/counts-alias");
            counts.setFolderScope(alias);
            require(counts.scopedFileCount() == 0, "Unknown alias count failed");
            counts.setFolderAliases({{alias, first}});
            require(counts.scopedFileCount() == 1, "Alias change left a stale count");
            countsModel.setFiles({});
            require(counts.scopedFileCount() == 0, "Source reset left a stale count");
            auto *replacement = new obs_seeker::FileModel;
            replacement->setFiles({{QStringLiteral("c.pptx"), first, first + QStringLiteral("/c.pptx"), QStringLiteral("c.pptx"), 0}});
            counts.setSourceModel(replacement);
            require(counts.scopedFileCount() == 1, "Source replacement left a stale count");
            delete replacement;
            require(counts.scopedFileCount() == 0, "Destroyed source left a stale count");
        }
        {
            obs_seeker::FileModel orderedModel;
            QList<obs_seeker::FileEntry> files;
            for (const auto &name : {QStringLiteral("001.First.pps"), QStringLiteral("002.Second.pptx"),
                    QStringLiteral("003.Third.pptx"), QStringLiteral("004.Other.ppt")})
                files.append({name, first, first + QLatin1Char('/') + name, obs_seeker::normalizedText(name), 0});
            orderedModel.setFiles(files);
            obs_seeker::NameFilter ordered;
            ordered.setSourceModel(&orderedModel);
            ordered.setFavoritesFirst(true);
            for (const auto row : {2, 0, 1})
                orderedModel.toggleFavorite(files.at(row).path);
            const QStringList added{files.at(2).path, files.at(0).path, files.at(1).path};
            for (const auto column : {0, 1, 2}) {
                for (const auto direction : {Qt::AscendingOrder, Qt::DescendingOrder}) {
                    ordered.sort(column, direction);
                    for (int row = 0; row < added.size(); ++row)
                        require(ordered.index(row, 0).data(obs_seeker::FileModel::PathRole).toString() == added.at(row),
                                "Sort column or direction changed the order added");
                }
            }
            ordered.setFavoritesFirst(false);
            ordered.sort(0, Qt::AscendingOrder);
            require(ordered.index(0, 0).data().toString() == files.first().name, "Disabled favorite priority reordered normal files");
            ordered.setFavoritesOnly(true);
            require(ordered.rowCount() == 3 && ordered.index(0, 0).data(obs_seeker::FileModel::PathRole).toString() == added.first(),
                    "Favorites-only mode did not use the stored order");
            ordered.sort(2, Qt::DescendingOrder);
            require(ordered.scopedFileCount() == 3 && orderedModel.moveFavoriteOrder({2, 0}, true), "Cannot move favorite positions");
            require(ordered.index(0, 0).data(obs_seeker::FileModel::PathRole).toString() == files.at(0).path
                && ordered.scopedFileCount() == 3, "Moving a favorite lost its order or count");
            require(orderedModel.moveFavoriteOrder({0, 2, 1}, false)
                && ordered.index(0, 0).data(obs_seeker::FileModel::PathRole).toString() == files.at(1).path
                && ordered.index(1, 0).data(obs_seeker::FileModel::PathRole).toString() == files.at(0).path,
                    "Move to top changed the remaining relative order");
            require(orderedModel.moveFavoriteOrder({1, 0, 2}, true)
                && ordered.index(0, 0).data(obs_seeker::FileModel::PathRole).toString() == files.at(0).path
                && ordered.index(1, 0).data(obs_seeker::FileModel::PathRole).toString() == files.at(2).path,
                    "Move to bottom changed the remaining relative order");
            require(!orderedModel.moveFavoriteOrder({-1, 0}, false) && !orderedModel.moveFavoriteOrder({0, 3}, true)
                && !orderedModel.moveFavoriteOrder({0, 0}, true) && !orderedModel.moveFavoriteOrder({0}, true)
                && !orderedModel.moveFavoriteOrder({2, 0}, false), "Invalid favorite move was accepted");
            orderedModel.toggleFavorite(files.at(0).path);
            orderedModel.toggleFavorite(files.at(0).path);
            require(ordered.index(2, 0).data(obs_seeker::FileModel::PathRole).toString() == files.at(0).path,
                    "Re-added favorite did not go to the end");
            orderedModel.setFiles({files.at(3), files.at(1), files.at(0), files.at(2)});
            require(ordered.index(0, 0).data(obs_seeker::FileModel::PathRole).toString() == files.at(2).path
                && ordered.index(2, 0).data(obs_seeker::FileModel::PathRole).toString() == files.at(0).path,
                    "Rescan lost the custom favorite order");
        }
        {
            const auto orderFolder = temp.path() + QStringLiteral("/favorite-order-ui");
            const auto otherFolder = temp.path() + QStringLiteral("/favorite-order-other");
            require(QDir().mkpath(orderFolder) && QDir().mkpath(otherFolder), "Cannot create favorite-order folders");
            const QStringList paths{orderFolder + QStringLiteral("/001.First.pps"), orderFolder + QStringLiteral("/002.Second.pptx"),
                orderFolder + QStringLiteral("/003.Third.pptx"), otherFolder + QStringLiteral("/004.Other.pptx")};
            for (const auto &path : paths)
                touch(path);
            const auto orderConfig = temp.path() + QStringLiteral("/favorite-order-settings.json");
            obs_seeker::Settings orderSettings;
            orderSettings.folders = {orderFolder, otherFolder};
            orderSettings.autoRefresh = false;
            require(obs_seeker::saveSettings(orderConfig, orderSettings), "Cannot save favorite-order settings");
            QFile configFile(orderConfig);
            require(configFile.open(QIODevice::ReadOnly), "Cannot read favorite-order settings");
            const auto savedConfig = configFile.readAll();
            configFile.close();
            {
                obs_seeker::SearchDock orderDock(orderConfig);
                orderDock.show();
                auto *orderTable = orderDock.findChild<QTableView *>(QStringLiteral("resultsTable"));
                auto *orderInput = orderDock.findChild<QLineEdit *>(QStringLiteral("searchInput"));
                auto *orderScope = orderDock.findChild<QComboBox *>(QStringLiteral("folderScopeCombo"));
                auto *orderButton = orderDock.findChild<QToolButton *>(QStringLiteral("favoritesButton"));
                auto *orderProxy = static_cast<QSortFilterProxyModel *>(orderTable->model());
                auto *orderModel = static_cast<obs_seeker::FileModel *>(orderProxy->sourceModel());
                const auto idle = [&](int count) {
                    return orderTable->model()->rowCount() == count && orderDock.findChildren<QThread *>().isEmpty();
                };
                const auto visiblePaths = [&]() {
                    QStringList values;
                    for (int row = 0; row < orderTable->model()->rowCount(); ++row)
                        values.append(orderTable->model()->index(row, 0).data(obs_seeker::FileModel::PathRole).toString());
                    return values;
                };
                const auto openMenu = [&](int row) {
                    const auto index = orderTable->model()->index(row, 0);
                    orderTable->scrollTo(index);
                    pumpEvents();
                    const auto point = orderTable->visualRect(index).center();
                    require(QMetaObject::invokeMethod(orderTable, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, point)),
                            "Cannot open favorite-order menu");
                    QPointer<QMenu> result = orderDock.findChild<QMenu *>(QStringLiteral("fileContextMenu"));
                    require(result && result->findChild<QAction *>(QStringLiteral("moveFavoriteUpAction"))->text() == obs_seeker::uiText("Menu.MoveUp")
                        && result->findChild<QAction *>(QStringLiteral("moveFavoriteDownAction"))->text() == obs_seeker::uiText("Menu.MoveDown")
                        && result->findChild<QAction *>(QStringLiteral("moveFavoriteTopAction"))->text() == obs_seeker::uiText("Menu.MoveTop")
                        && result->findChild<QAction *>(QStringLiteral("moveFavoriteBottomAction"))->text() == obs_seeker::uiText("Menu.MoveBottom"),
                            "Favorite-order menu is missing or untranslated");
                    const auto actions = result->actions();
                    require(!result->toolTipsVisible(), "Favorite ordering menu shows hover hints");
                    require(actions.size() == 12 && actions.at(4)->isSeparator()
                        && actions.at(5)->objectName() == QStringLiteral("moveFavoriteTopAction")
                        && actions.at(6)->objectName() == QStringLiteral("moveFavoriteUpAction")
                        && actions.at(7)->objectName() == QStringLiteral("moveFavoriteDownAction")
                        && actions.at(8)->objectName() == QStringLiteral("moveFavoriteBottomAction")
                        && actions.at(9)->isSeparator(), "Favorite move group is not ordered or separated");
                    return result;
                };
                const auto closeMenu = [&](QPointer<QMenu> &menu) {
                    menu->close();
                    waitFor([&]() { return menu.isNull(); });
                };
                waitFor([&]() { return idle(4); });
                for (const auto position : {2, 0, 1}) {
                    for (int row = 0; row < orderTable->model()->rowCount(); ++row) {
                        const auto index = orderTable->model()->index(row, 0);
                        if (index.data(obs_seeker::FileModel::PathRole).toString() == paths.at(position)) {
                            clickStar(orderTable, index);
                            break;
                        }
                    }
                }
                orderButton->click();
                const QStringList wanted{paths.at(2), paths.at(0), paths.at(1)};
                require(visiblePaths() == wanted, "Favorites view lost the order added");
                auto menu = openMenu(0);
                require(!menu->findChild<QAction *>(QStringLiteral("moveFavoriteUpAction"))->isEnabled()
                    && !menu->findChild<QAction *>(QStringLiteral("moveFavoriteTopAction"))->isEnabled()
                    && menu->findChild<QAction *>(QStringLiteral("moveFavoriteDownAction"))->isEnabled(), "First favorite move bounds are wrong");
                menu->findChild<QAction *>(QStringLiteral("moveFavoriteDownAction"))->trigger();
                closeMenu(menu);
                require(visiblePaths() == QStringList{paths.at(0), paths.at(2), paths.at(1)}
                    && orderTable->currentIndex().data(obs_seeker::FileModel::PathRole).toString() == paths.at(2),
                        "Move down changed the wrong favorite or selection");
                orderTable->sortByColumn(2, Qt::DescendingOrder);
                menu = openMenu(1);
                menu->findChild<QAction *>(QStringLiteral("moveFavoriteUpAction"))->trigger();
                closeMenu(menu);
                require(visiblePaths() == wanted && orderTable->currentIndex().row() == 0, "Move up or reverse sorting lost favorite order");
                menu = openMenu(2);
                require(!menu->findChild<QAction *>(QStringLiteral("moveFavoriteDownAction"))->isEnabled()
                    && !menu->findChild<QAction *>(QStringLiteral("moveFavoriteBottomAction"))->isEnabled(), "Last favorite can move down");
                menu->findChild<QAction *>(QStringLiteral("moveFavoriteTopAction"))->trigger();
                closeMenu(menu);
                require(visiblePaths() == QStringList{paths.at(1), paths.at(2), paths.at(0)}
                    && orderTable->currentIndex().row() == 0, "Move to top changed other favorites or selection");
                menu = openMenu(0);
                menu->findChild<QAction *>(QStringLiteral("moveFavoriteBottomAction"))->trigger();
                closeMenu(menu);
                require(visiblePaths() == wanted && orderTable->currentIndex().row() == 2,
                        "Move to bottom changed other favorites or selection");
                menu = openMenu(0);
                orderInput->setText(QStringLiteral("001"));
                waitFor([&]() { return idle(1); });
                menu->findChild<QAction *>(QStringLiteral("moveFavoriteDownAction"))->trigger();
                menu->findChild<QAction *>(QStringLiteral("moveFavoriteBottomAction"))->trigger();
                closeMenu(menu);
                orderInput->clear();
                waitFor([&]() { return idle(3); });
                require(visiblePaths() == wanted, "Stale menu moved a filtered favorite");
                orderScope->setCurrentIndex(orderScope->findData(orderFolder));
                menu = openMenu(0);
                require(!menu->findChild<QAction *>(QStringLiteral("moveFavoriteDownAction"))->isEnabled()
                    && !menu->findChild<QAction *>(QStringLiteral("moveFavoriteTopAction"))->isEnabled()
                    && !menu->findChild<QAction *>(QStringLiteral("moveFavoriteBottomAction"))->isEnabled(), "Folder scope allowed favorite reordering");
                closeMenu(menu);
                orderScope->setCurrentIndex(0);
                menu = openMenu(0);
                orderDock.refresh();
                menu->findChild<QAction *>(QStringLiteral("moveFavoriteDownAction"))->trigger();
                menu->findChild<QAction *>(QStringLiteral("moveFavoriteBottomAction"))->trigger();
                closeMenu(menu);
                waitFor([&]() { return idle(3); });
                require(visiblePaths() == wanted, "Scanning allowed a stale favorite move");
                menu = openMenu(0);
                orderModel->toggleFavorite(paths.at(0));
                menu->findChild<QAction *>(QStringLiteral("moveFavoriteDownAction"))->trigger();
                closeMenu(menu);
                require(visiblePaths() == QStringList{paths.at(2), paths.at(1)}, "Missing neighbor changed another favorite");
                orderModel->toggleFavorite(paths.at(0));
                require(visiblePaths() == QStringList{paths.at(2), paths.at(1), paths.at(0)}, "Re-added favorite did not append");
                orderDock.refresh();
                waitFor([&]() { return idle(3); });
                require(visiblePaths() == QStringList{paths.at(2), paths.at(1), paths.at(0)}, "Refresh lost the favorite order");
                require(configFile.open(QIODevice::ReadOnly), "Cannot inspect favorite-order persistence");
                require(configFile.readAll() == savedConfig, "Favorite order was written to settings");
                configFile.close();
            }
            obs_seeker::SearchDock restarted(orderConfig);
            restarted.show();
            auto *table = restarted.findChild<QTableView *>(QStringLiteral("resultsTable"));
            waitFor([&]() { return table->model()->rowCount() == 4; });
            for (int row = 0; row < table->model()->rowCount(); ++row)
                require(!table->model()->index(row, 0).data(obs_seeker::FileModel::FavoriteRole).toBool(), "Restart restored favorite order or flags");
        }
        {
            const auto keyFolder = temp.path() + QStringLiteral("/keynote-ui");
            const auto keyPackage = keyFolder + QStringLiteral("/010.Presentation.key");
            require(QDir().mkpath(keyPackage + QStringLiteral("/Data")), "Cannot create Keynote UI fixture");
            touch(keyPackage + QStringLiteral("/Data/internal.pptx"));
            touch(keyFolder + QStringLiteral("/110.Presentation.key"));
            obs_seeker::Settings keySettings;
            keySettings.folders = {keyFolder};
            keySettings.autoRefresh = false;
            const auto keyConfig = temp.path() + QStringLiteral("/keynote-settings.json");
            require(obs_seeker::saveSettings(keyConfig, keySettings), "Cannot save Keynote UI settings");
            obs_seeker::SearchDock keyDock(keyConfig);
            keyDock.show();
            auto *keyTable = keyDock.findChild<QTableView *>(QStringLiteral("resultsTable"));
            auto *keyInput = keyDock.findChild<QLineEdit *>(QStringLiteral("searchInput"));
            waitFor([&]() { return keyTable->model()->rowCount() == 2; });
            keyInput->setText(QStringLiteral("010"));
            QKeyEvent openPackage(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QCoreApplication::sendEvent(keyInput, &openPackage);
            require(recorder.openRequests == 1 && recorder.opened.toLocalFile() == keyPackage, "Enter did not open the Keynote package");
            auto *keyDialog = openSettings(keyDock);
            auto *keyFolders = keyDialog->findChild<QListWidget *>(QStringLiteral("folderList"));
            require(!dropFolders(keyFolders, {QUrl::fromLocalFile(keyPackage)}) && keyFolders->count() == 1,
                    "Keynote document was accepted as a search folder");
            keyDialog->reject();
            pumpEvents();
            recorder.openRequests = 0;
            recorder.opened.clear();
        }
        {
            const auto orderConfig = temp.path() + QStringLiteral("/folder-order.json");
            obs_seeker::Settings orderSettings;
            orderSettings.folders = {first, second, nested};
            orderSettings.autoRefresh = false;
            require(obs_seeker::saveSettings(orderConfig, orderSettings), "Cannot save folder-order fixture");
            const QStringList reordered{nested, first, second};
            {
                obs_seeker::SearchDock orderDock(orderConfig);
                orderDock.show();
                auto *orderTable = orderDock.findChild<QTableView *>(QStringLiteral("resultsTable"));
                auto *orderScope = orderDock.findChild<QComboBox *>(QStringLiteral("folderScopeCombo"));
                require(orderScope, "Folder-order location menu missing");
                waitFor([&]() { return orderTable->model()->rowCount() == 4; });
                QPointer<QDialog> orderDialog = openSettings(orderDock);
                auto *list = orderDialog->findChild<QListWidget *>(QStringLiteral("folderList"));
                require(list->dragEnabled() && list->acceptDrops() && list->showDropIndicator()
                    && list->dragDropMode() == QAbstractItemView::InternalMove
                    && list->defaultDropAction() == Qt::MoveAction, "Folder reorder is not enabled");
                require(list->model()->moveRows({}, 0, 1, {}, 3)
                    && list->item(2)->text() == QDir::toNativeSeparators(first), "Folder move to bottom failed");
                require(list->model()->moveRows({}, 2, 1, {}, 0)
                    && list->item(0)->text() == QDir::toNativeSeparators(first), "Folder move to top failed");
                require(list->model()->moveRows({}, 0, 2, {}, 3), "Multiple-folder reorder failed");
                require(obs_seeker::loadSettings(orderConfig).folders == orderSettings.folders,
                        "Reordering folders saved before Save");
                const auto extraFolder = temp.path() + QStringLiteral("/排序后添加");
                require(QDir().mkpath(extraFolder), "Cannot create reordered drop fixture");
                require(dropFolders(list, {QUrl::fromLocalFile(first), QUrl::fromLocalFile(extraFolder)})
                    && list->count() == 4 && list->item(0)->text() == QDir::toNativeSeparators(nested)
                    && list->item(3)->text() == QDir::toNativeSeparators(extraFolder), "Batch import changed folder order");
                orderDialog->reject();
                waitFor([&]() { return orderDialog.isNull(); });
                require(obs_seeker::loadSettings(orderConfig).folders == orderSettings.folders
                    && orderScope->itemData(1).toString() == first, "Cancel changed saved folders or location order");
                orderDialog = openSettings(orderDock);
                list = orderDialog->findChild<QListWidget *>(QStringLiteral("folderList"));
                require(list->count() == 3 && list->item(0)->text() == QDir::toNativeSeparators(first),
                        "Reopening Settings did not discard cancelled order");
                require(list->model()->moveRows({}, 0, 2, {}, 3), "Saved folder reorder failed");
                orderScope->setCurrentIndex(orderScope->findData(first));
                saveDialog(orderDialog);
                waitFor([&]() { return orderDialog.isNull() && orderTable->model()->rowCount() == 3; });
                require(obs_seeker::loadSettings(orderConfig).folders == reordered
                    && orderScope->currentData().toString() == first, "Save lost order or active location");
                for (int row = 0; row < reordered.size(); ++row)
                    require(orderScope->itemData(row + 1).toString() == reordered.at(row), "Location menu order differs from Settings");
            }
            obs_seeker::SearchDock restarted(orderConfig);
            restarted.show();
            auto *dialog = openSettings(restarted);
            auto *list = dialog->findChild<QListWidget *>(QStringLiteral("folderList"));
            for (int row = 0; row < reordered.size(); ++row)
                require(list->item(row)->text() == QDir::toNativeSeparators(reordered.at(row)), "Restart lost saved folder order");
            dialog->reject();
            pumpEvents();
        }
        {
            const auto folder = temp.path() + QStringLiteral("/separator-files");
            require(QDir().mkpath(folder), "Cannot create separator fixture");
            QStringList paths;
            for (int row = 0; row < 20; ++row) {
                paths.append(folder + QStringLiteral("/%1.song.pptx").arg(row, 3, 10, QLatin1Char('0')));
                touch(paths.last());
            }
            obs_seeker::Settings separatorSettings;
            separatorSettings.folders = {folder};
            separatorSettings.autoRefresh = false;
            const auto separatorConfig = temp.path() + QStringLiteral("/separator-settings.json");
            require(obs_seeker::saveSettings(separatorConfig, separatorSettings), "Cannot save separator settings");
            obs_seeker::SearchDock separatorDock(separatorConfig);
            separatorDock.resize(800, 360);
            separatorDock.show();
            auto *view = separatorDock.findChild<QTableView *>(QStringLiteral("resultsTable"));
            auto *search = separatorDock.findChild<QLineEdit *>(QStringLiteral("searchInput"));
            auto *favorites = separatorDock.findChild<QToolButton *>(QStringLiteral("favoritesButton"));
            auto *filter = static_cast<obs_seeker::NameFilter *>(view->model());
            auto *source = static_cast<obs_seeker::FileModel *>(filter->sourceModel());
            waitFor([&]() { return filter->rowCount() == 20; });
            const auto dividerAt = [&](int row) {
                const auto image = view->viewport()->grab().toImage();
                const auto scale = qreal(image.height()) / view->viewport()->height();
                const auto y = qRound(view->visualRect(filter->index(row, 0)).bottom() * scale);
                if (y < 0 || y >= image.height())
                    return false;
                const QColor color(128, 128, 128);
                for (int pixel = qMax(0, y - 1); pixel <= qMin(image.height() - 1, y + 1); ++pixel) {
                    if (image.pixelColor(image.width() / 4, pixel).rgb() == color.rgb()
                        && image.pixelColor(image.width() / 2, pixel).rgb() == color.rgb()
                        && image.pixelColor(image.width() * 3 / 4, pixel).rgb() == color.rgb())
                        return true;
                }
                if (filter->showsFavoriteSeparator() && filter->index(row, 0).data(obs_seeker::FileModel::FavoriteRole).toBool()
                    && row + 1 < filter->rowCount() && !filter->index(row + 1, 0).data(obs_seeker::FileModel::FavoriteRole).toBool()) {
                    QBuffer buffer;
                    buffer.open(QIODevice::WriteOnly);
                    image.save(&buffer, "PNG");
                    std::cout << "PAINTPNG:" << buffer.data().toBase64().constData() << '\n';
                }
                return false;
            };
            require(!dividerAt(0), "Empty favorites drew a separator");
            source->toggleFavorite(paths.at(0));
            require(dividerAt(0) && filter->rowCount() == 20, "Favorite separator is missing or added a row");
            source->toggleFavorite(paths.at(2));
            require(dividerAt(1) && !dividerAt(0), "Favorite addition left a duplicate separator");
            for (const bool light : {false, true}) {
                auto colors = view->palette();
                colors.setColor(QPalette::Mid, light ? QColor(240, 240, 240) : QColor(12, 12, 12));
                colors.setColor(QPalette::Base, light ? Qt::white : QColor(8, 8, 8));
                colors.setColor(QPalette::AlternateBase, light ? QColor(245, 245, 245) : QColor(16, 16, 16));
                view->setPalette(colors);
                require(dividerAt(1), "Separator gray changed with the theme");
            }
            search->setText(QStringLiteral("."));
            waitFor([&]() { return !filter->tokens().isEmpty(); });
            require(filter->rowCount() == 20 && !dividerAt(1), "Search still drew the favorite separator");
            search->clear();
            waitFor([&]() { return filter->tokens().isEmpty(); });
            favorites->click();
            require(filter->rowCount() == 2 && !dividerAt(1), "Favorites-only view drew a separator");
            favorites->click();
            filter->setFavoritesFirst(false);
            require(!dividerAt(0) && !dividerAt(1), "Disabled favorite priority drew a separator");
            filter->setFavoritesFirst(true);
            view->sortByColumn(2, Qt::DescendingOrder);
            require(dividerAt(1), "Reverse sorting lost the favorite separator");
            separatorDock.resize(320, 360);
            waitFor([&]() { return view->isColumnHidden(1) && view->isColumnHidden(2); });
            require(dividerAt(1), "Narrow view lost the favorite separator");
            source->toggleFavorite(paths.at(2));
            require(dividerAt(0) && !dividerAt(1), "Favorite removal left a duplicate separator");
            source->toggleFavorite(paths.at(0));
            require(!dividerAt(0), "Removing the last favorite left a separator");
            for (int row = 0; row < 10; ++row)
                source->toggleFavorite(paths.at(row));
            separatorDock.resize(320, 180);
            pumpEvents();
            view->scrollTo(filter->index(9, 0), QAbstractItemView::PositionAtBottom);
            require(dividerAt(9), "Scrolled favorite boundary is missing");
            view->scrollTo(filter->index(10, 0), QAbstractItemView::PositionAtTop);
            require(!dividerAt(9), "Offscreen boundary drew inside ordinary rows");
            for (int row = 10; row < 20; ++row)
                source->toggleFavorite(paths.at(row));
            view->scrollToTop();
            require(filter->rowCount() == 20 && !dividerAt(0), "All-favorite list drew a separator");
            source->toggleFavorite(paths.at(19));
            view->scrollTo(filter->index(18, 0), QAbstractItemView::PositionAtBottom);
            QElapsedTimer paintTimer;
            paintTimer.start();
            for (int repaint = 0; repaint < 200; ++repaint)
                view->viewport()->repaint();
            std::cout << "PERF: favorite separator, 200 viewport repaints " << paintTimer.elapsed() << " ms\n";
        }
        {
            const auto folder = temp.path() + QStringLiteral("/clear-selection");
            require(QDir().mkpath(folder), "Cannot create selection fixture");
            QStringList paths;
            for (const auto &name : {QStringLiteral("001.song.pptx"), QStringLiteral("018.song.pptx"),
                    QStringLiteral("020.song.pptx"), QStringLiteral("999.song.pptx")}) {
                paths.append(folder + QLatin1Char('/') + name);
                touch(paths.last());
            }
            obs_seeker::Settings keepSettings;
            keepSettings.folders = {folder};
            keepSettings.autoRefresh = false;
            const auto keepConfig = temp.path() + QStringLiteral("/clear-selection.json");
            require(obs_seeker::saveSettings(keepConfig, keepSettings), "Cannot save selection settings");
            obs_seeker::SearchDock keepDock(keepConfig);
            keepDock.resize(800, 360);
            keepDock.show();
            auto *view = keepDock.findChild<QTableView *>(QStringLiteral("resultsTable"));
            auto *search = keepDock.findChild<QLineEdit *>(QStringLiteral("searchInput"));
            auto *favorites = keepDock.findChild<QToolButton *>(QStringLiteral("favoritesButton"));
            auto *filter = static_cast<obs_seeker::NameFilter *>(view->model());
            auto *source = static_cast<obs_seeker::FileModel *>(filter->sourceModel());
            waitFor([&]() { return filter->rowCount() == 4; });
            keepDock.focusSearch();
            waitFor([&]() { return keepDock.window()->isActiveWindow() && keepDock.focusWidget() == search; });
            source->toggleFavorite(paths.at(0));
            QKeyEvent star(QEvent::KeyPress, Qt::Key_Asterisk, Qt::KeypadModifier, QStringLiteral("*"));
            QKeyEvent clear(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            const auto selectedPath = [&]() { return view->currentIndex().data(obs_seeker::FileModel::PathRole).toString(); };
            search->setText(QStringLiteral("18"));
            QCoreApplication::sendEvent(search, &star);
            require(search->text().isEmpty() && filter->rowCount() == 4 && selectedPath() == paths.at(1)
                && view->currentIndex().row() == 1, "Input star selected the first favorite instead of the new one");
            const auto beforeOpen = recorder.opened;
            const auto beforeRequests = recorder.openRequests;
            QKeyEvent open(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QCoreApplication::sendEvent(search, &open);
            require(recorder.openRequests == beforeRequests + 1 && recorder.opened.toLocalFile() == paths.at(1),
                    "Enter after starring opened a different file");
            recorder.opened = beforeOpen;
            recorder.openRequests = beforeRequests;
            QCoreApplication::sendEvent(search, &clear);
            require(selectedPath() == paths.at(1), "Escape with empty input changed the selection");
            search->setText(QStringLiteral("018"));
            waitFor([&]() { return filter->rowCount() == 1; });
            QCoreApplication::sendEvent(view, &clear);
            require(search->text().isEmpty() && filter->rowCount() == 4 && selectedPath() == paths.at(1),
                    "Escape did not preserve the searched file");
            search->setText(QStringLiteral("20"));
            QCoreApplication::sendEvent(search, &star);
            require(selectedPath() == paths.at(2) && view->currentIndex().row() == 2,
                    "A second new favorite did not stay selected");
            favorites->click();
            search->setText(QStringLiteral("20"));
            waitFor([&]() { return filter->rowCount() == 1; });
            QCoreApplication::sendEvent(search, &star);
            require(search->text().isEmpty() && filter->rowCount() == 2 && selectedPath() == paths.at(0),
                    "Hidden selection did not fall back to the first item");
            source->toggleFavorite(paths.at(0));
            source->toggleFavorite(paths.at(1));
            QCoreApplication::sendEvent(search, &clear);
            require(filter->rowCount() == 0 && !view->currentIndex().isValid() && !view->selectionModel()->hasSelection(),
                    "Empty list retained a selection after Escape");
            favorites->click();
            view->clearSelection();
            view->setCurrentIndex({});
            QCoreApplication::sendEvent(search, &clear);
            require(selectedPath() == paths.at(0), "Missing selection did not fall back to the first item");
        }
        const auto config = temp.path() + QStringLiteral("/settings.json");
        obs_seeker::Settings settings;
        settings.folders = {first};
        require(obs_seeker::saveSettings(config, settings), "Cannot save UI settings");
        obs_seeker::SearchDock panel(config);
        panel.setWindowTitle(obs_seeker::uiText("Plugin.Title"));
        panel.resize(920, 430);
        panel.show();
        auto *table = panel.findChild<QTableView *>(QStringLiteral("resultsTable"));
        auto *input = panel.findChild<QLineEdit *>(QStringLiteral("searchInput"));
        auto *status = panel.findChild<QLabel *>(QStringLiteral("statusLabel"));
        auto *emptyResults = panel.findChild<QLabel *>(QStringLiteral("emptyResultsLabel"));
        require(table && input && status && emptyResults, "Dock controls missing");
        auto *scope = panel.findChild<QComboBox *>(QStringLiteral("folderScopeCombo"));
        auto *scopeRow = panel.findChild<QWidget *>(QStringLiteral("scopeRow"));
        require(scope && scopeRow && !scopeRow->isVisible(), "Single-folder scope control should be hidden");
        auto *refreshButton = panel.findChild<QToolButton *>(QStringLiteral("refreshButton"));
        auto *settingsButton = panel.findChild<QToolButton *>(QStringLiteral("settingsButton"));
        auto *favoritesButton = panel.findChild<QToolButton *>(QStringLiteral("favoritesButton"));
        require(favoritesButton && favoritesButton->isCheckable() && !favoritesButton->isChecked(), "Favorites toolbar toggle missing");
        const auto hasFavoriteCount = [&](int count) {
            return favoritesButton->toolTip() == obs_seeker::uiText("Favorite.Count").arg(count)
                + QLatin1Char('\n') + obs_seeker::uiText("Favorite.Session");
        };
        require(hasFavoriteCount(0), "Initial favorite count is incorrect");
        require(refreshButton && settingsButton && !refreshButton->toolTip().isEmpty() && !settingsButton->toolTip().isEmpty(), "Compact toolbar actions missing");
        waitFor([&]() { return iconUsesColor(refreshButton->icon(), refreshButton->palette().color(QPalette::Active, QPalette::ButtonText))
            && iconUsesColor(settingsButton->icon(), settingsButton->palette().color(QPalette::Active, QPalette::ButtonText)); });
        refreshButton->setStyleSheet(QStringLiteral("QToolButton { color: #202020; }"));
        settingsButton->setStyleSheet(QStringLiteral("QToolButton { color: #202020; }"));
        waitFor([&]() { return iconUsesColor(refreshButton->icon(), QColor(32, 32, 32)) && iconUsesColor(settingsButton->icon(), QColor(32, 32, 32)); });
        refreshButton->setStyleSheet(QString());
        settingsButton->setStyleSheet(QString());
        auto *themeHistoryAction = input->findChild<QAction *>(QStringLiteral("searchHistoryAction"));
        require(themeHistoryAction, "History arrow missing");
        waitFor([&]() { return iconUsesColor(themeHistoryAction->icon(), input->palette().color(QPalette::Active, QPalette::Text)); });
        const auto originalInputStyle = input->styleSheet();
        input->setStyleSheet(QStringLiteral("QLineEdit { background: #f5f5f5; color: #202020; }"));
        waitFor([&]() { return iconUsesColor(themeHistoryAction->icon(), QColor(32, 32, 32)); });
        input->setStyleSheet(originalInputStyle);
        waitFor([&]() { return iconUsesColor(themeHistoryAction->icon(), input->palette().color(QPalette::Active, QPalette::Text)); });
        const auto hasTotal = [&](int total) {
            const auto *filter = static_cast<obs_seeker::NameFilter *>(table->model());
            const auto expected = filter->tokens().isEmpty()
                ? obs_seeker::uiText(favoritesButton->isChecked() ? "Status.Favorites" : "Status.Total").arg(total)
                : obs_seeker::uiText("Status.Count").arg(table->model()->rowCount()).arg(total);
            return (status->text() + status->toolTip()).contains(expected);
        };
        const auto scanIsIdle = [&]() {
            const auto text = status->text() + status->toolTip();
            return !text.contains(obs_seeker::uiText("Status.ScanningInitial"))
                && !text.contains(obs_seeker::uiText("Status.RescanPending"))
                && !text.contains(obs_seeker::uiText("Status.ScanningSuffix"));
        };
        waitFor([&]() { return table->model()->rowCount() == 3; });
        require(status->text() == obs_seeker::uiText("Status.Total").arg(3), "Empty search repeats the file count");
        const auto scanStamp = refreshButton->toolTip().section(QLatin1Char('\n'), 1).mid(obs_seeker::uiText("Action.LastScan").indexOf(QStringLiteral("%1")));
        require(QDateTime::fromString(scanStamp, QStringLiteral("yyyy-MM-dd HH:mm:ss")).isValid(), "Completed scan time missing from Refresh");
        require(!emptyResults->isVisible(), "Empty-results hint covers files");
        input->setText(QStringLiteral("__no_match__"));
        waitFor([&]() { return table->model()->rowCount() == 0 && scanIsIdle(); });
        require(emptyResults->isVisible() && emptyResults->text() == obs_seeker::uiText("Empty.Query")
            && hasTotal(3), "Search hint changed the total or stayed hidden");
        input->clear();
        waitFor([&]() { return table->model()->rowCount() == 3; });
        require(!emptyResults->isVisible(), "Empty-results hint remained after clearing the search");
        require(table->model()->columnCount() == 3, "Favorites added an extra result column");
        require(scope->itemText(0) == obs_seeker::uiText("Scope.All"), "All scope label incorrect");
        auto numberedIndex = table->model()->index(0, 0);
        require(numberedIndex.data(obs_seeker::FileModel::PathRole).toString() == numbered, "Favorite fixture selected the wrong file");
        QFile configBefore(config);
        require(configBefore.open(QIODevice::ReadOnly), "Cannot inspect initial favorite settings");
        const auto unchangedConfig = configBefore.readAll();
        configBefore.close();
        clickStar(table, numberedIndex);
        require(numberedIndex.data(obs_seeker::FileModel::FavoriteRole).toBool(), "Clicking the star did not add a favorite");
        require(hasFavoriteCount(1), "Favorite tooltip missed an addition");
        auto *noticeTimer = panel.findChild<QTimer *>(QStringLiteral("statusNoticeTimer"));
        require(noticeTimer && noticeTimer->isActive() && noticeTimer->interval() == 2000, "Favorite notice timer missing");
        require(status->text() == obs_seeker::uiText("Status.FavoriteAdded").arg(numberedIndex.data().toString())
            && status->textFormat() == Qt::PlainText && hasTotal(3), "Favorite notice or count tooltip incorrect");
        noticeTimer->setInterval(30);
        waitFor([&]() { return !noticeTimer->isActive(); });
        require(status->text() == obs_seeker::uiText("Status.Total").arg(3), "Favorite notice did not restore the count");
        noticeTimer->setInterval(2000);
        require(numberedIndex.data().toString() == QStringLiteral("010.奇异恩典.pptx") && recorder.openRequests == 0, "Favorite click changed the filename or opened it");
        const auto starPoint = QPoint(table->visualRect(numberedIndex).left() + 10, table->visualRect(numberedIndex).center().y());
        sendMouse(table->viewport(), QEvent::MouseButtonDblClick, starPoint);
        sendMouse(table->viewport(), QEvent::MouseButtonRelease, starPoint);
        require(recorder.openRequests == 0 && numberedIndex.data(obs_seeker::FileModel::FavoriteRole).toBool(), "Double-clicking the star opened a file or toggled twice");
        panel.refresh();
        waitFor(scanIsIdle);
        require(table->model()->index(0, 0).data(obs_seeker::FileModel::FavoriteRole).toBool(), "Disk refresh lost the favorite");
        require(hasFavoriteCount(1), "Refresh changed the favorite tooltip count");
        panel.toggleFavoritesView();
        require(table->model()->rowCount() == 1 && hasTotal(1), "Favorites-only button did not filter results");
        require(status->text() == obs_seeker::uiText("Status.Favorites").arg(1), "Empty favorites view repeats the count");
        require(favoritesButton->isChecked() && panel.focusWidget() == input, "Favorites view shortcut did not focus search");
        input->setText(QStringLiteral("赞美"));
        waitFor([&]() { return table->model()->rowCount() == 0; });
        require(hasTotal(1), "Query changed the favorites total");
        require(hasFavoriteCount(1), "Search changed the global favorite count");
        require(emptyResults->isVisible() && emptyResults->text() == obs_seeker::uiText("Empty.Favorites"),
                "Favorite-only results did not explain the active filter");
        input->clear();
        waitFor([&]() { return table->model()->rowCount() == 1; });
        clickStar(table, table->model()->index(0, 0));
        require(table->model()->rowCount() == 0 && !table->currentIndex().isValid(), "Removing the last favorite left a stale selection");
        require(hasFavoriteCount(0), "Favorite tooltip missed a removal");
        require(status->text() == obs_seeker::uiText("Status.FavoriteRemoved").arg(QStringLiteral("010.奇异恩典.pptx"))
            && hasTotal(0), "Removing the last favorite lost its notice or updated count");
        QKeyEvent favoriteEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(table, &favoriteEnter);
        require(recorder.openRequests == 0, "Enter opened a file after removing a favorite");
        panel.toggleFavoritesView();
        require(table->model()->rowCount() == 3, "Leaving favorites hid normal results");
        numberedIndex = table->model()->index(0, 0);
        sendMouse(table->viewport(), QEvent::MouseButtonPress, starPoint);
        input->setText(QStringLiteral("赞美"));
        waitFor([&]() { return table->model()->rowCount() == 1; });
        sendMouse(table->viewport(), QEvent::MouseButtonRelease, starPoint);
        require(!table->model()->index(0, 0).data(obs_seeker::FileModel::FavoriteRole).toBool(), "Changed results toggled the wrong favorite");
        input->clear();
        waitFor([&]() { return table->model()->rowCount() == 3; });
        clickStar(table, table->model()->index(0, 0));
        require(configBefore.open(QIODevice::ReadOnly), "Cannot inspect favorite persistence");
        require(configBefore.readAll() == unchangedConfig, "Favorites were written to disk");
        configBefore.close();
        panel.focusSearch();
        waitFor([&]() { return panel.window()->isActiveWindow() && panel.focusWidget() == input; });
        input->setText(QStringLiteral("赞美"));
        panel.toggleSelectedFavorite();
        require(table->model()->rowCount() == 1 && table->currentIndex().data(obs_seeker::FileModel::FavoriteRole).toBool(),
                "Favorite shortcut used a stale query or selection");
        require(recorder.openRequests == 0, "Favorite shortcut opened a file");
        panel.toggleSelectedFavorite();
        require(!table->currentIndex().data(obs_seeker::FileModel::FavoriteRole).toBool(), "Favorite shortcut did not remove a favorite");
        input->clear();
        waitFor([&]() { return table->model()->rowCount() == 3; });
        require(table->model()->index(0, 0).data(obs_seeker::FileModel::FavoriteRole).toBool(), "Favorite shortcut changed an unrelated file");
        const auto favoriteForPath = [&](const QString &path) {
            for (int row = 0; row < table->model()->rowCount(); ++row) {
                const auto index = table->model()->index(row, 0);
                if (index.data(obs_seeker::FileModel::PathRole).toString() == path)
                    return index.data(obs_seeker::FileModel::FavoriteRole).toBool();
            }
            throw std::runtime_error("Favorite fixture disappeared");
        };
        const auto sendFavoriteKey = [&](QWidget *target, int key, Qt::KeyboardModifiers modifiers, bool repeat = false) {
            const auto text = key == Qt::Key_Asterisk || (key == Qt::Key_8 && modifiers.testFlag(Qt::ShiftModifier))
                ? QStringLiteral("*") : QString();
            QKeyEvent overrideKey(QEvent::ShortcutOverride, key, modifiers, text);
            QCoreApplication::sendEvent(target, &overrideKey);
            QKeyEvent press(QEvent::KeyPress, key, modifiers, text, repeat);
            QCoreApplication::sendEvent(target, &press);
            QKeyEvent release(QEvent::KeyRelease, key, modifiers);
            QCoreApplication::sendEvent(target, &release);
        };
        input->setText(QStringLiteral("010"));
        sendFavoriteKey(input, Qt::Key_Asterisk, Qt::KeypadModifier);
        require(input->text().isEmpty() && table->model()->rowCount() == 3 && !favoriteForPath(numbered),
                "Input star did not toggle the pending-query favorite and clear search");
        sendFavoriteKey(input, Qt::Key_Asterisk, Qt::KeypadModifier, true);
        require(!favoriteForPath(numbered), "Held star toggled a favorite again");
        input->setText(QStringLiteral("010"));
        sendFavoriteKey(input, Qt::Key_8, Qt::ShiftModifier);
        require(input->text().isEmpty() && favoriteForPath(numbered), "Typed star did not restore the favorite");
        input->setText(QStringLiteral("010"));
        QKeyEvent rawShiftOverride(QEvent::ShortcutOverride, Qt::Key_8, Qt::ShiftModifier);
        QKeyEvent rawShiftPress(QEvent::KeyPress, Qt::Key_8, Qt::ShiftModifier);
        QCoreApplication::sendEvent(input, &rawShiftOverride);
        QCoreApplication::sendEvent(input, &rawShiftPress);
        require(input->text() == QStringLiteral("010") && favoriteForPath(numbered),
                "Shift+8 acted before an asterisk was entered");
        QInputMethodEvent preedit(QStringLiteral("*"), {});
        QCoreApplication::sendEvent(input, &preedit);
        require(input->text() == QStringLiteral("010") && favoriteForPath(numbered), "Uncommitted composition changed a favorite");
        QInputMethodEvent commitStar;
        commitStar.setCommitString(QStringLiteral("*"));
        QCoreApplication::sendEvent(input, &commitStar);
        waitFor([&]() { return input->text().isEmpty() && !favoriteForPath(numbered); });
        require(table->currentIndex().data(obs_seeker::FileModel::PathRole).toString() == numbered,
                "Input-method star did not preserve the favorited selection");
        input->setText(QStringLiteral("replace-this"));
        input->selectAll();
        QInputMethodEvent commitQuery;
        commitQuery.setCommitString(QStringLiteral("010*"));
        QCoreApplication::sendEvent(input, &commitQuery);
        waitFor([&]() { return input->text().isEmpty() && favoriteForPath(numbered); });
        require(table->currentIndex().data(obs_seeker::FileModel::PathRole).toString() == numbered,
                "Input-method query and star selected the wrong file");
        QApplication::clipboard()->setText(QStringLiteral("010*"));
        input->paste();
        waitFor([&]() { return input->text().isEmpty() && !favoriteForPath(numbered); });
        QApplication::clipboard()->setText(QStringLiteral("010**"));
        input->paste();
        waitFor([&]() { return input->text().isEmpty() && favoriteForPath(numbered); });
        auto *queryTimer = panel.findChildren<QTimer *>().first();
        for (auto *timer : panel.findChildren<QTimer *>()) {
            if (timer->interval() == 120)
                queryTimer = timer;
        }
        require(!queryTimer->isActive(), "Star left a delayed query that could reset selection");
        const auto favoriteTooltipBeforeNoMatch = favoritesButton->toolTip();
        input->setText(QStringLiteral("no-matching-file"));
        QCoreApplication::sendEvent(input, &commitStar);
        waitFor([&]() { return input->text() == QStringLiteral("no-matching-file") && table->model()->rowCount() == 0; });
        require(favoritesButton->toolTip() == favoriteTooltipBeforeNoMatch, "Unmatched input-method star changed favorites");
        input->setText(QStringLiteral("no-matching-file"));
        sendFavoriteKey(input, Qt::Key_Asterisk, Qt::KeypadModifier);
        require(input->text() == QStringLiteral("no-matching-file") && table->model()->rowCount() == 0,
                "Star changed an unmatched query or inserted text");
        input->clear();
        waitFor([&]() { return table->model()->rowCount() == 3; });
        table->clearSelection();
        sendFavoriteKey(input, Qt::Key_Asterisk, Qt::KeypadModifier);
        require(favoriteForPath(numbered), "Star changed a file without selection");
        panel.setFavoriteShortcutMatcher([](const QInputEvent &event) {
            return event.type() != QEvent::MouseButtonPress && static_cast<const QKeyEvent &>(event).key() == Qt::Key_F6;
        });
        input->setText(QStringLiteral("010"));
        sendFavoriteKey(input, Qt::Key_F6, Qt::NoModifier);
        require(input->text().isEmpty() && !favoriteForPath(numbered), "Custom input shortcut did not toggle and clear");
        panel.setFavoriteShortcutMatcher([](const QInputEvent &) { return false; });
        input->setText(QStringLiteral("010"));
        sendFavoriteKey(input, Qt::Key_F6, Qt::NoModifier);
        require(input->text() == QStringLiteral("010"), "Cleared shortcut still acted on input");
        sendFavoriteKey(input, Qt::Key_Asterisk, Qt::ShiftModifier);
        require(input->text().isEmpty() && favoriteForPath(numbered), "Filename-star command stopped after shortcut customization");
        panel.setFavoriteShortcutMatcher({});
        for (auto *target : {static_cast<QWidget *>(input), static_cast<QWidget *>(table),
                             static_cast<QWidget *>(favoritesButton), static_cast<QWidget *>(refreshButton),
                             static_cast<QWidget *>(settingsButton)}) {
            table->setCurrentIndex(table->model()->index(0, 0));
            const auto path = table->currentIndex().data(obs_seeker::FileModel::PathRole).toString();
            target->setFocus(Qt::ShortcutFocusReason);
            waitFor([&]() { return panel.focusWidget() == target; });
            sendFavoriteKey(target, Qt::Key_Asterisk, Qt::KeypadModifier);
            require(table->currentIndex().data(obs_seeker::FileModel::PathRole).toString() == path
                    && !table->currentIndex().data(obs_seeker::FileModel::FavoriteRole).toBool(), "Panel-wide shortcut did not remove a favorite");
            sendFavoriteKey(target, Qt::Key_Asterisk, Qt::KeypadModifier);
            require(table->currentIndex().data(obs_seeker::FileModel::PathRole).toString() == path
                    && table->currentIndex().data(obs_seeker::FileModel::FavoriteRole).toBool(), "Panel-wide shortcut did not restore a favorite");
            if (target != input) {
                sendFavoriteKey(target, Qt::Key_8, Qt::ShiftModifier);
                require(table->currentIndex().data(obs_seeker::FileModel::FavoriteRole).toBool(), "Shift+8 became a panel shortcut");
            }
        }
        const QPersistentModelIndex unselected = table->currentIndex();
        table->clearSelection();
        panel.toggleSelectedFavorite();
        require(unselected.data(obs_seeker::FileModel::FavoriteRole).toBool(), "Favorite shortcut changed an unselected file");
        panel.refresh();
        waitFor(scanIsIdle);
        require(!table->selectionModel()->hasSelection() && !table->currentIndex().isValid(), "Refresh restored a cleared selection");
        table->setCurrentIndex(table->model()->index(0, 0));
        obs_seeker::ShortcutSummary shortcutSummary{QStringLiteral("Pause / Ctrl+<"), {}, QStringLiteral("*"), QStringLiteral("Ctrl+F2")};
        int summaryReads = 0;
        panel.setShortcutSummaryProvider([&]() { ++summaryReads; return shortcutSummary; });
        auto *summaryDialog = openSettings(panel);
        require(summaryReads == 1 && summaryDialog->findChild<QLabel *>(QStringLiteral("searchShortcutValue"))->text() == shortcutSummary.search
            && summaryDialog->findChild<QLabel *>(QStringLiteral("refreshShortcutValue"))->text() == obs_seeker::uiText("Settings.ShortcutUnset")
            && summaryDialog->findChild<QLabel *>(QStringLiteral("favoriteShortcutValue"))->text() == shortcutSummary.favorite
            && summaryDialog->findChild<QLabel *>(QStringLiteral("favoritesViewShortcutValue"))->text() == shortcutSummary.favoritesView,
                "Settings shortcut summary did not use current bindings");
        require(summaryDialog->findChild<QLabel *>(QStringLiteral("searchShortcutValue"))->toolTip().contains(QStringLiteral("&lt;")),
                "Shortcut tooltip treated a key name as markup");
        summaryDialog->reject();
        pumpEvents();
        shortcutSummary.search = QStringLiteral("F13");
        shortcutSummary.favorite.clear();
        summaryDialog = openSettings(panel);
        require(summaryReads == 2 && summaryDialog->findChild<QLabel *>(QStringLiteral("searchShortcutValue"))->text() == shortcutSummary.search
            && summaryDialog->findChild<QLabel *>(QStringLiteral("favoriteShortcutValue"))->text() == obs_seeker::uiText("Settings.ShortcutUnset"),
                "Reopening Settings showed stale shortcut bindings");
        summaryDialog->reject();
        pumpEvents();
        panel.setShortcutSummaryProvider({});
        auto *blockedDialog = openSettings(panel);
        auto *dropList = blockedDialog->findChild<QListWidget *>(QStringLiteral("folderList"));
        const auto dropOnly = temp.path() + QStringLiteral("/拖入 文件夹#一");
        require(QDir().mkpath(dropOnly), "Cannot create folder-drop fixture");
        require(dropFolders(dropList, {QUrl::fromLocalFile(first), QUrl::fromLocalFile(first + QStringLiteral("/.")),
            QUrl::fromLocalFile(second), QUrl::fromLocalFile(numbered), QUrl(QStringLiteral("https://example.com/folder"))})
            && dropList->count() == 2, "Batch folder drop did not deduplicate or rejected valid folders");
        require(dropFolders(dropList, {QUrl::fromLocalFile(dropOnly)}) && dropList->count() == 3
            && dropList->item(2)->text() == QDir::toNativeSeparators(dropOnly), "Single Unicode folder drop failed");
        const auto beforeFolderOpen = recorder.opened;
        const auto beforeFolderRequests = recorder.openRequests;
        const auto folderPoint = dropList->visualItemRect(dropList->item(2)).center();
        sendMouse(dropList->viewport(), QEvent::MouseButtonPress, folderPoint);
        sendMouse(dropList->viewport(), QEvent::MouseButtonRelease, folderPoint);
        require(recorder.openRequests == beforeFolderRequests, "Single click opened a folder");
        sendMouse(dropList->viewport(), QEvent::MouseButtonDblClick, folderPoint);
        sendMouse(dropList->viewport(), QEvent::MouseButtonRelease, folderPoint);
        require(recorder.openRequests == beforeFolderRequests + 1 && recorder.opened.toLocalFile() == dropOnly
            && dropList->count() == 3, "Folder double-click opened the wrong path or changed the list");
        require(configBefore.open(QIODevice::ReadOnly), "Cannot inspect settings after opening a folder");
        require(configBefore.readAll() == unchangedConfig, "Opening a folder saved pending settings");
        configBefore.close();
        recorder.opened = beforeFolderOpen;
        recorder.openRequests = beforeFolderRequests;
        require(!dropFolders(dropList, {QUrl::fromLocalFile(numbered), QUrl(QStringLiteral("https://example.com/folder"))})
            && dropList->count() == 3 && QFileInfo(dropOnly).isDir() && QFileInfo(second).isDir(), "Invalid drop changed folders or moved a source");
        auto *dropTitle = blockedDialog->findChild<QLabel *>(QStringLiteral("folderCountLabel"));
        require(dropTitle->text() == obs_seeker::uiText("Settings.Folders").arg(3), "Folder count missed a batch drop");
        dropList->clearSelection();
        dropList->item(0)->setSelected(true);
        dropList->item(1)->setSelected(true);
        blockedDialog->findChild<QPushButton *>(QStringLiteral("removeFolderButton"))->click();
        require(dropList->count() == 1 && dropTitle->text() == obs_seeker::uiText("Settings.Folders").arg(1), "Folder count missed multiple removals");
        dropList->clear();
        require(dropTitle->text() == obs_seeker::uiText("Settings.Folders").arg(0), "Folder count missed a list reset");
        const auto fixedSettingsHeight = blockedDialog->height();
        for (int row = 0; row < 12; ++row)
            dropList->addItem(first + QStringLiteral("/folder-%1").arg(row));
        pumpEvents();
        dropList->scrollToTop();
        require(blockedDialog->height() == fixedSettingsHeight, "Folder changes resized Settings");
        const auto seventhRow = dropList->visualItemRect(dropList->item(6));
        require(seventhRow.top() >= 0 && seventhRow.bottom() < dropList->viewport()->height(),
                "Default Settings height does not show seven complete folder rows");
        QElapsedTimer reorderTimer;
        reorderTimer.start();
        for (int move = 0; move < 100; ++move)
            require(dropList->model()->moveRows({}, 0, 1, {}, dropList->count()), "Repeated folder reorder failed");
        require(dropList->count() == 12, "Repeated folder reorder changed the folder count");
        std::cout << "PERF: reorder 12 folders, 100 moves " << reorderTimer.elapsed() << " ms\n";
        panel.toggleSelectedFavorite();
        panel.toggleFavoritesView();
        require(table->model()->index(0, 0).data(obs_seeker::FileModel::FavoriteRole).toBool() && !favoritesButton->isChecked(),
                "Favorites shortcuts changed state while Settings was open");
        blockedDialog->reject();
        pumpEvents();
        auto *favoriteDialog = openSettings(panel);
        auto *priorityCheck = favoriteDialog->findChild<QCheckBox *>(QStringLiteral("favoritesFirstCheck"));
        require(priorityCheck && priorityCheck->isChecked(), "Favorites priority did not default to enabled");
        priorityCheck->setChecked(false);
        saveDialog(favoriteDialog);
        waitFor([&]() { return table->model()->rowCount() == 3 && scanIsIdle(); });
        require(!obs_seeker::loadSettings(config).favoritesFirst, "Disabling favorites priority was not saved");
        clickStar(table, table->model()->index(0, 0));
        const auto lastIndex = table->model()->index(2, 0);
        const auto lastPath = lastIndex.data(obs_seeker::FileModel::PathRole).toString();
        clickStar(table, lastIndex);
        require(table->model()->index(2, 0).data(obs_seeker::FileModel::PathRole).toString() == lastPath,
                "Disabled priority still moved a favorite");
        favoriteDialog = openSettings(panel);
        priorityCheck = favoriteDialog->findChild<QCheckBox *>(QStringLiteral("favoritesFirstCheck"));
        require(priorityCheck && !priorityCheck->isChecked(), "Settings lost the disabled favorites priority");
        priorityCheck->setChecked(true);
        saveDialog(favoriteDialog);
        waitFor([&]() { return table->model()->rowCount() == 3 && scanIsIdle(); });
        require(obs_seeker::loadSettings(config).favoritesFirst
                && table->model()->index(0, 0).data(obs_seeker::FileModel::PathRole).toString() == lastPath,
                "Enabling priority did not move the favorite to the front");
        clickStar(table, table->model()->index(0, 0));
        clickStar(table, table->model()->index(0, 0));
        require(table->model()->headerData(1, Qt::Horizontal).toString() == obs_seeker::uiText("Column.Location"), "Location header incorrect");
        table->setCurrentIndex(table->model()->index(1, 0));
        const auto resizeSelection = table->currentIndex().data(Qt::ToolTipRole).toString();
        table->setColumnWidth(0, 270);
        const auto originalNameWidth = table->columnWidth(0);
        panel.resize(440, 430);
        waitFor([&]() { return table->isColumnHidden(2) && !table->isColumnHidden(1); });
        require(!table->isColumnHidden(0), "Medium panel hid filenames");
        panel.resize(340, 430);
        waitFor([&]() { return table->isColumnHidden(1) && table->isColumnHidden(2)
            && table->columnWidth(0) >= table->viewport()->width() - 2; });
        require(!table->isColumnHidden(0), "Narrow panel hid filenames");
        require(table->model()->rowCount() == 3 && table->currentIndex().data(Qt::ToolTipRole).toString() == resizeSelection, "Narrow layout changed results or selection");
        panel.resize(440, 430);
        waitFor([&]() { return !table->isColumnHidden(1) && table->isColumnHidden(2) && table->columnWidth(0) == originalNameWidth; });
        panel.resize(920, 430);
        waitFor([&]() { return !table->isColumnHidden(1) && !table->isColumnHidden(2) && table->columnWidth(0) == originalNameWidth; });
        require(table->currentIndex().data(Qt::ToolTipRole).toString() == resizeSelection, "Restoring columns changed the selection");
        require(panel.findChildren<QShortcut *>().isEmpty(), "Shortcuts must be configured through OBS");
        input->setText(QStringLiteral("恩典"));
        waitFor([&]() { return table->model()->rowCount() == 2; });
        require(hasTotal(3), "Result count incorrect");

        // 列表回车也必须先应用待处理的编号搜索。
        input->setText(QStringLiteral("010"));
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QCoreApplication::sendEvent(input, &down);
        require(table->model()->rowCount() == 1 && panel.focusWidget() == table, "Down did not enter filtered results");
        QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QCoreApplication::sendEvent(table, &up);
        require(panel.focusWidget() == input && input->selectedText() == input->text(), "Up did not return to search");
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(table, &enter);
        require(recorder.opened.toLocalFile() == numbered, "Enter opened a stale result");
        require(recorder.openRequests == 1, "First open request missing");
        QCoreApplication::sendEvent(table, &enter);
        require(recorder.openRequests == 1, "Repeated Enter reopened the same file");
        QKeyEvent heldEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QString(), true);
        QCoreApplication::sendEvent(table, &heldEnter);
        require(recorder.openRequests == 1, "Held Enter reopened the same file");
        input->setText(QStringLiteral("赞美之泉"));
        QCoreApplication::sendEvent(table, &enter);
        require(recorder.openRequests == 2 && recorder.opened.toLocalFile().endsWith(QStringLiteral("赞美之泉.ppsx")), "Duplicate-open guard blocked a different file");
        input->setText(QStringLiteral("010"));
        QCoreApplication::sendEvent(input, &down);
        const auto clickedIndex = table->model()->index(0, 0);
        require(QDir::fromNativeSeparators(clickedIndex.data(Qt::ToolTipRole).toString()) == numbered, "Double-click fixture did not select the numbered file");
        input->setText(QStringLiteral("赞美之泉"));
        require(QMetaObject::invokeMethod(table, "doubleClicked", Qt::DirectConnection, Q_ARG(QModelIndex, clickedIndex)), "Double-click signal failed");
        require(recorder.opened.toLocalFile() == numbered && recorder.openRequests == 3, "Pending query changed the double-click target");
        HostKeyFilter enterHostFilter;
        input->installEventFilter(&enterHostFilter);
        table->installEventFilter(&enterHostFilter);
        bool nextNumbered = false;
        auto expectedOpens = recorder.openRequests;
        for (auto *target : {static_cast<QWidget *>(input), static_cast<QWidget *>(table)}) {
            for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
                const auto modifiers = key == Qt::Key_Enter ? Qt::KeypadModifier : Qt::NoModifier;
                input->setText(nextNumbered ? QStringLiteral("010") : QStringLiteral("赞美之泉"));
                QKeyEvent overrideEnter(QEvent::ShortcutOverride, key, modifiers);
                overrideEnter.ignore();
                QCoreApplication::sendEvent(target, &overrideEnter);
                require(overrideEnter.isAccepted() && recorder.openRequests == expectedOpens,
                        "Enter shortcut override opened a file or was not claimed");
                QKeyEvent press(QEvent::KeyPress, key, modifiers);
                QCoreApplication::sendEvent(target, &press);
                ++expectedOpens;
                require(recorder.openRequests == expectedOpens
                    && (nextNumbered ? recorder.opened.toLocalFile() == numbered
                                     : recorder.opened.toLocalFile().endsWith(QStringLiteral("赞美之泉.ppsx"))),
                        "Enter did not open the latest query in input or results");
                nextNumbered = !nextNumbered;
                input->setText(nextNumbered ? QStringLiteral("010") : QStringLiteral("赞美之泉"));
                QKeyEvent held(QEvent::KeyPress, key, modifiers, QString(), true);
                QCoreApplication::sendEvent(target, &held);
                require(recorder.openRequests == expectedOpens, "Held Enter opened another file");
            }
        }
        require(enterHostFilter.swallowedEnters == 0, "Host key filter swallowed a panel Enter key");
        input->removeEventFilter(&enterHostFilter);
        table->removeEventFilter(&enterHostFilter);
        input->clear();
        waitFor([&]() { return table->model()->rowCount() == 3; });
        table->clearSelection();
        QKeyEvent keypadEnter(QEvent::KeyPress, Qt::Key_Enter, Qt::KeypadModifier);
        QCoreApplication::sendEvent(table, &keypadEnter);
        require(recorder.openRequests == expectedOpens, "Keypad Enter opened an unselected file");
        input->setText(QStringLiteral("no-matching-file"));
        QCoreApplication::sendEvent(input, &keypadEnter);
        require(table->model()->rowCount() == 0 && recorder.openRequests == expectedOpens,
                "Keypad Enter opened a file without matching results");
        const auto historyProbe = QStringLiteral("本次会话独有历史词");
        input->setText(historyProbe);
        // 模拟 OBS 后安装的控件键盘过滤器，Esc 必须在应用层先处理。
        HostKeyFilter hostFilter;
        input->installEventFilter(&hostFilter);
        QKeyEvent overrideEscape(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
        overrideEscape.ignore();
        QCoreApplication::sendEvent(input, &overrideEscape);
        require(overrideEscape.isAccepted(), "Escape shortcut override was not claimed by the search panel");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(input, &escape);
        require(input->text().isEmpty() && table->model()->rowCount() == 3, "Esc did not clear the query immediately");
        auto *historyAction = input->findChild<QAction *>(QStringLiteral("searchHistoryAction"));
        require(historyAction, "Search history control missing");
        historyAction->trigger();
        QPointer<QMenu> historyMenu = panel.findChild<QMenu *>(QStringLiteral("searchHistoryMenu"));
        require(historyMenu && historyMenu->actions().first()->text() == historyProbe, "Esc lost the latest history");
        const auto historyCount = historyMenu->actions().size();
        historyMenu->actions().first()->trigger();
        require(input->text() == historyProbe && table->model()->rowCount() == 0, "History recall did not search");
        historyMenu->close();
        waitFor([&]() { return historyMenu.isNull(); });
        QCoreApplication::sendEvent(table, &escape);
        require(input->text().isEmpty() && table->model()->rowCount() == 3, "Esc from results did not clear search");
        historyAction->trigger();
        historyMenu = panel.findChild<QMenu *>(QStringLiteral("searchHistoryMenu"));
        require(historyMenu && historyMenu->actions().size() == historyCount, "Duplicate history was added");
        QCoreApplication::sendEvent(historyMenu, &escape);
        waitFor([&]() { return historyMenu.isNull(); });
        for (auto *target : {static_cast<QWidget *>(table->viewport()),
                             static_cast<QWidget *>(favoritesButton),
                             static_cast<QWidget *>(refreshButton),
                             static_cast<QWidget *>(settingsButton),
                             static_cast<QWidget *>(&panel)}) {
            require(target, "Escape target missing");
            target->installEventFilter(&hostFilter);
            input->setText(QStringLiteral("按钮历史词"));
            target->setFocus();
            QCoreApplication::sendEvent(target, &escape);
            require(input->text().isEmpty() && table->model()->rowCount() == 3, "Escape from panel controls did not clear search");
            require(panel.focusWidget() == input, "Escape did not return focus to search input");
        }
        require(hostFilter.swallowedEscapes == 0, "A host keyboard filter swallowed search-panel Escape");
        QLineEdit unrelated;
        input->setText(historyProbe);
        QCoreApplication::sendEvent(&unrelated, &escape);
        require(input->text() == historyProbe, "Escape outside the search panel cleared its query");
        QPointer<QDialog> escapeDialog = openSettings(panel);
        QCoreApplication::sendEvent(escapeDialog, &escape);
        waitFor([&]() { return escapeDialog.isNull(); });
        require(input->text() == historyProbe, "Settings Escape cleared the search query");
        historyAction->trigger();
        historyMenu = panel.findChild<QMenu *>(QStringLiteral("searchHistoryMenu"));
        auto *clearHistory = historyMenu->findChild<QAction *>(QStringLiteral("clearHistoryAction"));
        require(clearHistory && clearHistory->isEnabled(), "Clear history action missing");
        input->setText(historyProbe + QStringLiteral("+"));
        input->setText(historyProbe);
        auto *historyTimer = panel.findChild<QTimer *>(QStringLiteral("historyTimer"));
        require(historyTimer && historyTimer->isActive(), "Pending history timer fixture failed");
        const auto resultsBeforeClear = table->model()->rowCount();
        clearHistory->trigger();
        waitFor([&]() { return historyMenu.isNull(); });
        require(input->text() == historyProbe && table->model()->rowCount() == resultsBeforeClear
            && !historyTimer->isActive(), "Clearing history changed the active search or left a pending record");
        historyAction->trigger();
        historyMenu = panel.findChild<QMenu *>(QStringLiteral("searchHistoryMenu"));
        require(historyMenu->actions().size() == 1 && !historyMenu->actions().first()->isEnabled(), "Current input reappeared after clearing history");
        historyMenu->close();
        waitFor([&]() { return historyMenu.isNull(); });
        input->setText(historyProbe + QStringLiteral("新输入"));
        historyAction->trigger();
        historyMenu = panel.findChild<QMenu *>(QStringLiteral("searchHistoryMenu"));
        require(historyMenu->actions().first()->text() == input->text(), "New input stopped being recorded after history clear");
        historyMenu->close();
        waitFor([&]() { return historyMenu.isNull(); });
        input->clear();
        QFile savedConfig(config);
        require(savedConfig.open(QIODevice::ReadOnly), "Cannot inspect history persistence");
        require(!savedConfig.readAll().contains(historyProbe.toUtf8()), "History was written to settings");
        savedConfig.close();
        QCoreApplication::sendEvent(input, &escape);
        waitFor([&]() { return table->model()->rowCount() == 3; });

        const auto added = nested + QStringLiteral("/新的恩典.pptx");
        table->setCurrentIndex(table->model()->index(1, 0));
        const auto preservedPath = table->currentIndex().data(Qt::ToolTipRole).toString();
        touch(added);
        waitFor([&]() { return table->model()->rowCount() == 4; });
        require(table->currentIndex().data(Qt::ToolTipRole).toString() == preservedPath, "Auto refresh changed the selection");
        input->setText(QStringLiteral("新的恩典"));
        waitFor([&]() { return table->model()->rowCount() == 1 && scanIsIdle(); });
        const auto deletedSelection = nested + QStringLiteral("/另一个新的恩典.pptx");
        touch(deletedSelection);
        waitFor([&]() { return table->model()->rowCount() == 2 && scanIsIdle(); });
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            const auto index = table->model()->index(row, 0);
            if (QDir::fromNativeSeparators(index.data(Qt::ToolTipRole).toString()) == deletedSelection)
                table->setCurrentIndex(index);
        }
        require(QDir::fromNativeSeparators(table->currentIndex().data(Qt::ToolTipRole).toString()) == deletedSelection, "Deleted-selection fixture failed");
        require(QFile::remove(deletedSelection), "Cannot remove selected fixture");
        waitFor([&]() { return table->model()->rowCount() == 1 && scanIsIdle(); });
        require(!table->currentIndex().isValid(), "Deleting the selected file picked another result");
        panel.refresh();
        waitFor(scanIsIdle);
        require(!table->currentIndex().isValid(), "Repeated refresh restored an unintended selection");
        const auto openCount = recorder.openRequests;
        QCoreApplication::sendEvent(table, &enter);
        require(recorder.openRequests == openCount, "Enter opened another file after deletion");
        input->clear();
        waitFor([&]() { return table->model()->rowCount() == 4; });
        require(QFile::rename(added, nested + QStringLiteral("/改名后的恩典.pptx")), "Rename fixture failed");
        input->setText(QStringLiteral("改名后"));
        waitFor([&]() { return table->model()->rowCount() == 1; });
        require(QFile::remove(nested + QStringLiteral("/改名后的恩典.pptx")), "Delete fixture failed");
        waitFor([&]() { return table->model()->rowCount() == 0 && hasTotal(3); });
        input->clear();
        waitFor([&]() { return table->model()->rowCount() == 3; });
        touch(first + QStringLiteral("/新的恩典.pptx"));
        panel.refresh();
        panel.refresh();
        waitFor([&]() { return table->model()->rowCount() == 4; });

        QPointer<QDialog> dialog = openSettings(panel);
        auto *pollInterval = dialog->findChild<QSpinBox *>(QStringLiteral("pollIntervalSpin"));
        require(pollInterval && pollInterval->value() == 30, "Default refresh interval missing from settings");
        pollInterval->setValue(15);
        require(dropFolders(dialog->findChild<QListWidget *>(QStringLiteral("folderList")), {QUrl::fromLocalFile(second)}), "Dropping a folder before Save failed");
        saveDialog(dialog);
        waitFor([&]() { return dialog.isNull() && table->model()->rowCount() == 5; });
        require(obs_seeker::loadSettings(config).folders.size() == 2, "Multiple-folder settings not persisted");
        require(obs_seeker::loadSettings(config).pollIntervalSeconds == 15, "Custom refresh interval not persisted");
        require(scopeRow->isVisible() && scope->count() == 3, "Multiple-folder scope choices missing");
        require(scope->itemText(scope->findData(first)) == obs_seeker::uiText("Scope.Count").arg(QDir::toNativeSeparators(first), QStringLiteral("4"))
            && scope->itemText(scope->findData(second)) == obs_seeker::uiText("Scope.Count").arg(QDir::toNativeSeparators(second), QStringLiteral("1")),
                "Location counts did not match the completed scan");
        const auto locationCountText = scope->itemText(scope->findData(first));
        for (const auto width : {300, 404, 680, 920, 1600}) {
            panel.resize(width, 200);
            auto *help = panel.findChild<QLabel *>(QStringLiteral("helpLabel"));
            waitFor([&]() { return help && help->isVisible() && panel.height() == 200; });
            require(help->text() == QStringLiteral(" | ") + obs_seeker::uiText("Panel.Help")
                    && !help->wordWrap() && !status->wordWrap()
                    && help->geometry().top() == status->geometry().top(), "Footer is not merged into one line");
            require(status->geometry().right() < help->geometry().left() && help->geometry().right() < panel.width(), "Compact footer overlaps or clips the shortcut hint");
            require(qAbs(scopeRow->geometry().center().y() - input->geometry().center().y()) <= 2, "Location selector occupies another toolbar row");
            require(input->width() >= 90 && input->geometry().right() < scopeRow->geometry().left()
                && scopeRow->geometry().right() < favoritesButton->geometry().left()
                && favoritesButton->geometry().right() < refreshButton->geometry().left()
                && refreshButton->geometry().right() < settingsButton->geometry().left()
                && settingsButton->geometry().right() < panel.width(), "Compact toolbar controls overlap or clip");
            if (width >= 680)
                require(scopeRow->width() > 180 && scopeRow->width() * 2 >= input->width(), "Wide toolbar leaves too little space for locations");
            if (width >= 920)
                require(input->width() <= 480, "Wide toolbar stretches the search input too far");
            require(table->model()->rowCount() == 5 && hasTotal(5), "Compact layout changed results");
        }
        panel.resize(920, 430);
        waitFor([&]() { return panel.findChild<QLabel *>(QStringLiteral("helpLabel"))->isVisible(); });
        const auto globalFavoriteTooltip = favoritesButton->toolTip();
        scope->setCurrentIndex(scope->findData(second));
        require(table->model()->rowCount() == 1 && hasTotal(1), "Second location did not narrow results and totals");
        require(favoritesButton->toolTip() == globalFavoriteTooltip, "Location changed the global favorite count");
        scope->setCurrentIndex(scope->findData(first));
        require(table->model()->rowCount() == 4 && hasTotal(4), "First location scope failed");
        input->setText(QStringLiteral("__no_match__"));
        waitFor([&]() { return table->model()->rowCount() == 0; });
        require(emptyResults->isVisible() && emptyResults->text() == obs_seeker::uiText("Empty.Location"),
                "Empty location did not suggest changing the location filter");
        require(scope->itemText(scope->findData(first)) == locationCountText, "Search input recomputed location totals");
        input->clear();
        waitFor([&]() { return table->model()->rowCount() == 4; });
        input->setText(QStringLiteral("数算"));
        scope->setCurrentIndex(scope->findData(second));
        require(table->model()->rowCount() == 1, "Scope switch did not apply the pending query");
        QCoreApplication::sendEvent(scope, &escape);
        require(input->text().isEmpty() && scope->currentData().toString() == second && table->model()->rowCount() == 1, "Escape reset the selected location");
        scope->setCurrentIndex(0);
        require(table->model()->rowCount() == 5 && hasTotal(5), "All locations did not restore the complete list");
        auto *pollTimer = panel.findChild<QTimer *>(QStringLiteral("pollTimer"));
        require(pollTimer && pollTimer->interval() == 15000 && !pollTimer->isActive(), "Fallback interval applied incorrectly with working watchers");

        // 菜单打开后筛选变化，复制的仍是原先点击的文件。
        table->setCurrentIndex(table->model()->index(0, 0));
        const auto expectedPath = table->currentIndex().data(Qt::ToolTipRole).toString();
        const auto point = table->visualRect(table->currentIndex()).center();
        require(QMetaObject::invokeMethod(table, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, point)), "Context menu signal failed");
        QPointer<QMenu> menu = panel.findChild<QMenu *>(QStringLiteral("fileContextMenu"));
        require(menu, "Context menu missing");
        const auto menuActions = menu->actions();
        require(menuActions.size() == 7 && menuActions.at(0)->text() == obs_seeker::uiText("Menu.Open")
            && menuActions.at(1)->text() == obs_seeker::uiText("Menu.Reveal") && menuActions.at(2)->isSeparator()
            && menuActions.at(3)->objectName() == QStringLiteral("toggleFavoriteAction") && menuActions.at(4)->isSeparator()
            && menuActions.at(5)->objectName() == QStringLiteral("copyNameAction")
            && menuActions.at(6)->objectName() == QStringLiteral("copyPathAction")
            && !menu->findChild<QAction *>(QStringLiteral("moveFavoriteTopAction"))
            && !menu->findChild<QAction *>(QStringLiteral("moveFavoriteUpAction"))
            && !menu->findChild<QAction *>(QStringLiteral("moveFavoriteDownAction"))
            && !menu->findChild<QAction *>(QStringLiteral("moveFavoriteBottomAction")), "Normal menu contains favorite ordering actions");
        input->setText(QStringLiteral("数算"));
        waitFor([&]() { return table->model()->rowCount() == 1; });
        menu->findChild<QAction *>(QStringLiteral("copyNameAction"))->trigger();
        require(QApplication::clipboard()->text() == QFileInfo(expectedPath).fileName(), "Copy filename used a changed selection");
        menu->findChild<QAction *>(QStringLiteral("copyPathAction"))->trigger();
        require(QApplication::clipboard()->text() == expectedPath, "Context menu used a changed selection");
        menu->close();
        waitFor([&]() { return menu.isNull(); });
        table->setFocus();
        const auto copiedName = table->currentIndex().data().toString();
        QKeyEvent copyOverride(QEvent::ShortcutOverride, Qt::Key_C, Qt::ControlModifier);
        QCoreApplication::sendEvent(table, &copyOverride);
        require(copyOverride.isAccepted(), "Copy shortcut was not claimed by the table");
        QKeyEvent copyKey(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
        QCoreApplication::sendEvent(table, &copyKey);
        require(QApplication::clipboard()->text() == copiedName, "Table copy shortcut did not copy the filename");
        table->clearSelection();
        QApplication::clipboard()->setText(QStringLiteral("clipboard unchanged"));
        QCoreApplication::sendEvent(table, &copyKey);
        require(QApplication::clipboard()->text() == QStringLiteral("clipboard unchanged"), "Copy changed the clipboard without selection");
        input->setFocus();
        input->selectAll();
        QCoreApplication::sendEvent(input, &copyKey);
        require(QApplication::clipboard()->text() == input->text(), "Table copy shortcut intercepted input text copying");
        panel.hide();
        panel.toggleFavoritesView();
        require(panel.isVisible() && favoritesButton->isChecked(), "Global favorites view did not show the panel");
        panel.toggleFavoritesView();
        require(!favoritesButton->isChecked(), "Favorites view did not toggle back");
        panel.focusSearch();
        require(panel.isVisible() && input->selectedText() == input->text(), "Search focus did not restore the panel");
        panel.setWindowState(panel.windowState() | Qt::WindowMinimized);
        panel.focusSearch();
        waitFor([&]() { return !panel.isMinimized() && panel.isVisible(); });


        dialog = openSettings(panel);
        dialog->findChild<QCheckBox *>(QStringLiteral("autoRefreshCheck"))->setChecked(false);
        require(!dialog->findChild<QSpinBox *>(QStringLiteral("pollIntervalSpin"))->isEnabled(), "Disabled refresh interval remained editable");
        require(!dialog->findChild<QLabel *>(QStringLiteral("pollHintLabel"))->isEnabled(), "Disabled fallback hint remained active");
        saveDialog(dialog);
        waitFor([&]() { return dialog.isNull() && scanIsIdle(); });
        input->clear();
        waitFor([&]() { return table->model()->rowCount() == 5; });
        touch(second + QStringLiteral("/手动更新.pptx"));
        QElapsedTimer quiet;
        quiet.start();
        while (quiet.elapsed() < 1000) { pumpEvents(); QThread::msleep(5); }
        require(table->model()->rowCount() == 5, "Disabled auto refresh still scanned");
        panel.refresh();
        waitFor([&]() { return table->model()->rowCount() == 6; });
        require(scope->itemText(scope->findData(second)) == obs_seeker::uiText("Scope.Count").arg(QDir::toNativeSeparators(second), QStringLiteral("2")),
                "Manual scan did not update location counts");
        require(!obs_seeker::loadSettings(config).autoRefresh, "Auto-refresh setting not persisted");

        // 实际磁盘扫描也必须列出全部数字包含项。
        for (const auto &name : {QStringLiteral("110.圣歌.ppsx"), QStringLiteral("210.圣歌.ppsx"),
                                 QStringLiteral("10+.圣歌.ppsx"), QStringLiteral("10-.圣歌.ppsx")})
            touch(first + QLatin1Char('/') + name);
        input->setText(QStringLiteral("10"));
        panel.refresh();
        waitFor([&]() { return table->model()->rowCount() == 5 && scanIsIdle(); });
        scope->setCurrentIndex(scope->findData(second));
        dialog = openSettings(panel);
        auto *scopeFolders = dialog->findChild<QListWidget *>(QStringLiteral("folderList"));
        delete scopeFolders->takeItem(1);
        saveDialog(dialog);
        waitFor([&]() { return dialog.isNull() && table->model()->rowCount() == 5 && scanIsIdle(); });
        require(!scopeRow->isVisible() && scope->currentData().toString().isEmpty() && hasTotal(8), "Removing the selected location left a stale scope");
        waitFor([&]() { return input->width() > 480; });

        {
            const auto viewFolder = temp.path() + QStringLiteral("/view-files");
            require(QDir().mkpath(viewFolder), "Cannot create viewport fixture");
            for (int row = 1; row <= 300; ++row)
                touch(viewFolder + QStringLiteral("/%1.view.pptx").arg(row, 3, 10, QLatin1Char('0')));
            obs_seeker::Settings viewSettings;
            viewSettings.folders = {viewFolder};
            viewSettings.autoRefresh = false;
            const auto viewConfig = temp.path() + QStringLiteral("/view-settings.json");
            require(obs_seeker::saveSettings(viewConfig, viewSettings), "Cannot save viewport settings");
            obs_seeker::SearchDock viewPanel(viewConfig);
            viewPanel.resize(920, 430);
            viewPanel.show();
            auto *viewTable = viewPanel.findChild<QTableView *>(QStringLiteral("resultsTable"));
            auto *viewInput = viewPanel.findChild<QLineEdit *>(QStringLiteral("searchInput"));
            auto *viewStatus = viewPanel.findChild<QLabel *>(QStringLiteral("statusLabel"));
            const auto viewIdle = [&]() {
                const auto text = viewStatus->text() + viewStatus->toolTip();
                return !text.contains(obs_seeker::uiText("Status.ScanningInitial"))
                    && !text.contains(obs_seeker::uiText("Status.ScanningSuffix"))
                    && !text.contains(obs_seeker::uiText("Status.RescanPending"));
            };
            waitFor([&]() { return viewTable->model()->rowCount() == 300 && viewIdle(); });
            viewTable->setColumnWidth(0, 1200);
            for (const auto mode : {QAbstractItemView::ScrollPerItem, QAbstractItemView::ScrollPerPixel}) {
                viewTable->setVerticalScrollMode(mode);
                viewTable->setCurrentIndex(viewTable->model()->index(5, 0));
                const auto selectedPath = viewTable->currentIndex().data(obs_seeker::FileModel::PathRole).toString();
                viewTable->scrollTo(viewTable->model()->index(150, 0), QAbstractItemView::PositionAtTop);
                if (mode == QAbstractItemView::ScrollPerPixel)
                    viewTable->verticalScrollBar()->setValue(viewTable->verticalScrollBar()->value() + 7);
                viewTable->horizontalScrollBar()->setValue(50);
                const auto top = viewTable->indexAt(QPoint(0, 0));
                const auto topPath = top.data(obs_seeker::FileModel::PathRole).toString();
                const auto offset = viewTable->visualRect(top).top();
                const auto horizontal = viewTable->horizontalScrollBar()->value();
                const auto inserted = viewFolder + QStringLiteral("/000.before.pptx");
                touch(inserted);
                QElapsedTimer viewTime;
                viewTime.start();
                viewPanel.refresh();
                waitFor([&]() { return viewTable->model()->rowCount() == 301 && viewIdle(); });
                pumpEvents();
                auto currentTop = viewTable->indexAt(QPoint(0, 0));
                require(currentTop.data(obs_seeker::FileModel::PathRole).toString() == topPath
                    && viewTable->visualRect(currentTop).top() == offset
                    && viewTable->horizontalScrollBar()->value() == horizontal,
                        "Adding a file moved the visible list or horizontal offset");
                require(viewTable->currentIndex().data(obs_seeker::FileModel::PathRole).toString() == selectedPath,
                        "Viewport restoration changed an off-screen selection");
                std::cout << "PERF: viewport refresh 301 files " << viewTime.elapsed() << " ms\n";
                require(QFile::remove(inserted), "Cannot remove viewport insertion");
                viewTable->clearSelection();
                viewPanel.refresh();
                waitFor([&]() { return viewTable->model()->rowCount() == 300 && viewIdle(); });
                pumpEvents();
                currentTop = viewTable->indexAt(QPoint(0, 0));
                require(currentTop.data(obs_seeker::FileModel::PathRole).toString() == topPath
                    && viewTable->visualRect(currentTop).top() == offset && !viewTable->selectionModel()->hasSelection(),
                        "Refresh moved an unselected viewport or selected another file");
            }
            const auto deletedTop = viewTable->indexAt(QPoint(0, 0)).data(obs_seeker::FileModel::PathRole).toString();
            const auto oldScroll = viewTable->verticalScrollBar()->value();
            require(QFile::remove(deletedTop), "Cannot delete the visible anchor");
            viewPanel.refresh();
            waitFor([&]() { return viewTable->model()->rowCount() == 299 && viewIdle(); });
            require(viewTable->verticalScrollBar()->value() == qMin(oldScroll, viewTable->verticalScrollBar()->maximum())
                && !viewTable->selectionModel()->hasSelection(), "Deleted viewport anchor did not use a safe fallback");
            viewTable->sortByColumn(0, Qt::DescendingOrder);
            viewTable->setCurrentIndex(viewTable->model()->index(5, 0));
            const auto deletedSelection = viewTable->currentIndex().data(obs_seeker::FileModel::PathRole).toString();
            viewTable->scrollTo(viewTable->model()->index(150, 0), QAbstractItemView::PositionAtTop);
            const auto visiblePath = viewTable->indexAt(QPoint(0, 0)).data(obs_seeker::FileModel::PathRole).toString();
            require(QFile::remove(deletedSelection), "Cannot remove the off-screen selection");
            viewPanel.refresh();
            waitFor([&]() { return viewTable->model()->rowCount() == 298 && viewIdle(); });
            require(!viewTable->currentIndex().isValid()
                && viewTable->indexAt(QPoint(0, 0)).data(obs_seeker::FileModel::PathRole).toString() == visiblePath,
                    "Deleting an off-screen selection changed the viewport or selected a replacement");
            viewInput->setText(QStringLiteral("100.view"));
            viewPanel.refresh();
            waitFor([&]() { return viewTable->model()->rowCount() == 1 && viewIdle(); });
            require(viewTable->currentIndex().data().toString() == QStringLiteral("100.view.pptx"),
                    "Viewport restoration overrode a pending query");
        }

        {
            QTemporaryDir folderTemp;
            require(folderTemp.isValid(), "Cannot create folder-warning fixture");
            const auto available = folderTemp.path() + QStringLiteral("/available");
            const auto unavailable = folderTemp.path() + QStringLiteral("/unavailable");
            const auto added = folderTemp.path() + QStringLiteral("/added");
            require(QDir().mkpath(available) && QDir().mkpath(added), "Cannot create available folders");
            touch(available + QStringLiteral("/001.pps"));
            obs_seeker::Settings settings;
            settings.folders = {available, unavailable};
            settings.autoRefresh = false;
            const auto config = folderTemp.path() + QStringLiteral("/settings.json");
            require(obs_seeker::saveSettings(config, settings), "Cannot save folder-warning settings");
            obs_seeker::SearchDock folderDock(config);
            folderDock.show();
            const auto idle = [&]() { return folderDock.findChildren<QThread *>().isEmpty(); };
            auto *table = folderDock.findChild<QTableView *>(QStringLiteral("resultsTable"));
            waitFor([&]() { return table->model()->rowCount() == 1 && idle(); });
            QPointer<QDialog> dialog = openSettings(folderDock);
            auto *folders = dialog->findChild<QListWidget *>(QStringLiteral("folderList"));
            auto *missing = folders->item(1);
            require(folders->count() == 2 && folders->item(0)->icon().isNull()
                && folders->item(0)->toolTip().isEmpty() && !missing->icon().isNull()
                && missing->toolTip() == obs_seeker::uiText("Settings.FolderUnavailable")
                && missing->text() == QDir::toNativeSeparators(unavailable), "Folder warning or unchanged path is incorrect");
            folders->setCurrentItem(missing);
            require(!missing->icon().pixmap(QSize(16, 16), QIcon::Selected).isNull(), "Selected folder has no warning icon");
            require(dropFolders(folders, {QUrl::fromLocalFile(added)}), "Cannot add folder beside a warning");
            require(folders->count() == 3 && !folders->item(1)->icon().isNull()
                && folders->item(1)->toolTip() == obs_seeker::uiText("Settings.FolderUnavailable"), "Adding a folder removed an existing warning");
            require(folders->model()->moveRows({}, 1, 1, {}, 0), "Cannot reorder a warning folder");
            require(folders->item(0)->text() == QDir::toNativeSeparators(unavailable)
                && !folders->item(0)->icon().isNull(), "Reordering separated the warning from its path");
            folders->setCurrentRow(0);
            require(QDir().mkpath(unavailable), "Cannot restore missing folder");
            folderDock.refresh();
            waitFor([&]() { return idle() && folders->item(0)->icon().isNull(); });
            require(folders->item(0)->toolTip().isEmpty() && folders->count() == 3
                && folders->item(0)->isSelected(), "Clearing a warning changed selection or pending folders");
            require(QDir().rmdir(unavailable), "Cannot remove empty warning fixture");
            folderDock.refresh();
            waitFor([&]() { return idle() && !folders->item(0)->icon().isNull(); });
            require(folders->item(0)->toolTip() == obs_seeker::uiText("Settings.FolderUnavailable"), "Open settings missed a folder becoming unavailable");
            dialog->reject();
            waitFor([&]() { return dialog.isNull(); });
            require(obs_seeker::loadSettings(config).folders == settings.folders, "Warning display saved pending folder changes");
            dialog = openSettings(folderDock);
            folders = dialog->findChild<QListWidget *>(QStringLiteral("folderList"));
            require(folders->count() == 2 && !folders->item(1)->icon().isNull(), "Reopened settings lost the cached folder warning");
            saveDialog(dialog);
            waitFor([&]() { return dialog.isNull(); });
            require(obs_seeker::loadSettings(config).folders == settings.folders, "Warning icon changed a saved folder path");
        }

        {
            QTemporaryDir multiTemp;
            require(multiTemp.isValid(), "Cannot create multi-select fixture");
            for (int row = 0; row < 5; ++row)
                touch(multiTemp.path() + QStringLiteral("/%1.pps").arg(row));
            obs_seeker::Settings settings;
            settings.folders = {multiTemp.path()};
            settings.autoRefresh = false;
            const auto config = multiTemp.path() + QStringLiteral("/settings.json");
            require(obs_seeker::saveSettings(config, settings), "Cannot save multi-select settings");
            obs_seeker::SearchDock multiDock(config);
            multiDock.resize(680, 400);
            multiDock.show();
            multiDock.focusSearch();
            auto *multiTable = multiDock.findChild<QTableView *>(QStringLiteral("resultsTable"));
            auto *proxy = static_cast<QSortFilterProxyModel *>(multiTable->model());
            auto *model = static_cast<obs_seeker::FileModel *>(proxy->sourceModel());
            waitFor([&]() { return proxy->rowCount() == 5 && multiDock.findChildren<QThread *>().isEmpty(); });
            require(multiTable->selectionMode() == QAbstractItemView::SingleSelection, "Normal view allows multiple selection");
            for (int row = 0; row < model->rowCount(); ++row)
                model->toggleFavorite(model->fileAt(row)->path);
            multiDock.toggleFavoritesView();
            require(multiTable->selectionMode() == QAbstractItemView::ExtendedSelection, "Favorites view cannot select multiple rows");
            multiTable->setFocus();
            pumpEvents();
            int mouseTimestamp = 0;
            const auto click = [&](int row, Qt::KeyboardModifiers modifiers) {
                const auto point = multiTable->visualRect(proxy->index(row, 0)).center();
                require(multiTable->indexAt(point).row() == row, "Multi-select mouse target is outside the row");
                const QPointF local = multiTable->viewport()->mapTo(&multiDock, point);
                const QPointF global = multiTable->viewport()->mapToGlobal(point);
                mouseTimestamp += 501;
                // Qt 的测试事件入口同时同步修饰键状态。
                qt_handleMouseEvent(multiDock.windowHandle(), local, global, Qt::LeftButton, Qt::LeftButton,
                                    QEvent::MouseButtonPress, modifiers, mouseTimestamp);
                qt_handleMouseEvent(multiDock.windowHandle(), local, global, Qt::NoButton, Qt::LeftButton,
                                    QEvent::MouseButtonRelease, modifiers, ++mouseTimestamp);
            };
            const auto openMenu = [&](int row) {
                const auto point = multiTable->visualRect(proxy->index(row, 0)).center();
                QMetaObject::invokeMethod(multiTable, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, point));
                auto *menu = multiDock.findChild<QMenu *>(QStringLiteral("fileContextMenu"));
                require(menu, "Multi-select context menu missing");
                return menu;
            };
            click(0, Qt::NoModifier);
            click(2, Qt::ShiftModifier);
            require(multiTable->selectionModel()->selectedRows().size() == 3, "Shift range selection failed");
            click(0, Qt::NoModifier);
            click(2, Qt::ControlModifier);
            require(multiTable->selectionModel()->selectedRows().size() == 2, "Ctrl or Command selection failed");
            const auto firstPath = proxy->index(0, 0).data(obs_seeker::FileModel::PathRole).toString();
            const auto secondPath = proxy->index(2, 0).data(obs_seeker::FileModel::PathRole).toString();
            const auto opens = recorder.openRequests;
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QCoreApplication::sendEvent(multiTable, &enter);
            multiDock.toggleSelectedFavorite();
            require(recorder.openRequests == opens && model->favoriteCount() == 5, "Multi-selection ran a single-file action");
            QPointer<QMenu> menu = openMenu(0);
            require(multiTable->selectionModel()->selectedRows().size() == 2, "Right-click discarded multiple selection");
            auto *remove = menu->findChild<QAction *>(QStringLiteral("toggleFavoriteAction"));
            require(remove && remove->text() == obs_seeker::uiText("Favorite.RemoveMany").arg(2), "Batch remove label is incorrect");
            for (const auto *name : {"moveFavoriteTopAction", "moveFavoriteUpAction", "moveFavoriteDownAction", "moveFavoriteBottomAction", "copyNameAction", "copyPathAction"})
                require(!menu->findChild<QAction *>(QString::fromLatin1(name))->isEnabled(), "Multi-selection allows a single-file menu action");
            int updates = 0;
            QObject::connect(model, &QAbstractItemModel::dataChanged, &multiDock, [&](const QModelIndex &, const QModelIndex &, const QList<int> &) { ++updates; });
            remove->trigger();
            menu->close();
            waitFor([&]() { return menu.isNull(); });
            require(updates == 1 && model->favoriteCount() == 3 && proxy->rowCount() == 3
                && !model->isFavorite(firstPath) && !model->isFavorite(secondPath)
                && !multiTable->selectionModel()->hasSelection() && !multiTable->currentIndex().isValid(), "Batch removal changed the wrong favorites or updated repeatedly");
            click(0, Qt::NoModifier);
            click(1, Qt::ControlModifier);
            menu = openMenu(0);
            const auto stalePath = proxy->index(0, 0).data(obs_seeker::FileModel::PathRole).toString();
            model->toggleFavorite(stalePath);
            menu->findChild<QAction *>(QStringLiteral("toggleFavoriteAction"))->trigger();
            menu->close();
            waitFor([&]() { return menu.isNull(); });
            require(model->favoriteCount() == 1 && proxy->rowCount() == 1 && !model->isFavorite(stalePath), "Stale batch removal added a favorite back");
            click(0, Qt::NoModifier);
            menu = openMenu(0);
            require(menu->actions().first()->isEnabled(), "Single favorite cannot open after batch removal");
            menu->close();
            waitFor([&]() { return menu.isNull(); });
            int resets = 0;
            QObject::connect(model, &QAbstractItemModel::modelReset, &multiDock, [&]() { ++resets; });
            QPointer<QDialog> dialog = openSettings(multiDock);
            dialog->findChild<QCheckBox *>(QStringLiteral("favoritesFirstCheck"))->setChecked(false);
            dialog->findChild<QSpinBox *>(QStringLiteral("pollIntervalSpin"))->setValue(17);
            saveDialog(dialog);
            waitFor([&]() { return dialog.isNull(); });
            require(resets == 0 && proxy->rowCount() == 1 && multiDock.findChildren<QThread *>().isEmpty()
                && multiDock.findChild<QTimer *>(QStringLiteral("pollTimer"))->interval() == 17000,
                    "Display settings triggered a disk scan or lost results");
            multiDock.toggleFavoritesView();
            require(multiTable->selectionMode() == QAbstractItemView::SingleSelection && proxy->rowCount() == 5, "Normal view did not restore single selection");
        }

        {
            QTemporaryDir sceneTemp;
            require(sceneTemp.isValid(), "Cannot create scene-switch fixture");
            for (const auto &name : {"1.pptx", "2.PPS", "3.pdf", "4.key", "5.ppt", "6.ppsx"})
                touch(sceneTemp.path() + QLatin1Char('/') + QString::fromLatin1(name));
            touch(sceneTemp.path() + QStringLiteral("/7.txt"));
            obs_seeker::Settings settings;
            settings.switchSceneOnOpen = false;
            settings.escapeReturnsHome = true;
            settings.folders = {sceneTemp.path()};
            settings.extensions.append(QStringLiteral("pdf"));
            settings.autoRefresh = false;
            const auto config = sceneTemp.path() + QStringLiteral("/settings.json");
            require(obs_seeker::saveSettings(config, settings), "Cannot save scene-switch fixture");
            obs_seeker::SearchDock sceneDock(config);
            QList<obs_seeker::SceneChoice> choices{{QStringLiteral("scene-id"), QStringLiteral("诗歌 & PPT")}};
            QStringList switched;
            int sceneReads = 0;
            sceneDock.setSceneCallbacks([&]() { ++sceneReads; return choices; }, [&](const QString &id) {
                for (const auto &scene : choices) {
                    if (scene.id == id)
                        switched.append(id);
                }
            });
            sceneDock.resize(680, 400);
            sceneDock.show();
            auto *table = sceneDock.findChild<QTableView *>(QStringLiteral("resultsTable"));
            auto *input = sceneDock.findChild<QLineEdit *>(QStringLiteral("searchInput"));
            auto *timer = sceneDock.findChild<QTimer *>(QStringLiteral("sceneSwitchTimer"));
            waitFor([&]() { return table->model()->rowCount() == 6 && scanIdle(sceneDock); });
            require(timer && timer->isSingleShot() && !timer->isActive() && sceneReads == 0, "Scene switching polls in the background");
            const auto open = [&](const QString &number) {
                input->setText(number);
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QCoreApplication::sendEvent(input, &enter);
            };
            open(QStringLiteral("1"));
            require(!timer->isActive(), "Explicitly disabled scene switching scheduled a switch");
            QPointer<QDialog> dialog = openSettings(sceneDock);
            auto *enabled = dialog->findChild<QCheckBox *>(QStringLiteral("switchSceneCheck"));
            auto *scene = dialog->findChild<QComboBox *>(QStringLiteral("targetSceneCombo"));
            auto *delay = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("sceneDelaySpin"));
            auto *home = dialog->findChild<QCheckBox *>(QStringLiteral("escapeHomeCheck"));
            require(enabled && scene && delay && home && !enabled->isChecked() && !scene->isEnabled() && delay->isEnabled(),
                    "HOME-only mode disabled the shared delay or enabled the scene selector");
            home->setChecked(false);
            require(!scene->isEnabled() && !delay->isEnabled(), "Unused delay settings were enabled");
            enabled->setChecked(true);
            require(scene->isEnabled() && delay->isEnabled(), "Presentation-only mode disabled its delay");
            enabled->setChecked(false);
            home->setChecked(true);
            require(!scene->isEnabled() && delay->isEnabled(), "HOME-only delay did not become editable");

            require(delay->minimum() == 0.5 && delay->maximum() == 3.0 && delay->singleStep() == 0.5 && delay->value() == 0.5,
                    "Scene delay range, step or default is incorrect");
            const auto previewPath = qEnvironmentVariable("OBS_SEEKER_SETTINGS_SCREENSHOT");
            if (!previewPath.isEmpty() && obs_seeker::uiText("Settings.Save") == QStringLiteral("保存")) {
                auto *folders = dialog->findChild<QListWidget *>(QStringLiteral("folderList"));
                const auto originalFolder = folders->item(0)->text();
                folders->item(0)->setText(QStringLiteral("E:\\诗歌\\PPT"));
                folders->addItem(QStringLiteral("E:\\诗歌\\PDF"));
                home->setChecked(false);
                pumpEvents();
                require(dialog->grab().save(previewPath), "Cannot save settings preview");
                home->setChecked(true);
                delete folders->takeItem(1);
                folders->item(0)->setText(originalFolder);
            }
            enabled->setChecked(true);
            require(scene->isEnabled() && delay->isEnabled() && scene->count() == 2, "Scene choices or enabled state failed");
            scene->setCurrentIndex(1);
            delay->setValue(3.0);
            dialog->reject();
            waitFor([&]() { return dialog.isNull(); });
            require(!obs_seeker::loadSettings(config).switchSceneOnOpen && obs_seeker::loadSettings(config).sceneDelayMs == 500,
                    "Canceled scene settings were saved");
            dialog = openSettings(sceneDock);
            dialog->findChild<QCheckBox *>(QStringLiteral("switchSceneCheck"))->setChecked(true);
            dialog->findChild<QComboBox *>(QStringLiteral("targetSceneCombo"))->setCurrentIndex(1);
            dialog->findChild<QDoubleSpinBox *>(QStringLiteral("sceneDelaySpin"))->setValue(0.5);
            auto *proxy = static_cast<QSortFilterProxyModel *>(table->model());
            int resets = 0;
            QObject::connect(proxy->sourceModel(), &QAbstractItemModel::modelReset, &sceneDock, [&]() { ++resets; });
            saveDialog(dialog);
            waitFor([&]() { return dialog.isNull(); });
            auto saved = obs_seeker::loadSettings(config);
            require(saved.switchSceneOnOpen && saved.targetSceneId == QStringLiteral("scene-id") && saved.sceneDelayMs == 500
                && resets == 0 && scanIdle(sceneDock), "Saving scene settings triggered a scan or lost values");
            open(QStringLiteral("2"));
            require(timer->isActive() && timer->interval() == 500 && switched.isEmpty(), "PPT did not wait before switching");
            QElapsedTimer firstOpenDelay;
            firstOpenDelay.start();
            waitFor([&]() { return firstOpenDelay.elapsed() >= 150; });
            open(QStringLiteral("3"));
            require(timer->isActive() && timer->remainingTime() >= 450, "Configured PDF did not restart the scene delay");
            open(QStringLiteral("4"));
            require(timer->isActive(), "Configured Keynote did not trigger a scene switch");
            open(QStringLiteral("5"));
            require(timer->isActive(), "PPT extension did not schedule a scene switch");
            const auto opensBeforeRepeat = recorder.openRequests;
            open(QStringLiteral("5"));
            require(timer->isActive() && recorder.openRequests == opensBeforeRepeat, "Repeated Enter canceled or reopened a pending PPT");
            open(QStringLiteral("6"));
            require(timer->isActive() && switched.isEmpty(), "Latest PPT was not delayed");
            QElapsedTimer elapsed;
            elapsed.start();
            waitFor([&]() { return switched.size() == 1; });
            require(elapsed.elapsed() >= 450 && !timer->isActive() && sceneReads == 2, "Delay fired early, repeated or polled scenes");
            choices[0].name = QStringLiteral("已改名");
            dialog = openSettings(sceneDock);
            scene = dialog->findChild<QComboBox *>(QStringLiteral("targetSceneCombo"));
            require(scene->currentData().toString() == QStringLiteral("scene-id") && scene->currentText() == choices[0].name,
                    "Renaming a scene lost the saved selection");
            dialog->reject();
            waitFor([&]() { return dialog.isNull(); });
            open(QStringLiteral("1"));
            require(timer->isActive(), "PPTX did not schedule a scene switch");
            sceneDock.cancelSceneSwitch();
            require(!timer->isActive(), "Manual scene or collection change did not cancel the pending switch");
            open(QStringLiteral("2"));
            require(timer->isActive(), "Uppercase PPS did not schedule a scene switch");
            choices.clear();
            waitFor([&]() { return !timer->isActive(); });
            require(switched.size() == 1, "Deleted target scene was activated");
            dialog = openSettings(sceneDock);
            scene = dialog->findChild<QComboBox *>(QStringLiteral("targetSceneCombo"));
            require(scene->currentData().toString() == QStringLiteral("scene-id")
                && scene->currentText() == obs_seeker::uiText("Settings.SceneUnavailable"), "Missing scene was silently replaced");
            dialog->findChild<QDoubleSpinBox *>(QStringLiteral("sceneDelaySpin"))->setValue(3.0);
            saveDialog(dialog);
            waitFor([&]() { return dialog.isNull(); });
            require(obs_seeker::loadSettings(config).sceneDelayMs == 3000, "Maximum scene delay was not saved");
            open(QStringLiteral("6"));
            require(timer->isActive() && timer->interval() == 3000, "Saved delay did not take effect");
            dialog = openSettings(sceneDock);
            dialog->findChild<QCheckBox *>(QStringLiteral("switchSceneCheck"))->setChecked(false);
            saveDialog(dialog);
            waitFor([&]() { return dialog.isNull(); });
            require(!timer->isActive(), "Disabling scene switching did not cancel the pending timer");
            dialog = openSettings(sceneDock);
            dialog->findChild<QCheckBox *>(QStringLiteral("switchSceneCheck"))->setChecked(true);
            dialog->findChild<QComboBox *>(QStringLiteral("targetSceneCombo"))->setCurrentIndex(0);
            saveDialog(dialog);
            waitFor([&]() { return dialog.isNull(); });
            open(QStringLiteral("5"));
            require(!timer->isActive(), "Empty target scene scheduled a switch");
            choices.append({QStringLiteral("scene-id"), QStringLiteral("诗歌")});
            dialog = openSettings(sceneDock);
            dialog->findChild<QComboBox *>(QStringLiteral("targetSceneCombo"))->setCurrentIndex(1);
            saveDialog(dialog);
            waitFor([&]() { return dialog.isNull(); });
            open(QStringLiteral("2"));
            require(timer->isActive(), "Shutdown fixture has no pending scene switch");
            sceneDock.shutdown();
            require(!timer->isActive() && switched.size() == 1, "Shutdown left a scene switch pending");
        }

        if (argc < 2 || QString::fromUtf8(argv[1]) == QStringLiteral("en-US")) {
            QList<obs_seeker::FileEntry> manyFiles;
            manyFiles.reserve(20000);
            for (int row = 0; row < 20000; ++row) {
                const auto name = QStringLiteral("%1.诗歌.pptx").arg(row, 5, 10, QLatin1Char('0'));
                const auto folder = row % 2 ? second : first;
                manyFiles.append({name, folder, folder + QLatin1Char('/') + name, obs_seeker::normalizedText(name), 0});
            }
            QElapsedTimer performance;
            performance.start();
            obs_seeker::FileModel largeModel;
            largeModel.setFiles(std::move(manyFiles));
            obs_seeker::NameFilter largeFilter;
            largeFilter.setExtensionOrder(obs_seeker::Settings{}.extensions);
            largeFilter.setSourceModel(&largeModel);
            largeFilter.sort(0, Qt::AscendingOrder);
            require(largeFilter.rowCount() == 20000, "Large-index construction lost files");
            const auto indexMs = performance.elapsed();
            performance.restart();
            largeFilter.setQuery(QStringLiteral("10"));
            require(largeFilter.rowCount() > 0 && obs_seeker::leadingNumberKey(largeFilter.index(0, 0).data().toString()) == QStringLiteral("10"), "Large-index ranking failed");
            const auto queryMs = performance.elapsed();
            performance.restart();
            largeFilter.setFolderScope(first);
            require(largeFilter.scopedFileCount() == 10000, "Large-index folder scope count failed");
            const auto folderMs = performance.elapsed();
            performance.restart();
            for (int iteration = 0; iteration < 1000; ++iteration)
                require(largeFilter.scopedFileCount() == 10000, "Repeated count cache read changed the total");
            std::cout << "PERF: 1000 cached scope-count reads, 20000 files " << performance.elapsed() << " ms\n";
            performance.restart();
            largeFilter.setFavoritesFirst(true);
            const auto favoritePath = largeModel.fileAt(1010)->path;
            largeModel.toggleFavorite(favoritePath);
            require(obs_seeker::leadingNumberKey(largeFilter.index(0, 0).data().toString()) == QStringLiteral("10")
                && largeFilter.index(1, 0).data(obs_seeker::FileModel::PathRole).toString() == favoritePath,
                    "Large-index exact-number or same-group favorite priority failed");
            std::cout << "PERF: 20000 filenames; index/sort " << indexMs << " ms; query/ranking " << queryMs
                      << " ms; folder switch/count " << folderMs << " ms; favorites priority " << performance.elapsed() << " ms\n";
            largeFilter.setQuery({});
            largeFilter.setFolderScope({});
            largeFilter.setFavoritesOnly(true);
            QList<int> favoriteRows;
            for (int row = 0; row < 100; ++row) {
                largeModel.toggleFavorite(largeModel.fileAt(row)->path);
                favoriteRows.append(row);
            }
            require(largeFilter.scopedFileCount() == 101, "Favorite reorder benchmark lost favorites");
            require(largeModel.favoriteCount() == 101, "Global favorite count missed indexed favorites");
            performance.restart();
            for (int move = 0; move < 100; ++move) {
                require(largeModel.moveFavoriteOrder(favoriteRows, true), "Large-index favorite rotation failed");
                favoriteRows.move(0, favoriteRows.size() - 1);
                require(largeFilter.scopedFileCount() == 101, "Reordering changed the cached favorite count");
            }
            std::cout << "PERF: 100 favorites, 100 moves, 20000 indexed files " << performance.elapsed() << " ms\n";
            QStringList batchPaths;
            for (const auto row : favoriteRows)
                batchPaths.append(largeModel.fileAt(row)->path);
            int batchUpdates = 0;
            QObject::connect(&largeModel, &QAbstractItemModel::dataChanged, &largeFilter,
                [&](const QModelIndex &, const QModelIndex &, const QList<int> &) { ++batchUpdates; });
            performance.restart();
            require(largeModel.removeFavorites(batchPaths) == 100 && batchUpdates == 1
                && largeModel.favoriteCount() == 1 && largeFilter.rowCount() == 1
                && largeFilter.scopedFileCount() == 1, "Large batch removal changed counts or updated repeatedly");
            std::cout << "PERF: remove 100 favorites, 20000 indexed files " << performance.elapsed() << " ms\n";
            require(largeModel.removeFavorites(batchPaths) == 0 && batchUpdates == 1, "Repeated batch removal changed favorites");
        }

        {
            QMainWindow host;
            auto *floatingContainer = new QDockWidget(&host);
            auto *floatingSearch = new obs_seeker::SearchDock(config, &host);
            floatingContainer->setWidget(floatingSearch);
            host.addDockWidget(Qt::RightDockWidgetArea, floatingContainer);
            floatingContainer->setFloating(true);
            host.show();
            host.setWindowState(host.windowState() | Qt::WindowMinimized);
            floatingSearch->focusSearch();
            waitFor([&]() { return !host.isMinimized() && host.isVisible() && floatingContainer->isVisible(); });
            auto *floatingInput = floatingSearch->findChild<QLineEdit *>(QStringLiteral("searchInput"));
            require(floatingSearch->focusWidget() == floatingInput, "Floating-dock activation did not focus search input");
        }

        auto *closing = new obs_seeker::SearchDock(config);
        closing->show();
        auto *newInput = closing->findChild<QLineEdit *>(QStringLiteral("searchInput"));
        auto *newTable = closing->findChild<QTableView *>(QStringLiteral("resultsTable"));
        waitFor([&]() { return newTable && newTable->model()->rowCount() > 0; });
        require(closing->findChild<QToolButton *>(QStringLiteral("favoritesButton"))->toolTip()
            .startsWith(obs_seeker::uiText("Favorite.Count").arg(0)), "New session restored a favorite count");
        for (int row = 0; row < newTable->model()->rowCount(); ++row)
            require(!newTable->model()->index(row, 0).data(obs_seeker::FileModel::FavoriteRole).toBool(), "New session restored old favorites");
        newInput->findChild<QAction *>(QStringLiteral("searchHistoryAction"))->trigger();
        QPointer<QMenu> emptyHistory = closing->findChild<QMenu *>(QStringLiteral("searchHistoryMenu"));
        require(emptyHistory && emptyHistory->actions().size() == 1 && !emptyHistory->actions().first()->isEnabled(), "New session restored old history");
        emptyHistory->close();
        waitFor([&]() { return emptyHistory.isNull(); });
        closing->openSettings();
        closing->refresh();
        delete closing; // 设置窗口与扫描线程同时存在时也可安全销毁。
        input->setText(QStringLiteral("退出检查"));
        panel.shutdown();
        QCoreApplication::sendEvent(input, &escape);
        require(input->text() == QStringLiteral("退出检查"), "Shutdown did not remove the application key filter");
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
        std::cout << "PASS: numeric search, delayed Enter, automatic add/rename/delete, context-menu snapshot, settings, focus and shutdown\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

#include "ui-smoke.moc"
