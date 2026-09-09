#include "run_event_stream.hpp"

#include "mint_cli_protocol.hpp"

#include <QFile>
#include <QFutureWatcher>
#include <QPointer>

#include <QtConcurrentRun>

#include <utility>

namespace mint::infrastructure::desktop {
namespace {

constexpr qint64 frame_bytes = 64 * 1024;
constexpr std::size_t frame_records = 128;

struct EventBatch {
    std::vector<mint::desktop::RunEvent> events;
    QByteArray remainder;
    qint64 offset = 0;
    bool more = false;
};

EventBatch read_batch(const std::filesystem::path& path, qint64 offset, QByteArray buffer,
                      bool terminal) {
    EventBatch batch;
    batch.offset = offset;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) && file.seek(offset)) {
        // EN: Consume buffered records before reading another bounded slice of the file.
        // ZH-CN: 先处理已有完整记录，再读取下一段有界文件内容。
        if (!buffer.contains('\n')) {
            const auto bytes = file.read(frame_bytes);
            batch.offset += bytes.size();
            buffer.append(bytes);
        }
        batch.more = batch.offset < file.size();
    }
    qsizetype consumed = 0;
    for (std::size_t count = 0; count < frame_records; ++count) {
        const auto newline = buffer.indexOf('\n', consumed);
        if (newline < 0) {
            break;
        }
        if (auto event = MintCliProtocol::parse_event(
                buffer.mid(consumed, newline - consumed).trimmed().toStdString())) {
            batch.events.push_back(std::move(*event));
        }
        consumed = newline + 1;
    }
    buffer.remove(0, consumed);
    batch.more = batch.more || buffer.contains('\n');
    // EN: A finished writer can leave one complete JSON record without a final newline.
    // ZH-CN: 已结束的写入进程可能留下没有末尾换行的完整 JSON 记录。
    if (terminal && !batch.more && !buffer.trimmed().isEmpty()) {
        if (batch.events.size() == frame_records) {
            batch.more = true;
        } else {
            if (auto event = MintCliProtocol::parse_event(buffer.trimmed().toStdString())) {
                batch.events.push_back(std::move(*event));
            }
            buffer.clear();
        }
    }
    batch.remainder = std::move(buffer);
    return batch;
}

} // namespace

RunEventStream::RunEventStream(EventsReceived events_received, std::function<void()> drained)
    : events_received_(std::move(events_received)), drained_(std::move(drained)) {
    timer_.setInterval(120);
    connect(&timer_, &QTimer::timeout, this, [this] { read_next(); });
}

void RunEventStream::open(std::filesystem::path path) {
    reset();
    path_ = std::move(path);
    timer_.start();
    read_next();
}

void RunEventStream::drain() {
    timer_.stop();
    draining_ = true;
    read_next();
}

void RunEventStream::reset() {
    ++generation_;
    timer_.stop();
    path_.clear();
    buffer_.clear();
    offset_ = 0;
    reading_ = false;
    queued_ = false;
    draining_ = false;
}

void RunEventStream::read_next() {
    if (reading_) {
        return;
    }
    if (path_.empty()) {
        if (draining_) {
            complete();
        }
        return;
    }
    reading_ = true;
    const auto generation = generation_;
    const auto terminal = draining_;
    auto* watcher = new QFutureWatcher<EventBatch>(this);
    connect(watcher, &QFutureWatcher<EventBatch>::finished, this,
            [this, watcher, generation, terminal] {
                auto batch = watcher->future().takeResult();
                watcher->deleteLater();
                if (generation != generation_) {
                    return;
                }
                reading_ = false;
                offset_ = batch.offset;
                buffer_ = std::move(batch.remainder);
                const QPointer<RunEventStream> lifetime(this);
                if (const auto received = events_received_; !batch.events.empty() && received) {
                    received(std::move(batch.events));
                }
                if (!lifetime || generation != generation_) {
                    return;
                }
                if (batch.more || (draining_ && !terminal)) {
                    queue_read();
                } else if (draining_) {
                    complete();
                }
            });
    watcher->setFuture(QtConcurrent::run(
        [path = path_, offset = offset_, buffer = std::move(buffer_), terminal]() mutable {
            return read_batch(path, offset, std::move(buffer), terminal);
        }));
}

void RunEventStream::queue_read() {
    if (queued_) {
        return;
    }
    queued_ = true;
    const auto generation = generation_;
    QTimer::singleShot(0, this, [this, generation] {
        if (generation == generation_) {
            queued_ = false;
            read_next();
        }
    });
}

void RunEventStream::complete() {
    reset();
    if (const auto drained = drained_) {
        drained();
    }
}

} // namespace mint::infrastructure::desktop
