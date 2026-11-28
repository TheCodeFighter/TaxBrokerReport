#include "http_server.hpp"

#include "taxbroker/exception_kind.hpp"
#include "taxbroker/logging.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <chrono>
#include <csignal>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

/// The interim response that tells a client to send the body.
constexpr std::string_view kContinueResponse = "HTTP/1.1 100 Continue\r\n\r\n";

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

/// What a connection tells the server about a response it wrote.
struct ConnectionHooks {
    std::function<void()> mSessionUsed; ///< A response with `mSessionUsed` is about to be written.
    std::function<void()> mStop;        ///< A response with `mStopAfterWrite` was written.
    /// A response with `mLastPageClosed` is about to be written.
    std::function<void()> mPageClosed;
    /// A response with `mPageOpened` is about to be written.
    std::function<void()> mPageOpened;
};

/// One client connection: reads a request, answers it, and repeats while the client keeps the
/// connection alive. It lives as long as a read or write of it is pending.
class Connection : public std::enable_shared_from_this<Connection> {
  public:
    Connection(tcp::socket&& aSocket,
               const HttpServerOptions& aOptions,
               std::shared_ptr<const HttpServer::Handler> aHandler,
               std::shared_ptr<const HttpServer::BodyPolicyFunction> aBodyPolicy,
               std::shared_ptr<const ConnectionHooks> aHooks)
        : mStream(std::move(aSocket)), mOptions(aOptions), mHandler(std::move(aHandler)),
          mBodyPolicy(std::move(aBodyPolicy)), mHooks(std::move(aHooks)) {}

    void start() {
        read();
    }

  private:
    void read() {
        mParser.emplace();
        mParser->header_limit(static_cast<std::uint32_t>(mOptions.mHeaderLimit));

        // The body limit comes from the body policy after the header is read, and Beast checks a
        // declared length against the limit while it reads the header: no limit until then.
        mParser->body_limit(boost::none);
        mRequest = HttpRequest{};

        // The header has the whole timeout; the body gets it again for every chunk.
        mStream.expires_after(mOptions.mTimeout);

        http::async_read_header(mStream,
                                mBuffer,
                                *mParser,
                                [self = shared_from_this()](beast::error_code aError, std::size_t) {
                                    self->onHeader(aError);
                                });
    }

    void onHeader(beast::error_code aError) {
        if (aError)
        {
            onReadError(aError, false);

            return;
        }

        const http::request<http::string_body>& message = mParser->get();

        mRequest.mMethod = std::string(message.method_string());
        mRequest.mTarget = std::string(message.target());

        for (const auto& field : message)
        {
            mRequest.mHeaders.push_back(
                {std::string(field.name_string()), std::string(field.value())});
        }

        // The limit comes from the header alone, so the body of a caller without a session is
        // never held beyond the default limit.
        BodyPolicy policy{mOptions.mBodyLimit, true};

        if (*mBodyPolicy)
        {
            try
            { policy = (*mBodyPolicy)(mRequest); } catch (...)
            {
                // The verb is the library's name for it; the target and the text are the caller's.
                LOG_ERROR("A request handler failed: {} body_policy ({})",
                          http::to_string(message.method()),
                          currentExceptionKind());
                write(makeErrorResponse(500), 11, false);

                return;
            }
        }

        mParser->body_limit(policy.mLimit);

        const auto declaredLength = mParser->content_length();

        // A refused request with a body that is large or of unknown length gets the guard's own
        // answer at once: a 413 would hide the reason (a wrong session, for example). A small body
        // is read, and the handler refuses it.
        if (policy.mRefusal.has_value() && !mParser->is_done() &&
            (!declaredLength || *declaredLength > policy.mLimit))
        {
            LOG_WARNING("Request refused: {} {}", policy.mRefusal->mStatus, policy.mReason);
            write(std::move(*policy.mRefusal), message.version(), false);

            return;
        }

        // A declared length over the limit is refused at once, without waiting for the body.
        if (declaredLength && *declaredLength > policy.mLimit)
        {
            // Only the fixed limit: the declared length and the target are the caller's.
            LOG_WARNING("Request refused: 413 body_over_the_limit (limit {})", policy.mLimit);
            write(makeErrorResponse(413), 11, false);

            return;
        }

        if (mParser->is_done())
        {
            answer();

            return;
        }

        if (beast::iequals(message[http::field::expect], "100-continue") && policy.mGuardPassed)
        {
            asio::async_write(
                mStream,
                asio::buffer(kContinueResponse),
                [self = shared_from_this()](beast::error_code aWriteError, std::size_t) {
                    if (aWriteError)
                    {
                        self->close();

                        return;
                    }

                    self->readBody();
                });

            return;
        }

        readBody();
    }

    /// Reads the body in chunks; every chunk that arrives renews the idle timeout.
    void readBody() {
        mStream.expires_after(mOptions.mTimeout);

        http::async_read_some(mStream,
                              mBuffer,
                              *mParser,
                              [self = shared_from_this()](beast::error_code aError, std::size_t) {
                                  if (aError)
                                  {
                                      self->onReadError(aError, true);

                                      return;
                                  }

                                  if (self->mParser->is_done())
                                  {
                                      self->answer();

                                      return;
                                  }

                                  self->readBody();
                              });
    }

    /// An error while reading. A closed or timed-out connection gets no answer; one in the body
    /// leaves a warning, because an upload that stops is otherwise invisible. An idle keep-alive
    /// connection that ends (error in the header) is normal and writes nothing.
    void onReadError(beast::error_code aError, bool aInBody) {
        if (!isParserError(aError) || aError == http::error::end_of_stream ||
            aError == http::error::partial_message)
        {
            if (aInBody)
            {
                LOG_WARNING("Request not finished: {}",
                            aError == beast::error::timeout ? "timeout" : "connection_closed");
            }

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

        LOG_WARNING("Request refused: {} unreadable_request", status);

        // The rest of the message was not read, so the connection cannot be used again.
        write(makeErrorResponse(status), 11, false);
    }

    void answer() {
        http::request<http::string_body> message = mParser->release();

        // The body moves out of the parser: it is never copied.
        mRequest.mBody = std::move(message.body());

        HttpResponse response;

        try
        {
            // The one place where business code runs for a request.
            response = (*mHandler)(mRequest);
        } catch (...)
        {
            // The kind of the exception only: its text can hold a path or a value.
            LOG_ERROR("A request handler failed: {} handler ({})",
                      http::to_string(message.method()),
                      currentExceptionKind());
            response = makeErrorResponse(500);
        }

        // The request is not needed any more: free the body before the response is written.
        mRequest = HttpRequest{};

        if (response.mSessionUsed)
        {
            mHooks->mSessionUsed();
        }

        if (response.mPageOpened)
        {
            mHooks->mPageOpened();
        }

        if (response.mLastPageClosed)
        {
            mHooks->mPageClosed();
        }

        write(std::move(response), message.version(), message.keep_alive());
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

        // The answer of the quit route is the last one: the connection closes with it.
        message->keep_alive(aKeepAlive && !aResponse.mStopAfterWrite);
        message->prepare_payload();

        mStream.expires_after(mOptions.mTimeout);

        http::async_write(
            mStream,
            *message,
            [self = shared_from_this(),
             message,
             stopAfter = aResponse.mStopAfterWrite](beast::error_code aError, std::size_t) {
                if (stopAfter)
                {
                    // Written (or failed): the page is told or the connection is gone either way.
                    self->close();
                    self->mHooks->mStop();

                    return;
                }

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
    HttpRequest mRequest; ///< The request being read: the header first, the body at the end.
    HttpServerOptions mOptions;
    std::shared_ptr<const HttpServer::Handler> mHandler;
    std::shared_ptr<const HttpServer::BodyPolicyFunction> mBodyPolicy;
    std::shared_ptr<const ConnectionHooks> mHooks;
};

} // namespace

struct HttpServer::Impl {
    Impl(const std::string& aHost, std::uint16_t aPort, const HttpServerOptions& aOptions)
        : mOptions(aOptions), mAcceptor(mContext), mRetryTimer(mContext), mNoLoginTimer(mContext),
          mIdleTimer(mContext), mClosedPageTimer(mContext) {
        const tcp::endpoint endpoint(parseHost(aHost), aPort);

        try
        {
            mAcceptor.open(endpoint.protocol());

#if !defined(_WIN32)
            // Asio does not mark its sockets close-on-exec, so the browser that xdg-open starts
            // would keep the port open after the server ends. It cannot fail on an open socket.
            (void)::fcntl(mAcceptor.native_handle(), F_SETFD, FD_CLOEXEC);
#endif

#if defined(_WIN32)
            // On Windows SO_REUSEADDR lets another program bind the same port while this one
            // listens and take over the connections. SO_EXCLUSIVEADDRUSE stops that.
            const int exclusive = 1;

            if (::setsockopt(mAcceptor.native_handle(),
                             SOL_SOCKET,
                             SO_EXCLUSIVEADDRUSE,
                             reinterpret_cast<const char*>(&exclusive),
                             sizeof(exclusive)) != 0)
            {
                throw boost::system::system_error(
                    boost::system::error_code(::WSAGetLastError(),
                                              boost::system::system_category()));
            }
#else
            mAcceptor.set_option(asio::socket_base::reuse_address(true));
#endif
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
                // One warning for a series of failures, not one per try. The text is the system
                // message of the error, for example "Too many open files".
                if (!mAcceptFailing)
                {
                    LOG_WARNING("Accept failed: {}; trying again", aError.message());
                    mAcceptFailing = true;
                }

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

            mAcceptFailing = false;

            std::make_shared<Connection>(std::move(aSocket),
                                         mOptions,
                                         mHandler,
                                         mBodyPolicy,
                                         mHooks)
                ->start();
            accept();
        });
    }

    /// Starts the no-login timer; called once when `run` starts.
    void startNoLoginTimer() {
        if (mOptions.mNoLoginLimit.count() <= 0)
        {
            return;
        }

        mNoLoginTimer.expires_after(mOptions.mNoLoginLimit);
        mNoLoginTimer.async_wait([this](const beast::error_code& aError) {
            // A login after the timer fired but before this ran also counts.
            if (aError || mSessionSeen)
            {
                return;
            }

            LOG_INFO("Stopped: nobody opened the link");
            mContext.stop();
        });
    }

    /// A response that counts as use is about to be written: the first one ends the no-login
    /// wait, every one restarts the idle wait.
    void sessionUsed() {
        mSessionSeen = true;
        mNoLoginTimer.cancel();

        if (mOptions.mIdleLimit.count() <= 0)
        {
            return;
        }

        // Restarting cancels the pending wait. A wait that already expired may still run with
        // success: it checks the new expiry time.
        mIdleTimer.expires_after(mOptions.mIdleLimit);
        mIdleTimer.async_wait([this](const beast::error_code& aError) {
            if (aError || mIdleTimer.expiry() > asio::steady_timer::clock_type::now())
            {
                return;
            }

            LOG_INFO("Stopped: not used for a while");
            mContext.stop();
        });
    }

    /// A page said it is open: a reload came back, so the wait after the close ends. A request
    /// of a page that is closing (a run that was still going) does not end it.
    void pageOpened() {
        mClosePending = false;
        mClosedPageTimer.cancel();
    }

    /// The last open page closed: waits for a page to come back, then stops.
    void pageClosed() {
        if (mOptions.mClosedPageLimit.count() <= 0)
        {
            return;
        }

        mClosePending = true;
        mClosedPageTimer.expires_after(mOptions.mClosedPageLimit);
        mClosedPageTimer.async_wait([this](const beast::error_code& aError) {
            // An open message after the timer fired but before this ran also counts.
            if (aError || !mClosePending ||
                mClosedPageTimer.expiry() > asio::steady_timer::clock_type::now())
            {
                return;
            }

            LOG_INFO("Stopped: the page was closed");
            mContext.stop();
        });
    }

    HttpServerOptions mOptions;
    asio::io_context mContext{1};
    tcp::acceptor mAcceptor;
    asio::steady_timer mRetryTimer;      ///< Waits after an accept error.
    asio::steady_timer mNoLoginTimer;    ///< Stops the server when nobody logs in.
    asio::steady_timer mIdleTimer;       ///< Stops the server after a long time without use.
    asio::steady_timer mClosedPageTimer; ///< Stops the server when the last page closed.
    bool mClosePending{};  ///< True from a close message until a page says it is open.
    bool mSessionSeen{};   ///< True after the first response that counts as use.
    bool mAcceptFailing{}; ///< True after a failed accept, until one works; for the log.
    std::shared_ptr<const HttpServer::Handler> mHandler;
    std::shared_ptr<const HttpServer::BodyPolicyFunction> mBodyPolicy;
    std::shared_ptr<const ConnectionHooks> mHooks;
};

HttpServer::HttpServer(const std::string& aHost,
                       std::uint16_t aPort,
                       const HttpServerOptions& aOptions)
    : mImpl(std::make_unique<Impl>(aHost, aPort, aOptions)) {}

HttpServer::~HttpServer() = default;

std::uint16_t HttpServer::port() const {
    return mImpl->mAcceptor.local_endpoint().port();
}

void HttpServer::run(Handler aHandler, BodyPolicyFunction aBodyPolicy) {
    mImpl->mHandler = std::make_shared<const Handler>(std::move(aHandler));
    mImpl->mBodyPolicy = std::make_shared<const BodyPolicyFunction>(std::move(aBodyPolicy));

    std::shared_ptr<ConnectionHooks> hooks = std::make_shared<ConnectionHooks>();
    hooks->mSessionUsed = [this]() { mImpl->sessionUsed(); };
    hooks->mStop = [this]() { mImpl->mContext.stop(); };
    hooks->mPageClosed = [this]() { mImpl->pageClosed(); };
    hooks->mPageOpened = [this]() { mImpl->pageOpened(); };
    mImpl->mHooks = std::move(hooks);

    std::optional<asio::signal_set> signals;

    if (mImpl->mOptions.mHandleSignals)
    {
        signals.emplace(mImpl->mContext, SIGINT, SIGTERM);

#if !defined(_WIN32)
        // A closed terminal window: stop cleanly, so the launch file goes too.
        signals->add(SIGHUP);
#endif

        signals->async_wait([this](const beast::error_code&, int) { mImpl->mContext.stop(); });
    }

    mImpl->startNoLoginTimer();
    mImpl->accept();
    mImpl->mContext.run();
    mImpl->mAcceptor.close();
}

void HttpServer::stop() {
    asio::post(mImpl->mContext, [this]() { mImpl->mContext.stop(); });
}

} // namespace taxbroker
