#include "task_workspace_page.hpp"

#include "support/qt_path.hpp"
#include "ui/components/evidence_panel.hpp"
#include "ui/components/run_conversation.hpp"
#include "ui/components/settings_panel.hpp"
#include "ui/components/task_composer.hpp"
#include "ui/components/task_sidebar.hpp"
#include "ui/components/workspace_split_view.hpp"
#include "ui/support/localized_text.hpp"
#include "ui/theme/layout_metrics.hpp"
#include "ui/theme/mint_theme.hpp"

#include <FluentQt/BasicInput.h>
#include <FluentQt/Collections.h>
#include <FluentQt/Design.h>

#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPalette>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <optional>

namespace mint::ui {
namespace {

std::string string_from_qstring(const QString& value) {
    const auto bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

std::optional<desktop::CustomModelAdapter> custom_model_adapter_from_qstring(const QString& value) {
    if (value == QStringLiteral("chat_completions")) {
        return desktop::CustomModelAdapter::chat_completions;
    }
    if (value == QStringLiteral("responses")) {
        return desktop::CustomModelAdapter::responses;
    }
    if (value == QStringLiteral("anthropic_messages")) {
        return desktop::CustomModelAdapter::anthropic_messages;
    }
    return std::nullopt;
}

} // namespace

TaskWorkspacePage::TaskWorkspacePage(desktop::TaskWorkspaceController& controller,
                                     desktop::ModelSettingsController& model_settings,
                                     desktop::PreferencesController& preferences, QWidget* parent)
    : QWidget(parent), controller_(controller), model_settings_(model_settings),
      preferences_(preferences) {
    setObjectName(QStringLiteral("MintRoot"));
    setAutoFillBackground(false);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    split_view_ = new WorkspaceSplitView(this);

    sidebar_ = new TaskSidebar;
    fluent::collections::SplitViewPaneOptions sidebar_options;
    sidebar_options.minimumSize = metrics::task_sidebar_minimum_width;
    sidebar_options.preferredSize = metrics::task_sidebar_preferred_width;
    sidebar_options.maximumSize = metrics::task_sidebar_maximum_width;
    split_view_->addPane(sidebar_, sidebar_options);

    auto* main_surface = build_main_surface();
    fluent::collections::SplitViewPaneOptions main_options;
    main_options.minimumSize = metrics::workspace_minimum_width;
    main_options.preferredSize = metrics::conversation_maximum_width;
    main_options.fill = true;
    split_view_->addPane(main_surface, main_options);
    layout->addWidget(split_view_);

    connect(split_view_, &fluent::collections::SplitView::paneSizeChanged, this, [this](int, int) {
        emit navigation_surface_width_changed(navigation_surface_width());
    });

    evidence_panel_ = new EvidencePanel;
    evidence_drawer_ = new fluent::collections::DrawerView(this);
    evidence_drawer_->setObjectName(QStringLiteral("MintEvidenceDrawer"));
    evidence_drawer_->setEdge(fluent::collections::DrawerView::DrawerEdge::Right);
    evidence_drawer_->setDrawerLength(metrics::evidence_maximum_width);
    evidence_drawer_->setModal(true);
    evidence_drawer_->setDim(true);
    evidence_drawer_->setInteractive(false);
    evidence_drawer_->setContentWidget(evidence_panel_, fluent::WidgetOwnership::Owned);

    settings_panel_ = new SettingsPanel;
    settings_drawer_ = new fluent::collections::DrawerView(this);
    settings_drawer_->setObjectName(QStringLiteral("MintSettingsDrawer"));
    settings_drawer_->setEdge(fluent::collections::DrawerView::DrawerEdge::Right);
    settings_drawer_->setDrawerLength(metrics::settings_width);
    settings_drawer_->setModal(true);
    settings_drawer_->setDim(true);
    settings_drawer_->setInteractive(false);
    settings_drawer_->setContentWidget(settings_panel_, fluent::WidgetOwnership::Owned);

    connect_actions();
    controller_.set_listener([this](desktop::WorkspaceChange change) { render(change); });
    model_settings_.set_listener([this] {
        settings_panel_->set_model_state(model_settings_.state());
        controller_.set_model_execution_available(model_settings_.state().can_start_task());
        render_model_gate();
        if (model_settings_.state().legacy_conflict && !migration_prompt_shown_ &&
            !controller_.state().running) {
            migration_prompt_shown_ = true;
            QTimer::singleShot(0, this, [this] {
                if (model_settings_.state().legacy_conflict) {
                    open_settings();
                }
            });
        }
    });
    model_settings_.set_saved_listener([this] {
        controller_.set_config({});
        settings_panel_->set_model_persistence_error(false);
    });
    controller_.set_model_execution_available(model_settings_.state().can_start_task());
    render(desktop::WorkspaceChange::all);
    settings_panel_->set_model_state(model_settings_.state());
    QTimer::singleShot(0, this, [this] {
        emit navigation_surface_width_changed(navigation_surface_width());
        controller_.activate();
        model_settings_.activate();
    });
}

TaskWorkspacePage::~TaskWorkspacePage() {
    controller_.set_listener({});
    model_settings_.set_listener({});
    model_settings_.set_saved_listener({});
}

QWidget* TaskWorkspacePage::build_main_surface() {
    main_surface_ = new QWidget;
    main_surface_->setObjectName(QStringLiteral("MintMainSurface"));
    auto main_palette = main_surface_->palette();
    main_palette.setColor(QPalette::Window, theme::palette().canvas);
    main_surface_->setPalette(main_palette);
    main_surface_->setAutoFillBackground(true);
    auto* main_layout = new QVBoxLayout(main_surface_);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    conversation_canvas_ = new QWidget(main_surface_);
    conversation_canvas_->setObjectName(QStringLiteral("MintConversationCanvas"));
    auto* layers = new QGridLayout(conversation_canvas_);
    layers->setContentsMargins(0, 0, 0, 0);
    layers->setSpacing(0);

    conversation_ = new RunConversation(conversation_canvas_);
    layers->addWidget(conversation_, 0, 0);

    auto* composer_layer = new QWidget(conversation_canvas_);
    composer_layer->setObjectName(QStringLiteral("MintComposerLayer"));
    composer_layer->setAutoFillBackground(false);
    composer_layer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    auto* composer_layout = new QHBoxLayout(composer_layer);
    composer_layout->setContentsMargins(metrics::conversation_horizontal_padding, 0,
                                        metrics::conversation_horizontal_padding,
                                        metrics::space_lg);
    composer_layout->setSpacing(0);
    composer_layout->addStretch();
    composer_ = new TaskComposer(composer_layer);
    composer_->setMaximumWidth(metrics::conversation_maximum_width);
    composer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    composer_layout->addWidget(composer_, 1);
    composer_layout->addStretch();
    layers->addWidget(composer_layer, 0, 0, Qt::AlignBottom);
    composer_layer->raise();
    connect(composer_, &TaskComposer::height_changed, this,
            [this](int height) { update_composer_clearance(height); });
    QTimer::singleShot(0, this, [this] { update_composer_clearance(composer_->height()); });

    jump_to_latest_ = new fluent::basicinput::Button(conversation_canvas_);
    jump_to_latest_->setObjectName(QStringLiteral("MintJumpToLatestButton"));
    jump_to_latest_->setFluentLayout(fluent::basicinput::Button::IconOnly);
    jump_to_latest_->setFluentStyle(fluent::basicinput::Button::Standard);
    jump_to_latest_->setFluentSize(fluent::basicinput::Button::Small);
    jump_to_latest_->setIconGlyph(Typography::Icons::Down, Typography::IconSize::Standard);
    jump_to_latest_->setIconOffset(QPoint(0, 1));
    jump_to_latest_->setFixedSize(metrics::jump_to_latest_size, metrics::jump_to_latest_size);
    jump_to_latest_->setCornerRadii(
        QMargins(metrics::jump_to_latest_size / 2, metrics::jump_to_latest_size / 2,
                 metrics::jump_to_latest_size / 2, metrics::jump_to_latest_size / 2));
    jump_to_latest_->setFocusPolicy(Qt::StrongFocus);
    jump_to_latest_->setToolTip(text(localization::Message::gui_action_jump_to_latest));
    jump_to_latest_->setAccessibleName(jump_to_latest_->toolTip());
    jump_to_latest_->hide();
    jump_to_latest_->raise();
    connect(jump_to_latest_, &QPushButton::clicked, conversation_,
            &RunConversation::scroll_to_latest);
    connect(conversation_, &RunConversation::jump_to_latest_availability_changed, this,
            [this](bool available) {
                jump_to_latest_->setVisible(available);
                if (available) {
                    position_jump_to_latest();
                    jump_to_latest_->raise();
                }
            });

    main_layout->addWidget(conversation_canvas_, 1);
    return main_surface_;
}

void TaskWorkspacePage::connect_actions() {
    connect(sidebar_, &TaskSidebar::add_workspace_requested, this,
            &TaskWorkspacePage::choose_project);
    connect(sidebar_, &TaskSidebar::workspace_selected, this, [this](const QString& path) {
        pending_new_task_root_.reset();
        const auto root = desktop::normalize_workspace_root(mint::gui::path_from_qstring(path));
        if (root == controller_.state().root) {
            sidebar_->render(controller_.state(), desktop::WorkspaceChange::tasks);
            return;
        }
        if (!controller_.set_root(root)) {
            sidebar_->render(controller_.state(), desktop::WorkspaceChange::selection);
            return;
        }
        model_settings_.set_root(controller_.state().root);
    });
    connect(sidebar_, &TaskSidebar::workspace_remove_requested, this, [this](const QString& path) {
        const auto previous_root = controller_.state().root;
        if (!controller_.remove_root(mint::gui::path_from_qstring(path))) {
            sidebar_->render(controller_.state(), desktop::WorkspaceChange::tasks);
            return;
        }
        if (controller_.state().root != previous_root) {
            model_settings_.set_root(controller_.state().root);
        }
    });
    connect(sidebar_, &TaskSidebar::new_task_requested, this, [this](const QString& path) {
        const auto root = desktop::normalize_workspace_root(mint::gui::path_from_qstring(path));
        if (root == controller_.state().root) {
            sidebar_->render(controller_.state(), desktop::WorkspaceChange::tasks);
            controller_.create_task();
            composer_->focus_input();
            return;
        }

        if (!controller_.set_root(root)) {
            sidebar_->render(controller_.state(), desktop::WorkspaceChange::tasks);
            return;
        }
        pending_new_task_root_ = root;
        model_settings_.set_root(controller_.state().root);
    });
    connect(sidebar_, &TaskSidebar::task_selected, this, [this](const QString& task_id) {
        pending_new_task_root_.reset();
        if (!controller_.select_task(string_from_qstring(task_id))) {
            sidebar_->render(controller_.state(), desktop::WorkspaceChange::selection);
        }
    });
    connect(sidebar_, &TaskSidebar::task_pin_requested, this, [this](const QString& task_id) {
        if (!controller_.toggle_task_pin(string_from_qstring(task_id))) {
            sidebar_->render(controller_.state(), desktop::WorkspaceChange::tasks);
        }
    });
    connect(sidebar_, &TaskSidebar::task_archive_requested, this, [this](const QString& task_id) {
        if (!controller_.archive_task(string_from_qstring(task_id))) {
            sidebar_->render(controller_.state(), desktop::WorkspaceChange::tasks);
        }
    });
    connect(sidebar_, &TaskSidebar::task_unarchive_requested, this, [this](const QString& task_id) {
        if (!controller_.unarchive_task(string_from_qstring(task_id))) {
            sidebar_->render(controller_.state(), desktop::WorkspaceChange::tasks);
        }
    });
    connect(sidebar_, &TaskSidebar::archived_view_requested, this, [this](bool show) {
        if (!controller_.show_archived_tasks(show)) {
            sidebar_->render(controller_.state(), desktop::WorkspaceChange::tasks);
        }
    });
    connect(conversation_, &RunConversation::approval_resolved, this,
            [this](bool approved) { controller_.resolve_approval(approved); });
    connect(conversation_, &RunConversation::evidence_selected, this,
            [this](QString content) { show_evidence(content); });
    connect(composer_, &TaskComposer::submitted, this, [this](std::string question) {
        const auto submitted_draft_key = draft_key_;
        if (controller_.start(std::move(question))) {
            drafts_.remove(submitted_draft_key);
            composer_->set_draft({});
        }
    });
    connect(composer_, &TaskComposer::cancel_requested, this, [this] { controller_.cancel(); });
    connect(evidence_panel_, &EvidencePanel::close_requested, evidence_drawer_,
            &fluent::collections::DrawerView::close);
    connect(settings_panel_, &SettingsPanel::close_requested, settings_drawer_,
            &fluent::collections::DrawerView::close);
    connect(settings_panel_, &SettingsPanel::language_selected, this, [this](auto language) {
        settings_panel_->set_persistence_error(!preferences_.set_language(language));
    });
    connect(settings_panel_, &SettingsPanel::theme_selected, this, [this](auto selected_theme) {
        settings_panel_->set_theme_persistence_error(!preferences_.set_theme(selected_theme));
    });
    connect(settings_panel_, &SettingsPanel::custom_model_save_requested, this,
            [this](QString adapter, QString base_url, QString model, QString api_key,
                   QString revision) {
                const auto selected_adapter = custom_model_adapter_from_qstring(adapter);
                if (!selected_adapter.has_value()) {
                    settings_panel_->set_model_persistence_error(true);
                    api_key.fill(QChar{});
                    return;
                }

                std::optional<std::string> custom_api_key;
                auto key_bytes = api_key.toUtf8();
                if (!api_key.trimmed().isEmpty()) {
                    custom_api_key.emplace(key_bytes.constData(),
                                           static_cast<std::size_t>(key_bytes.size()));
                }
                (void)model_settings_.save_custom_profile(
                    {.expected_revision = string_from_qstring(revision),
                     .adapter = *selected_adapter,
                     .base_url = string_from_qstring(base_url),
                     .model = string_from_qstring(model),
                     .api_key = std::move(custom_api_key)});
                key_bytes.fill('\0');
                api_key.fill(QChar{});
            });
    connect(settings_panel_, &SettingsPanel::api_key_remove_requested, this,
            [this](QString revision) {
                (void)model_settings_.remove_api_key(string_from_qstring(revision));
            });
    connect(settings_panel_, &SettingsPanel::model_reload_requested, this,
            [this] { model_settings_.refresh(); });
    connect(settings_panel_, &SettingsPanel::legacy_model_import_requested, this, [this] {
        (void)model_settings_.resolve_legacy_conflict(desktop::LegacyModelChoice::import_legacy);
    });
    connect(settings_panel_, &SettingsPanel::legacy_model_keep_requested, this, [this] {
        (void)model_settings_.resolve_legacy_conflict(desktop::LegacyModelChoice::keep_shared);
    });
}

void TaskWorkspacePage::render_model_gate() {
    const auto& state = controller_.state();
    const auto& model = model_settings_.state();
    QString reason;
    if (!state.model_execution_available) {
        reason =
            text(model.saving            ? localization::Message::gui_task_wait_for_model_save
                 : model.legacy_conflict ? localization::Message::gui_task_choose_model_settings
                 : model.loading || !model.provider
                     ? localization::Message::gui_settings_provider_loading
                     : localization::Message::gui_task_fix_model_settings);
    }
    composer_->set_model_unavailable_reason(std::move(reason));
    composer_->set_run_available(!state.running && state.model_execution_available);
}

void TaskWorkspacePage::render(desktop::WorkspaceChange change) {
    const auto& state = controller_.state();
    const auto next_draft_key = mint::gui::qstring_from_path(state.root) + QChar(0x1f) +
                                QString::fromStdString(state.task_id);
    if (draft_key_ != next_draft_key) {
        if (!draft_key_.isEmpty()) {
            const auto draft = composer_->draft();
            if (draft.isEmpty()) {
                drafts_.remove(draft_key_);
            } else {
                drafts_.insert(draft_key_, draft);
            }
        }
        draft_key_ = next_draft_key;
        composer_->set_draft(drafts_.value(draft_key_));
    }
    const auto title = QString::fromStdString(state.title);
    const auto can_resume = state.can_resume();
    if (header_title_ != title || header_can_resume_ != can_resume) {
        header_title_ = title;
        header_can_resume_ = can_resume;
        emit task_header_changed(title, can_resume);
    }
    if (change == desktop::WorkspaceChange::all || change == desktop::WorkspaceChange::tasks ||
        change == desktop::WorkspaceChange::selection ||
        change == desktop::WorkspaceChange::run_state ||
        change == desktop::WorkspaceChange::approval ||
        change == desktop::WorkspaceChange::content || change == desktop::WorkspaceChange::error) {
        sidebar_->render(state, change);
    }
    if (change == desktop::WorkspaceChange::all || change == desktop::WorkspaceChange::selection) {
        evidence_drawer_->close();
    }
    conversation_->render(state, change);
    composer_->set_workspace_available(!state.root.empty());
    composer_->set_running(state.viewing_running_task());
    render_model_gate();
    sidebar_->set_mutations_enabled(true);

    if (pending_new_task_root_.has_value() && !state.loading_tasks &&
        state.root == *pending_new_task_root_) {
        const auto requested_root = *pending_new_task_root_;
        pending_new_task_root_.reset();
        QTimer::singleShot(0, this, [this, requested_root] {
            if (controller_.state().root != requested_root) {
                return;
            }
            controller_.create_task();
            composer_->focus_input();
        });
    }
}

void TaskWorkspacePage::retranslate() {
    sidebar_->retranslate();
    conversation_->retranslate();
    composer_->retranslate();
    evidence_panel_->retranslate();
    settings_panel_->retranslate();
    settings_panel_->set_preferences(preferences_.preferences());
    settings_panel_->set_model_state(model_settings_.state());
    jump_to_latest_->setToolTip(text(localization::Message::gui_action_jump_to_latest));
    jump_to_latest_->setAccessibleName(jump_to_latest_->toolTip());
    if (project_dialog_ != nullptr) {
        project_dialog_->setWindowTitle(text(localization::Message::gui_project_pick_title));
    }
    render(desktop::WorkspaceChange::all);
}

void TaskWorkspacePage::refresh_theme() {
    if (main_surface_ != nullptr) {
        auto main_palette = main_surface_->palette();
        main_palette.setColor(QPalette::Window, theme::palette().canvas);
        main_surface_->setPalette(main_palette);
    }
    update();
    if (split_view_ != nullptr) {
        split_view_->update();
    }
    if (conversation_ != nullptr) {
        conversation_->update();
    }
    if (settings_panel_ != nullptr) {
        settings_panel_->set_model_state(model_settings_.state());
    }
}

void TaskWorkspacePage::choose_project() {
    if (project_dialog_ != nullptr) {
        project_dialog_->show();
        project_dialog_->raise();
        project_dialog_->activateWindow();
        return;
    }

    auto* dialog = new QFileDialog(this);
    project_dialog_ = dialog;
    dialog->setObjectName(QStringLiteral("MintProjectDialog"));
    dialog->setWindowTitle(text(localization::Message::gui_project_pick_title));
    dialog->setDirectory(mint::gui::qstring_from_path(controller_.state().root));
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setOption(QFileDialog::ShowDirsOnly, true);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QFileDialog::accepted, this, [this, dialog] {
        const auto directories = dialog->selectedFiles();
        if (directories.isEmpty()) {
            return;
        }
        const auto root = mint::gui::path_from_qstring(directories.front());
        if (controller_.set_root(root)) {
            model_settings_.set_root(controller_.state().root);
        }
    });
    connect(dialog, &QFileDialog::finished, this, [this, dialog] {
        if (project_dialog_ == dialog) {
            project_dialog_ = nullptr;
        }
    });
    dialog->open();
}

void TaskWorkspacePage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    evidence_drawer_->setDrawerLength(
        qMax(metrics::evidence_minimum_width,
             qMin(metrics::evidence_maximum_width, event->size().width() - metrics::space_xl * 2)));
    settings_drawer_->setDrawerLength(
        qMin(metrics::settings_width, event->size().width() - metrics::space_xl * 2));
    QTimer::singleShot(0, this, [this] {
        emit navigation_surface_width_changed(navigation_surface_width());
        position_jump_to_latest();
    });
}

int TaskWorkspacePage::navigation_surface_width() const {
    if (split_view_ != nullptr && split_view_->handleCount() > 0) {
        return split_view_->handleGeometry(0).left();
    }
    return sidebar_ != nullptr ? sidebar_->width() : 0;
}

void TaskWorkspacePage::show_evidence(const QString& content) {
    if (content.isEmpty()) {
        return;
    }
    settings_drawer_->close();
    evidence_panel_->set_evidence(content);
    evidence_drawer_->open();
}

void TaskWorkspacePage::update_composer_clearance(int composer_height) {
    const auto clearance = qMax(metrics::composer_content_clearance,
                                composer_height + metrics::space_lg + metrics::space_md);
    conversation_->set_bottom_inset(clearance);
    position_jump_to_latest();
}

void TaskWorkspacePage::position_jump_to_latest() {
    if (conversation_canvas_ == nullptr || composer_ == nullptr || jump_to_latest_ == nullptr) {
        return;
    }
    const auto composer_top = composer_->mapTo(conversation_canvas_, QPoint{}).y();
    const auto x = (conversation_canvas_->width() - jump_to_latest_->width()) / 2;
    const auto y = composer_top - jump_to_latest_->height() / 2;
    jump_to_latest_->move(qMax(0, x),
                          qBound(0, y, conversation_canvas_->height() - jump_to_latest_->height()));
}

void TaskWorkspacePage::open_settings() {
    const auto already_visible = settings_drawer_->isVisible();
    evidence_drawer_->close();
    if (!already_visible) {
        settings_panel_->set_preferences(preferences_.preferences());
        settings_panel_->set_model_state(model_settings_.state());
    }
    settings_drawer_->open();
    if (!already_visible) {
        model_settings_.refresh();
    }
}

} // namespace mint::ui
