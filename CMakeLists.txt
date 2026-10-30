cmake_minimum_required(VERSION 3.16)
project(m_cowork VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network)
find_package(Qt6 COMPONENTS Multimedia QUIET)

set(SRC_AI
    src/ai/AiClient.cpp
    src/ai/AiClient.h
    src/ai/AntigravityAuth.cpp
    src/ai/AntigravityAuth.h
)

set(SRC_CORE
    src/core/Assets.cpp
    src/core/Assets.h
    src/core/AudioRecorder.cpp
    src/core/AudioRecorder.h
    src/core/ConversationCompactor.cpp
    src/core/ConversationCompactor.h
    src/core/Icons.cpp
    src/core/Icons.h
    src/core/ProviderKeyManager.cpp
    src/core/ProviderKeyManager.h
    src/core/SecretStore.cpp
    src/core/SecretStore.h
    src/core/SessionManager.cpp
    src/core/SessionManager.h
    src/core/Theme.h
    src/core/ThemeManager.cpp
    src/core/ThemeManager.h
    src/core/SettingsManager.cpp
    src/core/SettingsManager.h
)

set(SRC_WIDGETS
    src/widgets/ActionStepCard.cpp
    src/widgets/ActionStepCard.h
    src/widgets/CircleButton.cpp
    src/widgets/CircleButton.h
    src/widgets/DotMatrixWordmark.cpp
    src/widgets/DotMatrixWordmark.h
    src/widgets/IconButton.cpp
    src/widgets/LiquidGlass.cpp
    src/widgets/IconButton.h
    src/widgets/MicButton.cpp
    src/widgets/MicButton.h
    src/widgets/ModelSelector.cpp
    src/widgets/ModelSelector.h
    src/widgets/OutlinedLabel.cpp
    src/widgets/OutlinedLabel.h
    src/widgets/WindowControls.cpp
    src/widgets/WindowControls.h
)

set(SRC_UI
    src/ui/ChatArea.cpp
    src/ui/ChatArea.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/SettingsDialog.cpp
    src/ui/SettingsDialog.h
    src/ui/Sidebar.cpp
    src/ui/Sidebar.h
)

set(SRC_TOOLS
    src/tools/CoworkTool.cpp
    src/tools/CoworkTool.h
)

add_executable(m_cowork_ui
    src/main.cpp
    ${SRC_AI}
    ${SRC_CORE}
    ${SRC_WIDGETS}
    ${SRC_UI}
    ${SRC_TOOLS}
)

target_include_directories(m_cowork_ui PRIVATE src)
target_link_libraries(m_cowork_ui PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network)

if(TARGET Qt6::Multimedia)
    target_link_libraries(m_cowork_ui PRIVATE Qt6::Multimedia)
    target_compile_definitions(m_cowork_ui PRIVATE MCW_HAS_MULTIMEDIA=1)
endif()

if(WIN32)
    target_link_libraries(m_cowork_ui PRIVATE dwmapi crypt32)
    set_target_properties(m_cowork_ui PROPERTIES WIN32_EXECUTABLE TRUE)

    # Windows tool binary
    add_executable(matricX-cowork tools/matricX-cowork/matricX-cowork.cpp)
    target_link_libraries(matricX-cowork PRIVATE user32 gdi32 gdiplus winmm)
    set_target_properties(matricX-cowork PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/matricX-cowork"
    )
else()
    # Linux tool binary
    find_package(X11 REQUIRED)
    find_package(PNG REQUIRED)
    find_package(Threads REQUIRED)

    add_executable(matricX-cowork tools/matricX-cowork/matricX-cowork-linux.cpp)
    target_include_directories(matricX-cowork PRIVATE ${X11_INCLUDE_DIR} ${PNG_INCLUDE_DIRS})
    target_link_libraries(matricX-cowork PRIVATE
        ${X11_LIBRARIES}
        ${PNG_LIBRARIES}
        Threads::Threads
        Xtst
        Xfixes
    )
    set_target_properties(matricX-cowork PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/matricX-cowork"
    )
endif()
