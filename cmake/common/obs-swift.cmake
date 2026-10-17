# OBS 32 的 Metal 目标需要 Swift，即使只构建插件开发库。
if(APPLE AND PROJECT_NAME STREQUAL "obs-studio")
  enable_language(Swift)
endif()
