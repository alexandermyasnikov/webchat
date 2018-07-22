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
#include <sstream>
#include <memory>
#include <string>
#include <thread>
#include <list>

#include "logger.h"

using tcp = boost::asio::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>

//------------------------------------------------------------------------------

struct listener_t;
struct session_t;
struct game_loop_t;

//------------------------------------------------------------------------------

struct command_t {
  virtual bool from_string(size_t id, const std::string& msg) = 0;
  virtual bool process(std::shared_ptr<game_loop_t> sgl) = 0;
  virtual std::string name() = 0;
  virtual std::shared_ptr<command_t> prototype() = 0;
};

using action_t = command_t;
using evnent_t = command_t; // TODO

struct action_text_t : action_t {
  size_t _id;
  std::string _msg;

  bool from_string(size_t id, const std::string& msg) override;
  bool process(std::shared_ptr<game_loop_t> sgl) override;
  std::string name() override;
  std::shared_ptr<action_t> prototype() override;
};

struct action_join_t : action_t {
  size_t _id;

  bool from_string(size_t id, const std::string& msg) override;
  bool process(std::shared_ptr<game_loop_t> sgl) override;
  std::string name() override;
  std::shared_ptr<action_t> prototype() override;
};

struct action_leave_t : action_t {
  size_t _id;

  bool from_string(size_t id, const std::string& msg) override;
  bool process(std::shared_ptr<game_loop_t> sgl) override;
  std::string name() override;
  std::shared_ptr<action_t> prototype() override;
};

struct action_get_name_t : action_t {
  size_t _id;
  std::string _name;

  bool from_string(size_t id, const std::string& msg) override;
  bool process(std::shared_ptr<game_loop_t> sgl) override;
  std::string name() override;
  std::shared_ptr<action_t> prototype() override;
};

struct action_set_name_t : action_t {
  size_t _id;
  std::string _name;

  bool from_string(size_t id, const std::string& msg) override;
  bool process(std::shared_ptr<game_loop_t> sgl) override;
  std::string name() override;
  std::shared_ptr<action_t> prototype() override;
};

struct action_help_t : action_t {
  size_t _id;

  bool from_string(size_t id, const std::string& msg) override;
  bool process(std::shared_ptr<game_loop_t> sgl) override;
  std::string name() override;
  std::shared_ptr<action_t> prototype() override;
};

struct action_factory_t {
  std::map<std::string, std::shared_ptr<action_t>> _actions;

  bool registry(std::shared_ptr<action_t> action);
  std::shared_ptr<action_t> from_string(size_t id, const std::string& msg);
};

bool action_factory_t::registry(std::shared_ptr<action_t> action) {
  LOGGER_SERVER;
  _actions[action->name()] = action;
  return true;
}

std::shared_ptr<action_t> action_factory_t::from_string(size_t id, const std::string& msg) {
  LOGGER_SERVER;
  std::shared_ptr<action_t> action_ret;

  std::string name = msg.substr(0, msg.find(' '));
  auto it = _actions.find(name);
  if (it != _actions.end()) {
    action_ret = it->second->prototype();
  } else {
    action_ret = _actions[""]; // TODO default action.
  }

  action_ret->from_string(id, msg);
  return action_ret;
}

//------------------------------------------------------------------------------

struct game_loop_t : std::enable_shared_from_this<game_loop_t> {
  boost::asio::steady_timer                    _timer;
  std::shared_ptr<listener_t>                  _listener;
  std::map<size_t, std::shared_ptr<session_t>> _sessions;
  std::list<std::pair<size_t, std::string>>    _msg_in;
  std::list<std::pair<size_t, std::string>>    _msg_out;

  game_loop_t(boost::asio::io_context& ioc, tcp::endpoint endpoint);
  ~game_loop_t();
  void add_session(std::shared_ptr<session_t> s);
  void delete_session(std::shared_ptr<session_t> s);
  void add_message(std::shared_ptr<session_t> s, const std::string& msg);
  void on_update();
  void run();
};

//------------------------------------------------------------------------------

void fail(boost::system::error_code ec, char const* what) {
  LOGGER_SERVER;
  std::cerr << what << ": " << ec.message() << "\n";
}

struct session_t : public std::enable_shared_from_this<session_t> {
  websocket::stream<tcp::socket> _ws;
  boost::asio::strand<boost::asio::io_context::executor_type> _strand;
  boost::beast::multi_buffer _buffer_in;
  boost::beast::multi_buffer _buffer_out;
  std::weak_ptr<game_loop_t>   _wgl;
  std::list<std::string>     _msg_out;
  size_t                     _id;
  bool                       _write_msg;
  std::string                _name;

 public:
  explicit session_t(std::weak_ptr<game_loop_t> wgl, tcp::socket socket)
      : _ws(std::move(socket)), _strand(_ws.get_executor()), _wgl(wgl), _write_msg(false) {
    LOGGER_SERVER;
  }

  ~session_t() {
    LOGGER_SERVER;
  }

  void run() {
    LOGGER_SERVER;
    _ws.async_accept(
        boost::asio::bind_executor(
          _strand,
          std::bind(
            &session_t::on_accept,
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
    _ws.async_read(
        _buffer_in,
        boost::asio::bind_executor(
          _strand,
          std::bind(
            &session_t::on_read,
            shared_from_this(),
            std::placeholders::_1,
            std::placeholders::_2)));
  }

  void on_read(boost::system::error_code ec, std::size_t bytes_transferred) {
    LOGGER_SERVER;
    boost::ignore_unused(bytes_transferred);

    auto sgl = _wgl.lock();

    if(ec == websocket::error::closed) {
      if (sgl) {
        sgl->delete_session(shared_from_this());
      }
      return;
    }

    if(ec)
      fail(ec, "read");

    if (sgl) {
      std::string msg = boost::beast::buffers_to_string(_buffer_in.data());
      sgl->add_message(shared_from_this(), msg);
    }

    _buffer_in.consume(_buffer_in.size());

    do_read();
  }

  void do_write() {
    LOGGER_SERVER;
    if (!_msg_out.empty() && !_write_msg) {
      _write_msg = true;
      const auto& msg = _msg_out.front();

      _buffer_out.consume(_buffer_out.size());
      boost::beast::ostream(_buffer_out) << msg;

      _ws.text(_ws.got_text());
      _ws.async_write(
          _buffer_out.data(),
          boost::asio::bind_executor(
            _strand,
            std::bind(
              &session_t::on_write,
              shared_from_this(),
              std::placeholders::_1,
              std::placeholders::_2)));

      _msg_out.pop_front();
    }
  }

  void on_write(boost::system::error_code ec, std::size_t bytes_transferred) {
    LOGGER_SERVER;
    boost::ignore_unused(bytes_transferred);
    _write_msg = false;

    if(ec)
      return fail(ec, "write");

    if (!_msg_out.empty()) {
      do_write();
    }
  }
};

//------------------------------------------------------------------------------

struct listener_t : public std::enable_shared_from_this<listener_t> {
  tcp::acceptor              _acceptor;
  tcp::socket                _socket;
  std::weak_ptr<game_loop_t> _wgl;

 public:
  listener_t(boost::asio::io_context& ioc, std::weak_ptr<game_loop_t> wgl, tcp::endpoint endpoint)
      : _acceptor(ioc) , _socket(ioc), _wgl(wgl) {
    LOGGER_SERVER;
    boost::system::error_code ec;

    _acceptor.open(endpoint.protocol(), ec);
    if(ec) {
      fail(ec, "open");
      return;
    }

    _acceptor.set_option(boost::asio::socket_base::reuse_address(true));
    if(ec) {
      fail(ec, "set_option");
      return;
    }

    _acceptor.bind(endpoint, ec);
    if(ec) {
      fail(ec, "bind");
      return;
    }

    _acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
    if(ec) {
      fail(ec, "listen");
      return;
    }
  }

  ~listener_t() {
    LOGGER_SERVER;
  }

  void run() {
    LOGGER_SERVER;
    if(!_acceptor.is_open())
      return;
    do_accept();
  }

  void do_accept() {
    LOGGER_SERVER;
    _acceptor.async_accept(
        _socket,
        std::bind(
          &listener_t::on_accept,
          shared_from_this(),
          std::placeholders::_1));
  }

  void on_accept(boost::system::error_code ec) {
    LOGGER_SERVER;
    if(ec) {
      fail(ec, "accept");
    } else {
      auto sgl = _wgl.lock();
      if (sgl) {
        auto s = std::make_shared<session_t>(_wgl, std::move(_socket));
        s->run();
        sgl->add_session(s);
      } else {
        _socket.close();
      }
    }

    do_accept();
  }
};

//------------------------------------------------------------------------------

game_loop_t::game_loop_t(boost::asio::io_context& ioc, tcp::endpoint endpoint)
    : _timer(ioc, boost::asio::chrono::seconds(1)) {
  LOGGER_SERVER;
  _listener = std::make_shared<listener_t>(ioc, weak_from_this(), endpoint);
  _timer.async_wait(boost::bind(&game_loop_t::on_update, this));
}

game_loop_t::~game_loop_t() {
  LOGGER_SERVER;
}

void game_loop_t::add_session(std::shared_ptr<session_t> s) {
  LOGGER_SERVER;

  static size_t id = 0;
  s->_id = ++id;
  _sessions[s->_id] = s;

  _msg_in.push_back({s->_id, "/join"});
}

void game_loop_t::delete_session(std::shared_ptr<session_t> s) {
  LOGGER_SERVER;
  _msg_in.push_back({s->_id, "/leave"});
}

void game_loop_t::add_message(std::shared_ptr<session_t> s, const std::string& msg) {
  LOGGER_SERVER;

  if (msg.empty())
    return;

  _msg_in.push_back({s->_id, msg});
}

void game_loop_t::on_update() {
  LOGGER_SERVER;

  if (!_msg_in.empty()) {

    size_t id = _msg_in.front().first;
    std::string msg = std::move(_msg_in.front().second);

    _msg_in.pop_front();

    action_factory_t action_factory; // TODO
    action_factory.registry(std::make_shared<action_text_t>());
    action_factory.registry(std::make_shared<action_join_t>());
    action_factory.registry(std::make_shared<action_leave_t>());
    action_factory.registry(std::make_shared<action_get_name_t>());
    action_factory.registry(std::make_shared<action_set_name_t>());
    action_factory.registry(std::make_shared<action_help_t>());

    auto action = action_factory.from_string(id, msg);
    action->process(shared_from_this());
  }

  _timer.expires_at(_timer.expiry() + boost::asio::chrono::milliseconds(1000));
  _timer.async_wait(boost::bind(&game_loop_t::on_update, this));
}

void game_loop_t::run() {
  LOGGER_SERVER;
  _listener->_wgl = weak_from_this(); // XXX
  _listener->run();
}

//------------------------------------------------------------------------------

bool action_text_t::from_string(size_t id, const std::string& msg) {
  LOGGER_SERVER;
  _id = id;
  _msg = msg;
  return true;
}

bool action_text_t::process(std::shared_ptr<game_loop_t> sgl) {
  LOGGER_SERVER;
  const std::string& name = sgl->_sessions[_id]->_name;
  std::string msg = "<#"
      + std::to_string(_id)
      + (!name.empty() ? '(' + name + ')': "")
      + "> "
      + _msg;

  for (auto [id, session] : sgl->_sessions) {
    session->_msg_out.push_back(msg);
    session->do_write();
  }
  return true;
}

std::string action_text_t::name() {
  LOGGER_SERVER;
  return "";
}

//------------------------------------------------------------------------------

std::shared_ptr<action_t> action_text_t::prototype() {
  LOGGER_SERVER;
  return std::make_shared<action_text_t>();
}

bool action_join_t::from_string(size_t id, const std::string& msg) {
  LOGGER_SERVER;
  _id = id;
  return true;
}

bool action_join_t::process(std::shared_ptr<game_loop_t> sgl) {
  LOGGER_SERVER;
  const std::string& name = sgl->_sessions[_id]->_name;
  std::string msg = "<server> #"
      + std::to_string(_id)
      + (!name.empty() ? '(' + name + ')': "")
      + " has joined the room";

  for (auto [id, session] : sgl->_sessions) {
    session->_msg_out.push_back(msg);
    session->do_write();
  }
  return true;
}

std::string action_join_t::name() {
  LOGGER_SERVER;
  return "/join";
}

std::shared_ptr<action_t> action_join_t::prototype() {
  LOGGER_SERVER;
  return std::make_shared<action_join_t>();
}

//------------------------------------------------------------------------------

bool action_leave_t::from_string(size_t id, const std::string& msg) {
  LOGGER_SERVER;
  _id = id;
  return true;
}

bool action_leave_t::process(std::shared_ptr<game_loop_t> sgl) {
  LOGGER_SERVER;
  const std::string& name = sgl->_sessions[_id]->_name;
  std::string msg = "<server> #"
      + std::to_string(_id)
      + (!name.empty() ? '(' + name + ')': "")
      + " has left the room";

  for (auto [id, session] : sgl->_sessions) {
    session->_msg_out.push_back(msg);
    session->do_write();
  }

  sgl->_sessions.erase(_id);

  return true;
}

std::string action_leave_t::name() {
  LOGGER_SERVER;
  return "/leave";
}

std::shared_ptr<action_t> action_leave_t::prototype() {
  LOGGER_SERVER;
  return std::make_shared<action_leave_t>();
}

//------------------------------------------------------------------------------

bool action_get_name_t::from_string(size_t id, const std::string& msg) {
  LOGGER_SERVER;
  _id = id;
  return true;
}

bool action_get_name_t::process(std::shared_ptr<game_loop_t> sgl) {
  LOGGER_SERVER;
  std::string msg = "<server> #" + std::to_string(_id) + " has name \"" + sgl->_sessions[_id]->_name + "\"";

  sgl->_sessions[_id]->_msg_out.push_back(msg);
  sgl->_sessions[_id]->do_write();

  return true;
}

std::string action_get_name_t::name() {
  LOGGER_SERVER;
  return "/get_name";
}

std::shared_ptr<action_t> action_get_name_t::prototype() {
  return std::make_shared<action_get_name_t>();
}

//------------------------------------------------------------------------------

bool action_set_name_t::from_string(size_t id, const std::string& msg) {
  LOGGER_SERVER;
  _id = id;
  std::stringstream ss(msg);
  std::string cmd;
  ss >> cmd >> _name;
  return cmd == name() && !_name.empty();
}

bool action_set_name_t::process(std::shared_ptr<game_loop_t> sgl) {
  LOGGER_SERVER;
  bool is_val_idname = std::all_of(_name.begin(), _name.end(), [](char c) {
    return isdigit(c) || isalpha(c) || c == '_';
  });

  std::string msg;

  if (_name.size() <= 32 && is_val_idname) {
    sgl->_sessions[_id]->_name = _name;
    msg = "<server> #" + std::to_string(_id) + " has new name \"" + _name + "\"";
    for (auto [id, session] : sgl->_sessions) {
      session->_msg_out.push_back(msg);
      session->do_write();
    }
  } else {
    msg = "<server> error: invalid name";
    sgl->_sessions[_id]->_msg_out.push_back(msg);
    sgl->_sessions[_id]->do_write();
  }

  return true;
}

std::string action_set_name_t::name() {
  LOGGER_SERVER;
  return "/set_name";
}

std::shared_ptr<action_t> action_set_name_t::prototype() {
  LOGGER_SERVER;
  return std::make_shared<action_set_name_t>();
}

//------------------------------------------------------------------------------

bool action_help_t::from_string(size_t id, const std::string& msg) {
  LOGGER_SERVER;
  _id = id;
  return true;
}

bool action_help_t::process(std::shared_ptr<game_loop_t> sgl) {
  LOGGER_SERVER;
  std::string msg = "<server> avaivalbe commands: \n"
      "<server> /help                    # Display this help message\n"
      "<server> /get_name                # Get current name \n"
      "<server> /set_name <name>         # Set new name";
  sgl->_sessions[_id]->_msg_out.push_back(msg);
  sgl->_sessions[_id]->do_write();

  return true;
}

std::string action_help_t::name() {
  LOGGER_SERVER;
  return "/help";
}

std::shared_ptr<action_t> action_help_t::prototype() {
  LOGGER_SERVER;
  return std::make_shared<action_help_t>();
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

  auto gl = std::make_shared<game_loop_t>(ioc, tcp::endpoint{address, port});
  gl->run();

  ioc.run();

  return EXIT_SUCCESS;
}
