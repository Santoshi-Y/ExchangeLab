#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "exchange/tcp_server.hpp"

namespace {

TEST(TcpServerTest, ReceivesBytesFromLoopbackClient) {
    constexpr std::uint16_t port = 19000;

    exchange::TcpServer server(port);

    ASSERT_TRUE(server.start());

    std::vector<std::byte> received;

    std::thread server_thread([&server, &received]() {
        ASSERT_TRUE(server.accept_client());
        received = server.receive();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const int client_socket =
        ::socket(AF_INET, SOCK_STREAM, 0);

    ASSERT_GE(client_socket, 0);

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    ASSERT_EQ(
        ::inet_pton(
            AF_INET,
            "127.0.0.1",
            &address.sin_addr
        ),
        1
    );

    ASSERT_EQ(
        ::connect(
            client_socket,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ),
        0
    );

    const std::array<std::byte, 5> message {
        std::byte {0x68},
        std::byte {0x65},
        std::byte {0x6C},
        std::byte {0x6C},
        std::byte {0x6F}
    };

    const auto sent =
        ::send(
            client_socket,
            message.data(),
            message.size(),
            0
        );

    ASSERT_EQ(
        sent,
        static_cast<ssize_t>(message.size())
    );

    ::close(client_socket);

    server_thread.join();
    server.stop();

    ASSERT_EQ(received.size(), message.size());

    for (std::size_t index = 0;
         index < message.size();
         ++index) {
        EXPECT_EQ(received[index], message[index]);
    }
}

}  // namespace