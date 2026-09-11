cmake_minimum_required(VERSION 3.20)
project(MasterLooter LANGUAGES CXX RC)

# Master Looter: an ASI plugin for Crimson Desert with an in-game ImGui settings menu.
# Builds MasterLooter.asi; an ASI loader (winmm.dll from Ultimate ASI Loader) injects it.

if(NOT WIN32)
    message(FATAL_ERROR "Master Looter targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT, no redist needed

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# Dear ImGui, same tag Trinity pins (classic DX12 backend Init signature).
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5-docking
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    # The DX12 backend is a vendored copy with precompiled shaders (no d3dcompiler); see src/gui/imgui_impl_dx12.cpp.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/imgui_impl_dx12.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS=0)

# MinHook for the DX12 and XInput detours.
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

add_library(MasterLooter SHARED
    src/dllmain.cpp
    src/core/mod.cpp
    src/core/paths.cpp
    src/core/log.cpp
    src/core/settings.cpp
    src/core/itemdb.cpp
    src/core/rules.cpp
    src/core/creaturedb.cpp
    src/core/nodedb.cpp
    src/core/text.cpp
    src/hooks/dx12_hook.cpp
    src/hooks/input.cpp
    src/hooks/xinput_hook.cpp
    src/gui/menu.cpp
    src/loot/mem.cpp
    src/loot/game.cpp
    src/loot/events.cpp
    src/loot/hooks.cpp
    src/loot/engine.cpp
    src/loot/farhook.cpp
    src/resources.rc
)

# The data tables and every translation are compiled into the plugin
# (resources.rc, which still names each translation); rebuild when any changes.
file(GLOB ML_LANG_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/data/MasterLooter.*.txt")
set_source_files_properties(src/resources.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/data/MasterLooter.items.tsv;${CMAKE_SOURCE_DIR}/data/MasterLooter.creatures.tsv;${CMAKE_SOURCE_DIR}/data/MasterLooter.nodes.tsv;${ML_LANG_FILES}")

target_include_directories(MasterLooter PRIVATE src ${minhook_SOURCE_DIR}/src/hde)
target_link_libraries(MasterLooter PRIVATE imgui minhook d3d12 dxgi dwmapi imm32 xinput9_1_0)
target_compile_definitions(MasterLooter PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# Test switch, off for every real build. Forces the swapchain wrapper off no
# matter what the ini says, so the one experiment that isolates the wrapper
# does not depend on somebody editing a file on the machine under test.
#   cmake -DML_FORCE_NO_WRAP=ON ...
option(ML_FORCE_NO_WRAP "Force the swapchain wrapper off (test builds only)" OFF)
option(ML_NO_DRAW "Wrap as normal but never draw (test builds only)" OFF)
option(ML_YIELD_PROBE "Log what the game's ore-count function is given (test builds only)" OFF)
if(ML_YIELD_PROBE)
    target_compile_definitions(MasterLooter PRIVATE ML_YIELD_PROBE)
    message(WARNING "ML_YIELD_PROBE is on: this build hooks the game's yield function to log it.")
endif()
if(ML_NO_DRAW)
    target_compile_definitions(MasterLooter PRIVATE ML_NO_DRAW)
    message(WARNING "ML_NO_DRAW is on: this build wraps the swapchain but never draws.")
endif()
if(ML_FORCE_NO_WRAP)
    target_compile_definitions(MasterLooter PRIVATE ML_FORCE_NO_WRAP)
    message(WARNING "ML_FORCE_NO_WRAP is on: this build ignores WrapSwapChain and never wraps.")
endif()
target_compile_options(MasterLooter PRIVATE /utf-8 /W3)

set_target_properties(MasterLooter PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "MasterLooter")

if(MSVC)
    target_compile_options(MasterLooter PRIVATE /guard:cf)
    target_link_options(MasterLooter PRIVATE /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /guard:cf)
endif()

# Stage a ready-to-copy folder: the plugin, the data tables it also carries inside
# (a copy next to the plugin overrides the built-in one), and the documents.
add_custom_command(TARGET MasterLooter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:MasterLooter>" "${CMAKE_SOURCE_DIR}/dist/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/MasterLooter.items.tsv" "${CMAKE_SOURCE_DIR}/dist/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/MasterLooter.creatures.tsv" "${CMAKE_SOURCE_DIR}/dist/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/data/MasterLooter.nodes.tsv" "${CMAKE_SOURCE_DIR}/dist/"
    COMMAND ${CMAKE_COMMAND} -E copy ${ML_LANG_FILES} "${CMAKE_SOURCE_DIR}/dist/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/README.md" "${CMAKE_SOURCE_DIR}/dist/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${CMAKE_SOURCE_DIR}/dist/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/../LICENSE" "${CMAKE_SOURCE_DIR}/dist/"
    COMMENT "Staging dist/"
    VERBATIM
)
