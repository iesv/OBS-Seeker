#pragma once

#include <QString>

namespace obs_seeker {
void setUiLocale(const char *locale);
bool lookupText(const char *key, const char **translated);
const char *localizedText(const char *key);
QString uiText(const char *key);
} // namespace obs_seeker
