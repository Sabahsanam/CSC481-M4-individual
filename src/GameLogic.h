#ifndef GAME_LOGIC_H
#define GAME_LOGIC_H

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "GameObject.h"
#include "Components.h"
#include "GameComponents.h"
#include "GameObjectFactory.h"
#include "LevelObjects.h"
#include "Physics.h"
#include "Collision.h"
#include "EventManager.h"

// The game's rules, written as small "systems" that loop over GameObjects
// and act on whichever ones have the components they care about. This file
// has no window or renderer code, so the same logic can run headless.
//
// Milestone 4: Princess Star Race.
// Gameplay now runs through the team's EventManager. The systems below only
// DETECT things and raise events; the handlers registered in
// registerHandlers() decide what happens:
//
//   Input      raised for every key held this frame -> fills frameInput
//   Collision  "star", "goal" (networked) and "pit", "skull" (local)
//   Death      raised by a pit/skull collision (networked)
//   Spawn      raised by a death, puts the player on a checkpoint (networked)
//
// Networked events are sent to the other players with raiseNetworked().
// When they arrive, the same handlers run with sourceId = the other player,
// so a star someone else collects disappears here too, and their deaths,
// respawns and wins show up in the event feed.

struct GameInput {
    bool left = false;
    bool right = false;
    bool jump = false;
    bool run = false;
};

// One line in the on-screen event feed
struct FeedLine {
    std::string text;
    double shownAt = 0.0;   // game time when it was added
};

struct GameWorld {
    GameObject player;                 // the client-controlled character
    std::vector<GameObject> objects;   // everything else in the level
    Physics physics;

    float cameraX = 0.0f;              // left edge of the visible area
    double time = 0.0;                 // game time, drives moving objects
    int standingOn = -1;               // index in objects, or -1 if airborne

    int deaths = 0;
    int wins = 0;
    int coins = 0;                     // stars collected
    int totalCoins = 0;                // stars in the level
    bool facingLeft = false;           // which way the princess looks
    float grace = 0.0f;                // seconds after a respawn when
                                       // visible enemies can't hurt

    // --- Milestone 4: events and networking ---
    EventManager events;               // the team engine's event manager
    int clientId = 1;                  // this player's number (1-4)
    std::function<int64_t()> clock;    // event timestamps (Timeline in the game)
    GameInput frameInput;              // built by the Input event handler
    std::string lastInput;             // last input event, shown on the HUD
    bool respawnedThisStep = false;

    int round = 1;                     // a round ends when someone reaches the portal
    std::map<int, int> remoteStars;    // stars collected by each other player
    std::map<int, int> remoteDeaths;
    std::map<int, int> remoteWins;
    std::map<int, int> starOwner;      // star index -> player who has it
    std::vector<FeedLine> feed;        // event messages for the screen
    SDL_Texture* coinTexture = nullptr; // to show the stars again in a new round

    // The hitbox is the same size the princess had in Milestone 1.
    GameWorld() : player(makePlayer(1, 0.0f, 0.0f, 80.0f, 130.0f))
    {
        physics.setGravity(900.0f);
    }

    // The event handlers keep a pointer to this world, so it must stay put.
    GameWorld(const GameWorld&) = delete;
    GameWorld& operator=(const GameWorld&) = delete;
};

namespace game {

constexpr float MAX_CAMERA_X = level::WIDTH - level::SCREEN_W;

inline float clampCamera(float x)
{
    return std::max(0.0f, std::min(x, MAX_CAMERA_X));
}

// Keeps the two hidden scroll boundaries glued to the screen edges.
inline void updateScrollBoundaries(GameWorld& w)
{
    for (auto& obj : w.objects) {
        auto* b = obj.getComponent<ScrollBoundaryComponent>();
        auto* t = obj.getComponent<TransformComponent>();
        if (!b || !t) {
            continue;
        }

        if (b->side == ScrollSide::Left) {
            t->x = w.cameraX + b->inset;
        } else {
            t->x = w.cameraX + level::SCREEN_W - b->inset;
        }
    }
}

// Puts the player on the spawn point with the given index.
inline void respawn(GameWorld& w, int spawnIndex)
{
    auto* pt = w.player.getComponent<TransformComponent>();
    auto* pv = w.player.getComponent<VelocityComponent>();
    auto* pg = w.player.getComponent<GravityComponent>();

    const TransformComponent* chosen = nullptr;

    for (auto& obj : w.objects) {
        auto* sp = obj.getComponent<SpawnPointComponent>();
        auto* t = obj.getComponent<TransformComponent>();
        if (sp && t && sp->index == spawnIndex) {
            chosen = t;
            break;
        }
    }

    // Fall back to the first spawn point if the index doesn't exist
    if (!chosen) {
        for (auto& obj : w.objects) {
            if (obj.hasComponent<SpawnPointComponent>()) {
                chosen = obj.getComponent<TransformComponent>();
                break;
            }
        }
    }

    if (chosen) {
        // Players stand side by side on a checkpoint instead of on top of
        // each other: player 1 in the middle, 2 to the right, 3 to the left,
        // 4 further right.
        static const float SPAWN_OFFSET[5] = { 0.0f, 0.0f, 70.0f, -70.0f, 140.0f };
        float offset = (w.clientId >= 1 && w.clientId <= 4) ? SPAWN_OFFSET[w.clientId] : 0.0f;
        pt->x = chosen->x - pt->width / 2.0f + offset;
        pt->y = chosen->y - pt->height;
    }

    pv->vx = 0.0f;
    pv->vy = 0.0f;
    pg->grounded = false;
    w.standingOn = -1;
    w.grace = 1.0f;   // a moment to get moving before the skull counts

    w.cameraX = clampCamera(pt->x - 600.0f);
    updateScrollBoundaries(w);
    w.respawnedThisStep = true;
}

// ---------------------------------------------------------------------
// Milestone 4: events
// ---------------------------------------------------------------------

inline GameEvent makeEvent(GameWorld& w, EventType type, EventPriority priority,
                           const std::string& detail, int targetId = 0,
                           float x = 0.0f, float y = 0.0f)
{
    int64_t stamp = w.clock ? w.clock() : static_cast<int64_t>(w.time * 1000.0);
    return GameEvent{ type, priority, stamp, w.clientId, targetId, detail, x, y };
}

inline void addFeed(GameWorld& w, const std::string& text)
{
    w.feed.push_back({ text, w.time });
    if (w.feed.size() > 30) {
        w.feed.erase(w.feed.begin());
    }
}

inline std::string playerName(const GameWorld& w, int id)
{
    return id == w.clientId ? "You" : "Player " + std::to_string(id);
}

// Raised events can raise more events (collision -> death -> spawn),
// so dispatch a few times until the queue has settled.
inline void dispatchAll(GameWorld& w)
{
    for (int i = 0; i < 4; i++) {
        w.events.dispatch();
    }
}

inline int& starCount(GameWorld& w, int id)
{
    return id == w.clientId ? w.coins : w.remoteStars[id];
}

// A star was collected by player `who` (this client or another one).
inline void collectStar(GameWorld& w, int index, int who)
{
    if (index < 0 || index >= static_cast<int>(w.objects.size())) {
        return;
    }
    auto* c = w.objects[index].getComponent<CollectibleComponent>();
    if (!c) {
        return;
    }

    if (!c->collected) {
        c->collected = true;
        w.starOwner[index] = who;
        starCount(w, who) += c->value;
        // Remove the look: no Renderable means it is hidden now
        w.objects[index].removeComponent<RenderableComponent>();
        addFeed(w, "[Collision] " + playerName(w, who) + " grabbed a star");
        return;
    }

    // Both players grabbed the same star before hearing about each other.
    // Every client applies the same rule (lower player number keeps it),
    // so all clients agree without a server.
    int owner = w.starOwner.count(index) ? w.starOwner[index] : who;
    if (owner == who) {
        return;
    }
    int winner = std::min(owner, who);
    if (winner != owner) {
        starCount(w, owner) -= c->value;
        starCount(w, winner) += c->value;
        w.starOwner[index] = winner;
        addFeed(w, "[Collision] Tie! " + playerName(w, winner) + " keeps the star");
    }
}

// Someone reached the portal: everyone starts a new round.
inline void resetRound(GameWorld& w)
{
    for (auto& obj : w.objects) {
        auto* c = obj.getComponent<CollectibleComponent>();
        if (!c) {
            continue;
        }
        c->collected = false;
        if (!obj.hasComponent<RenderableComponent>()) {
            auto& r = obj.addComponent<RenderableComponent>();
            r.texture = w.coinTexture;
        }
    }
    w.coins = 0;
    w.remoteStars.clear();
    w.starOwner.clear();
    w.round++;
    respawn(w, 0);
}

// Registration: the game's handlers for the four event types.
inline void registerHandlers(GameWorld& w)
{
    GameWorld* wp = &w;

    // Input: build this frame's input from the input events
    w.events.subscribe(EventType::Input, [wp](const GameEvent& e) {
        if (e.detail == "left") wp->frameInput.left = true;
        else if (e.detail == "right") wp->frameInput.right = true;
        else if (e.detail == "jump") wp->frameInput.jump = true;
        else if (e.detail == "run") wp->frameInput.run = true;
        if (e.detail != "run") wp->lastInput = e.detail;
    });

    // Collision: stars, the portal, and hazards
    w.events.subscribe(EventType::Collision, [wp](const GameEvent& e) {
        GameWorld& w = *wp;
        bool local = e.sourceId == w.clientId;

        if (e.detail == "star") {
            collectStar(w, e.targetId, e.sourceId);
        }
        else if (e.detail == "goal") {
            if (local) {
                w.wins++;
            } else {
                w.remoteWins[e.sourceId]++;
            }
            addFeed(w, "[Collision] " + playerName(w, e.sourceId) + " reached the portal and won round " +
                       std::to_string(w.round) + " with " + std::to_string(e.targetId) + " stars!");
            resetRound(w);
        }
        else if (local) {
            // "pit" or "skull": the player dies
            w.events.raiseNetworked(makeEvent(w, EventType::Death, EventPriority::Critical,
                                              e.detail, e.targetId, e.x, e.y));
        }
    });

    // Death: count it and ask to be respawned
    w.events.subscribe(EventType::Death, [wp](const GameEvent& e) {
        GameWorld& w = *wp;
        std::string how = e.detail == "skull" ? "was caught by the skull" : "fell into a pit";
        addFeed(w, "[Death] " + playerName(w, e.sourceId) + " " + how);

        if (e.sourceId != w.clientId) {
            w.remoteDeaths[e.sourceId]++;
            return;
        }
        w.deaths++;
        w.events.raiseNetworked(makeEvent(w, EventType::Spawn, EventPriority::High,
                                          "checkpoint", e.targetId));
    });

    // Spawn: put the player on the checkpoint (other players: just report it)
    w.events.subscribe(EventType::Spawn, [wp](const GameEvent& e) {
        GameWorld& w = *wp;
        if (e.detail == "joined") {
            addFeed(w, "[Spawn] " + playerName(w, e.sourceId) + " joined the race");
            return;
        }
        addFeed(w, "[Spawn] " + playerName(w, e.sourceId) + " respawned at checkpoint " +
                   std::to_string(e.targetId));
        if (e.sourceId == w.clientId) {
            respawn(w, e.targetId);
        }
    });
}

inline void updateMovers(GameWorld& w)
{
    for (auto& obj : w.objects) {
        auto* m = obj.getComponent<MovementComponent>();
        auto* t = obj.getComponent<TransformComponent>();
        if (!m || !t) {
            continue;
        }

        float cx = 0.0f;
        float cy = 0.0f;
        evaluateMovement(*m, w.time, cx, cy);

        float newX = cx - t->width / 2.0f;
        float newY = cy - t->height / 2.0f;

        m->deltaX = newX - t->x;
        m->deltaY = newY - t->y;

        t->x = newX;
        t->y = newY;
    }
}

inline void updateAnimations(GameWorld& w, const GameInput& in, float dt)
{
    auto advance = [dt](RenderableComponent& r) {
        if (r.frameCount <= 1 || !r.useTimedAnimation) {
            return;
        }
        r.animationTimer += dt;
        while (r.animationTimer >= r.frameDuration) {
            r.currentFrame = (r.currentFrame + 1) % r.frameCount;
            r.animationTimer -= r.frameDuration;
        }
    };

    // The princess only walks when a direction is held
    if (auto* r = w.player.getComponent<RenderableComponent>()) {
        if (in.left || in.right) {
            advance(*r);
        } else {
            r->currentFrame = 0;
            r->animationTimer = 0.0f;
        }
    }
    for (auto& obj : w.objects) {
        if (auto* r = obj.getComponent<RenderableComponent>()) {
            advance(*r);
        }
    }
}

// Advance the whole game by dt seconds. `keys` is what the keyboard says;
// it only becomes movement after going through the Input events.
inline void step(GameWorld& w, const GameInput& keys, float dt)
{
    if (dt <= 0.0f) {
        return;
    }
    if (dt > 0.05f) {
        dt = 0.05f;   // avoid huge jumps after a stall
    }

    w.time += dt;
    w.respawnedThisStep = false;

    // 0. Raise one Input event per key held, then handle them
    //    (this also handles events that arrived from other players)
    w.frameInput = GameInput{};
    if (keys.left)  w.events.raise(makeEvent(w, EventType::Input, EventPriority::Normal, "left"));
    if (keys.right) w.events.raise(makeEvent(w, EventType::Input, EventPriority::Normal, "right"));
    if (keys.jump)  w.events.raise(makeEvent(w, EventType::Input, EventPriority::Normal, "jump"));
    if (keys.run)   w.events.raise(makeEvent(w, EventType::Input, EventPriority::Normal, "run"));
    dispatchAll(w);
    if (w.respawnedThisStep) {
        return;   // a remote "goal" event started a new round
    }
    const GameInput in = w.frameInput;

    auto* pt = w.player.getComponent<TransformComponent>();
    auto* pv = w.player.getComponent<VelocityComponent>();
    auto* pg = w.player.getComponent<GravityComponent>();

    float prevBottom = pt->y + pt->height;
    float prevX = pt->x;

    if (w.grace > 0.0f) {
        w.grace -= dt;
    }

    // 1. Input
    float speed = in.run ? 550.0f : 300.0f;
    if (in.left) {
        pt->x -= speed * dt;
        w.facingLeft = true;
    }
    if (in.right) {
        pt->x += speed * dt;
        w.facingLeft = false;
    }

    // 1b. Blockers (crates, totem): can't walk through them. If the
    // player was above the top last frame they are landing on it, not
    // walking into it, so it is left to the landing step below.
    for (auto& obj : w.objects) {
        if (!obj.hasComponent<BlockerComponent>()) {
            continue;
        }
        auto* t = obj.getComponent<TransformComponent>();
        if (!t) {
            continue;
        }
        bool overlapX = pt->x < t->x + t->width && pt->x + pt->width > t->x;
        bool overlapY = pt->y < t->y + t->height && pt->y + pt->height > t->y;
        bool wasAbove = prevBottom <= t->y + 4.0f;
        if (!overlapX || !overlapY || wasAbove) {
            continue;
        }
        if (prevX + pt->width <= t->x + 1.0f) {
            pt->x = t->x - pt->width;            // hit its left side
        } else if (prevX >= t->x + t->width - 1.0f) {
            pt->x = t->x + t->width;             // hit its right side
        }
    }
    if (in.jump) {
        w.physics.jump(w.player, 650.0f);
    }

    // 2. Gravity and velocity
    w.physics.update(w.player, dt);

    // 3. Moving platforms and the skull
    updateMovers(w);

    // 4. Carry the player if they were standing on a moving platform
    if (w.standingOn >= 0) {
        auto* m = w.objects[w.standingOn].getComponent<MovementComponent>();
        if (m) {
            pt->x += m->deltaX;
            pt->y += m->deltaY;
        }
    }

    // 5. Landing on solid objects (platforms and ground)
    pg->grounded = false;
    w.standingOn = -1;

    float left = pt->x;
    float right = pt->x + pt->width;
    float bottom = pt->y + pt->height;
    float bestTop = 0.0f;

    for (size_t i = 0; i < w.objects.size(); i++) {
        auto& obj = w.objects[i];
        auto* col = obj.getComponent<CollidableComponent>();
        auto* t = obj.getComponent<TransformComponent>();
        if (!col || !col->solid || !t) {
            continue;
        }

        if (right <= t->x || left >= t->x + t->width) {
            continue;
        }

        float platformDeltaY = 0.0f;
        if (auto* m = obj.getComponent<MovementComponent>()) {
            platformDeltaY = m->deltaY;
        }

        float top = t->y;
        float previousTop = top - platformDeltaY;

        bool falling = pv->vy >= 0.0f;
        bool wasAbove = prevBottom <= previousTop + 4.0f;
        bool reached = bottom >= top;

        if (falling && wasAbove && reached) {
            if (w.standingOn < 0 || top < bestTop) {
                w.standingOn = static_cast<int>(i);
                bestTop = top;
            }
        }
    }

    if (w.standingOn >= 0) {
        pt->y = bestTop - pt->height;
        pv->vy = 0.0f;
        pg->grounded = true;
    }

    // Keep the player inside the level
    pt->x = std::max(0.0f, std::min(pt->x, level::WIDTH - pt->width));

    // 6. Death zones (pits and the skull): raise a Collision event.
    //    The handler turns it into Death, and Death into Spawn.
    for (auto& obj : w.objects) {
        auto* dz = obj.getComponent<DeathZoneComponent>();
        if (!dz) {
            continue;
        }
        // Visible enemies (the skull) are harmless just after a respawn
        if (w.grace > 0.0f && obj.hasComponent<RenderableComponent>()) {
            continue;
        }
        if (Collision::checkCollision(w.player, obj)) {
            std::string what = obj.getType() == "skull" ? "skull" : "pit";
            w.events.raise(makeEvent(w, EventType::Collision, EventPriority::High,
                                     what, dz->targetSpawn, pt->x, pt->y));
            break;   // one death at a time
        }
    }
    dispatchAll(w);
    if (w.respawnedThisStep) {
        return;
    }

    // 7. Side-scrolling boundaries
    updateScrollBoundaries(w);

    for (auto& obj : w.objects) {
        auto* b = obj.getComponent<ScrollBoundaryComponent>();
        auto* t = obj.getComponent<TransformComponent>();
        if (!b || !t) {
            continue;
        }
        if (!Collision::checkCollision(w.player, obj)) {
            continue;
        }

        if (b->side == ScrollSide::Right) {
            w.cameraX += (pt->x + pt->width) - t->x;
        } else {
            w.cameraX -= (t->x + t->width) - pt->x;
        }
        w.cameraX = clampCamera(w.cameraX);
        updateScrollBoundaries(w);
    }

    // 8. Stars: a networked Collision event, so the star disappears
    //    for every player
    for (size_t i = 0; i < w.objects.size(); i++) {
        auto* c = w.objects[i].getComponent<CollectibleComponent>();
        if (!c || c->collected) {
            continue;
        }
        if (Collision::checkCollision(w.player, w.objects[i])) {
            w.events.raiseNetworked(makeEvent(w, EventType::Collision, EventPriority::High,
                                              "star", static_cast<int>(i)));
        }
    }
    dispatchAll(w);

    // 9. Goal: a networked Collision event that ends the round for everyone
    for (auto& obj : w.objects) {
        if (!obj.hasComponent<GoalComponent>()) {
            continue;
        }
        if (Collision::checkCollision(w.player, obj)) {
            w.events.raiseNetworked(makeEvent(w, EventType::Collision, EventPriority::High,
                                              "goal", w.coins));
            dispatchAll(w);
            return;
        }
    }

    // 10. Animation
    updateAnimations(w, in, dt);
}

// Builds the level. Scenery first (so it is drawn behind), then platforms
// and pickups, then the hidden objects (spawn points, death zones, scroll
// boundaries).
inline void buildLevel(GameWorld& w, const LevelTextures& tex)
{
    registerHandlers(w);
    w.coinTexture = tex.coin;

    const float G = level::GROUND_Y;
    const float PH = art::PLATFORM_H;
    auto& o = w.objects;

    // --- Scenery (drawn behind everything else) ---
    for (int i = 0; i < 10; i++) {
        float x = 150.0f + i * 330.0f;
        float y = 80.0f + (i % 3) * 110.0f;
        o.push_back(makeCloud(x, y, i * 1.3f, tex.cloud));
    }
    for (float x : { 380.0f, 1180.0f, 2650.0f, 3050.0f, 4700.0f, 5200.0f }) {
        o.push_back(makeBush(x, G, tex.bush));
    }

    // --- Static platforms (each has a different position/size/look) ---
    o.push_back(makeTexturedPlatform(0.0f,    G, 1536.0f, tex.platform));   // ground A
    o.push_back(makeTexturedPlatform(2304.0f, G, 1152.0f, tex.platform));   // ground B
    o.push_back(makeTexturedPlatform(4416.0f, G, 1344.0f, tex.platform));   // ground C
    o.push_back(makeColorPlatform(2500.0f, 760.0f, 256.0f, PH,
                                  ColorComponent(130, 80, 170)));           // purple
    o.push_back(makeColorPlatform(2900.0f, 600.0f, 256.0f, PH,
                                  ColorComponent(60, 160, 160)));           // teal
    o.push_back(makeTexturedPlatform(3240.0f, 720.0f, 192.0f, tex.platform)); // small

    // --- Moving platforms (three different movement patterns) ---
    o.push_back(makeMovingPlatform(MovementType::Horizontal, 1920.0f, G + 16.0f,
                                   200.0f, 1.0f, 0.0f, 192.0f, tex.platform));
    o.push_back(makeMovingPlatform(MovementType::Circular, 3770.0f, 760.0f,
                                   180.0f, 0.9f, 3.14159f, 192.0f, tex.platform));
    o.push_back(makeMovingPlatform(MovementType::Vertical, 4250.0f, 800.0f,
                                   150.0f, 1.1f, 0.0f, 192.0f, tex.platform));

    // --- Obstacles the princess has to jump over (they block sideways) ---
    o.push_back(makeTotem(700.0f, G, tex.totem));
    o.push_back(makeCrate(1100.0f, G, tex.crate));
    o.push_back(makeCrate(1300.0f, G, tex.crate));
    o.push_back(makeCrate(3150.0f, G, tex.crate));
    o.push_back(makeCrate(4780.0f, G, tex.crate));

    // --- Stars ---
    for (auto pos : { std::pair<float, float>{ 560.0f,  G - 120.0f },
                      { 1200.0f, G - 230.0f },
                      { 1920.0f, G - 170.0f },
                      { 2580.0f, 640.0f },
                      { 2990.0f, 480.0f },
                      { 3290.0f, 600.0f },
                      { 3770.0f, 560.0f },
                      { 4250.0f, 520.0f },
                      { 4700.0f, G - 150.0f },
                      { 5350.0f, G - 150.0f } }) {
        o.push_back(makeCoin(pos.first, pos.second, tex.coin));
        w.totalCoins++;
    }

    // --- Enemy and goal ---
    o.push_back(makeSkull(5100.0f, G, 220.0f, 1.0f, 2, tex.skull));
    o.push_back(makeGoal(5560.0f, G, tex.portal));

    // --- Hidden objects ---
    o.push_back(makeSpawnPoint(0, 200.0f,  G));
    o.push_back(makeSpawnPoint(1, 2450.0f, G));
    o.push_back(makeSpawnPoint(2, 4540.0f, G));

    o.push_back(makeDeathZone(1536.0f, 1000.0f, 768.0f, 600.0f, 0));   // gap 1
    o.push_back(makeDeathZone(3456.0f, 1000.0f, 960.0f, 600.0f, 1));   // gap 2
    o.push_back(makeDeathZone(-500.0f, 1500.0f, 7000.0f, 500.0f, 0));  // safety net

    o.push_back(makeScrollBoundary(ScrollSide::Left, 500.0f));
    o.push_back(makeScrollBoundary(ScrollSide::Right, 800.0f));

    // --- The princess: 8 walking frames, 96x156 each (Milestone 1 art) ---
    if (auto* r = w.player.getComponent<RenderableComponent>()) {
        r->texture = tex.player;
        r->frameCount = 8;
        r->frameWidth = 96;
        r->frameHeight = 156;
        r->frameDuration = 1.0f / 8.0f;
        r->useTimedAnimation = true;
    }

    respawn(w, 0);
}

} // namespace game

#endif
