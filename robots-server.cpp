#include <boost/program_options.hpp>
#include <iostream>
#include <iterator>
#include <algorithm>
#include <boost/asio.hpp>
#include <map>
#include <boost/array.hpp>
#include <thread>
#include <set>
#include "common.h"
#include <random>
#include <cstdint>
#include <memory>
#include <list>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <boost/system/system_error.hpp>

#define UP 0
#define RIGHT 1
#define DOWN 2
#define LEFT 3

using boost::asio::ip::tcp;
using boost::asio::ip::udp;
using std::string;
using std::endl;
namespace po = boost::program_options;
std::atomic <std::uint8_t> game_state(0);
std::atomic<bool> send_join(true);

std::map <PlayerId, Score> scores;

std::map <uint16_t, boost::asio::mutable_buffer> old_turn_mess;

bool ready = false;

typedef struct server_param {
    uint16_t bomb_timer;
    uint8_t players_count;
    uint64_t turn_duration; //milisekundy
    uint16_t explosion_radius;
    uint16_t initial_blocks;
    uint16_t game_length;
    string server_name;
    uint16_t port;
    uint32_t seed;
    uint16_t size_x;
    uint16_t size_y;
} server_param;

std::atomic <PlayerId> next_free_id(0);

std::atomic <uint32_t> next_bomb_id(0);

std::atomic <uint8_t> number_of_players(0);

std::atomic <uint8_t> number_of_game_started_sent(0);

std::map <PlayerId, Player> players;

std::map <PlayerId, Position> players_positions;

std::atomic <uint8_t> turn(0);

std::atomic<bool> new_game(false);

std::condition_variable condition;

std::set <Position> blocks;

std::map <BombId, Bomb> bombs;

typedef struct Event {
    uint8_t type;
    uint8_t id;
    uint32_t bomb_id;
    Position position;
    std::list <PlayerId> robots_destroyed;
    std::list <Position> blocks_destroyed;
} Event;

std::list <Event> events;

std::set <PlayerId> acc_players;

std::map <std::pair<PlayerId, uint16_t>, Event> turn_and_move;

std::minstd_rand random_number;

server_param parameters;

std::mutex mutex;

std::atomic<bool> is_playing[25];

namespace {

    int parse_param(int ac, char **av, server_param *p) {
        try {
            po::options_description desc("Allowed options");
            desc.add_options()
                    ("help,h", "Wypisuje jak uzywać programu")
                    ("port,p", po::value<uint16_t>(&(p->port))->required())
                    ("bomb-timer,b", po::value<uint16_t>(&(p->bomb_timer))->required())
                    ("players-count,c", po::value<uint16_t>()->required())
                    ("turn-duration,d", po::value<uint64_t>(&(p->turn_duration))->required())
                    ("explosion-radius,e", po::value<uint16_t>(&(p->explosion_radius))->required())
                    ("initial-blocks,k", po::value<uint16_t>(&(p->initial_blocks))->required())
                    ("game-length,l", po::value<uint16_t>(&(p->game_length))->required())
                    ("server-name,n", po::value<string>(&(p->server_name))->required())
                    ("seed,s", po::value<uint32_t>(&(p->seed))->default_value(
                            (uint32_t) std::chrono::system_clock::now().time_since_epoch().count()),
                     "parametr opcjonalny")
                    ("size-x,x", po::value<uint16_t>(&(p->size_x))->required())
                    ("size-y,y", po::value<uint16_t>(&(p->size_y))->required());
            po::variables_map vm;
            po::store(po::parse_command_line(ac, av, desc), vm);
            if (vm.count("help")) {
                std::cout << desc << "\n";
                return 1;
            }
            po::notify(vm);
            if (vm.count("players-count")) {
                uint16_t help = vm["players-count"].as<uint16_t>();
                p->players_count = (uint8_t) help;
            }
        } catch (std::exception &e) {
            std::cerr << "Error: " << e.what() << endl;
            exit(1);
        }
        return 0;
    }

    boost::asio::mutable_buffer hello_mess(const server_param &p) {
        auto size_string = (uint8_t) p.server_name.length();
        size_t size = size_string + 1 + 1 + 1 + 10;
        size_t where = 0;
        char *name = new char[size];
        uint8_t type = 0;
        memcpy(name, &type, 1);
        ++where;
        memcpy(name + where, &size_string, 1);
        ++where;
        memcpy(name + where, p.server_name.c_str(), p.server_name.length());
        where += p.server_name.length();
        memcpy(name + where, &(p.players_count), 1);
        ++where;
        uint16_t help = htons(p.size_x);
        memcpy(name + where, &(help), 2);
        where += 2;
        help = htons(p.size_y);
        memcpy(name + where, &(help), 2);
        where += 2;
        help = htons(p.game_length);
        memcpy(name + where, &(help), 2);
        where += 2;
        help = htons(p.explosion_radius);
        memcpy(name + where, &(help), 2);
        where += 2;
        help = htons(p.bomb_timer);
        memcpy(name + where, &(help), 2);
        where += 2;
        return boost::asio::buffer(name, size);
    }

    boost::asio::mutable_buffer accepted_player_mess(PlayerId id) {
        Player player = players[id];
        size_t size = 1 + 1 + 1 + 1 + player.name.length() + player.address.length();
        char *message = new char[size];
        uint8_t type = 1;
        size_t where = 0;
        memcpy(message + where, &type, 1);
        ++where;
        memcpy(message + where, &id, 1);
        ++where;
        auto sizee = (uint8_t) player.name.length();
        memcpy(message + where, &sizee, 1);
        where += 1;
        memcpy(message + where, player.name.c_str(), sizee);
        where += sizee;
        sizee = (uint8_t) player.address.length();
        memcpy(message + where, &sizee, 1);
        where += 1;
        memcpy(message + where, player.address.c_str(), sizee);
        where += sizee;
        return boost::asio::buffer(message, size);
    }

    size_t size_of_map() {
        size_t size = 0;
        for (auto &it : players) {
            size += 3;
            size += it.second.name.length();
            size += it.second.address.length();
        }
        return size;
    }

    boost::asio::mutable_buffer game_started_message() {
        size_t size = 1 + 4;
        size += size_of_map();
        char *message = new char[size];
        uint8_t type = 2;
        size_t where = 0;
        memcpy(message + where, &type, 1);
        ++where;
        uint32_t size_of_map = htonl((uint32_t) players.size());
        memcpy(message + where, &(size_of_map), 4);
        where += 4;
        for (auto &player : players) {
            memcpy(message + where, &(player.first), 1);
            where += 1;
            auto sizee = (uint8_t) player.second.name.length();
            memcpy(message + where, &sizee, 1);
            where += 1;
            memcpy(message + where, player.second.name.c_str(), sizee);
            where += sizee;
            sizee = (uint8_t) player.second.address.length();
            memcpy(message + where, &sizee, 1);
            where += 1;
            memcpy(message + where, player.second.address.c_str(), sizee);
            where += sizee;
        }
        return boost::asio::buffer(message, size);
    }

    size_t size_of_events() {
        size_t size = 0;
        size += 4;
        for (auto &event: events) {
            size += 1;
            switch (event.type) {
                case BOMB_PLACED:
                    size += 8;
                    break;
                case BOMB_EXPLODED:
                    size += 4;
                    size += 4;
                    size += event.robots_destroyed.size();
                    size += 4;
                    size += event.blocks_destroyed.size() * 4;
                    break;
                case PLAYER_MOVED:
                    size += 1;
                    size += 4;
                    break;
                case BLOCK_PLACED:
                    size += 4;
                    break;
            }
        }
        return size;
    }

    boost::asio::mutable_buffer turn_message(uint16_t turn_nb) {
        size_t size = 1 + 2 + size_of_events();
        char *message = new char[size];
        uint8_t type = TURN;
        size_t where = 0;
        memcpy(message + where, &type, 1);
        ++where;
        uint16_t turn_ = htons(turn_nb);
        memcpy(message + where, &turn_, 2);
        where += 2;
        uint32_t size_of_list = htonl((uint32_t) events.size());
        memcpy(message + where, &size_of_list, 4);
        where += 4;
        for (auto &event: events) {
            memcpy(message + where, &(event.type), 1);
            ++where;
            switch (event.type) {
                case BOMB_PLACED: {
                    uint32_t bombID = htonl(event.bomb_id);
                    memcpy(message + where, &bombID, 4);
                    where += 4;
                    uint16_t help = htons(event.position.first);
                    memcpy(message + where, &help, 2);
                    where += 2;
                    help = htons(event.position.second);
                    memcpy(message + where, &help, 2);
                    where += 2;
                    break;
                }
                case BOMB_EXPLODED: {
                    uint32_t bombID = htonl(event.bomb_id);
                    memcpy(message + where, &bombID, 4);
                    where += 4;
                    size_of_list = htonl((uint32_t) event.robots_destroyed.size());
                    memcpy(message + where, &size_of_list, 4);
                    where += 4;
                    for (auto p_id : event.robots_destroyed) {
                        memcpy(message + where, &(p_id), 1);
                        ++where;
                    }

                    size_of_list = htonl((uint32_t) event.blocks_destroyed.size());
                    memcpy(message + where, &size_of_list, 4);
                    where += 4;
                    for (auto pos : event.blocks_destroyed) {
                        uint16_t help = htons(pos.first);
                        memcpy(message + where, &help, 2);
                        where += 2;
                        help = htons(pos.second);
                        memcpy(message + where, &help, 2);
                        where += 2;
                    }
                    break;
                }
                case PLAYER_MOVED: {
                    memcpy(message + where, &(event.id), 1);
                    ++where;
                    uint16_t help = htons(event.position.first);
                    memcpy(message + where, &help, 2);
                    where += 2;
                    help = htons(event.position.second);
                    memcpy(message + where, &help, 2);
                    where += 2;
                    break;
                }
                case BLOCK_PLACED: {
                    uint16_t help = htons(event.position.first);
                    memcpy(message + where, &help, 2);
                    where += 2;
                    help = htons(event.position.second);
                    memcpy(message + where, &help, 2);
                    where += 2;
                    break;
                }
            }
        }
        return boost::asio::buffer(message, size);
    }

    boost::asio::mutable_buffer game_ended_mess() {
        size_t size = 1 + 4 + scores.size() * 5;
        char *message = new char[size];
        uint8_t type = GAME_ENDED;
        size_t where = 0;
        memcpy(message + where, &type, 1);
        ++where;
        for (auto score : scores) {
            memcpy(message + where, &(score.first), 1);
            ++where;
            uint32_t help = htonl(score.second);
            memcpy(message + where, &help, 4);
            where += 4;
        }
        return boost::asio::buffer(message, size);
    }

    bool receive_join(tcp::socket &tcp_socket, string *result, bool *disconnected) {
        boost::system::error_code error;
        try {
            uint8_t size[1];
            boost::asio::read(tcp_socket, boost::asio::buffer(size), error);
            if (error == boost::asio::error::eof) {
                *disconnected = true;
                return false;
            }
            char name[size[0] + 1];
            boost::asio::read(tcp_socket, boost::asio::buffer(name, size[0]), error);
            if (error == boost::asio::error::eof) {
                *disconnected = true;
                return false;
            }
            name[size[0]] = '\0';
            string name_Str(name);
            *result = name_Str;
            return true;
        } catch (std::exception &e) {
            std::cerr << "Exception in receiving join: " << e.what() << "\n";
            exit(1);
        }
    }


    PlayerId add_player(string player_name, string address) {
        Player new_player = {player_name, address};
        PlayerId my_id = next_free_id.fetch_add(1, std::memory_order_relaxed);
        printf("player id : %d\n", my_id);
        if (number_of_players.load() < parameters.players_count) {
            players[my_id] = new_player;
            acc_players.insert(my_id);
        }
        return my_id;
    }

    string get_address(tcp::socket &tcp_socket) {
        string a = tcp_socket.remote_endpoint().address().to_string();
        a.append(":");
        a.append(std::to_string(tcp_socket.remote_endpoint().port()));
        return a;
    }

    void place_bomb(PlayerId player_id) {
        Event new_event;
        Position pos = players_positions[player_id];
        u_int32_t bomb_id = next_bomb_id.fetch_add(1, std::memory_order_relaxed);
        new_event.type = 0;
        new_event.bomb_id = bomb_id;
        new_event.position = pos;
        turn_and_move[{player_id, turn.load()}] = new_event;
    }

    void place_block(PlayerId player_id) {
        Event new_event;
        Position pos = players_positions[player_id];
        new_event.type = 3;
        new_event.position = pos;
        turn_and_move[{player_id, turn.load()}] = new_event;
    }


    void player_moved(PlayerId player_id, uint8_t dirrection, server_param p) {
        Position pos = players_positions[player_id];
        switch (dirrection) {
            case UP:
                if (pos.second == p.size_y - 1) {
                    return;
                }
                pos.second += 1;
                break;

            case RIGHT:
                if (pos.first == p.size_x - 1) {
                    return;
                }
                pos.first += 1;
                break;

            case DOWN:
                if (pos.second == 0) {
                    return;
                }
                pos.second -= 1;
                break;

            case LEFT:
                if (pos.first == 0) {
                    return;
                }
                pos.first -= 1;
                break;
        }
        Event new_event;
        new_event.type = 2;
        new_event.position = pos;
        new_event.id = player_id;
        turn_and_move[{player_id, turn.load()}] = new_event;
    }

    void init_players_pos() {
        for (std::pair <PlayerId, Player> player: players) {
            PlayerId id = player.first;
            Position pos;
            uint64_t a = random_number() % parameters.size_x;
            pos.first = (uint16_t) a;
            a = random_number() % parameters.size_y;
            pos.second = (uint16_t) a;
            players_positions[id] = pos;
            Event new_event;
            new_event.type = PLAYER_MOVED;
            new_event.position = pos;
            new_event.id = id;
            events.push_back(new_event);
            scores[id] = 0;
        }
        for (int i = 0; i < parameters.initial_blocks; ++i) {
            uint64_t a = random_number() % parameters.size_x;
            uint16_t x = (uint16_t) a;
            a = random_number() % parameters.size_y;
            uint16_t y = (uint16_t) a;
            Position pos = {x, y};
            if (blocks.find(pos) == blocks.end()) {
                blocks.insert(pos);
                Event new_event;
                new_event.type = BLOCK_PLACED;
                new_event.position = pos;
                events.push_back(new_event);
            }
        }
    }

    bool receive_join_from_client(tcp::socket &tcp_socket, PlayerId *player_id, bool *disconnected) {
        boost::system::error_code error;
        try {
            uint8_t type[1];
            boost::asio::read(tcp_socket, boost::asio::buffer(type), error);
            if (error == boost::asio::error::would_block) {
                return false;
            }
            if (error == boost::asio::error::eof) {
                *disconnected = true;

                return false;
            }
            switch (type[0]) {
                case JOIN: {
                    string player_name;
                    bool not_disconected = receive_join(tcp_socket, &player_name, disconnected);
                    if (!not_disconected) {
                        return false;
                    }
                    string address = get_address(tcp_socket);
                    *player_id = add_player(player_name, address);
                    return true;
                }

                case PLACE_BOMB: {
                    // ignore
                    break;
                }

                case PLACE_BLOCK: {
                    // ignore
                    break;
                }

                case MOVE: {
                    // ignore
                    uint8_t dirrection[1];
                    boost::asio::read(tcp_socket, boost::asio::buffer(dirrection), error);
                    if (error == boost::asio::error::eof) {
                        *disconnected = true;
                        return false;
                    }
                    break;
                }
            }
            return false;
        } catch (std::exception &e) {
            std::cerr << "Exception in receiving join: " << e.what() << "\n";
            exit(1);
        }
    }

    bool receive_move_from_client(tcp::socket &tcp_socket, PlayerId player_id, server_param p, bool *disconnected) {
        boost::system::error_code error;
        try {
            uint8_t type[1];
            boost::asio::read(tcp_socket, boost::asio::buffer(type), error);
            if (error == boost::asio::error::would_block) {
                return true;
            }
            switch (type[0]) {
                case JOIN: {
                    // ignore
                    string player_name;
                    bool not_disconected = receive_join(tcp_socket, &player_name, disconnected);
                    if (!not_disconected) {
                        return false;
                    }
                    break;
                }

                case PLACE_BOMB: {
                    place_bomb(player_id);
                    break;
                }
                case PLACE_BLOCK: {
                    place_block(player_id);
                    break;
                }
                case MOVE: {
                    uint8_t dirrection[1];
                    boost::asio::read(tcp_socket, boost::asio::buffer(dirrection), error);
                    player_moved(player_id, dirrection[0], p);
                    break;
                }
            }
            return true;
        } catch (std::exception &e) {

            if (error == boost::asio::error::eof) {
                *disconnected = true;

                return false;
            }
            std::cerr << "Exception in receiving move: " << e.what() << "\n";
            exit(1);
        }
    }

    boost::asio::mutable_buffer handle_turn(uint16_t turn_nb) {
        for (std::pair <PlayerId, Player> player: players) {
            PlayerId id = player.first;
            if (players_positions.find(id) == players_positions.end()) {
                if (players_positions.find(id) == players_positions.end()) {
                    Position pos;
                    pos.first = (uint16_t) random_number() % parameters.size_x;
                    pos.second = (uint16_t) random_number() % parameters.size_y;
                    players_positions[id] = pos;
                    Event new_event;
                    new_event.type = PLAYER_MOVED;
                    new_event.position = pos;
                    new_event.id = id;
                    events.push_back(new_event);
                }
            } else {
                auto it = turn_and_move.find(std::pair<PlayerId, uint16_t>(id, turn_nb));
                if (it != turn_and_move.end()) {
                    Event event = it->second;
                    if (event.type == 2) {
                        if (blocks.find(event.position) == blocks.end()) {
                            players_positions[event.id] = event.position;
                            events.push_back(event);
                        }
                    } else if (event.type == 0) {
                        Bomb bomb = {event.position, parameters.bomb_timer};
                        bombs[event.bomb_id] = bomb;
                        events.push_back(event);
                    } else if (event.type == 3) {
                        if (blocks.find(event.position) == blocks.end()) {
                            blocks.insert(event.position);
                            events.push_back(event);
                        }
                    }
                }
            }
        }

        return turn_message(turn_nb);
    }

    bool will_robot_die(Position pos, PlayerId *id) {
        for (auto it : players_positions) {
            if (pos == it.second) {
                *id = it.first;
                return true;
            }
        }
        return false;
    }

    void calculate_explosions(Position bomb_position, Event *event) {
        uint16_t x = bomb_position.first;
        uint16_t y = bomb_position.second;
        PlayerId killed;
        for (uint16_t j = 1; j <= parameters.explosion_radius; ++j) {
            if (x + j < parameters.size_x) {
                if (will_robot_die({static_cast<uint16_t>(x + j), y}, &killed)) {
                    players_positions.erase(killed);
                    event->robots_destroyed.push_back(killed);
                }
                if (blocks.find({static_cast<uint16_t>(x + j), y}) != blocks.end()) {
                    event->blocks_destroyed.push_back({static_cast<uint16_t>(x + j), y});
                    break;
                }
            } else {
                break;
            }
        }
        for (uint16_t j = 1; j <= parameters.explosion_radius; ++j) {
            if (x - j >= 0) {
                if (will_robot_die({static_cast<uint16_t>(x - j), y}, &killed)) {
                    players_positions.erase(killed);
                    event->robots_destroyed.push_back(killed);
                }
                if (blocks.find({static_cast<uint16_t>(x - j), y}) != blocks.end()) {
                    event->blocks_destroyed.push_back({static_cast<uint16_t>(x - j), y});
                    break;
                }
            } else {
                break;
            }
        }
        for (uint16_t j = 1; j <= parameters.explosion_radius; ++j) {
            if (y - j >= 0) {
                if (will_robot_die({x, static_cast<uint16_t>(y - j)}, &killed)) {
                    players_positions.erase(killed);
                    event->robots_destroyed.push_back(killed);
                }
                if (blocks.find({x, static_cast<uint16_t>(y - j)}) != blocks.end()) {
                    event->blocks_destroyed.push_back({x, static_cast<uint16_t>(y - j)});
                    break;
                }
            } else {
                break;
            }
        }
        for (uint16_t j = 1; j <= parameters.explosion_radius; ++j) {
            if (y + j < parameters.size_y) {
                if (will_robot_die({x, static_cast<uint16_t>(y + j)}, &killed)) {
                    players_positions.erase(killed);
                    event->robots_destroyed.push_back(killed);
                }
                if (blocks.find({x, static_cast<uint16_t>(y + j)}) != blocks.end()) {
                    event->blocks_destroyed.push_back({x, static_cast<uint16_t>(y + j)});
                    break;
                }
            } else {
                break;
            }
        }
    }

    void handle_bombs() {
        for (auto &bomb : bombs) {
            bomb.second.timer -= 1;
            if (bomb.second.timer == 0) {
                Event event;
                event.type = 1;
                event.bomb_id = bomb.first;
                Position pos = bomb.second.position;
                PlayerId killed_robot;
                if (will_robot_die(pos, &killed_robot)) {
                    scores[killed_robot] += 1;
                    event.robots_destroyed.push_back(killed_robot);
                    players_positions.erase(killed_robot);
                }
                if (blocks.find(pos) != blocks.end()) {
                    event.blocks_destroyed.push_back(pos);
                } else {
                    calculate_explosions(pos, &event);
                }
                events.push_back(event);
            }
        }
        for (auto event : events) {
            bombs.erase(event.bomb_id);
            for (auto block : event.blocks_destroyed) {
                blocks.erase(block);
            }
        }

    }

    void
    master(tcp::socket &socket1, tcp::socket &socket2, tcp::socket &socket3, tcp::socket &socket4, tcp::socket &socket5,
           tcp::socket &socket6, tcp::socket &socket7, tcp::socket &socket8, tcp::socket &socket9,
           tcp::socket &socket10,
           tcp::socket &socket11, tcp::socket &socket12, tcp::socket &socket13, tcp::socket &socket14,
           tcp::socket &socket15, tcp::socket &socket16, tcp::socket &socket17, tcp::socket &socket18,
           tcp::socket &socket19, tcp::socket &socket20, tcp::socket &socket21, tcp::socket &socket22,
           tcp::socket &socket23, tcp::socket &socket24, tcp::socket &socket25) {
        while (true) {
            std::unique_lock <std::mutex> lck(mutex);
            condition.wait(lck, [] { return ready; });
            new_game = false;
            ready = false;
            turn = 1;
            boost::system::error_code error;
            boost::asio::mutable_buffer turn_mess = turn_message(0);
            old_turn_mess[0] = turn_mess;
            if (is_playing[0].load()) {
                boost::asio::write(socket1, turn_mess, error);
            }
            if (is_playing[1].load()) {
                boost::asio::write(socket2, turn_mess, error);
            }
            if (is_playing[2].load()) {
                boost::asio::write(socket3, turn_mess, error);
            }
            if (is_playing[3].load()) {
                boost::asio::write(socket4, turn_mess, error);
            }
            if (is_playing[4].load()) {
                boost::asio::write(socket5, turn_mess, error);
            }
            if (is_playing[5].load()) {
                boost::asio::write(socket6, turn_mess, error);
            }
            if (is_playing[6].load()) {
                boost::asio::write(socket7, turn_mess, error);
            }
            if (is_playing[7].load()) {
                boost::asio::write(socket8, turn_mess, error);
            }
            if (is_playing[8].load()) {
                boost::asio::write(socket9, turn_mess, error);
            }
            if (is_playing[9].load()) {
                boost::asio::write(socket10, turn_mess, error);
            }
            if (is_playing[10].load()) {
                boost::asio::write(socket11, turn_mess, error);
            }
            if (is_playing[11].load()) {
                boost::asio::write(socket12, turn_mess, error);
            }
            if (is_playing[12].load()) {
                boost::asio::write(socket13, turn_mess, error);
            }
            if (is_playing[12].load()) {
                boost::asio::write(socket14, turn_mess, error);
            }
            if (is_playing[14].load()) {
                boost::asio::write(socket15, turn_mess, error);
            }
            if (is_playing[15].load()) {
                boost::asio::write(socket16, turn_mess, error);
            }
            if (is_playing[16].load()) {
                boost::asio::write(socket17, turn_mess, error);
            }
            if (is_playing[17].load()) {
                boost::asio::write(socket18, turn_mess, error);
            }
            if (is_playing[18].load()) {
                boost::asio::write(socket19, turn_mess, error);
            }
            if (is_playing[19].load()) {
                boost::asio::write(socket20, turn_mess, error);
            }
            if (is_playing[20].load()) {
                boost::asio::write(socket21, turn_mess, error);
            }
            if (is_playing[21].load()) {
                boost::asio::write(socket22, turn_mess, error);
            }
            if (is_playing[22].load()) {
                boost::asio::write(socket23, turn_mess, error);
            }
            if (is_playing[23].load()) {
                boost::asio::write(socket24, turn_mess, error);
            }
            if (is_playing[24].load()) {
                boost::asio::write(socket25, turn_mess, error);
            }
            while (turn.load() != parameters.game_length + 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(parameters.turn_duration));
                handle_bombs();
                uint16_t done = turn.fetch_add(1);
                turn_mess = handle_turn(done);
                old_turn_mess[done] = turn_mess;
                if (is_playing[0].load()) {
                    boost::asio::write(socket1, turn_mess, error);
                }
                if (is_playing[1].load()) {
                    boost::asio::write(socket2, turn_mess, error);
                }
                if (is_playing[2].load()) {
                    boost::asio::write(socket3, turn_mess, error);
                }
                if (is_playing[3].load()) {
                    boost::asio::write(socket4, turn_mess, error);
                }
                if (is_playing[4].load()) {
                    boost::asio::write(socket5, turn_mess, error);
                }
                if (is_playing[5].load()) {
                    boost::asio::write(socket6, turn_mess, error);
                }
                if (is_playing[6].load()) {
                    boost::asio::write(socket7, turn_mess, error);
                }
                if (is_playing[7].load()) {
                    boost::asio::write(socket8, turn_mess, error);
                }
                if (is_playing[8].load()) {
                    boost::asio::write(socket9, turn_mess, error);
                }
                if (is_playing[9].load()) {
                    boost::asio::write(socket10, turn_mess, error);
                }
                if (is_playing[10].load()) {
                    boost::asio::write(socket11, turn_mess, error);
                }
                if (is_playing[11].load()) {
                    boost::asio::write(socket12, turn_mess, error);
                }
                if (is_playing[12].load()) {
                    boost::asio::write(socket13, turn_mess, error);
                }
                if (is_playing[12].load()) {
                    boost::asio::write(socket14, turn_mess, error);
                }
                if (is_playing[14].load()) {
                    boost::asio::write(socket15, turn_mess, error);
                }
                if (is_playing[15].load()) {
                    boost::asio::write(socket16, turn_mess, error);
                }
                if (is_playing[16].load()) {
                    boost::asio::write(socket17, turn_mess, error);
                }
                if (is_playing[17].load()) {
                    boost::asio::write(socket18, turn_mess, error);
                }
                if (is_playing[18].load()) {
                    boost::asio::write(socket19, turn_mess, error);
                }
                if (is_playing[19].load()) {
                    boost::asio::write(socket20, turn_mess, error);
                }
                if (is_playing[20].load()) {
                    boost::asio::write(socket21, turn_mess, error);
                }
                if (is_playing[21].load()) {
                    boost::asio::write(socket22, turn_mess, error);
                }
                if (is_playing[22].load()) {
                    boost::asio::write(socket23, turn_mess, error);
                }
                if (is_playing[23].load()) {
                    boost::asio::write(socket24, turn_mess, error);
                }
                if (is_playing[24].load()) {
                    boost::asio::write(socket25, turn_mess, error);
                }
                if (done == parameters.game_length) {
                    turn_mess = game_ended_mess();
                    if (is_playing[0].load()) {
                        boost::asio::write(socket1, turn_mess, error);
                    }
                    if (is_playing[1].load()) {
                        boost::asio::write(socket2, turn_mess, error);
                    }
                    if (is_playing[2].load()) {
                        boost::asio::write(socket3, turn_mess, error);
                    }
                    if (is_playing[3].load()) {
                        boost::asio::write(socket4, turn_mess, error);
                    }
                    if (is_playing[4].load()) {
                        boost::asio::write(socket5, turn_mess, error);
                    }
                    if (is_playing[5].load()) {
                        boost::asio::write(socket6, turn_mess, error);
                    }
                    if (is_playing[6].load()) {
                        boost::asio::write(socket7, turn_mess, error);
                    }
                    if (is_playing[7].load()) {
                        boost::asio::write(socket8, turn_mess, error);
                    }
                    if (is_playing[8].load()) {
                        boost::asio::write(socket9, turn_mess, error);
                    }
                    if (is_playing[9].load()) {
                        boost::asio::write(socket10, turn_mess, error);
                    }
                    if (is_playing[10].load()) {
                        boost::asio::write(socket11, turn_mess, error);
                    }
                    if (is_playing[11].load()) {
                        boost::asio::write(socket12, turn_mess, error);
                    }
                    if (is_playing[12].load()) {
                        boost::asio::write(socket13, turn_mess, error);
                    }
                    if (is_playing[12].load()) {
                        boost::asio::write(socket14, turn_mess, error);
                    }
                    if (is_playing[14].load()) {
                        boost::asio::write(socket15, turn_mess, error);
                    }
                    if (is_playing[15].load()) {
                        boost::asio::write(socket16, turn_mess, error);
                    }
                    if (is_playing[16].load()) {
                        boost::asio::write(socket17, turn_mess, error);
                    }
                    if (is_playing[17].load()) {
                        boost::asio::write(socket18, turn_mess, error);
                    }
                    if (is_playing[18].load()) {
                        boost::asio::write(socket19, turn_mess, error);
                    }
                    if (is_playing[19].load()) {
                        boost::asio::write(socket20, turn_mess, error);
                    }
                    if (is_playing[20].load()) {
                        boost::asio::write(socket21, turn_mess, error);
                    }
                    if (is_playing[21].load()) {
                        boost::asio::write(socket22, turn_mess, error);
                    }
                    if (is_playing[22].load()) {
                        boost::asio::write(socket23, turn_mess, error);
                    }
                    if (is_playing[23].load()) {
                        boost::asio::write(socket24, turn_mess, error);
                    }
                    if (is_playing[24].load()) {
                        boost::asio::write(socket25, turn_mess, error);
                    }
                }
                events.clear();
            }
            scores.clear();
            number_of_players = 0;
            number_of_game_started_sent = 0;
            players.clear();
            players_positions.clear();
            blocks.clear();
            bombs.clear();
            acc_players.clear();
            game_state = LOBBY;
            for (int i = 0; i < 25; ++i) {
                is_playing[i] = false;
            }
            old_turn_mess.clear();
            new_game = true;
        }
    }


    void handle_client(const server_param &p, tcp::socket &socket, tcp::acceptor &acceptor, int thread_nr) {
        boost::asio::ip::v6_only option(false);
        boost::system::error_code error;
        bool disconnected = true;
        while (true) {
            if (disconnected) {
                socket.close();
                acceptor.accept(socket);
                boost::asio::ip::tcp::no_delay option(true);
                socket.set_option(option);
                std::cout << "Client connected! Sending Hello message!\n";
                boost::asio::write(socket, hello_mess(p), error);
                disconnected = false;
            }
            socket.non_blocking(true);
            std::set <PlayerId> accepted_sent;
            PlayerId new_player_id;
            for (PlayerId pl:acc_players) {
                if (accepted_sent.find(pl) == accepted_sent.end()) {
                    boost::asio::write(socket, accepted_player_mess(pl), error);
                    accepted_sent.insert(pl);
                }
            }
            bool received_join = receive_join_from_client(socket, &new_player_id, &disconnected);
            if (disconnected) {
                continue;
            }
            while (!received_join) {
                for (PlayerId pl:acc_players) {
                    if (accepted_sent.find(pl) == accepted_sent.end()) {
                        boost::asio::write(socket, accepted_player_mess(pl), error);
                        accepted_sent.insert(pl);
                    }
                }
                received_join = receive_join_from_client(socket, &new_player_id, &disconnected);
                if (disconnected) {
                    break;
                }
            }
            if (disconnected) {
                continue;
            }
            if (players.find(new_player_id) == players.end()) {
                std::cout << "Obserwator" << endl;
                boost::asio::write(socket, game_started_message(), error);
                for (std::pair <uint16_t, boost::asio::mutable_buffer> old : old_turn_mess) {
                    boost::asio::write(socket, old.second, error);
                }
                is_playing[thread_nr] = true;
                while (!new_game.load()) {}
            } else {
                uint8_t player_nb = number_of_players.fetch_add(1, std::memory_order_relaxed);
                is_playing[thread_nr] = true;
                accepted_sent.insert(new_player_id);
                boost::asio::write(socket, accepted_player_mess(new_player_id), error);
                ++player_nb;
                uint8_t count;
                for (PlayerId pl:acc_players) {
                    if (accepted_sent.find(pl) == accepted_sent.end()) {
                        boost::asio::write(socket, accepted_player_mess(pl), error);
                        accepted_sent.insert(pl);
                    }
                }
                if (player_nb == parameters.players_count) {
                    turn = 1;
                    game_state = GAME_STARTED;
                    boost::asio::write(socket, game_started_message(), error);
                    init_players_pos();
                    count = number_of_game_started_sent.fetch_add(1, std::memory_order_relaxed);
                    ++count;
                    while (number_of_game_started_sent.load() != parameters.players_count) {}
                    ready = true;
                    condition.notify_all();

                } else {
                    bool game_sent = false;
                    while (number_of_game_started_sent.load() != parameters.players_count) {
                        if (acc_players.size() != accepted_sent.size()) {
                            for (PlayerId pl : acc_players) {
                                if (accepted_sent.find(pl) == accepted_sent.end()) {
                                    boost::asio::write(socket, accepted_player_mess(pl), error);
                                    accepted_sent.insert(pl);
                                }
                            }
                        }
                        if ((game_state.load() == GAME_STARTED) && (!game_sent)) {
                            game_sent = true;
                            boost::asio::write(socket, game_started_message(), error);
                            count = number_of_game_started_sent.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                    }
                }
                while (turn.load() != parameters.game_length + 1) {
                    receive_move_from_client(socket, new_player_id, p, &disconnected);
                    if (disconnected) {
                        break;
                    }
                }
                if (disconnected) {
                    continue;
                }
                while (!new_game.load()) {}
            }
        }
    }
}

int main(int ac, char **av) {
    server_param p;
    if (parse_param(ac, av, &p) == 1) {
        return 0;
    }
    random_number = std::minstd_rand(p.seed);
    parameters = p;
    try {
        boost::asio::io_context io_context;
        tcp::acceptor acceptor(io_context,
                               tcp::endpoint(tcp::v6(), p.port));
        tcp::socket socket1(io_context);
        tcp::socket socket2(io_context);
        tcp::socket socket3(io_context);
        tcp::socket socket4(io_context);
        tcp::socket socket5(io_context);
        tcp::socket socket6(io_context);
        tcp::socket socket7(io_context);
        tcp::socket socket8(io_context);
        tcp::socket socket9(io_context);
        tcp::socket socket10(io_context);
        tcp::socket socket11(io_context);
        tcp::socket socket12(io_context);
        tcp::socket socket13(io_context);
        tcp::socket socket14(io_context);
        tcp::socket socket15(io_context);
        tcp::socket socket16(io_context);
        tcp::socket socket17(io_context);
        tcp::socket socket18(io_context);
        tcp::socket socket19(io_context);
        tcp::socket socket20(io_context);
        tcp::socket socket21(io_context);
        tcp::socket socket22(io_context);
        tcp::socket socket23(io_context);
        tcp::socket socket24(io_context);
        tcp::socket socket25(io_context);
        std::thread client1([&p, &socket1, &acceptor]() {
            handle_client(p, socket1, acceptor, 0);
        });

        std::thread client2([&p, &socket2, &acceptor]() {
            handle_client(p, socket2, acceptor, 1);
        });

        std::thread client3([&p, &socket3, &acceptor]() {
            handle_client(p, socket3, acceptor, 2);
        });

        std::thread client4([&p, &socket4, &acceptor]() {
            handle_client(p, socket4, acceptor, 3);
        });

        std::thread client5([&p, &socket5, &acceptor]() {
            handle_client(p, socket5, acceptor, 4);
        });

        std::thread client6([&p, &socket6, &acceptor]() {
            handle_client(p, socket6, acceptor, 5);
        });

        std::thread client7([&p, &socket7, &acceptor]() {
            handle_client(p, socket7, acceptor, 6);
        });

        std::thread client8([&p, &socket8, &acceptor]() {
            handle_client(p, socket8, acceptor, 7);
        });

        std::thread client9([&p, &socket9, &acceptor]() {
            handle_client(p, socket9, acceptor, 8);
        });

        std::thread client10([&p, &socket10, &acceptor]() {
            handle_client(p, socket10, acceptor, 9);
        });

        std::thread client11([&p, &socket11, &acceptor]() {
            handle_client(p, socket11, acceptor, 10);
        });

        std::thread client12([&p, &socket12, &acceptor]() {
            handle_client(p, socket12, acceptor, 11);
        });

        std::thread client13([&p, &socket13, &acceptor]() {
            handle_client(p, socket13, acceptor, 12);
        });

        std::thread client14([&p, &socket14, &acceptor]() {
            handle_client(p, socket14, acceptor, 13);
        });

        std::thread client15([&p, &socket15, &acceptor]() {
            handle_client(p, socket15, acceptor, 14);
        });

        std::thread client16([&p, &socket16, &acceptor]() {
            handle_client(p, socket16, acceptor, 15);
        });

        std::thread client17([&p, &socket17, &acceptor]() {
            handle_client(p, socket17, acceptor, 16);
        });

        std::thread client18([&p, &socket18, &acceptor]() {
            handle_client(p, socket18, acceptor, 17);
        });

        std::thread client19([&p, &socket19, &acceptor]() {
            handle_client(p, socket19, acceptor, 18);
        });

        std::thread client20([&p, &socket20, &acceptor]() {
            handle_client(p, socket20, acceptor, 19);
        });

        std::thread client21([&p, &socket21, &acceptor]() {
            handle_client(p, socket21, acceptor, 20);
        });

        std::thread client22([&p, &socket22, &acceptor]() {
            handle_client(p, socket22, acceptor, 21);
        });

        std::thread client23([&p, &socket23, &acceptor]() {
            handle_client(p, socket23, acceptor, 22);
        });

        std::thread client24([&p, &socket24, &acceptor]() {
            handle_client(p, socket24, acceptor, 23);
        });

        std::thread client25([&p, &socket25, &acceptor]() {
            handle_client(p, socket25, acceptor, 24);
        });

        std::thread master_t(
                [&socket1, &socket2, &socket3, &socket4, &socket5, &socket6, &socket7, &socket8, &socket9, &socket10, &socket11, &socket12, &socket13, &socket14, &socket15, &socket16, &socket17, &socket18, &socket19, &socket20, &socket21, &socket22, &socket23, &socket24, &socket25]() {
                    master(socket1, socket2, socket3, socket4, socket5, socket6, socket7, socket8, socket9, socket10,
                           socket11, socket12, socket13, socket14, socket15, socket16, socket17, socket18, socket19,
                           socket20, socket21, socket22, socket23, socket24, socket25);
                });

        client1.join();
        client2.join();
        client3.join();
        client4.join();
        client5.join();
        client6.join();
        client7.join();
        client8.join();
        client9.join();
        client10.join();
        client11.join();
        client12.join();
        client13.join();
        client14.join();
        client15.join();
        client16.join();
        client17.join();
        client18.join();
        client19.join();
        client20.join();
        client21.join();
        client22.join();
        client23.join();
        client24.join();
        client25.join();
        master_t.join();

    } catch (std::exception &e) {
        std::cerr << e.what() << std::endl;
    }
    return 0;
}
