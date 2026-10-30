#include "ui/SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>

#include "ai/AntigravityAuth.h"
#include "core/Icons.h"
#include "core/ProviderKeyManager.h"
#include "core/SessionManager.h"
#include "core/SettingsManager.h"
#include "core/ThemeManager.h"

namespace mcw {

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("Settings");
    setModal(true);
    setFixedSize(640, 700);
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 20, 24, 20);
    mainLayout->setSpacing(14);

    // Header bar (Title + Close button)
    auto *headerLayout = new QHBoxLayout;
    auto *titleIcon = new QLabel;
    titleIcon->setPixmap(icons::pixmap(icons::IconType::Gear, QSize(20, 20), ThemeManager::instance().colors().accent));
    headerLayout->addWidget(titleIcon);

    auto *title = new QLabel("Settings");
    title->setStyleSheet("color: #ffffff; font-size: 18px; font-weight: bold;");
    headerLayout->addWidget(title);
    headerLayout->addStretch();

    auto *closeBtn = new QPushButton;
    closeBtn->setIcon(icons::icon(icons::IconType::X, QColor(160, 160, 160)));
    closeBtn->setFixedSize(28, 28);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setStyleSheet(
        "QPushButton { background: transparent; border: none; border-radius: 14px; }"
        "QPushButton:hover { background: #3f3f46; }");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    headerLayout->addWidget(closeBtn);
    mainLayout->addLayout(headerLayout);

    // Styled tab widget (Codex desktop segmented navigation)
    m_tabs = new QTabWidget(this);
    m_tabs->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #2e2e33; border-radius: 8px; background: #18181b; }"
        "QTabBar::tab {"
        "  background: #121214; color: #a1a1aa; padding: 8px 18px; margin-right: 4px;"
        "  border: 1px solid transparent; border-top-left-radius: 6px; border-top-right-radius: 6px;"
        "  font-weight: 500; font-size: 13px;"
        "}"
        "QTabBar::tab:selected { background: #18181b; color: #10b981; border: 1px solid #2e2e33; border-bottom: none; font-weight: 600; }"
        "QTabBar::tab:hover:!selected { background: #1c1c20; color: #f4f4f5; }");

    auto *providersTab = new QWidget;
    setupProvidersTab(providersTab);
    m_tabs->addTab(providersTab, "Providers & Models");

    auto *appearanceTab = new QWidget;
    setupAppearanceTab(appearanceTab);
    m_tabs->addTab(appearanceTab, "Appearance");

    auto *archivedTab = new QWidget;
    setupArchivedTab(archivedTab);
    m_tabs->addTab(archivedTab, "Archived Sessions");

    mainLayout->addWidget(m_tabs, 1);

    // Status label at bottom
    m_statusLabel = new QLabel;
    m_statusLabel->setStyleSheet("color: #10b981; font-size: 12px; font-weight: 600;");
    m_statusLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_statusLabel);

    // Bottom action buttons
    auto *btnLayout = new QHBoxLayout;
    btnLayout->addStretch();

    auto *doneBtn = new QPushButton("Done");
    doneBtn->setFixedHeight(36);
    doneBtn->setCursor(Qt::PointingHandCursor);
    doneBtn->setStyleSheet(
        "QPushButton { background: #10b981; color: #ffffff; border: none; border-radius: 6px; font-size: 14px; font-weight: bold; padding: 0 24px; }"
        "QPushButton:hover { background: #059669; }");
    connect(doneBtn, &QPushButton::clicked, this, [this] {
        if (onSettingsSaved) onSettingsSaved();
        accept();
    });
    btnLayout->addWidget(doneBtn);

    mainLayout->addLayout(btnLayout);

    connect(&SessionManager::instance(), &SessionManager::sessionsChanged, this, &SettingsDialog::refreshArchivedSessions);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { update(); });
}

void SettingsDialog::setupAppearanceTab(QWidget *tab) {
    auto *outer = new QVBoxLayout(tab);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // The dialog has a fixed size; scroll instead of squeezing rows into each other.
    auto *scroll = new QScrollArea(tab);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }"
        "QScrollBar::handle:vertical { background: rgba(255, 255, 255, 0.22); border-radius: 4px; min-height: 30px; }"
        "QScrollBar::handle:vertical:hover { background: rgba(255, 255, 255, 0.28); }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");

    auto *page = new QWidget;
    page->setStyleSheet("background: transparent;");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(8);

    auto *themeLabel = new QLabel("Theme Mode");
    themeLabel->setStyleSheet("color: #e5e7eb; font-size: 14px; font-weight: 600;");
    layout->addWidget(themeLabel);

    m_themeCombo = new QComboBox;
    m_themeCombo->setFixedHeight(36);
    m_themeCombo->setStyleSheet(
        "QComboBox { background: #28282e; border: 1px solid #3f3f46; border-radius: 6px; color: #f3f4f6; padding: 4px 12px; font-size: 13px; }"
        "QComboBox QAbstractItemView { background: #28282e; color: #f3f4f6; selection-background-color: #10b981; }");

    m_themeCombo->addItem(icons::icon(icons::IconType::Moon, QColor(156, 163, 175)), "Dark Mode", "dark");
    m_themeCombo->addItem(icons::icon(icons::IconType::Sun, QColor(156, 163, 175)), "Light Mode", "light");
    m_themeCombo->addItem(icons::icon(icons::IconType::Monitor, QColor(156, 163, 175)), "System / Auto", "system");

    const auto currentMode = ThemeManager::instance().mode();
    if (currentMode == ThemeMode::Light) m_themeCombo->setCurrentIndex(1);
    else if (currentMode == ThemeMode::System) m_themeCombo->setCurrentIndex(2);
    else m_themeCombo->setCurrentIndex(0);

    connect(m_themeCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        const QString modeStr = m_themeCombo->itemData(idx).toString();
        if (modeStr == "light") ThemeManager::instance().setMode(ThemeMode::Light);
        else if (modeStr == "system") ThemeManager::instance().setMode(ThemeMode::System);
        else ThemeManager::instance().setMode(ThemeMode::Dark);
        m_statusLabel->setText("Theme applied.");
    });

    layout->addWidget(m_themeCombo);

    auto *themeHint = new QLabel("Appearance changes take effect instantly across all windows.");
    themeHint->setStyleSheet("color: #9ca3af; font-size: 12px;");
    themeHint->setWordWrap(true);
    layout->addWidget(themeHint);

    layout->addSpacing(6);

    // ---- Window controls ----
    auto *chromeLabel = new QLabel("Window Controls");
    chromeLabel->setStyleSheet("color: #e5e7eb; font-size: 14px; font-weight: 600;");
    layout->addWidget(chromeLabel);

    const QString comboStyle =
        "QComboBox { background: #28282e; border: 1px solid #3f3f46; border-radius: 6px; color: #f3f4f6; padding: 4px 12px; font-size: 13px; }"
        "QComboBox QAbstractItemView { background: #28282e; color: #f3f4f6; selection-background-color: #10b981; }";

    auto addComboRow = [&](const QString &caption, QComboBox *combo) {
        auto *row = new QHBoxLayout;
        auto *label = new QLabel(caption);
        label->setStyleSheet("color: #d1d5db; font-size: 13px;");
        label->setFixedWidth(90);
        row->addWidget(label);
        combo->setFixedHeight(30);
        combo->setStyleSheet(comboStyle);
        row->addWidget(combo, 1);
        layout->addLayout(row);
    };

    m_controlsPlaceCombo = new QComboBox;
    m_controlsPlaceCombo->addItem("Chat header", "titlebar");
    m_controlsPlaceCombo->addItem("Sidebar", "sidebar");
    m_controlsPlaceCombo->setCurrentIndex(
        SettingsManager::instance().windowControlsPlace() == "sidebar" ? 1 : 0);
    connect(m_controlsPlaceCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        SettingsManager::instance().setWindowControlsPlace(
            m_controlsPlaceCombo->itemData(idx).toString());
        if (onWindowChromeChanged) onWindowChromeChanged();
        m_statusLabel->setText(idx == 0 ? "Window buttons moved to the chat header."
                                        : "Window buttons moved to the sidebar.");
    });
    addComboRow("Place", m_controlsPlaceCombo);

    m_controlsPositionCombo = new QComboBox;
    m_controlsPositionCombo->addItem("Right", "right");
    m_controlsPositionCombo->addItem("Left", "left");
    m_controlsPositionCombo->setCurrentIndex(
        SettingsManager::instance().windowControlsPosition() == "left" ? 1 : 0);
    connect(m_controlsPositionCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        SettingsManager::instance().setWindowControlsPosition(
            m_controlsPositionCombo->itemData(idx).toString());
        if (onWindowChromeChanged) onWindowChromeChanged();
        m_statusLabel->setText("Window controls moved.");
    });
    addComboRow("Position", m_controlsPositionCombo);

    m_controlsShapeCombo = new QComboBox;
    m_controlsShapeCombo->addItem("Circle", "circle");
    m_controlsShapeCombo->addItem("Rounded square", "rounded");
    m_controlsShapeCombo->addItem("Square", "square");
    const QString shape = SettingsManager::instance().windowControlsShape();
    m_controlsShapeCombo->setCurrentIndex(shape == "rounded" ? 1 : (shape == "square" ? 2 : 0));
    connect(m_controlsShapeCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        SettingsManager::instance().setWindowControlsShape(
            m_controlsShapeCombo->itemData(idx).toString());
        if (onWindowChromeChanged) onWindowChromeChanged();
        m_statusLabel->setText("Window control shape updated.");
    });
    addComboRow("Shape", m_controlsShapeCombo);

    m_controlsSizeCombo = new QComboBox;
    m_controlsSizeCombo->addItem("Small", "small");
    m_controlsSizeCombo->addItem("Medium", "medium");
    m_controlsSizeCombo->addItem("Large", "large");
    const QString size = SettingsManager::instance().windowControlsSize();
    m_controlsSizeCombo->setCurrentIndex(size == "small" ? 0 : (size == "large" ? 2 : 1));
    connect(m_controlsSizeCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        SettingsManager::instance().setWindowControlsSize(
            m_controlsSizeCombo->itemData(idx).toString());
        if (onWindowChromeChanged) onWindowChromeChanged();
        m_statusLabel->setText("Window control size updated.");
    });
    addComboRow("Size", m_controlsSizeCombo);

    auto *chromeHint = new QLabel(
        "Park the traffic-light buttons in the chat header (top-right) or the sidebar, "
        "then choose their side, shape and size.");
    chromeHint->setStyleSheet("color: #9ca3af; font-size: 12px;");
    chromeHint->setWordWrap(true);
    layout->addWidget(chromeHint);

    layout->addSpacing(6);

    auto *thinkingLabel = new QLabel("Thinking & Reasoning");
    thinkingLabel->setStyleSheet("color: #e5e7eb; font-size: 14px; font-weight: 600;");
    layout->addWidget(thinkingLabel);

    m_showReasoningCheck = new QCheckBox("Show the model's thinking in a bubble");
    m_showReasoningCheck->setChecked(SettingsManager::instance().showReasoning());
    m_showReasoningCheck->setCursor(Qt::PointingHandCursor);
    m_showReasoningCheck->setStyleSheet(
        "QCheckBox { color: #d1d5db; font-size: 13px; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #52525b; border-radius: 4px; background: #27272a; }"
        "QCheckBox::indicator:checked { background: #10b981; border-color: #10b981; }");
    connect(m_showReasoningCheck, &QCheckBox::toggled, this, [this](bool checked) {
        SettingsManager::instance().setShowReasoning(checked);
        m_statusLabel->setText(checked ? "Thinking bubbles enabled." : "Thinking bubbles hidden.");
    });
    layout->addWidget(m_showReasoningCheck);

    auto *reasoningHint = new QLabel("Shows the model's reasoning above the answer when the provider exposes it.");
    reasoningHint->setStyleSheet("color: #9ca3af; font-size: 12px;");
    reasoningHint->setWordWrap(true);
    layout->addWidget(reasoningHint);

    layout->addSpacing(6);

    auto *safetyLabel = new QLabel("Computer Control & Safety");
    safetyLabel->setStyleSheet("color: #e5e7eb; font-size: 14px; font-weight: 600;");
    layout->addWidget(safetyLabel);

    auto *confirmCheck = new QCheckBox("Prompt for user permission before executing OS control actions");
    confirmCheck->setChecked(SettingsManager::instance().requireConfirmation());
    confirmCheck->setCursor(Qt::PointingHandCursor);
    confirmCheck->setStyleSheet(
        "QCheckBox { color: #d1d5db; font-size: 13px; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #52525b; border-radius: 4px; background: #27272a; }"
        "QCheckBox::indicator:checked { background: #10b981; border-color: #10b981; }");
    connect(confirmCheck, &QCheckBox::toggled, this, [this](bool checked) {
        SettingsManager::instance().setRequireConfirmation(checked);
        if (checked) {
            m_statusLabel->setText("Permission prompts are on.");
        }
    });
    layout->addWidget(confirmCheck);

    auto *safetyHint = new QLabel("When enabled, M-Cowork shows an Allow/Deny banner before each mouse or keyboard action.");
    safetyHint->setStyleSheet("color: #9ca3af; font-size: 12px;");
    safetyHint->setWordWrap(true);
    layout->addWidget(safetyHint);

    auto *shellCheck = new QCheckBox("Allow shell command execution (shell_exec)");
    shellCheck->setChecked(SettingsManager::instance().allowShell());
    shellCheck->setCursor(Qt::PointingHandCursor);
    shellCheck->setStyleSheet(
        "QCheckBox { color: #d1d5db; font-size: 13px; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #52525b; border-radius: 4px; background: #27272a; }"
        "QCheckBox::indicator:checked { background: #10b981; border-color: #10b981; }");
    connect(shellCheck, &QCheckBox::toggled, this, [this](bool checked) {
        SettingsManager::instance().setAllowShell(checked);
        m_statusLabel->setText(checked ? "Shell tools enabled — applies to the next M-Cowork session."
                                       : "Shell tools disabled.");
    });
    layout->addWidget(shellCheck);

    auto *shellHint = new QLabel("Off by default. When enabled, the AI can run programs with an argv "
                                 "command list (never a shell one-liner) inside your home folder and "
                                 "other explicitly allowed roots, with output caps and a hard timeout. "
                                "Takes effect the next time a computer-control session starts.");
    shellHint->setStyleSheet("color: #9ca3af; font-size: 12px;");
    shellHint->setWordWrap(true);
    layout->addWidget(shellHint);

    layout->addStretch();

    scroll->setWidget(page);
    outer->addWidget(scroll);
}

void SettingsDialog::setupProvidersTab(QWidget *tab) {
    auto *layout = new QVBoxLayout(tab);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    auto *scrollContent = new QWidget;
    m_providerCardsLayout = new QVBoxLayout(scrollContent);
    m_providerCardsLayout->setContentsMargins(6, 6, 6, 6);
    m_providerCardsLayout->setSpacing(12);

    scroll->setWidget(scrollContent);
    layout->addWidget(scroll);

    refreshProviderCards();
}

void SettingsDialog::refreshProviderCards() {
    // Clear existing
    while (QLayoutItem *item = m_providerCardsLayout->takeAt(0)) {
        if (item->widget()) delete item->widget();
        delete item;
    }

    const QString activeP = SettingsManager::instance().provider();

    for (const auto &p : SettingsManager::availableProviders()) {
        auto *card = new QFrame;
        card->setObjectName("providerCard");
        const bool isSelected = (p.id == activeP);
        card->setStyleSheet(QString(
            "QFrame#providerCard {"
            "  background: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 8px;"
            "}"
        ).arg(isSelected ? "#22252a" : "#1a1a1e",
              isSelected ? "#10b981" : "#323238"));

        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(14, 12, 14, 12);
        cardLayout->setSpacing(8);

        // Header Row: Chevron, Name, Status
        auto *headerRow = new QHBoxLayout;
        auto *chevronBtn = new QPushButton;
        chevronBtn->setIcon(icons::icon(icons::IconType::ChevronRight, QColor(160, 160, 160)));
        chevronBtn->setFixedSize(22, 22);
        chevronBtn->setStyleSheet("background: transparent; border: none;");
        headerRow->addWidget(chevronBtn);

        auto *nameLabel = new QLabel(p.name);
        nameLabel->setStyleSheet("color: #ffffff; font-size: 14px; font-weight: bold;");
        headerRow->addWidget(nameLabel);

        // Status badge
        const auto keys = ProviderKeyManager::instance().keysForProvider(p.id);
        const bool hasKey = !keys.isEmpty() || (p.id == "gemini" && AntigravityAuth::hasToken()) || (p.id == "ollama");
        auto *badge = new QLabel(hasKey ? "Connected" : "Not configured");
        badge->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: 600; padding: 2px 6px; border-radius: 4px; background: %2;")
                                 .arg(hasKey ? "#10b981" : "#9ca3af", hasKey ? "rgba(16, 185, 129, 0.15)" : "#26262a"));
        headerRow->addWidget(badge);
        headerRow->addStretch();

        cardLayout->addLayout(headerRow);

        // Expandable details container
        auto *details = new QWidget;
        auto *dLayout = new QVBoxLayout(details);
        dLayout->setContentsMargins(10, 6, 4, 6);
        dLayout->setSpacing(8);

        // Model selector for this provider
        auto *modelRow = new QHBoxLayout;
        auto *modelLbl = new QLabel("Default Model:");
        modelLbl->setStyleSheet("color: #9ca3af; font-size: 12px;");
        modelRow->addWidget(modelLbl);

        auto *modelCombo = new QComboBox;
        modelCombo->setEditable(true);
        modelCombo->setFixedHeight(26);
        modelCombo->setStyleSheet("background: #2a2a30; border: 1px solid #3f3f46; border-radius: 4px; color: #f3f4f6; font-size: 12px;");
        for (const auto &m : p.models) modelCombo->addItem(m);
        const QString savedM = SettingsManager::instance().model(p.id);
        int mIdx = modelCombo->findText(savedM);
        if (mIdx >= 0) modelCombo->setCurrentIndex(mIdx);
        else modelCombo->setCurrentText(savedM);

        connect(modelCombo, &QComboBox::currentTextChanged, this, [p](const QString &text) {
            SettingsManager::instance().setModel(text.trimmed(), p.id);
        });
        modelRow->addWidget(modelCombo, 1);

        auto *detectBtn = new QPushButton;
        detectBtn->setIcon(icons::icon(icons::IconType::Sparkles, QColor(255, 255, 255)));
        detectBtn->setText("Autodetect");
        detectBtn->setToolTip("Query provider for available models");
        detectBtn->setFixedHeight(26);
        detectBtn->setStyleSheet("background: #3b82f6; color: #ffffff; border: none; border-radius: 4px; font-size: 11px; font-weight: 600; padding: 0 8px;");
        connect(detectBtn, &QPushButton::clicked, this, [this, p, modelCombo, detectBtn] {
            detectBtn->setEnabled(false);
            m_statusLabel->setText(QString("Detecting models for %1...").arg(p.name));
            m_ai.fetchAvailableModels(p.id, [this, p, modelCombo, detectBtn](bool ok, const QStringList &models, const QString &err) {
                detectBtn->setEnabled(true);
                if (ok && !models.isEmpty()) {
                    modelCombo->clear();
                    for (const auto &m : models) modelCombo->addItem(m);
                    SettingsManager::instance().setModel(models.first(), p.id);
                    m_statusLabel->setText(QString("✓ Found %1 models for %2").arg(models.size()).arg(p.name));
                } else {
                    m_statusLabel->setText(QString("Error: %1").arg(err.isEmpty() ? "No models returned" : err));
                }
            });
        });
        modelRow->addWidget(detectBtn);
        dLayout->addLayout(modelRow);

        // Google Antigravity OAuth specific row
        if (p.id == "gemini") {
            auto *oauthRow = new QHBoxLayout;
            auto *oaLbl = new QLabel(AntigravityAuth::hasToken()
                ? QString("Google OAuth: %1").arg(AntigravityAuth::email())
                : "Google Antigravity OAuth:");
            oaLbl->setStyleSheet("color: #d1d5db; font-size: 12px;");
            oauthRow->addWidget(oaLbl);
            oauthRow->addStretch();

            auto *oaBtn = new QPushButton(AntigravityAuth::hasToken() ? "Disconnect" : "Login with Google");
            oaBtn->setFixedHeight(26);
            oaBtn->setStyleSheet(AntigravityAuth::hasToken()
                ? "background: #dc2626; color: #ffffff; border: none; border-radius: 4px; font-size: 12px; padding: 0 10px;"
                : "background: #2563eb; color: #ffffff; border: none; border-radius: 4px; font-size: 12px; padding: 0 10px;");
            connect(oaBtn, &QPushButton::clicked, this, [this] { startGoogleOAuth(); });
            oauthRow->addWidget(oaBtn);
            dLayout->addLayout(oauthRow);
        }

        // Nested Keys Section
        auto *keysHeader = new QHBoxLayout;
        auto *keysLbl = new QLabel("API Keys:");
        keysLbl->setStyleSheet("color: #9ca3af; font-size: 12px; font-weight: 600;");
        keysHeader->addWidget(keysLbl);
        keysHeader->addStretch();

        auto *addKeyBtn = new QPushButton;
        addKeyBtn->setIcon(icons::icon(icons::IconType::Plus, QColor(16, 185, 129)));
        addKeyBtn->setText("Add Key");
        addKeyBtn->setFixedHeight(24);
        addKeyBtn->setStyleSheet("background: #27272a; color: #10b981; border: 1px solid #3f3f46; border-radius: 4px; font-size: 11px; padding: 0 8px;");
        connect(addKeyBtn, &QPushButton::clicked, this, [this, p] {
            bool ok = false;
            const QString k = QInputDialog::getText(this, "Add API Key", QString("Enter API key for %1:").arg(p.name), QLineEdit::Password, "", &ok);
            if (ok && !k.trimmed().isEmpty()) {
                ProviderKeyManager::instance().addKey(p.id, k.trimmed());
                refreshProviderCards();
            }
        });
        keysHeader->addWidget(addKeyBtn);
        dLayout->addLayout(keysHeader);

        // Keys list
        for (const auto &k : keys) {
            auto *kRow = new QHBoxLayout;
            kRow->setContentsMargins(8, 2, 4, 2);

            auto *keyIcon = new QLabel;
            keyIcon->setPixmap(icons::pixmap(icons::IconType::Key, QSize(14, 14), k.isActive ? QColor(16, 185, 129) : QColor(156, 163, 175)));
            kRow->addWidget(keyIcon);

            auto *lbl = new QLabel(QString("%1: %2").arg(k.label, ProviderKeyManager::maskKey(k.key)));
            lbl->setStyleSheet(QString("color: %1; font-size: 12px; font-family: monospace;").arg(k.isActive ? "#10b981" : "#d1d5db"));
            kRow->addWidget(lbl);
            kRow->addStretch();

            if (!k.isActive) {
                auto *setActBtn = new QPushButton("Active");
                setActBtn->setFixedHeight(20);
                setActBtn->setStyleSheet("background: #374151; color: #d1d5db; border: none; border-radius: 3px; font-size: 10px; padding: 0 6px;");
                connect(setActBtn, &QPushButton::clicked, this, [this, p, k] {
                    ProviderKeyManager::instance().setActiveKey(p.id, k.id);
                    refreshProviderCards();
                });
                kRow->addWidget(setActBtn);
            }

            auto *delKeyBtn = new QPushButton;
            delKeyBtn->setIcon(icons::icon(icons::IconType::Trash, QColor(239, 68, 68)));
            delKeyBtn->setFixedSize(20, 20);
            delKeyBtn->setStyleSheet("background: transparent; border: none;");
            connect(delKeyBtn, &QPushButton::clicked, this, [this, p, k] {
                ProviderKeyManager::instance().removeKey(p.id, k.id);
                refreshProviderCards();
            });
            kRow->addWidget(delKeyBtn);

            dLayout->addLayout(kRow);
        }

        details->setVisible(isSelected);
        cardLayout->addWidget(details);

        connect(chevronBtn, &QPushButton::clicked, this, [details, chevronBtn] {
            const bool willShow = !details->isVisible();
            details->setVisible(willShow);
            chevronBtn->setIcon(icons::icon(willShow ? icons::IconType::ChevronDown : icons::IconType::ChevronRight, QColor(160, 160, 160)));
        });

        m_providerCardsLayout->addWidget(card);
    }
}

void SettingsDialog::setupArchivedTab(QWidget *tab) {
    auto *layout = new QVBoxLayout(tab);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    auto *content = new QWidget;
    m_archivedListLayout = new QVBoxLayout(content);
    m_archivedListLayout->setContentsMargins(4, 4, 4, 4);
    m_archivedListLayout->setSpacing(8);

    m_emptyArchivedLabel = new QLabel("No archived sessions.");
    m_emptyArchivedLabel->setAlignment(Qt::AlignCenter);
    m_emptyArchivedLabel->setStyleSheet("color: #6b7280; font-size: 13px; padding: 40px;");
    m_archivedListLayout->addWidget(m_emptyArchivedLabel);

    scroll->setWidget(content);
    layout->addWidget(scroll);

    refreshArchivedSessions();
}

void SettingsDialog::refreshArchivedSessions() {
    while (QLayoutItem *item = m_archivedListLayout->takeAt(0)) {
        if (item->widget()) delete item->widget();
        delete item;
    }

    const auto archived = SessionManager::instance().archivedSessions();
    if (archived.isEmpty()) {
        m_emptyArchivedLabel = new QLabel("No archived sessions.");
        m_emptyArchivedLabel->setAlignment(Qt::AlignCenter);
        m_emptyArchivedLabel->setStyleSheet("color: #6b7280; font-size: 13px; padding: 40px;");
        m_archivedListLayout->addWidget(m_emptyArchivedLabel);
        return;
    }

    for (const auto &s : archived) {
        auto *itemFrame = new QFrame;
        itemFrame->setObjectName("archivedItem");
        itemFrame->setStyleSheet("QFrame#archivedItem { background: #222228; border: 1px solid #333338; border-radius: 6px; padding: 6px 12px; }");
        auto *row = new QHBoxLayout(itemFrame);
        row->setContentsMargins(8, 6, 8, 6);

        auto *archIcon = new QLabel;
        archIcon->setPixmap(icons::pixmap(icons::IconType::Archive, QSize(16, 16), QColor(156, 163, 175)));
        row->addWidget(archIcon);

        auto *title = new QLabel(s.title);
        title->setStyleSheet("color: #e5e7eb; font-size: 13px; font-weight: 500;");
        row->addWidget(title);

        auto *info = new QLabel(QString("%1 msgs").arg(s.messages.size()));
        info->setStyleSheet("color: #9ca3af; font-size: 11px;");
        row->addWidget(info);
        row->addStretch();

        auto *restoreBtn = new QPushButton("Restore");
        restoreBtn->setIcon(icons::icon(icons::IconType::RotateCcw, QColor(16, 185, 129)));
        restoreBtn->setFixedHeight(26);
        restoreBtn->setStyleSheet("background: #064e3b; color: #34d399; border: 1px solid #059669; border-radius: 4px; font-size: 11px; padding: 0 8px;");
        connect(restoreBtn, &QPushButton::clicked, this, [this, s] {
            SessionManager::instance().restoreSession(s.id);
            refreshArchivedSessions();
            m_statusLabel->setText(QString("Restored session '%1'").arg(s.title));
        });
        row->addWidget(restoreBtn);

        auto *deleteBtn = new QPushButton("Delete");
        deleteBtn->setIcon(icons::icon(icons::IconType::Trash, QColor(239, 68, 68)));
        deleteBtn->setFixedHeight(26);
        deleteBtn->setStyleSheet("background: #451a1a; color: #f87171; border: 1px solid #7f1d1d; border-radius: 4px; font-size: 11px; padding: 0 8px;");
        connect(deleteBtn, &QPushButton::clicked, this, [this, s] {
            // Step 1 confirm
            auto res1 = QMessageBox::question(this, "Delete Session",
                QString("Permanently delete '%1'? This cannot be undone.").arg(s.title),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (res1 != QMessageBox::Yes) return;

            // Step 2 double confirm
            auto res2 = QMessageBox::warning(this, "Final Confirmation",
                "Are you ABSOLUTELY sure? All message history and references will be permanently destroyed.",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (res2 == QMessageBox::Yes) {
                SessionManager::instance().deleteSessionPermanently(s.id);
                refreshArchivedSessions();
                m_statusLabel->setText(QString("Permanently deleted '%1'").arg(s.title));
            }
        });
        row->addWidget(deleteBtn);

        m_archivedListLayout->addWidget(itemFrame);
    }
}

void SettingsDialog::startGoogleOAuth() {
    if (AntigravityAuth::hasToken()) {
        AntigravityAuth::clearToken();
        refreshProviderCards();
        m_statusLabel->setText("Google Antigravity account disconnected.");
        return;
    }

    m_statusLabel->setText("Browser opened. Sign in with Google to authorize...");
    AntigravityAuth::instance().startAuthFlow([this](const GoogleTokenInfo &info) {
        if (info.valid) {
            refreshProviderCards();
            m_statusLabel->setText("Google Antigravity authorization successful!");
        } else {
            refreshProviderCards();
            m_statusLabel->setText(QString("OAuth failed: %1").arg(info.error));
        }
    });
}

void SettingsDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QPainterPath path;
    path.addRoundedRect(rect(), 14, 14);
    p.fillPath(path, QColor(24, 24, 27));

    QPen pen(QColor(50, 50, 56), 1.5);
    p.setPen(pen);
    p.drawPath(path);
}

}  // namespace mcw
