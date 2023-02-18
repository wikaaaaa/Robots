#ifndef SERVER_COMMON_H
#define SERVER_COMMON_H

//komunikaty do serwera:
#define JOIN 0
#define PLACE_BOMB 1
#define PLACE_BLOCK 2
#define MOVE 3

// komunikaty od serwera
#define HELLO 0
#define ACCEPTED_PLAYER 1
#define GAME_STARTED 2
#define TURN 3
#define GAME_ENDED 4

#define LOBBY 0

// events
#define BOMB_PLACED 0
#define BOMB_EXPLODED 1
#define PLAYER_MOVED 2
#define BLOCK_PLACED 3

// komunikaty od gui
#define PLACE_BOMB_GUI 0
#define PLACE_BLOCK_GUI 1
#define MOVE_GUI 2

typedef struct from_gui {
    uint8_t type;
    uint8_t direction;
} from_gui;

typedef struct Player {
    std::string name;
    std::string address;
} Player;

typedef std::pair<uint16_t, uint16_t> Position;

typedef struct Bomb {
    Position position;
    uint16_t timer;
} Bomb;

typedef uint8_t PlayerId;

typedef uint32_t BombId;

typedef uint32_t Score;

typedef struct server_info {
    std::string server_name;
    uint8_t player_count;
    uint16_t size_x;
    uint16_t size_y;
    uint16_t game_length;
    uint16_t explosion_radius;
    uint16_t bomb_timer;
} server_info;

typedef struct game_info {
    std::map<PlayerId , Player> players;
    uint16_t turn;
    std::map<PlayerId, Position> player_positions;
    std::set<Position> blocks;
    std::set<Position> explosions;
    std::map<PlayerId , Score> scores;
    std::map<BombId, Bomb> bombs;
} game_info;

typedef struct param {
    std::string gui_host_name_or_ip;
    std::string gui_port;
    std::string player_name;
    uint16_t port;
    std::string server_host_name_or_ip;
    std::string server_port;
} param;

#endif //SERVER_COMMON_H
