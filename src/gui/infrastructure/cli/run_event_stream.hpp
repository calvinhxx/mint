#pragma once

#include "application/desktop_types.hpp"

#include <QByteArray>
#include <QObject>
#include <QTimer>

#include <functional>

namespace mint::infrastructure::desktop {

class RunEventStream final : public QObject {
  public:
    using EventsReceived = std::function<void(std::vector<mint::desktop::RunEvent>)>;

    RunEventStream(EventsReceived events_received, std::function<void()> drained);

    void open(std::filesystem::path path);
    void drain();
    void reset();

  private:
    void read_next();
    void queue_read();
    void complete();

    EventsReceived events_received_;
    std::function<void()> drained_;
    QTimer timer_;
    std::filesystem::path path_;
    QByteArray buffer_;
    qint64 offset_ = 0;
    std::size_t generation_ = 0;
    bool reading_ = false;
    bool queued_ = false;
    bool draining_ = false;
};

} // namespace mint::infrastructure::desktop
