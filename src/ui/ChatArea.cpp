#include "ui/ChatArea.h"

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScrollArea>
#include <QScrollBar>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

#include "core/Icons.h"
#include "core/SessionManager.h"
#include "core/SettingsManager.h"
#include "core/ThemeManager.h"
#include "widgets/ActionStepCard.h"
#include "widgets/DotMatrixWordmark.h"
#include "widgets/MicButton.h"
#include "widgets/ModelSelector.h"
#include "widgets/WindowControls.h"

namespace mcw {

ChatArea::ChatArea(QWidget *parent) : QWidget(parent) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Top Navigation & Window Controls Bar
    m_topBar = new QHBoxLayout;
    m_topBar->setContentsMargins(16, 12, 16, 8);
    m_topBar->setSpacing(12);

    m_collapsedControls = new QWidget(this);
    auto *cLayout = new QHBoxLayout(m_collapsedControls);
    cLayout->setContentsMargins(0, 0, 0, 0);
    cLayout->setSpacing(8);

    m_uncollapseBtn = new QPushButton(m_collapsedControls);
    m_uncollapseBtn->setIcon(icons::icon(icons::IconType::PanelLeft, QColor(161, 161, 170)));
    m_uncollapseBtn->setFixedSize(28, 28);
    m_uncollapseBtn->setCursor(Qt::PointingHandCursor);
    m_uncollapseBtn->setToolTip("Open Sidebar");
    m_uncollapseBtn->setStyleSheet("QPushButton { background: transparent; border: none; border-radius: 6px; } QPushButton:hover { background: rgba(255,255,255,0.08); }");
    cLayout->addWidget(m_uncollapseBtn, 0, Qt::AlignVCenter);

    m_controls = new WindowControls(this);
    m_controls->hide();

    m_threadTitleLabel = new QLabel(this);
    m_threadTitleLabel->setStyleSheet("color: #f4f4f5; font-size: 14px; font-weight: 600;");

    m_statusPill = new QLabel(this);
    m_statusPill->setFixedHeight(24);
    m_statusPill->hide();

    applyWindowChrome();
    root->addLayout(m_topBar);

    // Center Message Stream (Codex centered column with max width 780px)
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setStyleSheet(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical { background: transparent; width: 6px; margin: 2px; }"
        "QScrollBar::handle:vertical { background: rgba(255, 255, 255, 0.15); border-radius: 3px; min-height: 30px; }"
        "QScrollBar::handle:vertical:hover { background: rgba(255, 255, 255, 0.28); }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");

    m_scrollContent = new QWidget;
    m_scrollContent->setStyleSheet("background: transparent;");

    // Centered wrapper
    auto *contentCenterRow = new QHBoxLayout(m_scrollContent);
    contentCenterRow->setContentsMargins(0, 0, 0, 0);
    contentCenterRow->addStretch();

    auto *columnWidget = new QWidget(m_scrollContent);
    columnWidget->setMaximumWidth(780);
    columnWidget->setMinimumWidth(320);

    m_msgLayout = new QVBoxLayout(columnWidget);
    m_msgLayout->setContentsMargins(16, 16, 16, 20);
    m_msgLayout->setSpacing(16);

    // Empty state container
    m_emptyStateWidget = new QWidget(columnWidget);
    setupEmptyState(m_emptyStateWidget);
    m_msgLayout->addWidget(m_emptyStateWidget);

    m_msgLayout->addStretch();

    contentCenterRow->addWidget(columnWidget, 1);
    contentCenterRow->addStretch();

    m_scroll->setWidget(m_scrollContent);
    root->addWidget(m_scroll, 1);

    // Bottom Floating Input Capsule (Codex prompt bar)
    auto *capsuleRow = new QHBoxLayout;
    capsuleRow->setContentsMargins(24, 6, 24, 20);
    capsuleRow->addStretch();

    m_capsule = new QFrame(this);
    m_capsule->setObjectName("promptCapsule");
    m_capsule->setMaximumWidth(780);
    m_capsule->setMinimumWidth(340);
    m_capsule->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_capsule->setStyleSheet(
        "QFrame#promptCapsule {"
        "  background: #27272a;"
        "  border: 1px solid #3f3f46;"
        "  border-radius: 16px;"
        "}"
        "QFrame#promptCapsule:focus-within {"
        "  border: 1px solid #10b981;"
        "  background: #27272d;"
        "}");

    auto *capsuleLayout = new QVBoxLayout(m_capsule);
    capsuleLayout->setContentsMargins(16, 12, 14, 10);
    capsuleLayout->setSpacing(8);

    // Text Input Row
    m_input = new QLineEdit(m_capsule);
    m_input->setPlaceholderText("Ask M-Cowork anything, or describe an OS automation task...");
    m_input->setStyleSheet(
        "QLineEdit {"
        "  background: transparent;"
        "  border: none;"
        "  color: #f4f4f5;"
        "  font-size: 15px;"
        "  padding: 0;"
        "}");
    QPalette pal = m_input->palette();
    pal.setColor(QPalette::PlaceholderText, QColor(140, 140, 150));
    m_input->setPalette(pal);
    capsuleLayout->addWidget(m_input);

    // Bottom Controls Toolbar inside Capsule
    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(8);

    // Model Selector pill on the left
    m_modelSelector = new ModelSelector(m_capsule);
    toolbar->addWidget(m_modelSelector, 0, Qt::AlignVCenter);

    toolbar->addStretch();

    // Microphone voice button
    m_micBtn = new MicButton(m_capsule);
    m_micBtn->setFixedSize(34, 34);
    toolbar->addWidget(m_micBtn, 0, Qt::AlignVCenter);

    // Codex Send button (circular up-arrow)
    m_sendBtn = new QPushButton(m_capsule);
    m_sendBtn->setFixedSize(34, 34);
    m_sendBtn->setCursor(Qt::PointingHandCursor);
    m_sendBtn->setIcon(icons::icon(icons::IconType::ArrowUp, QColor(24, 24, 27)));
    m_sendBtn->setIconSize(QSize(16, 16));
    m_sendBtn->setStyleSheet(
        "QPushButton {"
        "  background: #10b981;"
        "  border: none;"
        "  border-radius: 17px;"
        "}"
        "QPushButton:hover { background: #34d399; }"
        "QPushButton:disabled { background: #3f3f46; }");
    toolbar->addWidget(m_sendBtn, 0, Qt::AlignVCenter);

    capsuleLayout->addLayout(toolbar);

    capsuleRow->addWidget(m_capsule, 1);
    capsuleRow->addStretch();
    root->addLayout(capsuleRow);

    // Event connections
    connect(m_input, &QLineEdit::returnPressed, this, [this] { send(); });
    connect(m_input, &QLineEdit::textChanged, this, [this] { updateSendButtonState(); });

    connect(m_sendBtn, &QPushButton::clicked, this, [this] {
        if (m_busy) {
            m_tool.endSession();
            updateStatusIndicator("Session stopped.", false);
            m_busy = false;
            updateSendButtonState();
        } else {
            send();
        }
    });

    connect(m_micBtn, &QAbstractButton::clicked, this, [this] {
        if (m_micBtn->state() == MicButton::Recording) {
            m_micBtn->setState(MicButton::Idle);
            updateStatusIndicator("Transcribing audio...", true);
            m_audioRecorder.stopRecording([this](bool ok, const QString &filePath, const QString &err) {
                if (!ok) {
                    updateStatusIndicator(QString("Voice recording failed: %1").arg(err), false);
                    return;
                }
                m_ai.transcribeAudio(filePath, [this](bool success, const QString &text, const QString &txErr) {
                    if (success && !text.isEmpty()) {
                        m_input->setText(text);
                        updateStatusIndicator(QString(), false);
                        m_input->setFocus();
                        updateSendButtonState();
                    } else {
                        updateStatusIndicator(QString("Transcription failed: %1").arg(txErr), false);
                    }
                });
            });
        } else {
            m_micBtn->setState(MicButton::Recording);
            m_audioRecorder.startRecording();
            updateStatusIndicator("Listening... Click mic again to transcribe", true);
        }
    });

    connect(m_modelSelector, &ModelSelector::modelChanged, this, [this](const QString &providerId, const QString &model) {
        const QString currentSid = SessionManager::instance().currentSessionId();
        SessionManager::instance().setSessionModel(currentSid, providerId, model);
    });

    connect(&SessionManager::instance(), &SessionManager::currentSessionChanged, this, &ChatArea::loadCurrentSession);
    connect(&SessionManager::instance(), &SessionManager::messagesChanged, this, &ChatArea::loadCurrentSession);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { update(); });

    updateSendButtonState();
    setSidebarCollapsed(false);
    loadCurrentSession();
}

void ChatArea::setupEmptyState(QWidget *container) {
    auto *l = new QVBoxLayout(container);
    l->setContentsMargins(0, 40, 0, 20);
    l->setSpacing(18);
    l->setAlignment(Qt::AlignCenter);

    // Dot-matrix wordmark at top of empty dashboard
    m_wordmark = new DotMatrixWordmark(container);
    l->addWidget(m_wordmark, 0, Qt::AlignCenter);

    auto *heading = new QLabel("What do you want to work on?", container);
    heading->setAlignment(Qt::AlignCenter);
    heading->setStyleSheet("color: #f4f4f5; font-size: 22px; font-weight: 600;");
    l->addWidget(heading, 0, Qt::AlignCenter);

    // Suggestion pills grid
    auto *chipsLayout = new QHBoxLayout;
    chipsLayout->setSpacing(10);
    chipsLayout->setAlignment(Qt::AlignCenter);

    auto createChip = [this, container](const QString &text, const QString &cmd) {
        auto *btn = new QPushButton(text, container);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(
            "QPushButton {"
            "  background: #27272a;"
            "  color: #d4d4d8;"
            "  border: 1px solid #3f3f46;"
            "  border-radius: 18px;"
            "  padding: 8px 16px;"
            "  font-size: 13px;"
            "  font-weight: 500;"
            "}"
            "QPushButton:hover {"
            "  background: #323238;"
            "  color: #ffffff;"
            "  border-color: #52525b;"
            "}");
        connect(btn, &QPushButton::clicked, this, [this, cmd] {
            m_input->setText(cmd);
            send();
        });
        return btn;
    };

    chipsLayout->addWidget(createChip("⚡ Screen Info", "/tool screen_info"));
    chipsLayout->addWidget(createChip("📷 Screenshot", "/tool screenshot"));
    chipsLayout->addWidget(createChip("🖱 Click Center", "/tool mouse_click {\"x\":960,\"y\":540}"));
    chipsLayout->addWidget(createChip("❓ All Tools", "/tools"));

    l->addLayout(chipsLayout);
}

void ChatArea::focusInput() { m_input->setFocus(); }

void ChatArea::setCornerRadius(int radius) {
    m_radius = radius;
    update();
}

void ChatArea::setSidebarCollapsed(bool collapsed) {
    m_sidebarCollapsed = collapsed;
    m_collapsedControls->setVisible(collapsed);

    // The title-bar buttons live here permanently when the user picked the chat
    // header; otherwise they only appear while the sidebar (and its buttons) is hidden.
    const bool inTitleBar =
        SettingsManager::instance().windowControlsPlace() == "titlebar";
    m_controls->setVisible(collapsed || inTitleBar);
}

void ChatArea::applyWindowChrome() {
    const bool right = SettingsManager::instance().windowControlsPosition() == "right";
    const bool inTitleBar =
        SettingsManager::instance().windowControlsPlace() == "titlebar";
    const bool showControls = inTitleBar || m_sidebarCollapsed;

    while (QLayoutItem *item = m_topBar->takeAt(0)) delete item;

    m_controls->applyPreferences();

    if (!inTitleBar) {
        // Buttons belong to the sidebar; keep them out of this layout.
        m_controls->hide();
    }

    if (showControls && !right) m_topBar->addWidget(m_controls, 0, Qt::AlignVCenter);
    m_topBar->addWidget(m_collapsedControls, 0, Qt::AlignVCenter);
    m_topBar->addWidget(m_threadTitleLabel, 0, Qt::AlignVCenter);
    m_topBar->addWidget(m_statusPill, 0, Qt::AlignVCenter);
    m_topBar->addStretch();
    if (showControls && right) m_topBar->addWidget(m_controls, 0, Qt::AlignVCenter);

    if (showControls) m_controls->show();
    m_topBar->invalidate();
}

void ChatArea::updateStatusIndicator(const QString &text, bool active) {
    if (text.isEmpty()) {
        m_statusPill->hide();
    } else {
        m_statusPill->setText(text);
        if (active) {
            m_statusPill->setStyleSheet(
                "background: rgba(16, 185, 129, 0.15); border: 1px solid rgba(16, 185, 129, 0.4);"
                "border-radius: 12px; color: #34d399; font-size: 11px; font-weight: 600; padding: 2px 10px;");
        } else {
            m_statusPill->setStyleSheet(
                "background: rgba(56, 189, 248, 0.15); border: 1px solid rgba(56, 189, 248, 0.4);"
                "border-radius: 12px; color: #38bdf8; font-size: 11px; font-weight: 600; padding: 2px 10px;");
        }
        m_statusPill->show();
    }
}

void ChatArea::updateSendButtonState() {
    if (m_busy) {
        m_sendBtn->setIcon(icons::icon(icons::IconType::Square, Qt::white));
        m_sendBtn->setStyleSheet("QPushButton { background: #ef4444; border: none; border-radius: 17px; } QPushButton:hover { background: #dc2626; }");
        m_sendBtn->setEnabled(true);
        m_sendBtn->setToolTip("Stop agent");
    } else {
        const bool hasText = !m_input->text().trimmed().isEmpty();
        m_sendBtn->setIcon(icons::icon(icons::IconType::ArrowUp, hasText ? QColor(24, 24, 27) : QColor(161, 161, 170)));
        m_sendBtn->setStyleSheet(hasText
            ? "QPushButton { background: #10b981; border: none; border-radius: 17px; } QPushButton:hover { background: #34d399; }"
            : "QPushButton { background: #3f3f46; border: none; border-radius: 17px; }");
        m_sendBtn->setEnabled(hasText);
        m_sendBtn->setToolTip("Send message");
    }
}

void ChatArea::loadCurrentSession() {
    // Clear displayed message widgets (except empty state and trailing stretch).
    // Items may be either widgets (action cards, message rows) or bare sub-layouts;
    // deleting a sub-layout alone leaves its child widgets alive and visible, so
    // destroy those widgets explicitly before dropping the layout.
    while (m_msgLayout->count() > 2) {
        QLayoutItem *item = m_msgLayout->takeAt(1);
        if (!item) break;
        if (QLayout *sub = item->layout()) {
            while (QLayoutItem *child = sub->takeAt(0)) {
                delete child->widget();
                delete child;
            }
            delete sub;  // `sub` is the same object as `item`
        } else {
            delete item->widget();
            delete item;
        }
    }

    const auto s = SessionManager::instance().currentSession();
    m_history.clear();

    m_hasMessages = !s.messages.isEmpty();
    m_emptyStateWidget->setVisible(!m_hasMessages);
    m_threadTitleLabel->setText(s.title.isEmpty() ? "New Chat" : s.title);

    for (const auto &msg : s.messages) {
        if (msg.role == "thinking") {
            // Persisted reasoning: render read-only, never feed it back to the model.
            QLabel *body = nullptr;
            QLabel *detail = nullptr;
            m_msgLayout->insertWidget(m_msgLayout->count() - 1,
                                       buildThinkingRow(msg.content, false, &body, &detail));
            continue;
        }
        addMessage(msg.content, msg.role == "user");
        m_history.append({msg.role, msg.content});
    }

    // A rebuild destroys the live thinking bubble; restore it while a run is active.
    if (m_busy) {
        m_thinkingRow = nullptr;
        showThinkingBubble(m_thinkingDetailText.isEmpty() ? "Thinking" : m_thinkingDetailText);
    }

    if (!s.provider.isEmpty() && !s.model.isEmpty()) {
        m_modelSelector->setSelectedModel(s.provider, s.model);
        SettingsManager::instance().setProvider(s.provider);
        SettingsManager::instance().setModel(s.model, s.provider);
    } else {
        const QString p = SettingsManager::instance().provider();
        const QString m = SettingsManager::instance().model(p);
        m_modelSelector->setSelectedModel(p, m);
    }
    update();
}

void ChatArea::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const bool dark = ThemeManager::instance().isDark();

    QPainterPath clip;
    clip.addRoundedRect(QRectF(-m_radius, 0, width() + m_radius, height()), m_radius, m_radius);
    p.setClipPath(clip);
    p.fillRect(rect(), dark ? QColor(24, 24, 27) : QColor(255, 255, 255));
}

void ChatArea::addMessage(const QString &text, bool fromUser) {
    const bool dark = ThemeManager::instance().isDark();

    // Each row lives in its own container widget so loadCurrentSession() can delete
    // it as a whole. A bare QHBoxLayout has no widget() to delete, which used to
    // orphan the bubbles and leave them floating over the canvas.
    auto *rowWidget = new QWidget(this);
    rowWidget->setStyleSheet("background: transparent;");
    auto *row = new QHBoxLayout(rowWidget);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);

    if (fromUser) {
        row->addStretch();
        auto *bubble = new QLabel(text, rowWidget);
        bubble->setWordWrap(true);
        bubble->setMaximumWidth(540);
        bubble->setTextInteractionFlags(Qt::TextSelectableByMouse);
        bubble->setStyleSheet(
            "background: #27272a; color: #f4f4f5; border: 1px solid #3f3f46;"
            "border-radius: 14px; padding: 10px 16px; font-size: 14px; line-height: 1.45;");
        row->addWidget(bubble);
    } else {
        auto *card = new QFrame(rowWidget);
        card->setObjectName("assistantCard");
        card->setStyleSheet(
            "QFrame#assistantCard { background: transparent; border: none; }");
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(0, 0, 0, 0);
        cardLayout->setSpacing(6);

        // Header: Bot avatar + Name
        auto *hdr = new QHBoxLayout;
        hdr->setSpacing(6);
        auto *botIcon = new QLabel(card);
        botIcon->setPixmap(icons::pixmap(icons::IconType::Bot, QSize(18, 18), QColor(16, 185, 129)));
        hdr->addWidget(botIcon);

        auto *name = new QLabel("M-Cowork", card);
        name->setStyleSheet("font-size: 13px; font-weight: 600; color: #f4f4f5;");
        hdr->addWidget(name);
        hdr->addStretch();
        cardLayout->addLayout(hdr);

        // Text body
        auto *body = new QLabel(card);
        body->setTextFormat(Qt::MarkdownText);
        body->setOpenExternalLinks(true);
        body->setText(text);
        body->setWordWrap(true);
        body->setTextInteractionFlags(Qt::TextSelectableByMouse);
        body->setStyleSheet(dark ? "color: #e4e4e7; font-size: 14px; line-height: 1.5;"
                                 : "color: #18181b; font-size: 14px; line-height: 1.5;");
        cardLayout->addWidget(body);

        row->addWidget(card, 1);
    }

    m_msgLayout->insertWidget(m_msgLayout->count() - 1, rowWidget);
    QTimer::singleShot(10, this, [this] {
        m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->maximum());
    });
}

void ChatArea::addActionCard(const QString &toolName, const QJsonObject &args) {
    auto *card = new ActionStepCard(toolName, args, this);
    m_currentActionCard = card;
    m_msgLayout->insertWidget(m_msgLayout->count() - 1, card);
    QTimer::singleShot(10, this, [this] {
        m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->maximum());
    });
}

// Builds the assistant-style "thinking" bubble. When `live`, the caller drives the
// body/detail labels through show/updateThinkingBubble(); otherwise it is a static,
// persisted reasoning card shown after the fact.
QWidget *ChatArea::buildThinkingRow(const QString &text, bool live,
                                    QLabel **bodyOut, QLabel **detailOut) {
    const bool dark = ThemeManager::instance().isDark();

    auto *rowWidget = new QWidget(this);
    rowWidget->setStyleSheet("background: transparent;");
    auto *row = new QHBoxLayout(rowWidget);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);

    auto *card = new QFrame(rowWidget);
    card->setObjectName("thinkCard");
    card->setStyleSheet(dark
        ? "QFrame#thinkCard { background: #1b1b1f; border: 1px solid #2e2e33; border-radius: 14px; }"
        : "QFrame#thinkCard { background: #f4f4f5; border: 1px solid #e4e4e7; border-radius: 14px; }");

    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(14, 10, 14, 12);
    cardLayout->setSpacing(6);

    auto *hdr = new QHBoxLayout;
    hdr->setSpacing(6);
    auto *botIcon = new QLabel(card);
    botIcon->setPixmap(icons::pixmap(icons::IconType::Bot, QSize(15, 15), QColor(16, 185, 129)));
    hdr->addWidget(botIcon);
    auto *name = new QLabel(live ? "M-Cowork is thinking" : "M-Cowork's thoughts", card);
    name->setStyleSheet(dark ? "font-size: 12px; font-weight: 600; color: #a1a1aa;"
                             : "font-size: 12px; font-weight: 600; color: #52525b;");
    hdr->addWidget(name);
    hdr->addStretch();
    cardLayout->addLayout(hdr);

    auto *body = new QLabel(card);
    body->setWordWrap(true);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->setText(text);
    body->setStyleSheet(dark ? (live ? "color: #e4e4e7; font-size: 13px; font-style: italic;"
                                     : "color: #d4d4d8; font-size: 13px; font-style: italic; line-height: 1.45;")
                             : "color: #3f3f46; font-size: 13px; font-style: italic; line-height: 1.45;");
    cardLayout->addWidget(body);

    auto *detail = new QLabel(card);
    detail->setWordWrap(true);
    detail->setStyleSheet(dark ? "color: #71717a; font-size: 12px;" : "color: #71717a; font-size: 12px;");
    detail->setVisible(live);
    cardLayout->addWidget(detail);

    row->addWidget(card, 1);

    if (bodyOut) *bodyOut = body;
    if (detailOut) *detailOut = detail;
    return rowWidget;
}

void ChatArea::showThinkingBubble(const QString &detail) {
    if (!m_thinkingRow) {
        m_thinkingDots = 0;
        m_thinkingClock.start();

        QLabel *body = nullptr;
        QLabel *detailLbl = nullptr;
        QWidget *row = buildThinkingRow(QString(), true, &body, &detailLbl);
        m_thinkingBody = body;
        m_thinkingDetail = detailLbl;
        m_msgLayout->insertWidget(m_msgLayout->count() - 1, row);
        m_thinkingRow = row;

        if (!m_thinkingTick) {
            m_thinkingTick = new QTimer(this);
            m_thinkingTick->setInterval(400);
            connect(m_thinkingTick, &QTimer::timeout, this, [this] {
                if (!m_thinkingRow) {
                    m_thinkingTick->stop();
                    return;
                }
                m_thinkingDots = (m_thinkingDots + 1) % 4;
                if (m_thinkingBody) {
                    m_thinkingBody->setText(QString("Thinking%1").arg(QString(m_thinkingDots, '.')));
                }
                if (m_thinkingDetail) {
                    const qint64 secs = m_thinkingClock.elapsed() / 1000;
                    m_thinkingDetail->setText(secs >= 1
                        ? QString("%1  ·  %2s").arg(m_thinkingDetailText).arg(secs)
                        : m_thinkingDetailText);
                }
            });
        }
        m_thinkingTick->start();
    }

    updateThinkingBubble(detail);

    QTimer::singleShot(10, this, [this] {
        m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->maximum());
    });
}

void ChatArea::updateThinkingBubble(const QString &detail) {
    if (!detail.isEmpty()) m_thinkingDetailText = detail;
    if (m_thinkingBody) {
        m_thinkingBody->setText(QString("Thinking%1").arg(QString(m_thinkingDots, '.')));
    }
    if (m_thinkingDetail) m_thinkingDetail->setText(m_thinkingDetailText);
}

void ChatArea::removeThinkingBubble() {
    if (m_thinkingTick) m_thinkingTick->stop();
    if (m_thinkingRow) {
        m_thinkingRow->deleteLater();
        m_thinkingRow = nullptr;
    }
    m_thinkingBody = nullptr;
    m_thinkingDetail = nullptr;
    m_thinkingDetailText.clear();
}

void ChatArea::addErrorMessage(const QString &errorMessage) {
    auto *card = new QFrame(this);
    card->setObjectName("errorCard");
    card->setStyleSheet(
        "QFrame#errorCard {"
        "  background: rgba(239, 68, 68, 0.1);"
        "  border: 1px solid rgba(239, 68, 68, 0.3);"
        "  border-radius: 8px;"
        "}");
    auto *l = new QHBoxLayout(card);
    l->setContentsMargins(12, 10, 12, 10);
    l->setSpacing(10);

    auto *errIcon = new QLabel(card);
    errIcon->setPixmap(icons::pixmap(icons::IconType::X, QSize(16, 16), QColor(248, 113, 113)));
    l->addWidget(errIcon);

    auto *errLbl = new QLabel(errorMessage, card);
    errLbl->setWordWrap(true);
    errLbl->setStyleSheet("color: #fca5a5; font-size: 13px; font-weight: 500;");
    l->addWidget(errLbl, 1);

    auto *retryBtn = new QPushButton("Retry", card);
    retryBtn->setIcon(icons::icon(icons::IconType::RotateCcw, QColor(248, 113, 113)));
    retryBtn->setCursor(Qt::PointingHandCursor);
    retryBtn->setFixedHeight(28);
    retryBtn->setStyleSheet(
        "QPushButton {"
        "  background: rgba(239, 68, 68, 0.2);"
        "  color: #fecaca;"
        "  border: 1px solid rgba(239, 68, 68, 0.4);"
        "  border-radius: 4px;"
        "  padding: 0 10px;"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background: rgba(239, 68, 68, 0.35); }");
    connect(retryBtn, &QPushButton::clicked, this, [this, card] {
        card->hide();
        card->deleteLater();
        runAgentLoop();
    });
    l->addWidget(retryBtn);

    m_msgLayout->insertWidget(m_msgLayout->count() - 1, card);
    QTimer::singleShot(10, this, [this] {
        m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->maximum());
    });
}

void ChatArea::send() {
    const QString t = m_input->text().trimmed();
    if (t.isEmpty() || m_busy) return;
    m_input->clear();
    updateSendButtonState();

    const QString currentSid = SessionManager::instance().currentSessionId();

    if (t == "/help") {
        addMessage(t, true);
        const QString help = "Available commands:\n"
                             "• /tools — List available computer control tools\n"
                             "• /tool <name> [json] — Execute an OS tool directly\n"
                             "• /tool end_session — End active tool control session\n"
                             "• /tool-policy <name> <inherit|ask|session|allow> — Configure tool permissions\n"
                             "• /models — List available models for active provider\n"
                             "• /clear — Clear messages in current session\n"
                             "• /help — Show this help message";
        addMessage(help, false);
        return;
    }

    if (t == "/clear") {
        SessionManager::instance().deleteSessionPermanently(currentSid);
        SessionManager::instance().createSession("New Chat");
        return;
    }

    if (t == "/models") {
        addMessage(t, true);
        const QString p = SettingsManager::instance().provider();
        m_ai.fetchAvailableModels(p, [this, p](bool ok, const QStringList &models, const QString &err) {
            if (ok && !models.isEmpty()) {
                QString out = QString("Models for %1 (%2):\n").arg(p).arg(models.size());
                for (const auto &m : models) out += QString("• %1\n").arg(m);
                addMessage(out.trimmed(), false);
            } else {
                addMessage(QString("Failed to fetch models: %1").arg(err), false);
            }
        });
        return;
    }

    if (t == "/tool-policy" || t.startsWith("/tool-policy ")) {
        addMessage(t, true);
        const parts = t.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() < 2 || parts.size() > 3) {
            addMessage("Usage: /tool-policy <tool-name> <inherit|ask|session|allow>\nUse /tools to see tool names. 'inherit' follows the global confirmation setting; 'ask' prompts every time; 'session' asks once per app session; 'allow' permanently skips confirmation for that tool.", false);
            return;
        }
        const QString toolName = parts[1];
        bool knownTool = false;
        for (const auto &value : m_tool.definitions()) {
            if (value.toObject().value("name").toString() == toolName) { knownTool = true; break; }
        }
        if (!knownTool) {
            addMessage(QString("Unknown tool '%1'. Use /tools to see available names.").arg(toolName), false);
            return;
        }
        if (parts.size() == 2) {
            addMessage(QString("Safety policy for %1: %2").arg(toolName, SettingsManager::instance().toolSafetyPolicy(toolName)), false);
            return;
        }
        const QString policy = parts[2].toLower();
        if (policy != "inherit" && policy != "ask" && policy != "session" && policy != "allow") {
            addMessage("Invalid policy. Choose inherit, ask, session, or allow.", false);
            return;
        }
        SettingsManager::instance().setToolSafetyPolicy(toolName, policy);
        m_sessionApprovedTools.remove(toolName);
        addMessage(QString("Safety policy for %1 set to %2.").arg(toolName, policy), false);
        return;
    }

    if (t == "/tools") {
        addMessage(t, true);
        if (!m_tool.available()) {
            addMessage("Error: matricX-cowork tool binary not found. Build it first with scripts/build.ps1 or build.sh.", false);
            return;
        }
        const QJsonArray defs = m_tool.definitions();
        if (defs.isEmpty()) {
            addMessage("No tool definitions returned from matricX-cowork --describe.", false);
            return;
        }
        QString out = QString("Available Tools (%1):\n").arg(defs.size());
        for (const auto &val : defs) {
            const QJsonObject obj = val.toObject();
            out += QString("• %1: %2\n").arg(obj["name"].toString(), obj["description"].toString());
        }
        addMessage(out.trimmed(), false);
        return;
    }

    if (t.startsWith("/tool ") || t == "/tool") {
        addMessage(t, true);
        if (!m_tool.available()) {
            addMessage("Error: matricX-cowork tool binary not found. Build it first.", false);
            return;
        }
        const QString rest = t.mid(5).trimmed();
        if (rest.isEmpty()) {
            addMessage("Usage: /tool <name> [{\"key\": value, ...}]\nExample: /tool mouse_move {\"x\":500,\"y\":300}", false);
            return;
        }
        const int spaceIdx = rest.indexOf(' ');
        const QString toolName = (spaceIdx == -1) ? rest : rest.left(spaceIdx).trimmed();
        const QString jsonStr = (spaceIdx == -1) ? QString() : rest.mid(spaceIdx + 1).trimmed();

        QJsonObject args;
        if (!jsonStr.isEmpty()) {
            QJsonParseError parseErr;
            const QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &parseErr);
            if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
                addMessage(QString("JSON parse error: %1").arg(parseErr.errorString()), false);
                return;
            }
            args = doc.object();
        }

        addActionCard(toolName, args);

        m_tool.call(toolName, args, [this, toolName](const QJsonObject &reply) {
            const bool ok = reply["ok"].toBool();
            QString text;
            QString imgPath;
            if (ok) {
                QJsonObject copy = reply;
                copy.remove("id");
                copy.remove("ok");
                if (copy.contains("path")) {
                    imgPath = copy.value("path").toString();
                }
                text = QString::fromUtf8(QJsonDocument(copy).toJson(QJsonDocument::Indented)).trimmed();
                if (m_currentActionCard) {
                    m_currentActionCard->setStatus(ActionStepCard::Status::Success, text, imgPath);
                }
            } else {
                text = reply["error"].toString("unknown error");
                if (m_currentActionCard) {
                    m_currentActionCard->setStatus(ActionStepCard::Status::Failed, text);
                }
            }
        });
        return;
    }

    // Regular message
    SessionManager::instance().appendMessage(currentSid, "user", t);
    m_history.append({"user", t});

    runAgentLoop();
}

void ChatArea::runAgentLoop() {
    m_busy = true;
    m_input->setEnabled(false);
    updateSendButtonState();
    updateStatusIndicator(QString(), false);
    showThinkingBubble("Waiting for the model");

    QJsonArray tools = m_tool.definitions();
    const QString currentSid = SessionManager::instance().currentSessionId();

    m_ai.sendMessage(m_history, tools, [this, currentSid](const AiResponse &resp) {
        if (!resp.success) {
            m_busy = false;
            m_input->setEnabled(true);
            m_input->setFocus();
            updateSendButtonState();
            updateStatusIndicator(QString(), false);
            removeThinkingBubble();
            addErrorMessage(resp.errorMessage);
            return;
        }

        // Persist any provider-supplied reasoning so it stays visible in the transcript.
        if (!resp.reasoning.trimmed().isEmpty() && SettingsManager::instance().showReasoning()) {
            SessionManager::instance().appendMessage(currentSid, "thinking", resp.reasoning.trimmed());
        }

        if (!resp.text.isEmpty()) {
            SessionManager::instance().appendMessage(currentSid, "assistant", resp.text);
            m_history.append({"assistant", resp.text});
        }

        if (resp.toolCalls.isEmpty()) {
            m_busy = false;
            m_input->setEnabled(true);
            m_input->setFocus();
            updateSendButtonState();
            updateStatusIndicator(QString(), false);
            removeThinkingBubble();
            return;
        }

        executeNextToolCall(resp.toolCalls, 0);
    });
}

void ChatArea::executeNextToolCall(const QList<ToolCallRequest> &calls, int index) {
    if (index >= calls.size()) {
        runAgentLoop();
        return;
    }

    const auto &tc = calls[index];
    updateStatusIndicator(QString(), false);
    updateThinkingBubble(QString("Running %1").arg(tc.name));

    addActionCard(tc.name, tc.arguments);

    auto proceed = [this, calls, index, tc]() {
        m_tool.call(tc.name, tc.arguments, [this, calls, index, tc](const QJsonObject &reply) {
            const bool ok = reply["ok"].toBool();
            QString imgPath;
            if (reply.contains("path")) imgPath = reply.value("path").toString();
            const QString toolResultStr = QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact));

            if (m_currentActionCard) {
                m_currentActionCard->setStatus(ok ? ActionStepCard::Status::Success : ActionStepCard::Status::Failed,
                                               toolResultStr, imgPath);
            }

            // If the tool was screenshot, attach the image path so the AI model perceives the desktop
            if (tc.name == "screenshot" && ok && !imgPath.isEmpty()) {
                m_history.append({"user", QString("Tool result for %1: %2").arg(tc.name, toolResultStr), imgPath});
            } else {
                m_history.append({"user", QString("Tool result for %1: %2").arg(tc.name, toolResultStr), QString()});
            }
            executeNextToolCall(calls, index + 1);
        });
    };

    if (SettingsManager::instance().requireConfirmation()) {
        auto *confirmFrame = new QFrame(this);
        confirmFrame->setObjectName("confirmFrame");
        confirmFrame->setStyleSheet(
            "QFrame#confirmFrame {"
            "  background: #27272a;"
            "  border: 1px solid #eab308;"
            "  border-radius: 8px;"
            "}");
        auto *cLayout = new QHBoxLayout(confirmFrame);
        cLayout->setContentsMargins(12, 8, 12, 8);
        cLayout->setSpacing(10);

        auto *promptLbl = new QLabel(QString("Permission: Allow M-Cowork to run '%1'?").arg(tc.name), confirmFrame);
        promptLbl->setStyleSheet("color: #fef08a; font-size: 13px; font-weight: 500;");
        cLayout->addWidget(promptLbl, 1);

        auto *allowBtn = new QPushButton("Allow", confirmFrame);
        allowBtn->setCursor(Qt::PointingHandCursor);
        allowBtn->setStyleSheet("background: #10b981; color: white; border: none; border-radius: 4px; padding: 4px 12px; font-weight: 600; font-size: 12px;");
        cLayout->addWidget(allowBtn);

        auto *denyBtn = new QPushButton("Deny", confirmFrame);
        denyBtn->setCursor(Qt::PointingHandCursor);
        denyBtn->setStyleSheet("background: #3f3f46; color: #d4d4d8; border: none; border-radius: 4px; padding: 4px 12px; font-size: 12px;");
        cLayout->addWidget(denyBtn);

        m_msgLayout->insertWidget(m_msgLayout->count() - 1, confirmFrame);

        connect(allowBtn, &QPushButton::clicked, this, [confirmFrame, proceed] {
            confirmFrame->hide();
            confirmFrame->deleteLater();
            proceed();
        });
        connect(denyBtn, &QPushButton::clicked, this, [this, confirmFrame, tc, calls, index] {
            confirmFrame->hide();
            confirmFrame->deleteLater();
            if (m_currentActionCard) {
                m_currentActionCard->setStatus(ActionStepCard::Status::Failed, "User denied execution permission.");
            }
            m_history.append({"user", QString("Tool execution for %1 denied by user.").arg(tc.name)});
            executeNextToolCall(calls, index + 1);
        });
    } else {
        proceed();
    }
}

}  // namespace mcw
