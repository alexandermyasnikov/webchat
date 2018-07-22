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

//------------------------------------------------------------------------------

struct command_t {
  virtual bool from_string(size_t id, const std::string& msg) = 0;
  virtual bool process(std::shared_ptr<game_loop> sgl) = 0;
  virtual std::string name() = 0;
  virtual std::shared_ptr<command_t> prototype() = 0;
};

using action_t = command_t;
using evnent_t = command_t; // TODO

struct action_text_t : action_t {
  size_t _id;
  std::string _msg;

  bool from_string(size_t id, const std::string& msg) override;
  bool process(std::shared_ptr<game_loop> sgl) override;
  std::string name() override;
  std::shared_ptr<action_t> prototype() override;
};

struct action_join_t : action_t {
  size_t _id;

  bool from_string(size_t id, const std::string& msg) override;
  bool process(std::shared_ptr<game_loop> sgl) override;
  std::string name() override;
  std::shared_ptr<action_t> prototype() override;
};

struct action_leave_t : action_t {
  size_t _id;

  bool from_string(size_t id, const std::string& msg) override;
  bool process(std::shared_ptr<game_loop> sgl) override;
  std::string name() override;
  std::shared_ptr<action_t> prototype() override;
};


struct action_factory_t {
  std::map<std::string, std::shared_ptr<action_t>> actions;

  bool registry(std::shared_ptr<action_t> action);
  std::shared_ptr<action_t> from_string(size_t id, const std::string& msg);
};

bool action_factory_t::registry(std::shared_ptr<action_t> action) {
  actions[action->name()] = action;
  return true;
}

std::shared_ptr<action_t> action_factory_t::from_string(size_t id, const std::string& msg) {
  std::shared_ptr<action_t> action_ret;

  std::string name = msg.substr(0, msg.find(' '));
  auto it = actions.find(name);
  if (it != actions.end()) {
    action_ret = it->second->prototype();
  } else {
    action_ret = actions[""]; // TODO default action.
  }

  action_ret->from_string(id, msg);
  return action_ret;
}

//------------------------------------------------------------------------------

struct game_loop : std::enable_shared_from_this<game_loop> {
  boost::asio::steady_timer                  timer_;
  std::shared_ptr<listener>                  listener_;
  std::map<size_t, std::shared_ptr<session>> sessions_;
  std::list<std::pair<size_t, std::string>>  msg_in_;
  std::list<std::pair<size_t, std::string>>  msg_out_;

  game_loop(boost::asio::io_context& ioc, tcp::endpoint endpoint);
  ~game_loop();
  void add_session(std::shared_ptr<session> s);
  void delete_session(std::shared_ptr<session> s);
  void add_message(std::shared_ptr<session> s, const std::string& msg);
  void send_message(const std::string& msg); // old
  void on_update();
  void run();
};

//------------------------------------------------------------------------------

void fail(boost::system::error_code ec, char const* what) {
  LOGGER_SERVER;
  std::cerr << what << ": " << ec.message() << "\n";
}

struct session : public std::enable_shared_from_this<session> {
  websocket::stream<tcp::socket> ws_;
  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::beast::multi_buffer buffer_in_;
  boost::beast::multi_buffer buffer_out_;
  std::weak_ptr<game_loop>   wgl_;
  std::list<std::string>     msg_out_;
  size_t                     id_;
  bool                       write_msg_;

 public:
  explicit session(std::weak_ptr<game_loop> wgl, tcp::socket socket)
      : ws_(std::move(socket)), strand_(ws_.get_executor()), wgl_(wgl), write_msg_(false) {
    LOGGER_SERVER;
  }

  ~session() {
    LOGGER_SERVER;
  }

  void run() {
    LOGGER_SERVER;
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

    do_read();
  }

  void do_read() {
    LOGGER_SERVER;
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
    LOGGER_SERVER;
    if (!msg_out_.empty() && !write_msg_) {
      write_msg_ = true;
      const auto& msg = msg_out_.front();

      buffer_out_.consume(buffer_out_.size());
      boost::beast::ostream(buffer_out_) << msg;

      LOG_SERVER("buffer_out_.size(): %zd", buffer_out_.size());

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
    write_msg_ = false;

    if(ec)
      return fail(ec, "write");

    if (!msg_out_.empty()) {
      do_write();
    }
  }
};

//------------------------------------------------------------------------------

struct listener : public std::enable_shared_from_this<listener> {
  tcp::acceptor acceptor_;
  tcp::socket socket_;
  std::weak_ptr<game_loop> wgl_;

 public:
  listener(boost::asio::io_context& ioc, std::weak_ptr<game_loop> wgl, tcp::endpoint endpoint)
      : acceptor_(ioc) , socket_(ioc), wgl_(wgl) {
    LOGGER_SERVER;
    boost::system::error_code ec;

    acceptor_.open(endpoint.protocol(), ec);
    if(ec) {
      fail(ec, "open");
      return;
    }

    acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
    if(ec) {
      fail(ec, "set_option");
      return;
    }

    acceptor_.bind(endpoint, ec);
    if(ec) {
      fail(ec, "bind");
      return;
    }

    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if(ec) {
      fail(ec, "listen");
      return;
    }
  }

  ~listener() {
    LOGGER_SERVER;
  }

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

    do_accept();
  }
};

//------------------------------------------------------------------------------

game_loop::game_loop(boost::asio::io_context& ioc, tcp::endpoint endpoint)
    : timer_(ioc, boost::asio::chrono::seconds(1)) {
  LOGGER_SERVER;
  listener_ = std::make_shared<listener>(ioc, weak_from_this(), endpoint);
  timer_.async_wait(boost::bind(&game_loop::on_update, this));
}

game_loop::~game_loop() {
  LOGGER_SERVER;
}

void game_loop::add_session(std::shared_ptr<session> s) {
  LOGGER_SERVER;

  static size_t id = 0;
  s->id_ = ++id;
  sessions_[s->id_] = s;

  msg_in_.push_back({s->id_, "/join"});
}

void game_loop::delete_session(std::shared_ptr<session> s) {
  LOGGER_SERVER;
  sessions_.erase(s->id_);
  msg_in_.push_back({s->id_, "/leave"});
}

void game_loop::add_message(std::shared_ptr<session> s, const std::string& msg) {
  LOGGER_SERVER;

  if (msg.empty())
    return;

  msg_in_.push_back({s->id_, msg});
}

void game_loop::send_message(const std::string& msg) {
  LOGGER_SERVER;
}

void game_loop::on_update() {
  LOGGER_SERVER;

  if (!msg_in_.empty()) {

    size_t id = msg_in_.front().first;
    std::string msg = std::move(msg_in_.front().second);

    msg_in_.pop_front();

    action_factory_t action_factory; // TODO
    action_factory.registry(std::make_shared<action_text_t>());
    action_factory.registry(std::make_shared<action_join_t>());
    action_factory.registry(std::make_shared<action_leave_t>());

    auto action = action_factory.from_string(id, msg);
    action->process(shared_from_this());
  }

  timer_.expires_at(timer_.expiry() + boost::asio::chrono::milliseconds(1000));
  timer_.async_wait(boost::bind(&game_loop::on_update, this));
}

void game_loop::run() {
  LOGGER_SERVER;
  listener_->wgl_ = weak_from_this(); // XXX
  listener_->run();
}

//------------------------------------------------------------------------------

bool action_text_t::from_string(size_t id, const std::string& msg) {
  _id = id;
  _msg = msg;
  return true;
}

bool action_text_t::process(std::shared_ptr<game_loop> sgl) {
  std::string msg = "<#" + std::to_string(_id) + "> " + _msg;
  for (auto [id, session] : sgl->sessions_) {
    session->msg_out_.push_back(msg);
    session->do_write();
  }
  return true;
}

std::string action_text_t::name() {
  return "";
}

//------------------------------------------------------------------------------

std::shared_ptr<action_t> action_text_t::prototype() {
  return std::make_shared<action_text_t>();
}

bool action_join_t::from_string(size_t id, const std::string& msg) {
  _id = id;
  return true;
}

bool action_join_t::process(std::shared_ptr<game_loop> sgl) {
  std::string msg = "<server> #" + std::to_string(_id) + " has joined the room";
  for (auto [id, session] : sgl->sessions_) {
    session->msg_out_.push_back(msg);
    session->do_write();
  }
  return true;
}

std::string action_join_t::name() {
  return "/join";
}

std::shared_ptr<action_t> action_join_t::prototype() {
  return std::make_shared<action_join_t>();
}

//------------------------------------------------------------------------------

bool action_leave_t::from_string(size_t id, const std::string& msg) {
  _id = id;
  return true;
}

bool action_leave_t::process(std::shared_ptr<game_loop> sgl) {
  std::string msg = "<server> #" + std::to_string(_id) + " has left the room";
  for (auto [id, session] : sgl->sessions_) {
    session->msg_out_.push_back(msg);
    session->do_write();
  }
  return true;
}

std::string action_leave_t::name() {
  return "/leave";
}

std::shared_ptr<action_t> action_leave_t::prototype() {
  return std::make_shared<action_leave_t>();
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
