cmake_minimum_required(VERSION 3.16)
# The framework's components and the board come from espforge at a tag (main/idf_component.yml)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The app name: the firmware file name, esp_app_desc.project_name (an update must match it) and forge.json "app"
project(colormesh)
