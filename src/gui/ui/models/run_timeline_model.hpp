#pragma once

#include "application/task_workspace_controller.hpp"

#include <QAbstractListModel>

#include <optional>
#include <vector>

namespace mint::ui {

class RunTimelineModel final : public QAbstractListModel {
  public:
    enum class ItemKind { empty, event, result, spacer };

    enum Role {
        ItemKindRole = Qt::UserRole + 1,
        TitleRole,
        DetailRole,
        ToneRole,
        BodyRole,
        UsageRole,
        EvidenceRole,
        EvidenceActionRole,
        BottomInsetRole
    };

    explicit RunTimelineModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_state(const desktop::TaskWorkspaceState& state);
    [[nodiscard]] bool append_latest_event(const desktop::TaskWorkspaceState& state);
    [[nodiscard]] bool set_bottom_inset(int height);
    void retranslate();

    [[nodiscard]] QString plain_text() const;
    [[nodiscard]] QString evidence_at(std::size_t index) const;
    [[nodiscard]] QString first_evidence() const;

  private:
    [[nodiscard]] ItemKind item_kind(int row) const;
    [[nodiscard]] const desktop::RunEvent* event_at(int row) const;

    std::vector<desktop::RunEvent> events_;
    std::optional<desktop::TaskResult> result_;
    bool has_named_task_ = false;
    bool has_workspace_ = false;
    int bottom_inset_ = 0;
};

} // namespace mint::ui
