//
// Copyright (c) 2016-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

//------------------------------------------------------------------------------
//
// Example: WebSocket server, asynchronous
//
//------------------------------------------------------------------------------

#include <boost/thread/thread.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/bind.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <list>

#include "logger.h"

using tcp = boost::asio::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>

//------------------------------------------------------------------------------

struct listener;
struct session;
struct game_loop;

struct game_loop : std::enable_shared_from_this<game_loop> {
  boost::asio::steady_timer                  timer_;
  std::shared_ptr<listener>                  listener_;
  std::map<size_t, std::shared_ptr<session>> sessions_;
  std::list<std::string>                     msg_in_;

  game_loop(boost::asio::io_context& ioc, tcp::endpoint endpoint);
  ~game_loop();
  void add_session(std::shared_ptr<session> s);
  void delete_session(std::shared_ptr<session> s);
  void add_message(std::shared_ptr<session> s, const std::string& msg);
  void update();
  void run();
};

//------------------------------------------------------------------------------

// Report a failure
void fail(boost::system::error_code ec, char const* what) {
  LOGGER_SERVER;
  std::cerr << what << ": " << ec.message() << "\n";
}

// Echoes back all received WebSocket messages
struct session : public std::enable_shared_from_this<session> {
  websocket::stream<tcp::socket> ws_;
  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::beast::multi_buffer buffer_in_;
  boost::beast::multi_buffer buffer_out_;
  std::weak_ptr<game_loop>   wgl_;
  std::list<std::string>     msg_out_;
  size_t                     id_;

 public:
  // Take ownership of the socket
  explicit session(std::weak_ptr<game_loop> wgl, tcp::socket socket)
      : ws_(std::move(socket)), strand_(ws_.get_executor()), wgl_(wgl) {
    LOGGER_SERVER;
  }

  ~session() {
    LOGGER_SERVER;
  }

  // Start the asynchronous operation
  void run() {
    LOGGER_SERVER;
    // Accept the websocket handshake
    ws_.async_accept(
        boost::asio::bind_executor(
          strand_,
          std::bind(
            &session::on_accept,
            shared_from_this(),
            std::placeholders::_1)));
  }

  void on_accept(boost::system::error_code ec) {
    LOGGER_SERVER;
    if(ec)
      return fail(ec, "accept");

    // Read a message
    do_read();
  }

  void do_read() {
    LOGGER_SERVER;
    // Read a message into our buffer
    ws_.async_read(
        buffer_in_,
        boost::asio::bind_executor(
          strand_,
          std::bind(
            &session::on_read,
            shared_from_this(),
            std::placeholders::_1,
            std::placeholders::_2)));
  }

  void on_read(boost::system::error_code ec, std::size_t bytes_transferred) {
    LOGGER_SERVER;
    boost::ignore_unused(bytes_transferred);

    auto sgl = wgl_.lock();

    // This indicates that the session was closed
    if(ec == websocket::error::closed) {
      if (sgl) {
        sgl->delete_session(shared_from_this());
      }
      return;
    }

    if(ec)
      fail(ec, "read");

    if (sgl) {
      std::string msg = boost::beast::buffers_to_string(buffer_in_.data());
      sgl->add_message(shared_from_this(), msg);
    }

    buffer_in_.consume(buffer_in_.size());

    do_read();
  }

  void do_write() {
    if (!msg_out_.empty()) {
      const auto& msg = msg_out_.front();

      boost::beast::ostream(buffer_out_) << msg;

      ws_.text(ws_.got_text());
      ws_.async_write(
          buffer_out_.data(),
          boost::asio::bind_executor(
            strand_,
            std::bind(
              &session::on_write,
              shared_from_this(),
              std::placeholders::_1,
              std::placeholders::_2)));

      msg_out_.pop_front();
    }
  }

  void on_write(boost::system::error_code ec, std::size_t bytes_transferred) {
    LOGGER_SERVER;
    boost::ignore_unused(bytes_transferred);

    if(ec)
      return fail(ec, "write");

    buffer_out_.consume(buffer_out_.size());

    if (!msg_out_.empty()) {
      do_write();
    }
  }
};

//------------------------------------------------------------------------------

// Accepts incoming connections and launches the sessions
struct listener : public std::enable_shared_from_this<listener> {
  tcp::acceptor acceptor_;
  tcp::socket socket_;
  std::weak_ptr<game_loop> wgl_;

 public:
  listener(boost::asio::io_context& ioc, std::weak_ptr<game_loop> wgl, tcp::endpoint endpoint)
      : acceptor_(ioc) , socket_(ioc), wgl_(wgl) {
    LOGGER_SERVER;
    boost::system::error_code ec;

    // Open the acceptor
    acceptor_.open(endpoint.protocol(), ec);
    if(ec) {
      fail(ec, "open");
      return;
    }

    // Allow address reuse
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
    if(ec) {
      fail(ec, "set_option");
      return;
    }

    // Bind to the server address
    acceptor_.bind(endpoint, ec);
    if(ec) {
      fail(ec, "bind");
      return;
    }

    // Start listening for connections
    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if(ec) {
      fail(ec, "listen");
      return;
    }
  }

  ~listener() {
    LOGGER_SERVER;
  }

  // Start accepting incoming connections
  void run() {
    LOGGER_SERVER;
    if(!acceptor_.is_open())
      return;
    do_accept();
  }

  void do_accept() {
    LOGGER_SERVER;
    acceptor_.async_accept(
        socket_,
        std::bind(
          &listener::on_accept,
          shared_from_this(),
          std::placeholders::_1));
  }

  void on_accept(boost::system::error_code ec) {
    LOGGER_SERVER;
    if(ec) {
      fail(ec, "accept");
    } else {
      auto sgl = wgl_.lock();
      if (sgl) {
        auto s = std::make_shared<session>(wgl_, std::move(socket_));
        s->run();
        sgl->add_session(s);
      } else {
        socket_.close();
      }
    }

    // Accept another connection
    do_accept();
  }
};

//------------------------------------------------------------------------------

game_loop::game_loop(boost::asio::io_context& ioc, tcp::endpoint endpoint)
    : timer_(ioc, boost::asio::chrono::seconds(1)) {
  LOGGER_SERVER;
  listener_ = std::make_shared<listener>(ioc, weak_from_this(), endpoint);
  timer_.async_wait(boost::bind(&game_loop::update, this));
}

game_loop::~game_loop() {
  LOGGER_SERVER;
}

void game_loop::add_session(std::shared_ptr<session> s) {
  LOGGER_SERVER;

  static size_t id = 0;
  id++;

  s->id_ = id;
  sessions_[id] = s;
  msg_in_.push_back("#" + std::to_string(id) + " has joined the room");
}

void game_loop::delete_session(std::shared_ptr<session> s) {
  sessions_.erase(s->id_);
  msg_in_.push_back("#" + std::to_string(s->id_) + " has left the room");
}

void game_loop::add_message(std::shared_ptr<session> s, const std::string& msg) {
  msg_in_.push_back("#" + std::to_string(s->id_) + ": " + msg);
}

void game_loop::update() {
  LOGGER_SERVER;

  if (!msg_in_.empty()) {
    const auto& msg = msg_in_.front();
    for (auto [_, session] : sessions_) {
      session->msg_out_.push_back(msg);
      session->do_write();
    }
    msg_in_.pop_front();
  }

  timer_.expires_at(timer_.expiry() + boost::asio::chrono::milliseconds(1000));
  timer_.async_wait(boost::bind(&game_loop::update, this));
}

void game_loop::run() {
  LOGGER_SERVER;
  listener_->wgl_ = weak_from_this(); // XXX
  listener_->run();
}

//------------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::cerr <<
      "Usage: websocket-server-async <address> <port> \n";
    return EXIT_FAILURE;
  }
  auto const address = boost::asio::ip::make_address(argv[1]);
  auto const port = static_cast<unsigned short>(std::atoi(argv[2]));

  boost::asio::io_context ioc;

  auto gl = std::make_shared<game_loop>(ioc, tcp::endpoint{address, port});
  gl->run();

  ioc.run();

  return EXIT_SUCCESS;
}
