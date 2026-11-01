cmake_minimum_required(VERSION 3.16)
# The board: one directory under boards/ holding a component named "board" (docs/NEW-PROJECT.md, "A new board")
set(EXTRA_COMPONENT_DIRS boards/ws_amoled175)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The app name: the firmware file name, esp_app_desc.project_name (an update must match it) and forge.json "app"
project(espforge)
