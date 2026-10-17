#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs-hotkey.h>
#include <obs-interaction.h>
#include <util/bmem.h>
#include <util/config-file.h>
#include <util/dstr.h>
#include <QAction>
#include <QDockWidget>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QWidget>
#include "seek-dock.hpp"
#include "i18n.hpp"

OBS_DECLARE_MODULE()

namespace {
constexpr const char *DockId = "OBS-Seeker";
constexpr const char *SearchKeyName = "OBS-Seeker.focus";
constexpr const char *RefreshKeyName = "OBS-Seeker.refresh";
constexpr const char *FavoriteKeyName = "OBS-Seeker.favorite";
constexpr const char *FavoritesViewKeyName = "OBS-Seeker.favorites-view";
constexpr const char *HomeKeyName = "OBS-Seeker.home";
obs_hotkey_id searchKey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id refreshKey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id favoriteKey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id favoritesViewKey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id homeKey = OBS_INVALID_HOTKEY_ID;
QPointer<obs_seeker::SearchDock> dock;
QPointer<QAction> settingsAction;
bool frontendExited = false;
bool presentationStartupAttempted = false;

void searchHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (pressed)
        QMetaObject::invokeMethod(static_cast<obs_seeker::SearchDock *>(data), &obs_seeker::SearchDock::focusSearch, Qt::QueuedConnection);
}

void refreshHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (pressed)
        QMetaObject::invokeMethod(static_cast<obs_seeker::SearchDock *>(data), &obs_seeker::SearchDock::refresh, Qt::QueuedConnection);
}

void loadHotkey(obs_hotkey_id id, const char *name, obs_key_t defaultKey = OBS_KEY_NONE, uint32_t defaultModifiers = 0)
{
    auto *config = obs_frontend_get_profile_config();
    if (id == OBS_INVALID_HOTKEY_ID || !config)
        return;
    // 保留用户绑定，包括显式清空。
    if (config_has_user_value(config, "Hotkeys", name)) {
        const char *json = config_get_string(config, "Hotkeys", name);
        auto *data = json ? obs_data_create_from_json(json) : nullptr;
        auto *bindings = data ? obs_data_get_array(data, "bindings") : nullptr;
        obs_hotkey_load(id, bindings);
        obs_data_array_release(bindings);
        obs_data_release(data);
    } else {
        obs_key_combination_t combination{defaultModifiers, defaultKey};
        const bool hasDefault = defaultKey != OBS_KEY_NONE;
        obs_hotkey_load_bindings(id, hasDefault ? &combination : nullptr, hasDefault ? 1 : 0);
    }
}

bool matchesFavoriteShortcut(const QInputEvent &event)
{
    obs_key_combination_t combination{0, OBS_KEY_NONE};
    if (event.type() == QEvent::MouseButtonPress) {
        const auto button = static_cast<const QMouseEvent &>(event).button();
        for (int bit = 2; bit <= 26; ++bit) {
            if (button == (1 << bit)) {
                combination.key = static_cast<obs_key_t>(OBS_KEY_MOUSE3 + bit - 2);
                break;
            }
        }
    } else {
        const auto &key = static_cast<const QKeyEvent &>(event);
        combination.key = obs_key_from_virtual_key(static_cast<int>(key.nativeVirtualKey()));
    }
    if (combination.key == OBS_KEY_NONE)
        return false;
    const auto modifiers = event.modifiers();
    if (modifiers.testFlag(Qt::ShiftModifier))
        combination.modifiers |= INTERACT_SHIFT_KEY;
    if (modifiers.testFlag(Qt::AltModifier))
        combination.modifiers |= INTERACT_ALT_KEY;
#ifdef Q_OS_MACOS
    if (modifiers.testFlag(Qt::ControlModifier))
        combination.modifiers |= INTERACT_COMMAND_KEY;
    if (modifiers.testFlag(Qt::MetaModifier))
        combination.modifiers |= INTERACT_CONTROL_KEY;
#else
    if (modifiers.testFlag(Qt::ControlModifier))
        combination.modifiers |= INTERACT_CONTROL_KEY;
    if (modifiers.testFlag(Qt::MetaModifier))
        combination.modifiers |= INTERACT_COMMAND_KEY;
#endif
    struct Match { obs_key_combination_t combination; bool found = false; } state{combination};
    // 直接读取当前绑定，自定义或清空快捷键立即生效。
    obs_enum_hotkey_bindings([](void *data, size_t, obs_hotkey_binding_t *binding) {
        auto &match = *static_cast<Match *>(data);
        const auto bound = obs_hotkey_binding_get_key_combination(binding);
        match.found = obs_hotkey_binding_get_hotkey_id(binding) == favoriteKey
            && bound.key == match.combination.key && bound.modifiers == match.combination.modifiers;
        return !match.found;
    }, &state);
    return state.found;
}

QString shortcutText(obs_hotkey_id id)
{
    if (id == OBS_INVALID_HOTKEY_ID)
        return {};
    struct Bindings { obs_hotkey_id id; QList<obs_key_combination_t> keys; } state{id, {}};
    obs_enum_hotkey_bindings([](void *data, size_t, obs_hotkey_binding_t *binding) {
        auto &bindings = *static_cast<Bindings *>(data);
        const auto key = obs_hotkey_binding_get_key_combination(binding);
        if (obs_hotkey_binding_get_hotkey_id(binding) == bindings.id && !obs_key_combination_is_empty(key))
            bindings.keys.append(key);
        return true;
    }, &state);
    QStringList names;
    // 枚举结束后按 OBS 的当前键名格式化。
    for (const auto key : state.keys) {
        dstr text{};
        obs_key_combination_to_str(key, &text);
        const auto name = QString::fromUtf8(text.array ? text.array : "");
        dstr_free(&text);
        if (!name.isEmpty() && !names.contains(name))
            names.append(name);
    }
    return names.join(QStringLiteral(" / "));
}

obs_seeker::ShortcutSummary currentShortcuts()
{
    return {shortcutText(searchKey), shortcutText(refreshKey), shortcutText(favoriteKey), shortcutText(favoritesViewKey)};
}

void favoritesViewHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (pressed)
        QMetaObject::invokeMethod(static_cast<obs_seeker::SearchDock *>(data), &obs_seeker::SearchDock::toggleFavoritesView, Qt::QueuedConnection);
}

void loadHotkeys()
{
#ifdef Q_OS_MACOS
    loadHotkey(searchKey, SearchKeyName, OBS_KEY_F13);
#else
    loadHotkey(searchKey, SearchKeyName, OBS_KEY_PAUSE);
#endif
    loadHotkey(refreshKey, RefreshKeyName);
    loadHotkey(favoriteKey, FavoriteKeyName, OBS_KEY_NUMASTERISK);
    loadHotkey(favoritesViewKey, FavoritesViewKeyName);
    loadHotkey(homeKey, HomeKeyName, OBS_KEY_ESCAPE);
}

QList<obs_seeker::SceneChoice> sceneChoices()
{
    obs_frontend_source_list scenes{};
    obs_frontend_get_scenes(&scenes);
    QList<obs_seeker::SceneChoice> choices;
    for (size_t index = 0; index < scenes.sources.num; ++index) {
        auto *scene = scenes.sources.array[index];
        choices.append({QString::fromUtf8(obs_source_get_uuid(scene)), QString::fromUtf8(obs_source_get_name(scene))});
    }
    obs_frontend_source_list_free(&scenes);
    return choices;
}

void switchScene(const QString &id)
{
    auto *current = obs_frontend_get_current_scene();
    const bool unchanged = current && id == QString::fromUtf8(obs_source_get_uuid(current));
    obs_source_release(current);
    if (unchanged)
        return;
    // 只查当前场景集合；UUID 不受改名影响。
    obs_frontend_source_list scenes{};
    obs_frontend_get_scenes(&scenes);
    for (size_t index = 0; index < scenes.sources.num; ++index) {
        auto *scene = scenes.sources.array[index];
        if (id == QString::fromUtf8(obs_source_get_uuid(scene))) {
            obs_frontend_set_current_scene(scene);
            break;
        }
    }
    obs_frontend_source_list_free(&scenes);
}

void createDock()
{
    // 加载回调可能重复触发，只注册一次。
    if (dock || frontendExited)
        return;
    auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
    if (!parent)
        return;
    char *configPath = obs_module_config_path("settings.json");
    if (!configPath) {
        blog(LOG_ERROR, "[OBS-Seeker] Cannot obtain plugin config path");
        return;
    }
    const auto path = QString::fromUtf8(configPath);
    bfree(configPath);
    auto *panel = new obs_seeker::SearchDock(path, parent, obs_seeker::presentationCommand);
    if (!obs_frontend_add_dock_by_id(DockId, obs_seeker::localizedText("Plugin.Title"), panel)) {
        delete panel;
        blog(LOG_ERROR, "[OBS-Seeker] Dock registration failed");
        return;
    }
    dock = panel;
    searchKey = obs_hotkey_register_frontend(SearchKeyName, obs_seeker::localizedText("Hotkey.Search"), searchHotkey, panel);
    refreshKey = obs_hotkey_register_frontend(RefreshKeyName, obs_seeker::localizedText("Hotkey.Refresh"), refreshHotkey, panel);
    // 收藏由面板事件执行，OBS 保留快捷键设置入口。
    favoriteKey = obs_hotkey_register_frontend(FavoriteKeyName, obs_seeker::localizedText("Hotkey.Favorite"),
        [](void *, obs_hotkey_id, obs_hotkey_t *, bool) {}, panel);
    panel->setFavoriteShortcutMatcher(matchesFavoriteShortcut);
    favoritesViewKey = obs_hotkey_register_frontend(FavoritesViewKeyName, obs_seeker::localizedText("Hotkey.FavoritesView"), favoritesViewHotkey, panel);
    homeKey = obs_hotkey_register_frontend(HomeKeyName, obs_seeker::localizedText("Hotkey.Home"),
        [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
            auto *panel = static_cast<obs_seeker::SearchDock *>(data);
            QMetaObject::invokeMethod(panel, [panel, pressed]() { panel->homeHotkey(pressed); }, Qt::QueuedConnection);
        }, panel);

    panel->setShortcutSummaryProvider(currentShortcuts);
    panel->setSceneCallbacks(sceneChoices, switchScene);
    if (searchKey == OBS_INVALID_HOTKEY_ID || refreshKey == OBS_INVALID_HOTKEY_ID
        || favoriteKey == OBS_INVALID_HOTKEY_ID || favoritesViewKey == OBS_INVALID_HOTKEY_ID
        || homeKey == OBS_INVALID_HOTKEY_ID
        )
        blog(LOG_ERROR, "[OBS-Seeker] Hotkey registration failed");
    else {
        blog(LOG_INFO, "[OBS-Seeker] Search and refresh hotkeys registered");
        blog(LOG_INFO, "[OBS-Seeker] Favorites hotkeys registered");
    }
    loadHotkeys();
    if (auto *container = qobject_cast<QDockWidget *>(panel->parentWidget()))
        container->setFloating(false);
    blog(LOG_INFO, "[OBS-Seeker] Search dock registered");
    blog(LOG_INFO, "[OBS-Seeker] UI title: %s", obs_seeker::localizedText("Plugin.Title"));
    settingsAction = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_seeker::localizedText("Tools.Settings")));
    if (settingsAction)
        QObject::connect(settingsAction.data(), &QAction::triggered, panel, &obs_seeker::SearchDock::openSettings);
}

void removeDock()
{
    // 先注销快捷键，再释放控件。
    if (searchKey != OBS_INVALID_HOTKEY_ID)
        obs_hotkey_unregister(searchKey);
    if (refreshKey != OBS_INVALID_HOTKEY_ID)
        obs_hotkey_unregister(refreshKey);
    if (favoriteKey != OBS_INVALID_HOTKEY_ID)
        obs_hotkey_unregister(favoriteKey);
    if (favoritesViewKey != OBS_INVALID_HOTKEY_ID)
        obs_hotkey_unregister(favoritesViewKey);
    if (homeKey != OBS_INVALID_HOTKEY_ID)
        obs_hotkey_unregister(homeKey);
    searchKey = refreshKey = favoriteKey = favoritesViewKey = OBS_INVALID_HOTKEY_ID;
    homeKey = OBS_INVALID_HOTKEY_ID;
    delete settingsAction.data();
    settingsAction = nullptr;
    if (dock) {
        dock->shutdown();
        // DLL 卸载前释放插件控件；OBS 负责停靠容器。
        obs_frontend_remove_dock(DockId);
        delete dock.data();
        dock = nullptr;
    }
}

void frontendEvent(obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        createDock();
        loadHotkeys();
        if (dock && !presentationStartupAttempted) {
            presentationStartupAttempted = true;
#ifdef Q_OS_WIN
            obs_seeker::preloadPowerPoint(dock.data());
#elif defined(Q_OS_MACOS)
            obs_seeker::retryPresentationAutomation(dock.data());
#endif
        }
    } else if (event == OBS_FRONTEND_EVENT_PROFILE_CHANGED) {
        loadHotkeys();
    } else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING || event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
        if (dock)
            dock->cancelSceneSwitch();
    } else if (event == OBS_FRONTEND_EVENT_EXIT) {
        removeDock();
        frontendExited = true;
    }
}
} // namespace

bool obs_module_load(void)
{
    frontendExited = false;
    presentationStartupAttempted = false;
    obs_frontend_add_event_callback(frontendEvent, nullptr);
    createDock();
    blog(LOG_INFO, "[OBS-Seeker] Loaded version %s", OBS_SEEKER_VERSION);
    return true;
}

void obs_module_post_load(void) { createDock(); }

void obs_module_unload(void)
{
    // EXIT 之后前端 API 已销毁，不再调用。
    if (!frontendExited) {
        obs_frontend_remove_event_callback(frontendEvent, nullptr);
        removeDock();
    }
}

// OBS 在加载模块前设置语言；界面资源内置在插件中。
MODULE_EXPORT void obs_module_set_locale(const char *locale) { obs_seeker::setUiLocale(locale); }
MODULE_EXPORT bool obs_module_get_string(const char *key, const char **translated)
{
    return obs_seeker::lookupText(key, translated);
}
MODULE_EXPORT const char *obs_module_name(void) { return obs_seeker::localizedText("Plugin.Title"); }
MODULE_EXPORT const char *obs_module_description(void)
{
    return obs_seeker::localizedText("Plugin.Description");
}
