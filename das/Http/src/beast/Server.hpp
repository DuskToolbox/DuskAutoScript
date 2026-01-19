#ifndef DAS_HTTP_BEAST_SERVER_HPP
#define DAS_HTTP_BEAST_SERVER_HPP

#include "Request.hpp"
#include "Router.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/config.hpp>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <functional>

namespace Das::Http::Beast
{

namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

// 检测会话，处理单个HTTP连接
class Session : public std::enable_shared_from_this<Session>
{
    tcp::socket socket_;
    http::request<http::string_body> request_;
    http::response<http::string_body> response_;
    std::shared_ptr<Router> router_;
    std::function<bool()> stop_condition_;

public:
    Session(tcp::socket socket, std::shared_ptr<Router> router, std::function<bool()> stop_condition)
        : socket_(std::move(socket))
        , router_(std::move(router))
        , stop_condition_(std::move(stop_condition))
    {
    }

    void Run()
    {
        DoRead();
    }

private:
    void DoRead()
    {
        auto self = shared_from_this();
        
        http::async_read(
            socket_,
            buffer_,
            request_,
            [self](boost::beast::error_code ec, std::size_t bytes_transferred) {
                boost::ignore_unused(bytes_transferred);
                if (ec)
                {
                    return;
                }
                self->ProcessRequest();
            });
    }

    void ProcessRequest()
    {
        HttpRequest req(std::move(request_));
        HttpResponse http_response = router_->Handle(req);
        response_ = http_response.Release();
        
        DoWrite();
    }

    void DoWrite()
    {
        auto self = shared_from_this();
        
        http::async_write(
            socket_,
            response_,
            [self](boost::beast::error_code ec, std::size_t bytes_transferred) {
                boost::ignore_unused(bytes_transferred);
                if (ec)
                {
                    return;
                }
                
                // 关闭连接
                boost::beast::error_code shutdown_ec;
                self->socket_.shutdown(tcp::socket::shutdown_send, shutdown_ec);
            });
    }

private:
    boost::beast::flat_buffer buffer_;
};

// 监听器，接受传入的连接
class Listener : public std::enable_shared_from_this<Listener>
{
    tcp::acceptor acceptor_;
    std::shared_ptr<Router> router_;
    std::function<bool()> stop_condition_;

public:
    Listener(
        boost::asio::io_context& ioc,
        tcp::endpoint endpoint,
        std::shared_ptr<Router> router,
        std::function<bool()> stop_condition)
        : acceptor_(boost::asio::make_strand(ioc))
        , router_(std::move(router))
        , stop_condition_(std::move(stop_condition))
    {
        boost::beast::error_code ec;

        acceptor_.open(endpoint.protocol(), ec);
        if (ec)
        {
            return;
        }

        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec)
        {
            return;
        }

        acceptor_.bind(endpoint, ec);
        if (ec)
        {
            return;
        }

        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec)
        {
            return;
        }
    }

    void Run()
    {
        DoAccept();
    }

private:
    void DoAccept()
    {
        acceptor_.async_accept(
            boost::asio::make_strand(acceptor_.get_executor()),
            [self = shared_from_this()](boost::beast::error_code ec, tcp::socket socket) {
                if (ec)
                {
                    return;
                }
                
                // 创建会话并运行
                std::make_shared<Session>(
                    std::move(socket),
                    self->router_,
                    self->stop_condition_
                )->Run();
                
                // 继续接受下一个连接
                self->DoAccept();
            });
    }
};

// HTTP服务器
class Server
{
public:
    Server(
        const std::string& address,
        unsigned short port,
        std::shared_ptr<Router> router,
        std::function<bool()> stop_condition)
        : router_(std::move(router))
        , stop_condition_(std::move(stop_condition))
        , ioc_(1)
    {
        auto const addr = boost::asio::ip::make_address(address);
        auto const endpoint = tcp::endpoint{addr, port};
        
        listener_ = std::make_shared<Listener>(ioc_, endpoint, router_, stop_condition_);
    }
    
    void Run()
    {
        listener_->Run();
        ioc_.run();
    }
    
    void Stop()
    {
        ioc_.stop();
    }
    
    Router& GetRouter()
    {
        return *router_;
    }
    
    const Router& GetRouter() const
    {
        return *router_;
    }

private:
    std::shared_ptr<Router> router_;
    std::function<bool()> stop_condition_;
    boost::asio::io_context ioc_;
    std::shared_ptr<Listener> listener_;
};

} // namespace Das::Http::Beast

#endif // DAS_HTTP_BEAST_SERVER_HPP
