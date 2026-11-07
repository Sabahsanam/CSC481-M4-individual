#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

// Milestone 4 Section 1.3: the Replay* types control the replay system.
// They are added AFTER Spawn so the network decoder (which only accepts
// Input..Spawn) keeps rejecting them: replay control stays on one client.
enum class EventType {
    Input, Collision, Death, Spawn,
    ReplayRecordStart, ReplayRecordStop,
    ReplayPlaybackStart, ReplayPlaybackStop, ReplayPlaybackSpeed
};
enum class EventPriority { Low = 0, Normal = 1, High = 2, Critical = 3 };

struct GameEvent {
    EventType type;
    EventPriority priority = EventPriority::Normal;
    int64_t timestamp = 0; // From Timeline::getTime()
    int sourceId = 0;
    int targetId = 0;
    std::string detail;
    float x = 0.0f;
    float y = 0.0f;
};

class EventManager {
public:
    using Handler = std::function<void(const GameEvent&)>;
    using SubscriptionId = std::size_t;

    // Registration: game systems and objects subscribe by event type
    SubscriptionId subscribe(EventType type, Handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto id = nextId_++;
        listeners_[type].push_back({id, std::move(handler)});
        return id;
    }

    void unsubscribe(EventType type, SubscriptionId id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entries = listeners_[type];
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [id](const Listener& entry) { return entry.first == id; }), entries.end());
    }

    // Raising: queue, never immediately dispatch (safe from networking thread)
    void raise(GameEvent event) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(QueuedEvent{std::move(event), nextSequence_++});
    }

    // Milestone 4 Section 1.2: Networked raising
    // The event is queued locally and handed to the network sink
    // so it is sent to every other client.
    void raiseNetworked(GameEvent event) {
        NetworkSink sink;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sink = networkSink_;
        }
        if (sink) sink(event); // sink only queues the event, it does not block
        raise(std::move(event));
    }

    // Set by the game: The function that sends an event over the network.
    // If it is never set, raiseNetworked simply behaves like raise.
    using NetworkSink = std::function<void(const GameEvent&)>;
    void setNetworkSink(NetworkSink sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        networkSink_ = std::move(sink);
    }

    // Handling: called on main game thread, no callback executes under lock
    // Priority first, then earliest timestamp, then FIFO for equal timestamps
    // Events raised by handlers are processed on the next dispatch call
    void dispatch() {
        std::vector<QueuedEvent> ready;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            while (!queue_.empty()) {
                ready.push_back(queue_.top());
                queue_.pop();
            }
        }
        for (const auto& queued : ready) {
            std::vector<Handler> callbacks;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = listeners_.find(queued.event.type);
                if (it != listeners_.end()) {
                    for (const auto& listener : it->second) callbacks.push_back(listener.second);
                }
            }
            for (const auto& callback : callbacks) callback(queued.event);
        }
    }

private:
    using Listener = std::pair<SubscriptionId, Handler>;
    struct QueuedEvent { GameEvent event; std::size_t sequence; };
    struct EarlierFirst {
        bool operator()(const QueuedEvent& a, const QueuedEvent& b) const {
            if (a.event.priority != b.event.priority)
                return static_cast<int>(a.event.priority) < static_cast<int>(b.event.priority);
            if (a.event.timestamp != b.event.timestamp)
                return a.event.timestamp > b.event.timestamp;
            return a.sequence > b.sequence;
        }
    };
    std::mutex mutex_;
    NetworkSink networkSink_;
    std::map<EventType, std::vector<Listener>> listeners_;
    std::priority_queue<QueuedEvent, std::vector<QueuedEvent>, EarlierFirst> queue_;
    SubscriptionId nextId_ = 1;
    std::size_t nextSequence_ = 0;
};