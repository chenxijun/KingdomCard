#include <spdlog/spdlog.h>

#include <memory>
#include "GameServer.h"
#include "basic/Utility.h"
#include "basic/Player.h"
#include "basic/GameController.h"
#include "basic_message.pb.h"

// TODO: 切换成利用 monitor 监控连接状态

namespace kc {
    /// @brief 服务器构造函数
    /// @param context ZeroMQ 上下文
    /// @param port 服务器端口号
    GameServer::GameServer(zmq::context_t &context, const uint16_t port) : context(context) {
        potentialPort = port;
        bridgeRepSocket = zmq::socket_t(context, ZMQ_REP);
        bridgeRepSocket.set(zmq::sockopt::rcvtimeo, 500);
        bool bindSuccess = false;
        while (!bindSuccess) {
            try {
                // 开放登入端口
                bridgeRepSocket.bind("tcp://*:" + std::to_string(potentialPort));
                bindSuccess = true;
                potentialPort++;
            } catch (zmq::error_t &e) {
                // 假如失败则端口号+1
                spdlog::warn("服务器开放端口 {} 失败", potentialPort);
                if (e.num() == EADDRINUSE && potentialPort < 65535)
                    potentialPort++;
                else
                    throw e;
            }
        }
        spdlog::info("服务器已开放端口: {}", potentialPort - 1);
    }

    /// @brief 服务器析构函数
    GameServer::~GameServer() {
        // 等待线程结束
        isWaiting = false;
        if (connectionThread.joinable())
            connectionThread.join();
        // 关闭所有套接字
        bridgeRepSocket.close();
        for (auto &player: players) {
            player->socket.close();
        }
        context.close();
    }

    /// @brief 等待客户端连接
    void GameServer::waitForConnection() {
        if (isWaiting)
            return;
        spdlog::info("服务器等待客户端连接");
        isWaiting = true;
        connectionThread = std::thread([&]() {
            while (isWaiting) {
                try {
                    zmq::message_t message;
                    zmq::recv_result_t size = bridgeRepSocket.recv(message);
                    if (!size.has_value()) {
//                        spdlog::debug("服务器收到了一个空消息");
                        continue;
                    }
                    std::string msg = std::string(static_cast<char *>(message.data()), message.size());
                    BasicMessage parsedMessage;
                    bool result = parsedMessage.ParseFromString(msg);
                    if (!result) {
                        spdlog::debug("无法解析消息内容, 消息文本为: {}", msg);
                        continue;
                    }
                    if (parsedMessage.type() == CommandType::CONNECT_REQ) {
                        spdlog::info("客户端连接, 下发连接信息");
                        connectWithClient();
                    } else
                        spdlog::debug("错误的消息类型: {}", parsedMessage.GetTypeName());
                } catch (std::exception &e) {
                    spdlog::error("服务器等待连接时发生错误: {}", e.what());
                }
                if (players.size() >= waitingPlayerNum)
                    isWaiting = false;
            }
            spdlog::debug("结束等待");
        });
    }

    /// @brief 与客户端建立连接
    /// @return 玩家对象指针
    void GameServer::connectWithClient() {
        bool bindSuccess = false;
        zmq::socket_t socket(context, ZMQ_PAIR);
        // 开放与玩家连接的端口
        while (!bindSuccess) {
            try {
                socket.bind("tcp://*:" + std::to_string(potentialPort));
                // 设置 TCP KeepAlive 检测
                socket.set(zmq::sockopt::tcp_keepalive, 1);
                socket.set(zmq::sockopt::tcp_keepalive_cnt, 3);
                socket.set(zmq::sockopt::tcp_keepalive_idle, 30);
                socket.set(zmq::sockopt::tcp_keepalive_intvl, 5);
                bindSuccess = true;
                potentialPort++;
            } catch (zmq::error_t &e) {
                spdlog::warn("服务器开放端口 {} 失败", potentialPort);
                if (e.num() == EADDRINUSE && potentialPort < 65535)
                    potentialPort++;
                else
                    throw e;
            }
        }
        spdlog::debug("服务器对玩家 {} 端口: {}", assignedId, potentialPort - 1);
        // 发送连接信息
        ConnectResponse connect_r;
        connect_r.set_port(potentialPort - 1);
        connect_r.set_player_id(assignedId);
        BasicMessage connect_m;
        connect_m.set_type(CommandType::CONNECT_REP);
        connect_m.set_message(connect_r.SerializeAsString());
        zmq::message_t connect_msg(connect_m.ByteSizeLong());
        connect_m.SerializeToArray(connect_msg.data(), static_cast<int>(connect_msg.size()));
        bridgeRepSocket.send(connect_msg, zmq::send_flags::none);
        // 验证玩家连接
        socket.set(zmq::sockopt::rcvtimeo, 1000); // 设置超时时间为1s
        PlayerPtr player = std::make_shared<Player>(assignedId++, std::move(socket));
        util::RecvResult rslt = util::recvCommand(player);
        if (rslt.has_value() && rslt.value() == CommandType::CONNECT_ACK) {
            spdlog::info("玩家 {} 连接成功", player->id);
            // 创建玩家对象
            std::lock_guard<std::mutex> lock(mtx);
            players.emplace_back(std::move(player));
        } else {
            if (rslt.has_value())
                spdlog::warn("玩家 {} 连接失败, 消息类型错误: {}", player->id, CommandType_Name(rslt.value()));
            else
                spdlog::warn("玩家 {} 连接失败, 其他错误原因", player->id);
        }
    }

    /// @brief 判断服务器是否准备就绪
    /// @return 服务器是否准备就绪
    bool GameServer::isReady() {
        checkAndKick();
        return players.size() >= MIN_PLAYER_NUM;
    }

    /// @brief 检查连通性并踢出掉线的玩家
    void GameServer::checkAndKick() {
        spdlog::debug("检查玩家连通性, 当前玩家数: {}", players.size());
        recheck:
        for (auto it = players.begin(); it != players.end(); it++) {
            // 发送验证连接请求
            bool s_rslt = util::sendCommand(*it, CommandType::CONNECT_ACK);
            util::RecvResult r_rslt = util::recvCommand(*it);
            if (!s_rslt || !r_rslt.has_value() || r_rslt.value() != CommandType::CONNECT_ACK) {
                if (r_rslt.has_value())
                    spdlog::warn("玩家 {} 掉线, 消息类型错误: {}", (*it)->id, CommandType_Name(r_rslt.value()));
                else
                    spdlog::warn("玩家 {} 掉线, 其他错误原因", (*it)->id);
                // 踢出掉线玩家 // 掉线了是发不出去的所以不发了
//                util::sendCommand(*it, CommandType::KICK);
                std::lock_guard<std::mutex> lock(mtx);
                std::lock_guard<std::mutex> lock_m((*it)->mtx);
                (*it)->socket.close();
                players.erase(it);
                goto recheck;
            }
        }
    }

    /// @brief 开始游戏
    void GameServer::start() {
        if (!isReady()) {
            throw std::runtime_error("人数不足, 无法开始游戏");
        }
        isWaiting = false;
        connectionThread.join();
        spdlog::info("开始游戏");
        // 移交 GameController 控制
        GameController controller(players);
        controller.start();
    }

    /// @brief 设置等待玩家人数
    /// @param num 等待玩家人数
    void GameServer::setWaitingPlayerNum(uint16_t num) {
        waitingPlayerNum = num;
    }

    /// @brief 列出所有玩家
    void GameServer::listPlayers() {
        spdlog::info("当前玩家数: {}", players.size());
        for (auto &player: players) {
            spdlog::info("玩家 ID: {}", player->id);
        }
    }

    /// @brief 踢出玩家
    /// @param player_id 玩家 ID
    void GameServer::kickPlayer(uint16_t player_id) {
        for (auto it = players.begin(); it != players.end(); it++) {
            if ((*it)->id == player_id) {
                util::sendCommand(*it, CommandType::KICK);
                std::lock_guard<std::mutex> lock(mtx);
                std::lock_guard<std::mutex> lock_m((*it)->mtx);
                (*it)->socket.close();
                players.erase(it);
                spdlog::info("玩家 {} 已被踢出", player_id);
                return;
            }
        }
        spdlog::warn("玩家 {} 不存在", player_id);
    }
}

