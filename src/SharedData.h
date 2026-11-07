#pragma once

#include <mutex>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>
#include <deque>

#include "NetworkFormat.h" // Milestone 3 Section C.1: which data format this client sends (TEXT or BINARY)
#include "EventManager.h" // Milestone 4 Section 1.2: events shared across the network

// Structure holding location data for remote peers
struct RemotePlayerState {
    float x;
    float y;
        std::chrono::steady_clock::time_point lastSeen = std::chrono::steady_clock::now();
};

struct SharedData {
    // Controls when worker threads should stop
    std::atomic<bool> running{true};

    // Milestone 3 Section C.1: Data format used for player positions
    NetworkFormat networkFormat = NetworkFormat::TEXT;

    /* Milestone 3 Sections C.2-C.4 (performance experiments)*/
    // Used by the benchmark program; empty during normal gameplay
    // Local client's moving object positions
    std::vector<ObjectState> localObjects;

    // Latest object snapshot received from each peer (key = peer ID)
    std::unordered_map<int, std::vector<ObjectState>> remoteObjects;

    // Fixed network tick in microseconds (0 = original pacing (about 16 ms))
    int networkTickMicros = 0;

    // Network performance counters for benchmarking
    struct NetStats {
        std::atomic<uint64_t> ticks{0}; // total send rounds
        std::atomic<uint64_t> bytesSent{0}; // total payload bytes sent
        std::atomic<uint64_t> encodeNanos{0}; // time spent building messages
        std::atomic<uint64_t> messagesReceived{0};// messages decoded
        std::atomic<uint64_t> decodeNanos{0}; // time spent parsing messages
    } stats;

    // Milestone 4 Section 1.2: Networked events
    // The game thread puts events here; the networking thread sends them.
    std::deque<GameEvent> outgoingEvents;
    std::mutex eventMutex; // protects outgoingEvents

    // Events received from other clients are raised on this manager
    EventManager* eventManager = nullptr;

    // Called by the game thread to send an event to all other clients
    void queueOutgoingEvent(const GameEvent& event) {
        std::lock_guard<std::mutex> lock(eventMutex);
        outgoingEvents.push_back(event);
    }

    // Local player's latest position
    float playerX = 0.0f;
    float playerY = 0.0f;

    // Current timeline/game speed for this client
    // 0.5 = half speed
    // 1.0 = normal speed
    // 2.0 = double speed
    std::atomic<double> timeScale{1.0};

    // Positions received from the server
    std::unordered_map<int, RemotePlayerState> remotePlayers;

    // Protect player positions shared between threads
    std::mutex playerMutex;
};