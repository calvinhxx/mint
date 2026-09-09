#include "desktop_component_test_support.hpp"
#include "ui/components/conversation_transcript.hpp"
#include "ui/components/run_conversation.hpp"
#include "ui/components/task_composer.hpp"
#include "ui/components/task_sidebar.hpp"
#include "ui/delegates/run_timeline_delegate.hpp"
#include "ui/models/run_timeline_model.hpp"
#include "ui/pages/task_workspace_page.hpp"
#include "ui/shell/mint_window.hpp"
#include "ui/theme/layout_metrics.hpp"
#include "ui/theme/mint_theme.hpp"

#include <FluentQt/BasicInput.h>
#include <FluentQt/Collections.h>
#include <FluentQt/Scrolling.h>
#include <FluentQt/TextFields.h>

#include <QApplication>
#include <QImage>
#include <QListView>
#include <QMouseEvent>
#include <QPalette>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>

#include <gtest/gtest.h>

#include <cstddef>
#include <utility>

namespace {

using mint::test::ComponentRuntime;
using mint::test::MemoryPreferencesRepository;

TEST(DesktopComponents, WorkspaceUsesResizableMicaNavigationAndEdgeToEdgeOpaqueContent) {
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::MintWindow window(controller, model_settings, preferences);
    window.resize(720, 580);
    window.show();
    QApplication::processEvents();

    auto* sidebar = window.findChild<mint::ui::TaskSidebar*>();
    auto* page = window.findChild<mint::ui::TaskWorkspacePage*>();
    auto* main_surface = window.findChild<QWidget*>(QStringLiteral("MintMainSurface"));
    auto* transcript = window.findChild<mint::ui::ConversationTranscript*>(
        QStringLiteral("MintConversationTranscript"));
    auto* task_title =
        window.findChild<fluent::textfields::Label*>(QStringLiteral("MintTitleBarTaskTitle"));
    auto* title_bar_surface = window.findChild<QWidget*>(QStringLiteral("MintTitleBarSurface"));
    auto* split_view =
        window.findChild<fluent::collections::SplitView*>(QStringLiteral("MintWorkspaceSplitView"));
    ASSERT_NE(sidebar, nullptr);
    ASSERT_NE(page, nullptr);
    ASSERT_NE(main_surface, nullptr);
    ASSERT_NE(transcript, nullptr);
    ASSERT_NE(task_title, nullptr);
    ASSERT_NE(title_bar_surface, nullptr);
    ASSERT_NE(split_view, nullptr);
    ASSERT_EQ(split_view->paneCount(), 2);
    EXPECT_EQ(split_view->paneAt(0), sidebar);
    EXPECT_EQ(split_view->paneAt(1), main_surface);
    EXPECT_FALSE(sidebar->autoFillBackground());
    EXPECT_TRUE(main_surface->autoFillBackground());
    EXPECT_EQ(main_surface->palette().color(QPalette::Window), mint::ui::theme::palette().canvas);
    EXPECT_TRUE(split_view->isPaneFill(main_surface));
    EXPECT_EQ(split_view->paneMinimumSize(sidebar), mint::ui::metrics::task_sidebar_minimum_width);
    EXPECT_EQ(split_view->paneMaximumSize(sidebar), mint::ui::metrics::task_sidebar_maximum_width);
    EXPECT_TRUE(sidebar->isVisible());
    EXPECT_GT(main_surface->width(), 0);
    EXPECT_EQ(window.findChild<QWidget*>(QStringLiteral("MintSidebarToggle")), nullptr);

    const auto handle = split_view->handleGeometry(0);
    ASSERT_TRUE(handle.isValid());
    EXPECT_EQ(handle.width(), mint::ui::metrics::workspace_split_handle_width);
    QImage split_pixels(split_view->size(), QImage::Format_ARGB32_Premultiplied);
    split_pixels.fill(Qt::transparent);
    split_view->render(&split_pixels);
    EXPECT_EQ(split_pixels.pixelColor(handle.center()), mint::ui::theme::palette().canvas);

    QMouseEvent hover_event(QEvent::MouseMove, handle.center(),
                            split_view->mapToGlobal(handle.center()), Qt::NoButton, Qt::NoButton,
                            Qt::NoModifier);
    QApplication::sendEvent(split_view, &hover_event);
    EXPECT_EQ(split_view->cursor().shape(), Qt::SplitHCursor);

    window.resize(1280, 800);
    QApplication::processEvents();

    EXPECT_TRUE(sidebar->isVisible());
    EXPECT_GT(main_surface->width(), 0);
    const auto previous_width = sidebar->width();
    split_view->setPanePreferredSize(sidebar, previous_width + 48);
    QApplication::processEvents();
    EXPECT_GT(sidebar->width(), previous_width);

    const auto expected_title_boundary =
        window.titleBar()
            ->mapFromGlobal(page->mapToGlobal(QPoint(page->navigation_surface_width(), 0)))
            .x();
    EXPECT_EQ(title_bar_surface->x(), expected_title_boundary);
    EXPECT_EQ(title_bar_surface->geometry().right(), window.titleBar()->width() - 1);
    EXPECT_TRUE(window.titleBar()->isAncestorOf(task_title));
    EXPECT_GE(task_title->mapTo(window.titleBar(), QPoint{}).x(),
              expected_title_boundary + mint::ui::metrics::space_md);
    QImage title_pixels(title_bar_surface->size(), QImage::Format_ARGB32_Premultiplied);
    title_pixels.fill(Qt::transparent);
    title_bar_surface->render(&title_pixels);
    const auto sample_x = title_bar_surface->width() / 2;
    EXPECT_EQ(title_pixels.pixelColor(sample_x, 0), mint::ui::theme::palette().canvas);
    EXPECT_EQ(title_pixels.pixelColor(sample_x, title_bar_surface->height() - 1),
              mint::ui::theme::palette().stroke);

    ASSERT_TRUE(controller.start("Inspect timeline"));
    for (std::size_t index = 0; index < 40; ++index) {
        runtime.callbacks.event_received({.sequence = index,
                                          .kind = mint::desktop::RunEventKind::model_completed,
                                          .turn = index});
    }
    QApplication::processEvents();
    const QRect transcript_rect(transcript->mapTo(main_surface, QPoint{}), transcript->size());
    EXPECT_EQ(transcript_rect.left(), 0);
    EXPECT_EQ(transcript_rect.top(), 0);
    EXPECT_EQ(transcript_rect.right(), main_surface->width() - 1);
    auto* scrollbar = transcript->verticalFluentScrollBar();
    ASSERT_NE(scrollbar, nullptr);
    const auto scrollbar_right =
        scrollbar->mapTo(main_surface, QPoint(scrollbar->width() - 1, 0)).x();
    EXPECT_GE(scrollbar_right, main_surface->width() - mint::ui::metrics::space_sm);
    const QRect scrollbar_rect(scrollbar->mapTo(transcript, QPoint{}), scrollbar->size());
    EXPECT_LE(scrollbar_rect.top(), mint::ui::metrics::space_xs);
    EXPECT_GE(scrollbar_rect.bottom(), transcript->height() - mint::ui::metrics::space_xs);
    EXPECT_EQ(transcript_rect.bottom(), main_surface->height() - 1);
    const auto readable_width =
        transcript->viewport()->width() - transcript->content_horizontal_padding() * 2;
    EXPECT_LE(readable_width, mint::ui::metrics::conversation_maximum_width);
    EXPECT_EQ(transcript->header(), nullptr);
    EXPECT_EQ(transcript->footer(), nullptr);
}

TEST(DesktopComponents, ComposerFloatsAboveTheConversationCanvas) {
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1200, 760);
    page.show();
    QApplication::processEvents();

    auto* canvas = page.findChild<QWidget*>(QStringLiteral("MintConversationCanvas"));
    auto* conversation = page.findChild<mint::ui::RunConversation*>();
    auto* composer = page.findChild<mint::ui::TaskComposer*>();
    auto* composer_layer = page.findChild<QWidget*>(QStringLiteral("MintComposerLayer"));
    auto* transcript = page.findChild<mint::ui::ConversationTranscript*>(
        QStringLiteral("MintConversationTranscript"));
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(conversation, nullptr);
    ASSERT_NE(composer, nullptr);
    ASSERT_NE(composer_layer, nullptr);
    ASSERT_NE(transcript, nullptr);

    const QRect conversation_rect(conversation->mapTo(canvas, QPoint{}), conversation->size());
    const QRect composer_rect(composer->mapTo(canvas, QPoint{}), composer->size());
    EXPECT_TRUE(conversation_rect.intersects(composer_rect));
    const QRect transcript_rect(transcript->mapTo(canvas, QPoint{}), transcript->size());
    EXPECT_EQ(transcript_rect, canvas->rect());
    EXPECT_EQ(composer_rect.bottom(), canvas->height() - mint::ui::metrics::space_lg - 1);
    EXPECT_LE(composer->width(), mint::ui::metrics::conversation_maximum_width);
    EXPECT_LE(qAbs(composer_rect.center().x() - canvas->rect().center().x()), 1);
    const auto spacer = transcript->model()->index(transcript->model()->rowCount() - 1, 0);
    EXPECT_EQ(spacer.data(mint::ui::RunTimelineModel::ItemKindRole).toInt(),
              static_cast<int>(mint::ui::RunTimelineModel::ItemKind::spacer));
    EXPECT_GE(spacer.data(mint::ui::RunTimelineModel::BottomInsetRole).toInt(),
              composer->height() + mint::ui::metrics::space_lg + mint::ui::metrics::space_md);

    ASSERT_TRUE(controller.start("Inspect the complete timeline"));
    for (std::size_t index = 0; index < 20; ++index) {
        runtime.callbacks.event_received({.sequence = index,
                                          .kind = mint::desktop::RunEventKind::model_completed,
                                          .turn = index});
    }
    runtime.callbacks.run_finished({.status = "completed", .answer = "Finished"});
    QApplication::processEvents();
    transcript->verticalScrollBar()->setValue(transcript->verticalScrollBar()->maximum());
    QApplication::processEvents();

    const auto result = transcript->model()->index(transcript->model()->rowCount() - 2, 0);
    const auto result_rect = static_cast<QAbstractItemView*>(transcript)->visualRect(result);
    const QRect composer_in_transcript(transcript->mapFromGlobal(composer->mapToGlobal(QPoint{})),
                                       composer->size());
    EXPECT_LE(result_rect.bottom(), composer_in_transcript.top() - mint::ui::metrics::space_md);
}

TEST(DesktopComponents, JumpToLatestTracksTheTranscriptTailAndComposer) {
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1200, 760);
    page.show();
    QApplication::processEvents();

    ASSERT_TRUE(controller.start("Inspect the timeline"));
    for (std::size_t index = 0; index < 40; ++index) {
        runtime.callbacks.event_received({.sequence = index,
                                          .kind = mint::desktop::RunEventKind::model_completed,
                                          .turn = index});
    }
    QApplication::processEvents();

    auto* canvas = page.findChild<QWidget*>(QStringLiteral("MintConversationCanvas"));
    auto* composer = page.findChild<mint::ui::TaskComposer*>();
    auto* transcript = page.findChild<mint::ui::ConversationTranscript*>(
        QStringLiteral("MintConversationTranscript"));
    auto* jump =
        page.findChild<fluent::basicinput::Button*>(QStringLiteral("MintJumpToLatestButton"));
    ASSERT_NE(canvas, nullptr);
    ASSERT_NE(composer, nullptr);
    ASSERT_NE(transcript, nullptr);
    ASSERT_NE(jump, nullptr);
    auto* scrollbar = transcript->verticalScrollBar();
    ASSERT_GT(scrollbar->maximum(), 0);
    EXPECT_TRUE(jump->isHidden());

    scrollbar->setValue(scrollbar->maximum() / 3);
    QApplication::processEvents();

    ASSERT_TRUE(jump->isVisible());
    const QRect composer_rect(composer->mapTo(canvas, QPoint{}), composer->size());
    EXPECT_LE(qAbs(jump->geometry().center().x() - canvas->rect().center().x()), 1);
    EXPECT_LE(qAbs(jump->geometry().center().y() - composer_rect.top()), 1);

    jump->click();
    QApplication::processEvents();

    EXPECT_EQ(scrollbar->value(), scrollbar->maximum());
    EXPECT_TRUE(jump->isHidden());
}

TEST(DesktopComponents, TranscriptKeepsEventsAndAnswerInOneScrollOwner) {
    mint::ui::ConversationTranscript transcript;
    mint::desktop::TaskWorkspaceState state;
    state.title = "Inspect";
    state.events.push_back({.kind = mint::desktop::RunEventKind::tool_started,
                            .name = "read_file",
                            .evidence = "event evidence"});
    state.result = mint::desktop::TaskResult{.status = "completed", .answer = "Done"};
    QString selected_evidence;
    QObject::connect(
        &transcript, &mint::ui::ConversationTranscript::evidence_selected,
        [&selected_evidence](QString evidence) { selected_evidence = std::move(evidence); });

    transcript.render(state, mint::desktop::WorkspaceChange::all);
    transcript.select_first_evidence();

    EXPECT_TRUE(transcript.findChildren<QAbstractScrollArea*>().isEmpty());
    EXPECT_EQ(transcript.model()->rowCount(), 3);
    EXPECT_EQ(transcript.model()
                  ->index(transcript.model()->rowCount() - 1, 0)
                  .data(mint::ui::RunTimelineModel::ItemKindRole)
                  .toInt(),
              static_cast<int>(mint::ui::RunTimelineModel::ItemKind::spacer));
    EXPECT_TRUE(transcript.toPlainText().contains(QStringLiteral("Done")));
    EXPECT_EQ(selected_evidence, QStringLiteral("event evidence"));
}

class DesktopTranscriptPointer : public ::testing::Test {
  protected:
    void SetUp() override {
        state.title = "Inspect";
        state.events = {{.kind = mint::desktop::RunEventKind::tool_completed,
                         .name = "read_file",
                         .evidence = "first evidence"},
                        {.kind = mint::desktop::RunEventKind::tool_completed,
                         .name = "search_files",
                         .evidence = "second evidence"}};
        transcript.resize(800, 420);
        transcript.show();
        transcript.render(state, mint::desktop::WorkspaceChange::all);
        QApplication::processEvents();
    }

    [[nodiscard]] QRect evidence_rect(int row) const {
        const auto index = transcript.model()->index(row, 0);
        QStyleOptionViewItem option;
        option.initFrom(&transcript);
        option.widget = &transcript;
        option.rect = static_cast<const QListView*>(&transcript)->visualRect(index);
        const auto* delegate =
            static_cast<mint::ui::RunTimelineDelegate*>(transcript.itemDelegate());
        return delegate->evidence_action_rect(option, index);
    }

    mint::ui::ConversationTranscript transcript;
    mint::desktop::TaskWorkspaceState state;
};

TEST_F(DesktopTranscriptPointer, RightClickDoesNotOpenEvidence) {
    QSignalSpy evidence(&transcript, &mint::ui::ConversationTranscript::evidence_selected);
    ASSERT_TRUE(evidence_rect(0).isValid());

    QTest::mouseClick(transcript.viewport(), Qt::RightButton, Qt::NoModifier,
                      evidence_rect(0).center());
    QApplication::processEvents();

    EXPECT_TRUE(evidence.isEmpty());
}

TEST_F(DesktopTranscriptPointer, ReleaseOverEvidenceRequiresPressOnTheSameAction) {
    QSignalSpy evidence(&transcript, &mint::ui::ConversationTranscript::evidence_selected);
    const auto first_action = evidence_rect(0);
    const auto second_action = evidence_rect(1);
    ASSERT_TRUE(first_action.isValid());
    ASSERT_TRUE(second_action.isValid());

    QTest::mousePress(transcript.viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(32, first_action.center().y()));
    QTest::mouseRelease(transcript.viewport(), Qt::LeftButton, Qt::NoModifier,
                        first_action.center());
    QApplication::processEvents();
    EXPECT_TRUE(evidence.isEmpty());

    QTest::mousePress(transcript.viewport(), Qt::LeftButton, Qt::NoModifier, first_action.center());
    QTest::mouseRelease(transcript.viewport(), Qt::LeftButton, Qt::NoModifier,
                        second_action.center());
    QApplication::processEvents();
    EXPECT_TRUE(evidence.isEmpty());
}

TEST_F(DesktopTranscriptPointer, EachEvidenceClickPreservesTheInheritedClickContract) {
    QSignalSpy evidence(&transcript, &mint::ui::ConversationTranscript::evidence_selected);
    QSignalSpy clicked(&transcript, &QAbstractItemView::clicked);
    QSignalSpy row_clicked(&transcript, &fluent::collections::ListView::itemClicked);

    QTest::mouseClick(transcript.viewport(), Qt::LeftButton, Qt::NoModifier,
                      evidence_rect(0).center());
    QApplication::processEvents();

    ASSERT_EQ(evidence.count(), 1);
    EXPECT_EQ(evidence.front().front().toString(), QStringLiteral("first evidence"));
    EXPECT_EQ(clicked.count(), 1);
    EXPECT_EQ(row_clicked.count(), 1);

    QTest::mouseClick(transcript.viewport(), Qt::LeftButton, Qt::NoModifier,
                      evidence_rect(1).center());
    QApplication::processEvents();

    ASSERT_EQ(evidence.count(), 2);
    EXPECT_EQ(evidence.back().front().toString(), QStringLiteral("second evidence"));
    EXPECT_EQ(clicked.count(), 2);
    EXPECT_EQ(row_clicked.count(), 2);
}

TEST_F(DesktopTranscriptPointer, ModelResetCancelsThePendingEvidenceAction) {
    QSignalSpy evidence(&transcript, &mint::ui::ConversationTranscript::evidence_selected);
    QSignalSpy reset(transcript.model(), &QAbstractItemModel::modelReset);
    QTest::mousePress(transcript.viewport(), Qt::LeftButton, Qt::NoModifier,
                      evidence_rect(0).center());
    state.events.front().evidence = "replacement evidence";
    transcript.render(state, mint::desktop::WorkspaceChange::selection);
    QApplication::processEvents();
    ASSERT_EQ(reset.count(), 1);

    QTest::mouseRelease(transcript.viewport(), Qt::LeftButton, Qt::NoModifier,
                        evidence_rect(0).center());
    QApplication::processEvents();

    EXPECT_TRUE(evidence.isEmpty());
}

TEST(DesktopComponents, ConversationKeepsOneScrollViewportAndOverlaysConditionalActions) {
    mint::ui::RunConversation conversation;
    conversation.resize(900, 700);
    conversation.show();
    mint::desktop::TaskWorkspaceState state;
    state.title = "Inspect";
    conversation.render(state, mint::desktop::WorkspaceChange::all);
    QApplication::processEvents();

    auto* transcript = conversation.findChild<mint::ui::ConversationTranscript*>(
        QStringLiteral("MintConversationTranscript"));
    auto* action_layer =
        conversation.findChild<QWidget*>(QStringLiteral("MintConversationActionLayer"));
    ASSERT_NE(transcript, nullptr);
    ASSERT_NE(action_layer, nullptr);
    EXPECT_EQ(transcript->header(), nullptr);
    EXPECT_EQ(transcript->footer(), nullptr);
    EXPECT_TRUE(action_layer->isHidden());
    const auto transcript_height = transcript->height();

    state.running = true;
    state.selected_task_running = true;
    state.approval = mint::desktop::ApprovalRequest{.kind = mint::desktop::ApprovalKind::command,
                                                    .program = "cmake",
                                                    .working_directory = "/workspace"};
    conversation.render(state, mint::desktop::WorkspaceChange::approval);
    QApplication::processEvents();
    EXPECT_FALSE(action_layer->isHidden());
    EXPECT_EQ(transcript->height(), transcript_height);
    EXPECT_EQ(transcript->footer(), nullptr);
    EXPECT_TRUE(transcript->geometry().intersects(action_layer->geometry()));

    state.approval.reset();
    conversation.render(state, mint::desktop::WorkspaceChange::approval);
    QApplication::processEvents();
    EXPECT_TRUE(action_layer->isHidden());
    EXPECT_EQ(transcript->footer(), nullptr);

    state.approval = mint::desktop::ApprovalRequest{.kind = mint::desktop::ApprovalKind::command,
                                                    .program = "cmake",
                                                    .working_directory = "/workspace"};
    state.selected_task_running = false;
    conversation.render(state, mint::desktop::WorkspaceChange::selection);
    QApplication::processEvents();
    EXPECT_TRUE(action_layer->isHidden());
    EXPECT_EQ(transcript->height(), transcript_height);
}

TEST(DesktopComponents, AppendedEventKeepsTheReadersScrollPosition) {
    mint::ui::ConversationTranscript transcript;
    transcript.resize(540, 220);
    transcript.show();
    mint::desktop::TaskWorkspaceState state;
    state.title = "Inspect";
    for (std::size_t index = 0; index < 40; ++index) {
        state.events.push_back({.sequence = index,
                                .kind = mint::desktop::RunEventKind::model_completed,
                                .turn = index});
    }
    transcript.render(state, mint::desktop::WorkspaceChange::all);
    QApplication::processEvents();
    auto* scrollbar = transcript.verticalScrollBar();
    ASSERT_GT(scrollbar->maximum(), 0);
    scrollbar->setValue(scrollbar->maximum() / 3);
    const auto previous_value = scrollbar->value();
    const auto previous_text_size = transcript.toPlainText().size();

    state.events.push_back({.sequence = state.events.size(),
                            .kind = mint::desktop::RunEventKind::tool_completed,
                            .name = "read_file",
                            .evidence = "new evidence"});
    transcript.render(state, mint::desktop::WorkspaceChange::event_appended);
    QApplication::processEvents();

    EXPECT_EQ(scrollbar->value(), previous_value);
    EXPECT_GT(transcript.toPlainText().size(), previous_text_size);
}

TEST(DesktopComponents, EvidenceLinkOpensAsASameWindowDrawer) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1200, 760);
    page.show();
    QApplication::processEvents();

    ASSERT_TRUE(controller.start("Inspect"));
    runtime.callbacks.event_received({.kind = mint::desktop::RunEventKind::tool_completed,
                                      .name = "read_file",
                                      .evidence = "event evidence"});
    QApplication::processEvents();

    auto* transcript = page.findChild<mint::ui::ConversationTranscript*>(
        QStringLiteral("MintConversationTranscript"));
    auto* drawer =
        page.findChild<fluent::collections::DrawerView*>(QStringLiteral("MintEvidenceDrawer"));
    ASSERT_NE(transcript, nullptr);
    ASSERT_NE(drawer, nullptr);
    drawer->setAnimationEnabled(false);

    transcript->select_first_evidence();
    QApplication::processEvents();

    EXPECT_TRUE(drawer->isOpen());
}

TEST(DesktopComponents, ResultEvidenceRemainsProgressivelyDisclosed) {
    mint::localization::ScopedLanguage language(mint::localization::Language::english);
    ComponentRuntime runtime;
    MemoryPreferencesRepository repository;
    mint::desktop::PreferencesController preferences(repository);
    preferences.initialize(mint::localization::Language::english);
    mint::desktop::ModelSettingsController model_settings(runtime,
                                                          {.root = "/workspace", .language = "en"});
    mint::desktop::TaskWorkspaceController controller(runtime,
                                                      {.root = "/workspace", .language = "en"});
    mint::ui::TaskWorkspacePage page(controller, model_settings, preferences);
    page.resize(1200, 760);
    page.show();
    QApplication::processEvents();

    ASSERT_TRUE(controller.start("Inspect"));
    runtime.callbacks.run_finished(
        {.status = "completed", .answer = "Done", .unified_diff = "diff --git a/a b/a"});
    QApplication::processEvents();

    auto* drawer =
        page.findChild<fluent::collections::DrawerView*>(QStringLiteral("MintEvidenceDrawer"));
    auto* transcript = page.findChild<mint::ui::ConversationTranscript*>(
        QStringLiteral("MintConversationTranscript"));
    ASSERT_NE(drawer, nullptr);
    ASSERT_NE(transcript, nullptr);
    EXPECT_EQ(page.findChild<QWidget*>(QStringLiteral("MintEvidenceButton")), nullptr);
    EXPECT_FALSE(drawer->isOpen());
    EXPECT_TRUE(transcript->toPlainText().contains(QStringLiteral("View changes")));

    drawer->setAnimationEnabled(false);
    transcript->select_first_evidence();
    QApplication::processEvents();
    EXPECT_TRUE(drawer->isOpen());
}

} // namespace
