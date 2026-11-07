// Milestone 4: headless tests of the event-driven, two-player race.
// Two GameWorlds (player 1 and player 2) run in one process. Their
// networked events are passed straight to each other, the same way the
// networking thread passes them between two game windows.
#include "GameLogic.h"
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); failures++; } else { std::printf("ok:   %s\n", msg); } } while (0)

static TransformComponent* P(GameWorld& w) { return w.player.getComponent<TransformComponent>(); }

static bool feedHas(const GameWorld& w, const std::string& text) {
    for (const auto& f : w.feed) if (f.text.find(text) != std::string::npos) return true;
    return false;
}

static int starIndex(GameWorld& w, int n) {   // index of the n-th star
    for (size_t i = 0; i < w.objects.size(); i++)
        if (w.objects[i].hasComponent<CollectibleComponent>() && n-- == 0) return (int)i;
    return -1;
}

// Connect two worlds: a networked event raised in one is raised in the other.
// Events that are not networked (raise) never reach the sink.
struct Pair {
    GameWorld a, b;
    int sentAtoB = 0, sentBtoA = 0;
    std::vector<EventType> typesSent;
    Pair() {
        LevelTextures tex;
        a.clientId = 1; b.clientId = 2;
        game::buildLevel(a, tex);
        game::buildLevel(b, tex);
        a.events.setNetworkSink([this](const GameEvent& e) { sentAtoB++; typesSent.push_back(e.type); b.events.raise(e); });
        b.events.setNetworkSink([this](const GameEvent& e) { sentBtoA++; a.events.raise(e); });
    }
    void run(GameInput inA, GameInput inB, float seconds) {
        int n = (int)(seconds * 60.0f);
        for (int i = 0; i < n; i++) { game::step(a, inA, 1.0f / 60); game::step(b, inB, 1.0f / 60); }
    }
};

int main() {
    { // input goes through events, and input events stay local
        Pair p;
        GameInput r; r.right = true;
        float x0 = P(p.a)->x;
        p.run(r, {}, 0.3f);
        CHECK(P(p.a)->x > x0 + 50, "Input events move the player");
        CHECK(p.a.lastInput == "right", "the last Input event is remembered for the HUD");
        CHECK(p.sentAtoB == 0, "Input events are not sent over the network");
    }
    { // a star collected by player 1 disappears for player 2
        Pair p;
        int s = starIndex(p.a, 0);
        auto* t = p.a.objects[s].getComponent<TransformComponent>();
        P(p.a)->x = t->x - 10; P(p.a)->y = t->y - 40;
        p.run({}, {}, 1.0f / 60);
        CHECK(p.a.coins == 1, "player 1 collects the star (Collision event)");
        CHECK(!p.b.objects[s].hasComponent<RenderableComponent>(), "the star disappears on player 2's screen too");
        CHECK(p.b.remoteStars[1] == 1 && p.b.coins == 0, "player 2 counts it for player 1, not for themselves");
        CHECK(feedHas(p.b, "Player 1 grabbed a star"), "player 2's feed says Player 1 grabbed a star");
        // player 2 walks into the same (already taken) star: nothing happens
        P(p.b)->x = t->x - 10; P(p.b)->y = t->y - 40;
        p.run({}, {}, 1.0f / 60);
        CHECK(p.b.coins == 0, "a star already taken by someone else can't be collected again");
    }
    { // both grab the same star at the same time: lower player number keeps it everywhere
        Pair p;
        int s = starIndex(p.a, 2);
        auto* t = p.a.objects[s].getComponent<TransformComponent>();
        P(p.a)->x = t->x - 10; P(p.a)->y = t->y - 40;
        P(p.b)->x = t->x - 10; P(p.b)->y = t->y - 40;
        // both step before either hears from the other: hold B's messages back
        std::vector<GameEvent> held;
        p.b.events.setNetworkSink([&](const GameEvent& e) { held.push_back(e); });
        game::step(p.b, {}, 1.0f / 60);          // B grabs it first locally...
        game::step(p.a, {}, 1.0f / 60);          // ...A grabs it too (A's message reaches B)
        for (auto& e : held) p.a.events.raise(e);  // now B's message reaches A
        game::step(p.a, {}, 1.0f / 60);
        game::step(p.b, {}, 1.0f / 60);
        CHECK(p.a.coins == 1 && p.a.remoteStars[2] == 0, "tie: player 1 keeps the star on player 1's side");
        CHECK(p.b.coins == 0 && p.b.remoteStars[1] == 1, "tie: player 2 gives the star to player 1 on player 2's side");
    }
    { // death in a pit: Collision -> Death -> Spawn, and player 2 is told
        Pair p;
        GameInput r; r.right = true; r.run = true;
        P(p.a)->x = 1380;
        for (int i = 0; i < 60 * 12 && p.a.deaths == 0; i++) { game::step(p.a, r, 1.0f / 60); game::step(p.b, {}, 1.0f / 60); }
        game::step(p.b, {}, 1.0f / 60);
        CHECK(p.a.deaths == 1, "falling into a pit is a Death");
        CHECK(std::abs(P(p.a)->x - 160.0f) < 1.0f, "the Spawn event puts player 1 back on checkpoint 0");
        CHECK(feedHas(p.a, "[Death] You fell into a pit"), "player 1's feed shows the death");
        CHECK(feedHas(p.b, "[Death] Player 1 fell into a pit"), "player 2's feed shows player 1's death");
        CHECK(feedHas(p.b, "[Spawn] Player 1 respawned at checkpoint 0"), "player 2's feed shows player 1's respawn");
        CHECK(p.b.remoteDeaths[1] == 1 && p.b.deaths == 0, "player 2 counts player 1's death, not their own");
        CHECK(std::abs(P(p.b)->x - 230.0f) < 1.0f && p.b.deaths == 0, "player 2 is not moved by player 1's events");
        bool sentCollisionForPit = false;
        for (auto t : p.typesSent) if (t == EventType::Collision) sentCollisionForPit = true;
        CHECK(!sentCollisionForPit, "pit collisions stay local; only Death and Spawn are sent");
    }
    { // players respawn side by side, not on top of each other
        Pair p;
        for (int sp = 0; sp < 3; sp++) {
            game::respawn(p.a, sp); game::respawn(p.b, sp);
            float gap = P(p.b)->x - P(p.a)->x;
            char m[80]; std::snprintf(m, sizeof m, "at checkpoint %d player 2 stands next to player 1", sp);
            CHECK(gap >= P(p.a)->width - 15.0f, m);
        }
        // every player at every checkpoint can idle without dying
        bool safe = true;
        for (int id = 1; id <= 4; id++) for (int sp = 0; sp < 3; sp++) {
            GameWorld w; w.clientId = id; LevelTextures tex; game::buildLevel(w, tex);
            game::respawn(w, sp);
            for (int i = 0; i < 60 * 20; i++) game::step(w, {}, 1.0f / 60);
            if (w.deaths != 0 || !w.player.getComponent<GravityComponent>()->grounded) safe = false;
        }
        CHECK(safe, "players 1-4 can stand at every checkpoint for 20 s without dying");
    }
    { // the skull
        Pair p;
        P(p.a)->x = 5100; P(p.a)->y = 700; p.a.grace = 0;
        p.run({}, {}, 3.0f);
        CHECK(p.a.deaths > 0 && feedHas(p.b, "Player 1 was caught by the skull"), "the skull kills, and player 2 hears about it");
    }
    { // reaching the portal ends the round for both players
        Pair p;
        int s = starIndex(p.b, 5);
        auto* t = p.b.objects[s].getComponent<TransformComponent>();
        P(p.b)->x = t->x - 10; P(p.b)->y = t->y - 40;   // player 2 takes a star first
        p.run({}, {}, 2.0f / 60);   // one frame for the message to arrive
        CHECK(p.a.remoteStars[2] == 1, "player 1 sees player 2's star");
        P(p.a)->x = 5500; P(p.a)->y = 700;
        p.run({}, {}, 1.0f);
        CHECK(p.a.wins == 1 && p.b.remoteWins[1] == 1, "player 1 wins the round on both screens");
        CHECK(p.a.round == 2 && p.b.round == 2, "both players start round 2");
        CHECK(p.a.remoteStars[2] == 0 && p.b.coins == 0, "star counts are reset for the new round");
        CHECK(p.a.objects[s].hasComponent<RenderableComponent>() && p.b.objects[s].hasComponent<RenderableComponent>(),
              "collected stars come back in the new round");
        CHECK(P(p.a)->x < 600.0f && P(p.b)->x < 600.0f, "both players go back to the start");
        CHECK(feedHas(p.b, "Player 1 reached the portal and won round 1"), "player 2's feed announces the winner");
    }
    { // event timestamps come from the clock the game provides (its Timeline)
        Pair p;
        p.a.clock = [] { return int64_t(4242); };
        int64_t seen = -1;
        p.b.events.subscribe(EventType::Collision, [&](const GameEvent& e) { if (e.sourceId == 1) seen = e.timestamp; });
        int s = starIndex(p.a, 1);
        auto* t = p.a.objects[s].getComponent<TransformComponent>();
        P(p.a)->x = t->x - 10; P(p.a)->y = t->y - 40;
        p.run({}, {}, 1.0f / 60);
        CHECK(seen == 4242, "events carry the Timeline timestamp");
    }
    std::printf("\n%s (%d failures)\n", failures ? "RACE TESTS FAILED" : "ALL RACE TESTS PASSED", failures);
    return failures ? 1 : 0;
}
