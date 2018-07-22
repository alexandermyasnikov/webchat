
// TODO Убрать "area.value +="

function websocket_new(out) {
  this.websocket = null;
  this.out = out;

  this.create = function(ip_port) {
    this.websocket = new WebSocket("ws://" + ip_port);
    this.websocket.onmessage = function(event) {
      area.value += (event.data + " \n");
    };
    this.websocket.onopen = function(event) {
      area.value += ("<websocket> socket opened \n");
    };
    this.websocket.onclose = function(event) {
      area.value += ("<websocket> socket closed" + (!event.wasClean ? " (not clean closing)" : "") + " \n");
    };
    this.websocket.onerror = function(error) {
      area.value += ("<websocket> error: \"" + error.message + "\" \n");
    };
  }

  this.open = function(ip_port) {
    this.close();
    this.create(ip_port);
    this.log("<websocket> socket opening \n");
  }

  this.close = function() {
    if (this.websocket) {
      this.websocket.close();
      this.log("<websocket> socket closing \n");
    }
  }

  this.send_message = function(msg) {
    this.websocket.send(msg.toString());
    this.log("<websocket> sent message \"" + msg + "\" \n");
  }

  this.state = function() {
    let st = "";
    switch (this.websocket.readyState) {
      case 0: st = "CONNECTING"; break;
      case 1: st = "OPEN";       break;
      case 2: st = "CLOSING";    break;
      case 3: st = "CLOSED";     break;
    }
    this.log("<websocket> socket has status \"" + st + "\" \n");
  }

  this.log = function(str) {
    document.getElementById(this.out).value += str;
  }

}
