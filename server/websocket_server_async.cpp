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
#include <vector>

#include "logger.h"

using tcp = boost::asio::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>

//------------------------------------------------------------------------------

struct listener;
struct session;
struct game_loop;

//------------------------------------------------------------------------------

// Report a failure
void fail(boost::system::error_code ec, char const* what) {
  LOGGER_SERVER;
  std::cerr << what << ": " << ec.message() << "\n";
}

// Echoes back all received WebSocket messages
class session : public std::enable_shared_from_this<session> {
  websocket::stream<tcp::socket> ws_;
  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::beast::multi_buffer buffer_;

 public:
  // Take ownership of the socket
  explicit session(tcp::socket socket) : ws_(std::move(socket)), strand_(ws_.get_executor()) {
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
        buffer_,
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

    // This indicates that the session was closed
    if(ec == websocket::error::closed)
      return;

    if(ec)
      fail(ec, "read");

    // Echo the message
    ws_.text(ws_.got_text());
    ws_.async_write(
        buffer_.data(),
        boost::asio::bind_executor(
          strand_,
          std::bind(
            &session::on_write,
            shared_from_this(),
            std::placeholders::_1,
            std::placeholders::_2)));
  }

  void on_write(boost::system::error_code ec, std::size_t bytes_transferred) {
    LOGGER_SERVER;
    boost::ignore_unused(bytes_transferred);

    if(ec)
      return fail(ec, "write");

    // Clear the buffer
    buffer_.consume(buffer_.size());

    // Do another read
    do_read();
  }
};

//------------------------------------------------------------------------------

// Accepts incoming connections and launches the sessions
class listener : public std::enable_shared_from_this<listener> {
  tcp::acceptor acceptor_;
  tcp::socket socket_;
  std::weak_ptr<game_loop> wgl;

 public:
  listener(boost::asio::io_context& ioc, std::weak_ptr<game_loop> wgl, tcp::endpoint endpoint)
      : acceptor_(ioc) , socket_(ioc), wgl(wgl) {
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

  // Start accepting incoming connections
  void run() {
    LOGGER_SERVER;
    if(! acceptor_.is_open())
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
      // Create the session and run it
      std::make_shared<session>(std::move(socket_))->run();
    }

    // Accept another connection
    do_accept();
  }
};

//------------------------------------------------------------------------------

struct game_loop : std::enable_shared_from_this<game_loop> {
  boost::asio::steady_timer timer_;
  std::shared_ptr<listener> listener_;
  std::vector<std::shared_ptr<session>> sessions;

  game_loop(boost::asio::io_context& ioc, tcp::endpoint endpoint)
      : timer_(ioc, boost::asio::chrono::seconds(1)) {
    LOGGER_SERVER;
    listener_ = std::make_shared<listener>(ioc, weak_from_this(), endpoint);
    timer_.async_wait(boost::bind(&game_loop::update, this));
  }

  void update() {
    LOGGER_SERVER;

    timer_.expires_at(timer_.expiry() + boost::asio::chrono::milliseconds(1000));
    timer_.async_wait(boost::bind(&game_loop::update, this));
  }

  void run() {
    LOGGER_SERVER;
    listener_->run();
  }
};

//------------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  // Check command line arguments.
  if (argc != 3) {
    std::cerr <<
      "Usage: websocket-server-async <address> <port> \n";
    return EXIT_FAILURE;
  }
  auto const address = boost::asio::ip::make_address(argv[1]);
  auto const port = static_cast<unsigned short>(std::atoi(argv[2]));

  boost::asio::io_context ioc;

  game_loop gl(ioc, tcp::endpoint{address, port});
  gl.run();

  ioc.run();

  return EXIT_SUCCESS;
}
