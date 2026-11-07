#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <thread>

#include "GameLogic.h"
#include "Input.h"
#include "Timeline.h"
#include "SharedData.h"
#include "Networking.h"

// Milestone 4 individual game: Princess Star Race.
// Up to four princesses race through the same level, each in their own
// window. Run one window per player from the build folder:
//   ./game 1      ./game 2      (./game 3, ./game 4)
// Stars are shared: whoever grabs one first gets it, and it disappears for
// everyone. The first princess to reach the portal wins the round.
//
// Controls
//   A / D       move left / right
//   Shift       run
//   W           jump
//   P           pause / unpause
//   - / +       slow down / speed up the game (Timeline scale)
//   H or F1     show the hidden objects (spawn points, death zones,
//               scroll boundaries)
//   Esc         quit
//
// Run it with a player number: ./game 1 in one window, ./game 2 in another.

// Draws one GameObject. Objects with neither a Renderable nor a Color are
// hidden and are skipped.
static void drawObject(SDL_Renderer* renderer, GameObject& obj, float cameraX,
                       bool flip = false)
{
    auto* t = obj.getComponent<TransformComponent>();
    auto* r = obj.getComponent<RenderableComponent>();
    auto* color = obj.getComponent<ColorComponent>();

    if (!t || (!r && !color)) {
        return;
    }

    // Far-away scenery (clouds) scrolls slower than the level
    if (auto* p = obj.getComponent<ParallaxComponent>()) {
        cameraX *= p->factor;
    }

    SDL_FRect dst = { t->x - cameraX, t->y, t->width, t->height };

    // Skip anything completely off screen
    if (dst.x + dst.w < 0.0f || dst.x > level::SCREEN_W) {
        return;
    }

    if (color) {
        SDL_SetRenderDrawColor(renderer, color->r, color->g, color->b, color->a);
        SDL_RenderFillRect(renderer, &dst);
        return;
    }

    if (!r->texture) {
        // No texture loaded: plain rectangle
        SDL_SetRenderDrawColor(renderer, 150, 100, 70, 255);
        SDL_RenderFillRect(renderer, &dst);
        return;
    }

    if (auto* tile = obj.getComponent<TileComponent>()) {
        for (float x = 0.0f; x < t->width; x += tile->tileWidth) {
            SDL_FRect piece = { dst.x + x, dst.y, tile->tileWidth, dst.h };
            SDL_RenderTexture(renderer, r->texture, nullptr, &piece);
        }
        return;
    }

    if (r->frameCount > 1 && r->frameWidth > 0 && r->frameHeight > 0) {
        SDL_FRect src = {
            static_cast<float>(r->currentFrame * r->frameWidth), 0.0f,
            static_cast<float>(r->frameWidth),
            static_cast<float>(r->frameHeight)
        };
        SDL_RenderTextureRotated(renderer, r->texture, &src, &dst, 0.0, nullptr,
                                 flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    } else {
        SDL_RenderTexture(renderer, r->texture, nullptr, &dst);
    }
}

// Each player has their own princess color
static void playerTint(int id, Uint8& r, Uint8& g, Uint8& b)
{
    switch (id) {
        case 2:  r = 140; g = 190; b = 255; break;   // blue
        case 3:  r = 160; g = 255; b = 160; break;   // green
        case 4:  r = 255; g = 220; b = 120; break;   // gold
        default: r = 255; g = 255; b = 255; break;   // player 1: original colors
    }
}

// Draws another player's princess at the position the network gave us.
struct RemoteLook {
    float lastX = 0.0f;
    bool facingLeft = false;
    int frame = 0;
    float distance = 0.0f;   // walked since the last animation frame
    bool seen = false;
};

static void drawRemotePrincess(SDL_Renderer* renderer, SDL_Texture* tex, int id,
                               float x, float y, float cameraX, RemoteLook& look)
{
    // Work out walking direction and animation from how far they moved
    if (look.seen) {
        float dx = x - look.lastX;
        if (dx < -0.5f) look.facingLeft = true;
        if (dx > 0.5f) look.facingLeft = false;
        look.distance += std::fabs(dx);
        if (look.distance > 40.0f) {
            look.frame = (look.frame + 1) % 8;
            look.distance = 0.0f;
        }
    }
    look.lastX = x;
    look.seen = true;

    SDL_FRect dst = { x - cameraX, y, 80.0f, 130.0f };
    if (dst.x + dst.w < 0.0f || dst.x > level::SCREEN_W) {
        return;
    }
    if (tex) {
        Uint8 r, g, b;
        playerTint(id, r, g, b);
        SDL_SetTextureColorMod(tex, r, g, b);
        SDL_FRect src = { static_cast<float>(look.frame * 96), 0.0f, 96.0f, 156.0f };
        SDL_RenderTextureRotated(renderer, tex, &src, &dst, 0.0, nullptr,
                                 look.facingLeft ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
        SDL_SetTextureColorMod(tex, 255, 255, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 120, 160, 255, 255);
        SDL_RenderFillRect(renderer, &dst);
    }
}

// Small "P1"/"P2" label above a princess
static void drawLabel(SDL_Renderer* renderer, int id, float x, float y, float cameraX)
{
    char label[8];
    std::snprintf(label, sizeof(label), "P%d", id);
    SDL_SetRenderScale(renderer, 2.0f, 2.0f);
    SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
    SDL_RenderDebugText(renderer, (x - cameraX + 28.0f) / 2.0f, (y - 22.0f) / 2.0f, label);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
}

// Debug view: draws the hidden objects as translucent shapes.
static void drawHiddenObjects(SDL_Renderer* renderer, GameWorld& w)
{
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    for (auto& obj : w.objects) {
        auto* t = obj.getComponent<TransformComponent>();
        if (!t) {
            continue;
        }
        if (obj.hasComponent<RenderableComponent>() ||
            obj.hasComponent<ColorComponent>()) {
            continue;   // visible objects are not hidden
        }

        if (obj.hasComponent<SpawnPointComponent>()) {
            SDL_FRect marker = { t->x - w.cameraX - 14.0f, t->y - 90.0f, 28.0f, 90.0f };
            SDL_SetRenderDrawColor(renderer, 60, 220, 90, 150);
            SDL_RenderFillRect(renderer, &marker);

            auto* sp = obj.getComponent<SpawnPointComponent>();
            char label[16];
            std::snprintf(label, sizeof(label), "SPAWN %d", sp->index);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDebugText(renderer, marker.x - 14.0f, marker.y - 12.0f, label);
        }
        else if (obj.hasComponent<DeathZoneComponent>()) {
            SDL_FRect zone = { t->x - w.cameraX, t->y, t->width, t->height };
            SDL_SetRenderDrawColor(renderer, 230, 50, 50, 110);
            SDL_RenderFillRect(renderer, &zone);
        }
        else if (obj.hasComponent<ScrollBoundaryComponent>()) {
            SDL_FRect line = { t->x - w.cameraX, t->y, t->width, t->height };
            SDL_SetRenderDrawColor(renderer, 60, 120, 255, 140);
            SDL_RenderFillRect(renderer, &line);
        }
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

int main(int argc, char* argv[])
{
    // Which player this window is (1-4). Each window needs a different one.
    int clientId = 1;
    if (argc >= 2) {
        clientId = std::atoi(argv[1]);
    }
    if (clientId < 1 || clientId > 4) {
        SDL_Log("Usage: ./game <player 1-4>");
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("Could not initialize SDL: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;

    char title[64];
    std::snprintf(title, sizeof(title), "Princess Star Race - Player %d", clientId);
    if (!SDL_CreateWindowAndRenderer(title, 1280, 720,
                                     SDL_WINDOW_RESIZABLE,
                                     &window, &renderer)) {
        SDL_Log("Could not create window/renderer: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // The game is laid out for 1920x1080 and scales to any window size
    SDL_SetRenderLogicalPresentation(renderer,
                                     static_cast<int>(level::SCREEN_W),
                                     static_cast<int>(level::SCREEN_H),
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);

    // Textures
    LevelTextures tex;
    tex.player = IMG_LoadTexture(renderer, "../assets/my_player.png");
    tex.platform = IMG_LoadTexture(renderer, "../assets/my_platform.png");
    tex.crate  = IMG_LoadTexture(renderer, "../assets/my_crate.png");
    tex.coin   = IMG_LoadTexture(renderer, "../assets/coin.png");
    tex.bush   = IMG_LoadTexture(renderer, "../assets/bush.png");
    tex.cloud  = IMG_LoadTexture(renderer, "../assets/cloud.png");
    tex.background = IMG_LoadTexture(renderer, "../assets/background.png");
    tex.skull  = IMG_LoadTexture(renderer, "../assets/enemy_skull.png");
    tex.portal = IMG_LoadTexture(renderer, "../assets/swirlingorb.png");
    tex.totem  = IMG_LoadTexture(renderer, "../assets/totem.png");

    if (!tex.player || !tex.platform || !tex.crate || !tex.coin || !tex.bush ||
        !tex.cloud || !tex.background || !tex.skull || !tex.portal || !tex.totem) {
        SDL_Log("Some textures did not load (run from the build folder): %s",
                SDL_GetError());
    }

    Timeline gameTime;

    GameWorld world;
    world.clientId = clientId;
    world.clock = [&gameTime]() { return gameTime.getTime(); };   // event timestamps
    game::buildLevel(world, tex);

    // Moving platforms and the skull follow the wall clock, so they are in
    // the same place in every player's window.
    world.time = std::fmod(std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count(), 3600.0);

    // ---- Networking (team engine, peer-to-peer over ZeroMQ) ----
    SharedData shared;
    shared.eventManager = &world.events;   // events from other players are raised here
    world.events.setNetworkSink([&shared](const GameEvent& e) { shared.queueOutgoingEvent(e); });
    {
        auto* pt = world.player.getComponent<TransformComponent>();
        shared.playerX = pt->x;
        shared.playerY = pt->y;
    }
    std::thread networkThread(networkingThread, std::ref(shared), clientId);

    // Tell the others we are here (a networked Spawn event)
    world.events.raiseNetworked(game::makeEvent(world, EventType::Spawn,
                                                EventPriority::High, "joined", 0));

    std::map<int, RemoteLook> remoteLooks;
    std::set<int> playersLastFrame;

    bool running = true;
    bool showHidden = false;
    bool f1WasPressed = false;
    bool pauseWasPressed = false;
    bool minusWasPressed = false;
    bool plusWasPressed = false;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }

        if (Input::isKeyPressed(SDL_SCANCODE_ESCAPE)) {
            running = false;
        }

        float dt = static_cast<float>(gameTime.getDeltaTime());

        // Pause / unpause
        bool pauseNow = Input::isKeyPressed(SDL_SCANCODE_P);
        if (pauseNow && !pauseWasPressed) {
            if (gameTime.isPaused()) {
                gameTime.unpause();
            } else {
                gameTime.pause();
            }
        }
        pauseWasPressed = pauseNow;

        // Timeline speed: 0.5x, 1x, 2x
        bool minusNow = Input::isKeyPressed(SDL_SCANCODE_MINUS);
        bool plusNow = Input::isKeyPressed(SDL_SCANCODE_EQUALS);
        if (minusNow && !minusWasPressed) {
            double s = gameTime.getScale();
            if (s == 2.0) gameTime.setScale(1.0);
            else if (s == 1.0) gameTime.setScale(0.5);
        }
        if (plusNow && !plusWasPressed) {
            double s = gameTime.getScale();
            if (s == 0.5) gameTime.setScale(1.0);
            else if (s == 1.0) gameTime.setScale(2.0);
        }
        minusWasPressed = minusNow;
        plusWasPressed = plusNow;

        // Show / hide the hidden objects
        bool f1Now = Input::isKeyPressed(SDL_SCANCODE_F1) ||
                     Input::isKeyPressed(SDL_SCANCODE_H);   // F1 needs Fn on a Mac
        if (f1Now && !f1WasPressed) {
            showHidden = !showHidden;
        }
        f1WasPressed = f1Now;

        // Read the keyboard into a GameInput and advance the game
        GameInput input;
        input.left  = Input::isKeyPressed(SDL_SCANCODE_A);
        input.right = Input::isKeyPressed(SDL_SCANCODE_D);
        input.jump  = Input::isKeyPressed(SDL_SCANCODE_W);
        input.run   = Input::isKeyPressed(SDL_SCANCODE_LSHIFT) ||
                      Input::isKeyPressed(SDL_SCANCODE_RSHIFT);

        shared.timeScale.store(gameTime.getScale());

        if (!gameTime.isPaused()) {
            game::step(world, input, dt);
        }

        // Share our position with the other players
        {
            auto* pt = world.player.getComponent<TransformComponent>();
            std::lock_guard<std::mutex> lock(shared.playerMutex);
            shared.playerX = pt->x;
            shared.playerY = pt->y;
        }

        // Copy the other players' positions (the network thread updates them)
        std::map<int, RemotePlayerState> others;
        {
            std::lock_guard<std::mutex> lock(shared.playerMutex);
            for (const auto& entry : shared.remotePlayers) {
                others[entry.first] = entry.second;
            }
        }
        for (int id : playersLastFrame) {
            if (!others.count(id)) {
                game::addFeed(world, "Player " + std::to_string(id) + " left the race");
                remoteLooks.erase(id);
            }
        }
        playersLastFrame.clear();
        for (const auto& entry : others) {
            playersLastFrame.insert(entry.first);
        }

        // ---- Draw ----
        SDL_SetRenderDrawColor(renderer, 169, 186, 157, 255);
        SDL_RenderClear(renderer);

        if (tex.background) {
            SDL_RenderTexture(renderer, tex.background, nullptr, nullptr);
        }

        for (auto& obj : world.objects) {
            drawObject(renderer, obj, world.cameraX);
        }
        // Other players first, so your own princess is on top
        for (const auto& entry : others) {
            drawRemotePrincess(renderer, tex.player, entry.first, entry.second.x,
                               entry.second.y, world.cameraX, remoteLooks[entry.first]);
            drawLabel(renderer, entry.first, entry.second.x, entry.second.y, world.cameraX);
        }

        {
            Uint8 r, g, b;
            playerTint(clientId, r, g, b);
            if (tex.player) SDL_SetTextureColorMod(tex.player, r, g, b);
            drawObject(renderer, world.player, world.cameraX, world.facingLeft);
            if (tex.player) SDL_SetTextureColorMod(tex.player, 255, 255, 255);
            auto* pt = world.player.getComponent<TransformComponent>();
            drawLabel(renderer, clientId, pt->x, pt->y, world.cameraX);
        }

        if (showHidden) {
            drawHiddenObjects(renderer, world);
        }

        // On-screen help and counters
        SDL_SetRenderScale(renderer, 2.0f, 2.0f);
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderDebugText(renderer, 10.0f, 8.0f,
                            "A/D move  Shift run  W jump  P pause  H hidden objects");

        char status[160];
        std::snprintf(status, sizeof(status),
                      "PLAYER %d (you)  Round %d  Stars: %d/%d  Deaths: %d  Wins: %d  Speed: %.1fx",
                      clientId, world.round, world.coins, world.totalCoins, world.deaths,
                      world.wins, gameTime.getScale());
        SDL_RenderDebugText(renderer, 10.0f, 24.0f, status);

        // One line per other player
        float lineY = 40.0f;
        for (const auto& entry : others) {
            int id = entry.first;
            std::snprintf(status, sizeof(status), "Player %d          Stars: %d  Deaths: %d  Wins: %d",
                          id, world.remoteStars[id], world.remoteDeaths[id], world.remoteWins[id]);
            SDL_RenderDebugText(renderer, 10.0f, lineY, status);
            lineY += 16.0f;
        }
        if (others.empty()) {
            SDL_RenderDebugText(renderer, 10.0f, lineY, "Waiting for other players... (start ./game 2)");
            lineY += 16.0f;
        }

        std::snprintf(status, sizeof(status), "Last input event: %s",
                      world.lastInput.empty() ? "-" : world.lastInput.c_str());
        SDL_RenderDebugText(renderer, 10.0f, lineY, status);
        lineY += 16.0f;

        if (gameTime.isPaused()) {
            SDL_RenderDebugText(renderer, 10.0f, lineY, "PAUSED");
            lineY += 16.0f;
        }

        // Event feed: the newest messages from the event handlers
        lineY += 8.0f;
        int shown = 0;
        for (auto it = world.feed.rbegin(); it != world.feed.rend() && shown < 6; ++it) {
            if (world.time - it->shownAt > 6.0) {
                break;   // older messages fade out
            }
            SDL_SetRenderDrawColor(renderer, 150, 20, 60, 255);
            SDL_RenderDebugText(renderer, 10.0f, lineY, it->text.c_str());
            lineY += 12.0f;
            shown++;
        }
        SDL_SetRenderScale(renderer, 1.0f, 1.0f);

        SDL_RenderPresent(renderer);
    }

    // Stop the networking thread
    shared.running.store(false);
    if (networkThread.joinable()) {
        networkThread.join();
    }

    SDL_DestroyTexture(tex.player);
    SDL_DestroyTexture(tex.platform);
    SDL_DestroyTexture(tex.crate);
    SDL_DestroyTexture(tex.coin);
    SDL_DestroyTexture(tex.bush);
    SDL_DestroyTexture(tex.cloud);
    SDL_DestroyTexture(tex.background);
    SDL_DestroyTexture(tex.skull);
    SDL_DestroyTexture(tex.portal);
    SDL_DestroyTexture(tex.totem);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
