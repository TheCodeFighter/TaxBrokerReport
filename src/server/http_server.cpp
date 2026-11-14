#include "http_server.hpp"

#include "taxbroker/logging.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <chrono>
#include <csignal>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#endif

namespace taxbroker {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

/// The wait before the next accept after an accept error.
constexpr std::chrono::milliseconds kAcceptRetryDelay{100};

/// True when the error comes from the HTTP parser (a bad request), not from the socket.
bool isParserError(const beast::error_code& aError) {
    return aError.category() == http::make_error_code(http::error::bad_method).category();
}

/// The IP address to listen on; `localhost` is the IPv4 loopback.
asio::ip::address parseHost(const std::string& aHost) {
    if (aHost == "localhost")
    {
        return asio::ip::address_v4::loopback();
    }

    boost::system::error_code error;
    const asio::ip::address address = asio::ip::make_address(aHost, error);

    if (error)
    {
        throw std::runtime_error("TBR_SERVER_HOST is not an IP address.");
    }

    return address;
}

/// One client connection: reads a request, answers it, and repeats while the client keeps the
/// connection alive. It lives as long as a read or write of it is pending.
class Connection : public std::enable_shared_from_this<Connection> {
  public:
    Connection(tcp::socket&& aSocket,
               const HttpServerOptions& aOptions,
               std::shared_ptr<const HttpServer::Handler> aHandler)
        : mStream(std::move(aSocket)), mOptions(aOptions), mHandler(std::move(aHandler)) {}

    void start() {
        read();
    }

  private:
    void read() {
        mParser.emplace();
        mParser->header_limit(static_cast<std::uint32_t>(mOptions.mHeaderLimit));
        mParser->body_limit(mOptions.mBodyLimit);
        mStream.expires_after(mOptions.mTimeout);

        http::async_read(mStream,
                         mBuffer,
                         *mParser,
                         [self = shared_from_this()](beast::error_code aError, std::size_t) {
                             self->onRead(aError);
                         });
    }

    void onRead(beast::error_code aError) {
        if (!aError)
        {
            answer();

            return;
        }

        // A timeout, a closed or reset connection: nothing to answer.
        if (!isParserError(aError) || aError == http::error::end_of_stream ||
            aError == http::error::partial_message)
        {
            close();

            return;
        }

        unsigned status = 400;

        if (aError == http::error::header_limit)
        {
            status = 431;
        }
        else if (aError == http::error::body_limit)
        {
            status = 413;
        }

        // The rest of the message was not read, so the connection cannot be used again.
        write(makeErrorResponse(status), 11, false);
    }

    void answer() {
        const http::request<http::string_body>& request = mParser->get();

        HttpRequest converted;
        converted.mMethod = std::string(request.method_string());
        converted.mTarget = std::string(request.target());
        converted.mBody = request.body();

        for (const auto& field : request)
        {
            converted.mHeaders.push_back(
                {std::string(field.name_string()), std::string(field.value())});
        }

        HttpResponse response;

        try
        {
            // The one place where business code runs for a request.
            response = (*mHandler)(converted);
        } catch (const std::exception&)
        {
            // No detail in the log: an exception text can hold a path or a value.
            LOG_ERROR("A request handler failed");
            response = makeErrorResponse(500);
        }

        write(std::move(response), request.version(), request.keep_alive());
    }

    void write(HttpResponse aResponse, unsigned aVersion, bool aKeepAlive) {
        auto message = std::make_shared<http::response<http::string_body>>();
        message->result(aResponse.mStatus);
        message->version(aVersion);

        for (const HttpField& field : aResponse.mHeaders)
        {
            message->insert(field.mName, field.mValue);
        }

        message->body() = std::move(aResponse.mBody);
        message->keep_alive(aKeepAlive);
        message->prepare_payload();

        mStream.expires_after(mOptions.mTimeout);

        http::async_write(
            mStream,
            *message,
            [self = shared_from_this(), message](beast::error_code aError, std::size_t) {
                if (aError || message->need_eof())
                {
                    self->close();

                    return;
                }

                self->read();
            });
    }

    void close() {
        beast::error_code ignored;
        mStream.socket().shutdown(tcp::socket::shutdown_send, ignored);
    }

    beast::tcp_stream mStream;
    beast::flat_buffer mBuffer;
    std::optional<http::request_parser<http::string_body>> mParser;
    HttpServerOptions mOptions;
    std::shared_ptr<const HttpServer::Handler> mHandler;
};

} // namespace

struct HttpServer::Impl {
    Impl(const std::string& aHost, std::uint16_t aPort, const HttpServerOptions& aOptions)
        : mOptions(aOptions), mAcceptor(mContext), mRetryTimer(mContext) {
        const tcp::endpoint endpoint(parseHost(aHost), aPort);

        try
        {
            mAcceptor.open(endpoint.protocol());

#if !defined(_WIN32)
            // Asio does not mark its sockets close-on-exec, so the browser that xdg-open starts
            // would keep the port open after the server ends. It cannot fail on an open socket.
            (void)::fcntl(mAcceptor.native_handle(), F_SETFD, FD_CLOEXEC);
#endif

            mAcceptor.set_option(asio::socket_base::reuse_address(true));
            mAcceptor.bind(endpoint);
            mAcceptor.listen(asio::socket_base::max_listen_connections);
        } catch (const boost::system::system_error&)
        {
            // The system message can hold the address; say only what failed.
            throw std::runtime_error("cannot listen: the port is in use or not allowed");
        }
    }

    void accept() {
        mAcceptor.async_accept([this](beast::error_code aError, tcp::socket aSocket) {
            if (aError == asio::error::operation_aborted)
            {
                return;
            }

            if (aError)
            {
                // Out of file handles, for example: the waiting connection stays queued, so an
                // accept at once would fail again and again at full CPU. Wait, then try again.
                mRetryTimer.expires_after(kAcceptRetryDelay);
                mRetryTimer.async_wait([this](const beast::error_code& aWaitError) {
                    if (!aWaitError)
                    {
                        accept();
                    }
                });

                return;
            }

            std::make_shared<Connection>(std::move(aSocket), mOptions, mHandler)->start();
            accept();
        });
    }

    HttpServerOptions mOptions;
    asio::io_context mContext{1};
    tcp::acceptor mAcceptor;
    asio::steady_timer mRetryTimer; ///< Waits after an accept error.
    std::shared_ptr<const HttpServer::Handler> mHandler;
};

HttpServer::HttpServer(const std::string& aHost,
                       std::uint16_t aPort,
                       const HttpServerOptions& aOptions)
    : mImpl(std::make_unique<Impl>(aHost, aPort, aOptions)) {}

HttpServer::~HttpServer() = default;

std::uint16_t HttpServer::port() const {
    return mImpl->mAcceptor.local_endpoint().port();
}

void HttpServer::run(Handler aHandler) {
    mImpl->mHandler = std::make_shared<const Handler>(std::move(aHandler));

    std::optional<asio::signal_set> signals;

    if (mImpl->mOptions.mHandleSignals)
    {
        signals.emplace(mImpl->mContext, SIGINT, SIGTERM);
        signals->async_wait([this](const beast::error_code&, int) { mImpl->mContext.stop(); });
    }

    mImpl->accept();
    mImpl->mContext.run();
    mImpl->mAcceptor.close();
}

void HttpServer::stop() {
    asio::post(mImpl->mContext, [this]() { mImpl->mContext.stop(); });
}

} // namespace taxbroker
