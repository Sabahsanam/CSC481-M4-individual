#pragma once

/*
 * Milestone 4 Section 1.2: Networked Event Management
 * This file turns a GameEvent into a string that can be sent over ZeroMQ,
 * and turns a received string back into a GameEvent.
 */

#include <cstddef>
#include <sstream>
#include <string>
 
#include "EventManager.h"
 
namespace NetEvent {
    constexpr char EVENT_PREFIX = 'E';
 
    // True if a received message is a networked event
    inline bool isEventMessage(const void* data, size_t size) {
        return size > 0 && static_cast<const char*>(data)[0] == EVENT_PREFIX;
    }
 
    // Event representation on the wire (GameEvent -> string)
    inline std::string encode(const GameEvent& e) {
        std::ostringstream out;
        out << EVENT_PREFIX << " "
            << static_cast<int>(e.type) << " "
            << static_cast<int>(e.priority) << " "
            << e.timestamp << " "
            << e.sourceId << " "
            << e.targetId << " "
            << e.x << " " << e.y << " "
            << e.detail; // last, so it may be read to the end of the line
        return out.str();
    }
 
    // Returns false if the message is malformed (string -> GameEvent)
    inline bool decode(const void* data, size_t size, GameEvent& e) {
        std::istringstream in(std::string(static_cast<const char*>(data), size));
        char prefix = 0;
        int type = 0, priority = 0;
        if (!(in >> prefix >> type >> priority >> e.timestamp >> e.sourceId
                >> e.targetId >> e.x >> e.y) || prefix != EVENT_PREFIX) {
            return false;
        }
        // Reject values outside the enums (protects against bad/old packets)
        if (type < 0 || type > static_cast<int>(EventType::Spawn)) return false;
        if (priority < 0 || priority > static_cast<int>(EventPriority::Critical)) return false;
        e.type = static_cast<EventType>(type);
        e.priority = static_cast<EventPriority>(priority);
 
        // Remaining text on the line is the detail string
        in >> std::ws;
        std::getline(in, e.detail);
        return true;
    }
} // namespace NetEvent