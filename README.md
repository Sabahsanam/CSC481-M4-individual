# CSC 481/581 Milestone 4 - Individual Game: Princess Star Race (Sabah Afroz)

A two-to-four player race built on Team 8's game engine.
Team repository: https://github.com/mglenn001/CSC481-581Project

Each player runs the game in their own window. All princesses are in the same
side-scrolling level at the same time. Stars are shared: whoever grabs a star
first gets it, and it disappears for everyone. The first princess to reach the
portal wins the round, and the next round starts for all players.

Everything that happens in the game goes through the team's event manager
(`EventManager.h`), and the important events are shared over the team's
peer-to-peer ZeroMQ networking (`Networking.cpp`, `NetworkEvent.h`).

## Build and run (macOS / Linux)
Requires CMake, SDL3, SDL3_image, ZeroMQ and cppzmq
(macOS: `brew install sdl3 sdl3_image zeromq cppzmq`).

    mkdir build && cd build
    cmake ..
    make

Run one window per player, each from the build folder (assets load from ../assets):

    ./game 1        # first terminal
    ./game 2        # second terminal

## Controls
| Key | Action |
|-----|--------|
| A / D | move left / right |
| Shift | run |
| W | jump |
| P | pause |
| - / = | slow down / speed up the game clock (0.5x, 1x, 2x) |
| H or F1 | show hidden objects (spawn points, death zones, scroll boundaries) |
| Esc | quit |

## How the game uses events (Milestone 4)
| Event | Raised when | Handled by | Sent to other players? |
|-------|-------------|------------|------------------------|
| Input | every key held in a frame (`left`, `right`, `jump`, `run`) | builds this frame's movement | no |
| Collision `star` | the princess touches a star | star is collected and hidden; the other players hide it too | yes |
| Collision `goal` | the princess touches the portal | round won; every player starts a new round | yes |
| Collision `pit` / `skull` | the princess touches a hidden pit or the skull | raises a Death event | no |
| Death | from a pit/skull collision | death counted, raises Spawn; others show "Player N fell into a pit" | yes |
| Spawn | from a death (and `joined` at start) | princess moved to the checkpoint; others show it in the feed | yes |

If two players grab the same star before hearing about each other, every
client applies the same rule (the lower player number keeps it), so all
windows agree without a server.

The on-screen event feed shows the latest events, and the HUD shows every
player's stars, deaths and wins.

## Source layout
- Team engine: `GameObject.h`, `Component.h`, `Components.h`, `Physics.*`, `Collision.*`, `Timeline.*`,
  `EventManager.h`, `NetworkEvent.h`, `Networking.*`, `SharedData.h`, `NetworkFormat.h`.
- `src/GameComponents.h`: components added for this game (movement, spawn point, death zone, scroll boundary, goal, blocker, collectible, parallax, tile, color).
- `src/LevelObjects.h`: factory functions that build each kind of object from components.
- `src/GameLogic.h`: the game rules as systems that raise events, plus the event handlers and `buildLevel`.
- `src/Game.cpp`: window, keyboard, networking thread, drawing, HUD and event feed.

## Tests
`make check` builds and runs all tests:
- `logic_test`: level rules from Milestone 3 (landing, moving platforms, scrolling, death zones, blockers, stars), now running through events.
- `bot`: an automatic player that must reach the portal.
- `race_test`: the two-player race. Two game worlds pass networked events to each other: shared stars, ties, deaths and respawns shown to the other player, round wins, and that input and pit collisions stay local.

## Credits
Art (princess, platforms, crate, star, bush, cloud, background) is from my Milestone 1 project. Skull, portal and totem art are from the team assets.
AI assistance is disclosed in my written report.
