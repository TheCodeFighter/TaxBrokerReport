#include "server/http_server.hpp"

#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <chrono>

namespace {
using namespace taxbroker;

TEST(HttpServerBindTest, AnotherProgramCannotBindThePortOfTheServer) {
    HttpServerOptions options;
    options.mTimeout = std::chrono::milliseconds(1000);
    options.mHeaderLimit = 8192;
    options.mBodyLimit = 1024;
    options.mHandleSignals = false;
    options.mNoLoginLimit = std::chrono::milliseconds(0);
    options.mIdleLimit = std::chrono::milliseconds(0);
    options.mClosedPageLimit = std::chrono::milliseconds(0);

    // Port 0: the system picks a free port, so no fixed port is used on a shared machine.
    const HttpServer server("127.0.0.1", 0, options);

    // A hostile program asks for SO_REUSEADDR itself. On Windows that option lets it take the
    // port of a server that did not set SO_EXCLUSIVEADDRUSE. The test checks only that the bind
    // fails, not the error: Linux and macOS give "address in use", Windows "access denied".
    boost::asio::io_context context;
    boost::asio::ip::tcp::acceptor other(context);
    const boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address_v4("127.0.0.1"),
                                                  server.port());
    boost::system::error_code error;

    other.open(endpoint.protocol());
    other.set_option(boost::asio::socket_base::reuse_address(true));
    other.bind(endpoint, error);

    EXPECT_TRUE(static_cast<bool>(error));
}

} // namespace
