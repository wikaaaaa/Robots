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

using boost::asio::ip::tcp;
using boost::asio::ip::udp;
using std::string;
using std::endl;
namespace po = boost::program_options;
std::atomic<std::uint8_t> game_state(0);
std::atomic<bool> send_join(true);

namespace {

    string divide(string input, string *port, bool *error) {
        size_t idx = input.size();
        --idx;
        while ((input.at(idx) != ':') && (idx > 0)) {
            --idx;
        }
        if (idx == 0) {
            *error = true;
            return "";
        }
        *port = input.substr(idx + 1);
        return input.substr(0, idx);
    }

    int parse_param(int ac, char **av, param *parameters) {
        po::options_description desc("Allowed options");
        desc.add_options()
                ("help,h", "Wypisuje jak uzywać programu")
                ("gui-address,d", po::value<string>(),
                 "<(nazwa hosta):(port) lub (IPv4):(port) lub (IPv6):(port)> [OBOWIĄZKOWE]")
                ("player-name,n", po::value<string>(), "Nazwa gracza [OBOWIĄZKOWE]")
                ("port,p", po::value<uint16_t>(), "Port na którym klient nasłuchuje komunikatów od GUI [OBOWIĄZKOWE]")
                ("server-address,s", po::value<string>(),
                 "<(nazwa hosta):(port) lub (IPv4):(port) lub (IPv6):(port)> [OBOWIĄZKOWE]");
        po::variables_map vm;
        po::store(po::parse_command_line(ac, av, desc), vm);
        po::notify(vm);
        if (vm.count("help")) {
            std::cout << desc << "\n";
            return 1;
        }
        if (vm.count("gui-address")) {
            string port;
            bool error = false;
            parameters->gui_host_name_or_ip = divide(vm["gui-address"].as<string>(), &(port), &error);
            if (error) {
                std::cerr << desc << "\n";
                exit(1);
            }
            parameters->gui_port = port;
        } else {
            std::cerr << desc << "\n";
            exit(1);
        }
        if (vm.count("player-name")) {
            parameters->player_name = vm["player-name"].as<string>();
        } else {
            std::cerr << desc << "\n";
            exit(1);
        }
        if (vm.count("server-address")) {
            string port;
            bool error = false;
            parameters->server_host_name_or_ip = divide(vm["server-address"].as<string>(), &port, &error);
            if (error) {
                std::cerr << desc << "\n";
                exit(1);
            }
            parameters->server_port = port;
        } else {
            std::cerr << desc << "\n";
            exit(1);
        }
        if (vm.count("port")) {
            parameters->port = vm["port"].as<uint16_t>();
        } else {
            std::cerr << desc << "\n";
            exit(1);
        }
        return 0;
    }

    boost::asio::mutable_buffer join_message(const param &p) {
        uint8_t type = JOIN;
        auto size = (uint8_t) p.player_name.size();
        size_t message_size = size + 2;
        char *message = new char[message_size];
        memcpy(message, &type, 1);
        memcpy(message + 1, &size, 1);
        memcpy(message + 2, p.player_name.c_str(), size);
        return boost::asio::buffer(message, message_size);
    }

    boost::asio::mutable_buffer place_bomb_message() {
        uint8_t type = PLACE_BOMB;
        size_t message_size = 1;
        char *message = new char[message_size];
        memcpy(message, &type, 1);
        return boost::asio::buffer(message, message_size);
    }

    boost::asio::mutable_buffer place_block_message() {
        uint8_t type = PLACE_BLOCK;
        size_t message_size = 1;
        char *message = new char[message_size];
        memcpy(message, &type, 1);
        return boost::asio::buffer(message, message_size);
    }

    boost::asio::mutable_buffer move_message(uint8_t direction) {
        uint8_t type = MOVE;
        size_t message_size = 2;
        char *message = new char[message_size];
        memcpy(message, &type, 1);
        memcpy(message + 1, &direction, 1);
        return boost::asio::buffer(message, message_size);
    }

    void send_message_to_server(from_gui next_move, tcp::socket &tcp_socket) {
        try {
            boost::system::error_code error;
            boost::asio::mutable_buffer message;
            switch (next_move.type) {
                case PLACE_BLOCK_GUI:
                    message = place_block_message();
                    break;
                case PLACE_BOMB_GUI:
                    message = place_bomb_message();
                    break;
                case MOVE_GUI:
                    message = move_message(next_move.direction);
                    break;
            }
            boost::asio::write(tcp_socket, message, error);
        } catch (std::exception &e) {
            std::cerr << "Exception in sending to server: " << e.what() << "\n";
            exit(1);
        }
    }

    void send_join_to_server(const param &p, tcp::socket &tcp_socket) {
        try {
            boost::system::error_code error;
            boost::asio::mutable_buffer message;
            message = join_message(p);
            boost::asio::write(tcp_socket, message, error);
        } catch (std::exception &e) {
            std::cerr << "Exception in sending to server: " << e.what() << "\n";
            exit(1);
        }
    }

    void receive_hello_from_server(tcp::socket &tcp_socket, server_info *server_info) {
        try {
            std::cerr << "Received HELLO from server: ";
            uint8_t size[1];
            boost::asio::read(tcp_socket, boost::asio::buffer(size));
            char name[size[0] + 1];
            boost::asio::read(tcp_socket, boost::asio::buffer(name, size[0]));
            name[size[0]] = '\0';
            string name_Str(name);
            std::cerr << name_Str << "!" << endl;
            server_info->server_name = name_Str;
            uint8_t players_count[1];
            boost::asio::read(tcp_socket, boost::asio::buffer(players_count));
            server_info->player_count = players_count[0];
            uint16_t other_info[5];
            boost::asio::read(tcp_socket, boost::asio::buffer(other_info));
            server_info->size_x = ntohs(other_info[0]);
            server_info->size_y = ntohs(other_info[1]);
            server_info->game_length = ntohs(other_info[2]);
            server_info->explosion_radius = ntohs(other_info[3]);
            server_info->bomb_timer = ntohs(other_info[4]);
        } catch (std::exception &e) {
            std::cerr << "Exception in receiving from server: " << e.what() << "\n";
            exit(1);
        }
    }

    void receive_acc_player_from_server(tcp::socket &tcp_socket, game_info *info) {
        try {
            std::cerr << "SERVER: accepted player ";
            uint8_t size[1];
            uint8_t Player_Id[1];
            boost::asio::read(tcp_socket, boost::asio::buffer(Player_Id));
            boost::asio::read(tcp_socket, boost::asio::buffer(size));
            char name[size[0] + 1];
            boost::asio::read(tcp_socket, boost::asio::buffer(name, size[0]));
            name[size[0]] = '\0';
            string name_str(name);
            std::cerr << name_str << " ";
            boost::asio::read(tcp_socket, boost::asio::buffer(size));
            char address[size[0] + 1];
            boost::asio::read(tcp_socket, boost::asio::buffer(address, size[0]));
            address[size[0]] = '\0';
            string address_str(address);
            std::cerr << address_str << endl;
            Player player = {name_str, address_str};
            info->players.insert(std::pair<PlayerId, Player>(Player_Id[0], player));
        } catch (std::exception &e) {
            std::cerr << "Exception in receiving from server: " << e.what() << "\n";
            exit(1);
        }
    }

    void receive_game_started_from_server(tcp::socket &tcp_socket, game_info *info) {
        try {
            std::cerr << "SERVER: game started!" << endl;
            uint8_t size[1];
            uint32_t size_of_map_buffer[1];
            boost::asio::read(tcp_socket, boost::asio::buffer(size_of_map_buffer));
            uint32_t size_of_map = ntohl(size_of_map_buffer[0]);
            for (uint32_t i = 0; i < size_of_map; ++i) {
                uint8_t player_id[1];
                boost::asio::read(tcp_socket, boost::asio::buffer(player_id));
                boost::asio::read(tcp_socket, boost::asio::buffer(size));
                char name[size[0] + 1];
                boost::asio::read(tcp_socket, boost::asio::buffer(name, size[0]));
                name[size[0]] = '\0';
                string name_str(name);
                boost::asio::read(tcp_socket, boost::asio::buffer(size));
                char address[size[0] + 1];
                boost::asio::read(tcp_socket, boost::asio::buffer(address, size[0]));
                address[size[0]] = '\0';
                string address_str(address);
                Player player = {name_str, address_str};
                info->players.insert(std::pair<PlayerId, Player>(player_id[0], player));
                info->scores.insert(std::pair<PlayerId, Score>(player_id[0], 0));
            }
        } catch (std::exception &e) {
            std::cerr << "Exception in receiving from server: " << e.what() << "\n";
            exit(1);
        }
    }

    void calculate_explosions(Position bomb_position, game_info *info, server_info *server_info) {
        uint16_t x = bomb_position.first;
        uint16_t y = bomb_position.second;
        if (info->blocks.find(bomb_position) == info->blocks.end()) {
            for (uint16_t j = 1; j <= server_info->explosion_radius; ++j) {
                if (x + j < server_info->size_x) {
                    info->explosions.insert({static_cast<uint16_t>(x + j), y});
                    if (info->blocks.find({static_cast<uint16_t>(x + j), y}) != info->blocks.end()) {
                        break;
                    }
                } else {
                    break;
                }
            }
            for (uint16_t j = 1; j <= server_info->explosion_radius; ++j) {
                if (x - j >= 0) {
                    info->explosions.insert({static_cast<uint16_t>(x - j), y});
                    if (info->blocks.find({static_cast<uint16_t>(x - j), y}) != info->blocks.end()) {
                        break;
                    }
                } else {
                    break;
                }
            }
            for (uint16_t j = 1; j <= server_info->explosion_radius; ++j) {
                if (y - j >= 0) {
                    info->explosions.insert({x, static_cast<uint16_t>(y - j)});
                    if (info->blocks.find({x, static_cast<uint16_t>(y - j)}) != info->blocks.end()) {
                        break;
                    }
                } else {
                    break;
                }
            }
            for (uint16_t j = 1; j <= server_info->explosion_radius; ++j) {
                if (y + j < server_info->size_y) {
                    info->explosions.insert({x, static_cast<uint16_t>(y + j)});
                    if (info->blocks.find({x, static_cast<uint16_t>(y + j)}) != info->blocks.end()) {
                        break;
                    }
                } else {
                    break;
                }
            }
        }
    }

    void receive_turn_from_server(tcp::socket &tcp_socket, game_info *info, server_info *server_info) {
        try {
            info->explosions.clear();
            for (auto it = info->bombs.begin(); it != info->bombs.end(); ++it) {
                uint16_t timer = it->second.timer - 1;
                info->bombs[it->first].timer = timer;
            }
            uint16_t turn_buf[1];
            boost::asio::read(tcp_socket, boost::asio::buffer(turn_buf));
            uint16_t turn = ntohs(turn_buf[0]);
            info->turn = turn;
            uint32_t size_of_list_buf[1];
            boost::asio::read(tcp_socket, boost::asio::buffer(size_of_list_buf));
            uint32_t size_of_list = ntohl(size_of_list_buf[0]);
            std::set<Position> blocks_after_turn = info->blocks;
            for (uint32_t i = 0; i < size_of_list; ++i) {
                uint8_t type_of_event[1];
                boost::asio::read(tcp_socket, boost::asio::buffer(type_of_event));
                switch (type_of_event[0]) {
                    case BOMB_PLACED: {
                        uint32_t bomb_id_buf[1];
                        boost::asio::read(tcp_socket, boost::asio::buffer(bomb_id_buf));
                        uint32_t bomb_id = ntohl(bomb_id_buf[0]);
                        uint16_t position_buf[2];
                        boost::asio::read(tcp_socket, boost::asio::buffer(position_buf));
                        Position position = {ntohs(position_buf[0]), ntohs(position_buf[1])};
                        Bomb bomb = {position, server_info->bomb_timer};
                        info->bombs.insert(std::pair<BombId, Bomb>(bomb_id, bomb));
                        break;
                    }
                    case BOMB_EXPLODED: {
                        uint32_t bomb_id_buf[1];
                        boost::asio::read(tcp_socket, boost::asio::buffer(bomb_id_buf));
                        uint32_t bomb_id = ntohl(bomb_id_buf[0]);
                        Bomb bomb = info->bombs[bomb_id];
                        info->explosions.insert(bomb.position);
                        calculate_explosions(bomb.position, info, server_info);
                        info->bombs.erase(bomb_id);
                        uint32_t size_of_list_buf1[1];
                        boost::asio::read(tcp_socket, boost::asio::buffer(size_of_list_buf1));
                        uint32_t size_of_list1 = ntohl(size_of_list_buf1[0]);
                        for (uint32_t j = 0; j < size_of_list1; ++j) {
                            PlayerId playerId[1];
                            boost::asio::read(tcp_socket, boost::asio::buffer(playerId));
                            auto player_pos = info->player_positions.find(playerId[0]);
                            if (player_pos != info->player_positions.end()) {
                                info->player_positions.erase(player_pos);
                                info->scores[playerId[0]] = info->scores[playerId[0]] + 1;
                            }
                        }
                        uint32_t size_of_list_buf2[1];
                        boost::asio::read(tcp_socket, boost::asio::buffer(size_of_list_buf2));
                        size_of_list1 = ntohl(size_of_list_buf2[0]);
                        for (uint32_t j = 0; j < size_of_list1; ++j) {
                            uint16_t position_buf[2];
                            boost::asio::read(tcp_socket, boost::asio::buffer(position_buf));
                            Position position = {ntohs(position_buf[0]), ntohs(position_buf[1])};
                            auto del = blocks_after_turn.find(position);
                            if (del != blocks_after_turn.end()) {
                                blocks_after_turn.erase(del);
                            }
                        }
                        break;
                    }
                    case PLAYER_MOVED: {
                        PlayerId playerId[1];
                        boost::asio::read(tcp_socket, boost::asio::buffer(playerId));
                        uint16_t position_buf[2];
                        boost::asio::read(tcp_socket, boost::asio::buffer(position_buf));
                        Position position = {ntohs(position_buf[0]), ntohs(position_buf[1])};
                        info->player_positions[playerId[0]] = position;
                        break;
                    }
                    case BLOCK_PLACED: {
                        uint16_t position_buf[2];
                        boost::asio::read(tcp_socket, boost::asio::buffer(position_buf));
                        Position position = {ntohs(position_buf[0]), ntohs(position_buf[1])};
                        blocks_after_turn.insert(position);
                        break;
                    }
                }
            }
            info->blocks = blocks_after_turn;
        } catch (std::exception &e) {
            std::cerr << "Exception in receiving from server: " << e.what() << "\n";
            exit(1);
        }
    }

    void receive_game_ended_from_server(tcp::socket &tcp_socket, game_info *info) {
        try {
            std::cerr << "SERVER: game ended!" << endl;
            uint32_t size_of_map_buffer[1];
            boost::asio::read(tcp_socket, boost::asio::buffer(size_of_map_buffer));
            uint32_t size_of_map = ntohl(size_of_map_buffer[0]);
            for (uint32_t i = 0; i < size_of_map; ++i) {
                uint8_t player_id[1];
                boost::asio::read(tcp_socket, boost::asio::buffer(player_id));
                uint32_t score_buf[1];
                boost::asio::read(tcp_socket, boost::asio::buffer(score_buf));
                Score score = ntohl(score_buf[0]);
                info->scores[player_id[0]] = score;
            }
        } catch (std::exception &e) {
            std::cerr << "Exception in receiving from server: " << e.what() << "\n";
            exit(1);
        }
    }

    uint8_t receive_from_server(game_info *info, tcp::socket &tcp_socket, server_info *server_info) {
        try {
            uint8_t data[1];
            boost::asio::read(tcp_socket, boost::asio::buffer(data));
            switch (data[0]) {
                case HELLO: {
                    receive_hello_from_server(tcp_socket, server_info);
                    break;
                }
                case ACCEPTED_PLAYER: {
                    receive_acc_player_from_server(tcp_socket, info);
                    break;
                }
                case GAME_STARTED: {
                    receive_game_started_from_server(tcp_socket, info);
                    break;
                }
                case TURN: {
                    receive_turn_from_server(tcp_socket, info, server_info);
                    break;
                }
                case GAME_ENDED: {
                    receive_game_ended_from_server(tcp_socket, info);
                    break;
                }
            }
            return data[0];
        } catch (std::exception &e) {
            std::cerr << "Exception in receiving from server: " << e.what() << "\n";
            exit(1);
        }
    }

    size_t size_of_map(const std::map<PlayerId, Player> &map) {
        size_t size = 0;
        for (auto &it : map) {
            size += 3;
            size += it.second.name.length();
            size += it.second.address.length();
        }
        return size;
    }

    boost::asio::mutable_buffer lobby_message(game_info info, server_info s_info) {
        size_t size = 17;
        size += size_of_map(info.players);
        size += s_info.server_name.size();
        char *message = new char[size];
        size_t where = 0;
        uint8_t type = 0;
        memcpy(message, &type, 1);
        where += 1;
        auto size_string = (uint8_t) s_info.server_name.length();
        memcpy(message + where, &size_string, 1);
        where += 1;
        memcpy(message + where, s_info.server_name.c_str(), s_info.server_name.length());
        where += s_info.server_name.length();
        memcpy(message + where, &(s_info.player_count), 1);
        where += 1;
        uint16_t help = htons(s_info.size_x);
        memcpy(message + where, &help, 2);
        where += 2;
        help = htons(s_info.size_y);
        memcpy(message + where, &help, 2);
        where += 2;
        help = htons(s_info.game_length);
        memcpy(message + where, &help, 2);
        where += 2;
        help = htons(s_info.explosion_radius);
        memcpy(message + where, &help, 2);
        where += 2;
        help = htons(s_info.bomb_timer);
        memcpy(message + where, &help, 2);
        where += 2;
        uint32_t size_of_map = htonl((uint32_t) info.players.size());
        memcpy(message + where, &size_of_map, 4);
        where += 4;
        for (auto &player : info.players) {
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

    size_t size_of_others(const game_info &info) {
        size_t size = 0;
        size += info.player_positions.size() * 5;
        size += info.blocks.size() * 4;
        size += info.bombs.size() * 6;
        size += info.explosions.size() * 4;
        size += info.scores.size() * 5;
        return size;
    }

    boost::asio::mutable_buffer game_message(const game_info &info, const server_info &server_info) {
        size_t size = 34;
        size += size_of_map(info.players);
        size += server_info.server_name.size();
        size += size_of_others(info);
        char *message = new char[size];
        size_t where = 0;
        uint8_t type = 1;
        memcpy(message, &type, 1);
        where += 1;
        auto size_string = (uint8_t) server_info.server_name.length();
        memcpy(message + where, &size_string, 1);
        where += 1;
        memcpy(message + where, server_info.server_name.c_str(), server_info.server_name.length());
        where += server_info.server_name.length();
        uint16_t help = htons(server_info.size_x);
        memcpy(message + where, &help, 2);
        where += 2;
        help = htons(server_info.size_y);
        memcpy(message + where, &help, 2);
        where += 2;
        help = htons(server_info.game_length);
        memcpy(message + where, &help, 2);
        where += 2;
        help = htons(info.turn);
        memcpy(message + where, &help, 2);
        where += 2;
        uint32_t size_of = htonl((uint32_t) info.players.size());
        memcpy(message + where, &size_of, 4);
        where += 4;
        for (auto &player : info.players) {
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
        size_of = htonl((uint32_t) info.player_positions.size());
        memcpy(message + where, &size_of, 4);
        where += 4;
        for (auto it = info.player_positions.begin(); it != info.player_positions.end(); ++it) {
            memcpy(message + where, &(it->first), 1);
            where += 1;
            help = htons(it->second.first);
            memcpy(message + where, &help, 2);
            where += 2;
            help = htons(it->second.second);
            memcpy(message + where, &help, 2);
            where += 2;
        }
        size_of = htonl((uint32_t) info.blocks.size());
        memcpy(message + where, &size_of, 4);
        where += 4;
        for (auto it = info.blocks.begin(); it != info.blocks.end(); ++it) {
            help = htons(it->first);
            memcpy(message + where, &help, 2);
            where += 2;
            help = htons(it->second);
            memcpy(message + where, &help, 2);
            where += 2;
        }
        size_of = htonl((uint32_t) info.bombs.size());
        memcpy(message + where, &size_of, 4);
        where += 4;
        for (auto it = info.bombs.begin(); it != info.bombs.end(); ++it) {
            help = htons(it->second.position.first);
            memcpy(message + where, &help, 2);
            where += 2;
            help = htons(it->second.position.second);
            memcpy(message + where, &help, 2);
            where += 2;
            help = htons(it->second.timer);
            memcpy(message + where, &help, 2);
            where += 2;
        }
        size_of = htonl((uint32_t) info.explosions.size());
        memcpy(message + where, &size_of, 4);
        where += 4;
        for (auto it = info.explosions.begin(); it != info.explosions.end(); ++it) {
            help = htons(it->first);
            memcpy(message + where, &help, 2);
            where += 2;
            help = htons(it->second);
            memcpy(message + where, &help, 2);
            where += 2;
        }
        size_of = htonl((uint32_t) info.scores.size());
        memcpy(message + where, &size_of, 4);
        where += 4;
        for (auto it = info.scores.begin(); it != info.scores.end(); ++it) {
            memcpy(message + where, &(it->first), 1);
            where += 1;
            uint32_t help1 = htonl(it->second);
            memcpy(message + where, &help1, 4);
            where += 4;
        }
        return boost::asio::buffer(message, size);
    }

    from_gui receive_from_gui(udp::socket &udp_socket) {
        try {
            uint8_t type[1];
            uint8_t direction[1];
            while (true) {
                boost::array<boost::asio::mutable_buffer, 2> reply = {
                        boost::asio::buffer(type),
                        boost::asio::buffer(direction)
                };
                size_t reply_length = udp_socket.receive(reply);
                if ((type[0] == 0) || (type[0] == 1)) {
                    if (reply_length == 1) {
                        return {type[0], direction[0]};
                    }
                } else if ((type[0] == 2) && (reply_length == 2)) {
                    if (direction[0] <= 3) {
                        return {type[0], direction[0]};
                    }
                }
            }
        } catch (std::exception &e) {
            std::cerr << "Exception in receiving from gui: " << e.what() << "\n";
            exit(1);
        }
    }

    void send_to_gui_lobby(const game_info &info, const server_info &s_info, udp::socket &udp_socket,
                           udp::resolver::results_type &udp_endpoints) {
        try {
            udp_socket.send_to(lobby_message(info, s_info), *udp_endpoints.begin());
        } catch (std::exception &e) {
            std::cerr << "Exception in sending to gui: " << e.what() << "\n";
            exit(1);
        }
    }

    void send_to_gui_game(const game_info &info, const server_info &s_info, udp::socket &udp_socket,
                          udp::resolver::results_type &udp_endpoints) {
        try {
            udp_socket.send_to(game_message(info, s_info), *udp_endpoints.begin());
        } catch (std::exception &e) {
            std::cerr << "Exception in sending to gui: " << e.what() << "\n";
            exit(1);
        }
    }

    udp::socket open_comunication_with_gui(const param &p, udp::resolver::results_type *endpoints) {
        try {
            boost::asio::io_context io_context;
            udp::socket s(io_context, udp::endpoint(udp::v6(), p.port));
            udp::resolver resolver(io_context);
            *endpoints = resolver.resolve(p.gui_host_name_or_ip, p.gui_port);
            return s;
        } catch (std::exception &e) {
            std::cerr << "Exception in connecting to gui: " << e.what() << "\n";
            exit(1);
        }
    }

    tcp::socket connect_to_server(const param &p) {
        try {
            std::cerr << "Starting communication with SERVER!" << endl;
            boost::asio::io_context ioContext;
            tcp::resolver resolver{ioContext};
            auto endpoints = resolver.resolve(p.server_host_name_or_ip, p.server_port);
            tcp::socket socket{ioContext};
            boost::asio::connect(socket, endpoints);
            boost::asio::ip::tcp::no_delay option(true);
            socket.set_option(option);
            std::cerr << "CONNCECTED with SERVER!" << endl;
            return socket;
        } catch (std::exception &e) {
            std::cerr << "Exception in connecting to server: " << e.what() << "\n";
            exit(1);
        }

    }

    // Funkcja, którą wykonuje wątek odpowiedzialny za odbieranie od serwera komunikatów
    // i wysyłanie komunikatów do gui.
    void
    from_server_to_gui(tcp::socket &tcp_socket, udp::socket &udp_socket, udp::resolver::results_type &endpoints_udp) {
        server_info server_info;
        while (true) {
            game_info info;
            game_state = receive_from_server(&info, tcp_socket, &server_info);
            while (game_state.load() == ACCEPTED_PLAYER || game_state.load() == HELLO) {
                send_to_gui_lobby(info, server_info, udp_socket, endpoints_udp);
                game_state = receive_from_server(&info, tcp_socket, &server_info);
            }
            if (game_state.load() == GAME_STARTED) {
                game_state = receive_from_server(&info, tcp_socket, &server_info);
            } else {
                std::cerr << "Error: received unexpected message from server!" << "\n";
                exit(1);
            }
            while (game_state.load() == TURN) {
                send_to_gui_game(info, server_info, udp_socket, endpoints_udp);
                game_state = receive_from_server(&info, tcp_socket, &server_info);
            }
            if (game_state.load() == GAME_ENDED) {
                game_info clear;
                send_to_gui_lobby(clear, server_info, udp_socket, endpoints_udp);
                send_join = true;
                game_state = LOBBY;
            } else {
                std::cerr << "Error: received unexpected message from server!" << "\n";
                exit(1);
            }
        }
    }

    // Funkcja, którą wykonuje wątek odpowiedzialny za odbieranie komunikatów od gui
    // i wysyłanie komunikatów do serwera
    void from_gui_to_server(tcp::socket &tcp_socket, udp::socket &udp_socket, const param &p) {
        while (true) {
            from_gui rec = receive_from_gui(udp_socket);
            if ((game_state.load() == LOBBY) || (game_state.load() == ACCEPTED_PLAYER)) {
                if (send_join.load()) {
                    send_join_to_server(p, tcp_socket);
                    send_join = false;
                }
            } else if (game_state.load() == TURN) {
                send_message_to_server(rec, tcp_socket);
            }
        }
    }
}

int main(int ac, char **av) {
    param p;
    if (parse_param(ac, av, &p) != 0) {
        return 0;
    };
    udp::resolver::results_type endpoints_gui;
    udp::socket socket_gui = open_comunication_with_gui(p, &endpoints_gui);
    tcp::socket socket_tcp = connect_to_server(p);
    std::thread t([&socket_tcp, &socket_gui, &endpoints_gui]() {
        from_server_to_gui(socket_tcp, socket_gui, endpoints_gui);
    });
    std::thread t1([&socket_tcp, &socket_gui, p]() {
        from_gui_to_server(socket_tcp, socket_gui, p);
    });
    t.join();
    t1.join();
    return 0;
}


