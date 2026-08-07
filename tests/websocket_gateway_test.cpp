#include <gtest/gtest.h>

#include "exchange/websocket_gateway.hpp"

TEST(
    WebSocketGatewayTest,
    ComputesRfc6455AcceptKey
) {
    /*
     * RFC 6455 section 1.3 example.
     */
    EXPECT_EQ(
        exchange::websocket_detail::
            websocket_accept_key(
                "dGhlIHNhbXBsZSBub25jZQ=="
            ),
        "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    );
}