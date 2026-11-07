#include "Networking.h"
#include "NetworkEvent.h"

#include <zmq.hpp>
#include <sstream>
#include <string>
#include <chrono>
#include <thread>
#include <iostream>
#include <cstring>
#include <vector>
#include <deque>

// Section 5: Ports used for direct peer-to-peer communication.
#define PEER_BASE_PORT 6000
#define TOTAL_PEERS 4

void networkingThread(
    SharedData& sharedData,
    int clientID
) {
    try {
        zmq::context_t context(1);

        // Section 5: This socket sends this player's position to the other clients.
        zmq::socket_t peerPublisher(context, zmq::socket_type::pub);
        peerPublisher.set(zmq::sockopt::linger,0);

        // Section 5: Each client has its own peer port. Client 1 -> 6001, Client 2 -> 6002, and Client 3 -> 6003, Client 4 -> 6004
        int peerPort = PEER_BASE_PORT + clientID;
        std::string peerAddress = "tcp://*:" + std::to_string(peerPort);
        peerPublisher.bind(peerAddress);

        // This socket receives positions from the other clients.
        zmq::socket_t peerSubscriber(context, zmq::socket_type::sub);
        peerSubscriber.set(zmq::sockopt::linger,0);
        // Receive every peer message.
        peerSubscriber.set(zmq::sockopt::subscribe,"");

        // Section 5: Connect to the other clients.
        for (int peerID = 1; peerID <= TOTAL_PEERS; peerID++) {
            if (peerID == clientID) { continue; } // skip connecting to self
        
            int otherPeerPort = PEER_BASE_PORT + peerID;
            std::string otherPeerAddress = "tcp://localhost:" + std::to_string(otherPeerPort);
            peerSubscriber.connect(otherPeerAddress);
        }

        // Allow ZeroMQ time to establish P2P subscriptions across peers
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::cout << "Peer " << clientID << " P2P networking thread started on port " << peerPort
                  << " (data format: " << NetFormat::formatName(sharedData.networkFormat) << ")" << std::endl;

        while (sharedData.running.load()) {

            float localX;
            float localY;

            /*
             * Safely read the local player's
             * latest position.
             */
            {
                std::lock_guard<std::mutex> lock(sharedData.playerMutex);

                localX = sharedData.playerX;
                localY = sharedData.playerY;
            }

            // Section C.2-C.4: copy the extra moving objects (empty in the normal game).
            std::vector<ObjectState> localObjects;
            {
                std::lock_guard<std::mutex> lock(sharedData.playerMutex);
                localObjects = sharedData.localObjects;
            }

            // Section 5: Send this player's position directly to the other clients.
            // The server does not relay this data.
            // Section C.1: encode() builds either the TEXT string or the BINARY
            // struct, depending on the format chosen when the client was started.
            const auto encodeStart = std::chrono::steady_clock::now();
            std::string peerString = NetFormat::encode(sharedData.networkFormat, clientID, localX, localY);
            std::string objectString;
            if (!localObjects.empty()) {
                objectString = NetFormat::encodeObjects(sharedData.networkFormat, clientID, localObjects);
            }
            const auto encodeEnd = std::chrono::steady_clock::now();

            peerPublisher.send(zmq::buffer(peerString), zmq::send_flags::none);
            if (!objectString.empty()) {
                peerPublisher.send(zmq::buffer(objectString), zmq::send_flags::none);
            }

            // Section C.2-C.4: Record how much work this send round took.
            sharedData.stats.ticks++;
            sharedData.stats.bytesSent += peerString.size() + objectString.size();
            sharedData.stats.encodeNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(encodeEnd - encodeStart).count();

            // Milestone 4 Section 1.2: Send queued game events to the other clients.
            // Events are always sent as text, whatever the position format is.
            {
                std::deque<GameEvent> toSend;
                {
                    std::lock_guard<std::mutex> lock(sharedData.eventMutex);
                    toSend.swap(sharedData.outgoingEvents); // take all, release lock fast
                }
                for (const GameEvent& event : toSend) {
                    std::string eventString = NetEvent::encode(event);
                    peerPublisher.send(zmq::buffer(eventString), zmq::send_flags::none);
                }
            }

            // Section 5: Non-blocking loop to receive messages from all other peers
            while (true) {
                zmq::message_t peerMessage;

                // Prevents this client from getting stuck waiting for another client.
                auto peerResult = peerSubscriber.recv(peerMessage, zmq::recv_flags::dontwait);

                // No more pending messages from peers
                if (!peerResult) { break; }

                // Milestone 4 Section 1.2: A networked event from another client.
                // Raise it on our own EventManager.
                if (NetEvent::isEventMessage(peerMessage.data(), peerMessage.size())) {
                    GameEvent remoteEvent;
                    if (sharedData.eventManager &&
                        NetEvent::decode(peerMessage.data(), peerMessage.size(), remoteEvent) &&
                        remoteEvent.sourceId != clientID &&
                        remoteEvent.sourceId >= 1 && remoteEvent.sourceId <= TOTAL_PEERS) {
                        // The sender's Timeline is not comparable to ours, so the
                        // timestamp is reset. Priority and arrival order still apply.
                        remoteEvent.timestamp = 0;
                        sharedData.eventManager->raise(std::move(remoteEvent));
                    }
                    continue; // done with this message
                }

                int remoteID;
                float remoteX;
                float remoteY;

                // Section C.2-C.4: Object snapshots (benchmark only) are stored separately.
                if (NetFormat::isObjectMessage(peerMessage.data(), peerMessage.size())) {
                    std::vector<ObjectState> remoteObjectList;
                    const auto decodeStart = std::chrono::steady_clock::now();
                    const bool decoded = NetFormat::decodeObjects(peerMessage.data(), peerMessage.size(), remoteID, remoteObjectList);
                    sharedData.stats.decodeNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - decodeStart).count();
                    sharedData.stats.messagesReceived++;

                    if (decoded && remoteID != clientID && remoteID >= 1 && remoteID <= TOTAL_PEERS) {
                        std::lock_guard<std::mutex> lock(sharedData.playerMutex);
                        sharedData.remoteObjects[remoteID] = std::move(remoteObjectList);
                    }
                    continue; // done with this message, check for the next one
                }

                // Section C.1: decode() detects TEXT vs BINARY from the message itself.
                const auto decodeStart = std::chrono::steady_clock::now();
                const bool playerDecoded = NetFormat::decode(peerMessage.data(), peerMessage.size(), remoteID, remoteX, remoteY);
                sharedData.stats.decodeNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - decodeStart).count();
                sharedData.stats.messagesReceived++;

                if (playerDecoded) {
                    if (remoteID != clientID) {
                        // Thread-safe update of remote player position map
                        std::lock_guard<std::mutex> lock(sharedData.playerMutex);
                        if (remoteID >= 1 && remoteID <= TOTAL_PEERS) {
                            sharedData.remotePlayers[remoteID] = {remoteX, remoteY, std::chrono::steady_clock::now()};
                        }
                    }
                }
            }

            /*
             * Section 4: Asynchronicity
             *
             * The communication rate for this
             * client depends on its own Timeline
             * scale.
             *
             * 0.5x -> about 32 ms
             * 1.0x -> about 16 ms
             * 2.0x -> about 8 ms
             *
             * Each client has a dedicated server
             * communication thread, so changing
             * one client's rate does not slow
             * down or speed up the others.
             */
            // PUB/SUB does not report peer disconnects. Expire stale players.
            {
                std::lock_guard<std::mutex> lock(sharedData.playerMutex);
                const auto now = std::chrono::steady_clock::now();
                for (auto it = sharedData.remotePlayers.begin(); it != sharedData.remotePlayers.end();) {
                    if (now - it->second.lastSeen > std::chrono::seconds(2)) {
                        std::cout << "Peer " << it->first << " disconnected (timeout)" << std::endl;
                        sharedData.remoteObjects.erase(it->first);  // Section C.2-C.4
                        it = sharedData.remotePlayers.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            double scale = sharedData.timeScale.load();

            int sleepMilliseconds = static_cast<int>(16.0 / (scale > 0.0 ? scale : 1.0));

            if (sharedData.networkTickMicros > 0) {
                // Section C.2-C.4: The benchmark uses a faster fixed tick so the cost of
                // each data format is easy to measure. The game leaves this at 0.
                std::this_thread::sleep_for(std::chrono::microseconds(sharedData.networkTickMicros));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMilliseconds));
            }
        }

        // Clean resource shutdown
        peerPublisher.close();
        peerSubscriber.close();
        context.close();
    }
    catch (const zmq::error_t& e) {
        if (sharedData.running.load()) {
            std::cerr << "Networking error for client " << clientID << ": " << e.what() << std::endl;
        }
    }
}