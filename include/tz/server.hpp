// Editor server: VM is a single state, editors are clients (§13.1, M5).
//
// 
// 
// Connections - Unix socket, events - stream from io_uring (multishot
// accept/recv, like the network in M3):
//
#pragma once

#include "proto.hpp"
#include "session.hpp"

namespace tz {

// Place of the cursor in projection: node, a piece of text, offset in code points.
struct caret {
  node_id node;
  uint32_t piece;
  uint32_t offset;
};

// Module representation: projection layout and cursor. The cursor is in VM, the
// client only draws.
struct editor_view {
  uint32_t id;
  node_id root;
  std::vector<node_layout> layouts;
  std::vector<caret> places; // cursor
  std::string lead; // the first byte of the symbol in each place ('\n' at the end)
  uint32_t cursor = 0;
};

uint32_t code_points(std::string_view s) {
  uint32_t n = 0;
  for(char c: s)
    if((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
  return n;
}

struct editor_server {
  enum op : uint64_t { op_accept = 1, op_recv, op_send };

  struct peer {
    int fd = -1;
    bool open = true;
    std::string in;
    std::deque<std::string> out;
    std::size_t sent = 0;
    bool sending = false;
    bool recv_armed = false;
    uint32_t handled = 0; // cast
    std::deque<editor_view> views;
  };

  explicit editor_server(vm_session & session) : session(session) {
    if(!ring.ok) throw diagnostic("io_uring недоступен", {});
  }

  void listen(std::string const & path) {
    listen_fd = os::listen_unix(path);
    if(listen_fd < 0) throw diagnostic(cat("сокет ", path, ": ", std::strerror(errno)), {});
    ring.accept_all(listen_fd, tag(op_accept, 0));
  }

  peer & add_peer(int fd) {
    peers.push_back({});
    peers.back().fd = fd;
    arm_recv(peers.size() - 1);
    return peers.back();
  }

  // Wait for the core events and process them.
  void step() {
    for(auto const & c: ring.wait()) complete(c);
  }

  // Everything the customer sent was processed and sent.
  bool settled(peer const & p) const { return !p.sending && p.out.empty(); }

  static uint64_t tag(op o, std::size_t i) { return (uint64_t(o) << 56) | i; }

  void arm_recv(std::size_t i) {
    peers[i].recv_armed = true;
    ring.recv_all(peers[i].fd, tag(op_recv, i));
  }

  void complete(os::uring::completion const & c) {
    if(!c.tag) return;
    op o = op(c.tag >> 56);
    std::size_t i = c.tag & 0xFFFFFFFFFFFFFF;
    bool more = c.flags & IORING_CQE_F_MORE;
    switch(o) {
      case op_accept:
        if(c.res >= 0) add_peer(c.res);
        if(!more) ring.accept_all(listen_fd, tag(op_accept, 0));
        break;
      case op_recv: {
        peer & p = peers[i];
        if(!more) p.recv_armed = false;
        if(c.res > 0) {
          unsigned bid = c.flags >> IORING_CQE_BUFFER_SHIFT;
          p.in.append(ring.buffer(bid), std::size_t(c.res));
          ring.give_back(bid);
          proto::take_frames(p.in, [&](std::string_view body) { on_frame(p, body); });
          if(!more && p.open) arm_recv(i);
        } else if(c.res == -ENOBUFS) {
          if(!more && p.open) arm_recv(i);
        } else if(!more) close_peer(p);
        break;
      }
      case op_send: {
        peer & p = peers[i];
        p.sending = false;
        if(c.res < 0) {
          close_peer(p);
          break;
        }
        p.sent += std::size_t(c.res);
        if(p.sent >= p.out.front().size()) {
          p.out.pop_front();
          p.sent = 0;
        }
        send_next(i);
        break;
      }
    }
  }

  void close_peer(peer & p) {
    if(!p.open) return;
    p.open = false;
    p.out.clear();
    os::close_fd(p.fd);
  }

  void send(peer & p, proto::server_msg const & m) {
    if(!p.open) return;
    p.out.push_back(proto::frame(m));
    send_next(index_of(p));
  }
  std::size_t index_of(peer const & p) const {
    for(std::size_t i = 0; i < peers.size(); ++i)
      if(&peers[i] == &p) return i;
    return 0;
  }
  void send_next(std::size_t i) {
    peer & p = peers[i];
    if(p.sending || p.out.empty() || !p.open) return;
    p.sending = true;
    ring.send(p.fd, p.out.front().data() + p.sent, p.out.front().size() - p.sent, tag(op_send, i));
  }

  void on_frame(peer & p, std::string_view body) {
    ++p.handled;
    proto::client_msg m;
    try {
      proto::decode(body, m);
      std::visit([&](auto const & x) { handle(p, x); }, m);
    } catch(diagnostic const & d) {
      send(p, proto::s_error{d.what()});
    }
  }

  // --- message ---

  void handle(peer & p, proto::c_hello const & m) {
    if(m.version != proto::version) {
      send(p, proto::s_error{cat("версия протокола ", m.version, " не поддерживается (VM: ", proto::version, ")")});
      return;
    }
    send(p, proto::s_hello{proto::version, session.name, session.db.revision});
  }

  void handle(peer & p, proto::c_open_view const & m) {
    if(m.kind != "module") {
      send(p, proto::s_error{cat("неизвестный вид представления: ", m.kind)});
      return;
    }
    editor_view & v = p.views.emplace_back();
    v.id = uint32_t(p.views.size());
    v.root = session.root;
    layout(v);
    proto::s_view out{v.id, v.root, session.db.revision, {}};
    for(auto const & l: v.layouts) out.nodes.push_back({l.node, std::string(kind_name(session.db[l.node])), l.pieces});
    send(p, out);
    send_cursor(p, v);
  }

  void handle(peer & p, proto::c_close_view const & m) {
    if(editor_view * v = find(p, m.view)) v->root = {};
  }

  // The keys interpret the view: for now - the movement of the cursor on the text projection.
  void handle(peer & p, proto::c_key const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    if(m.key == "ArrowRight") {
      if(v->cursor + 1 < v->places.size()) ++v->cursor;
    } else if(m.key == "ArrowLeft") {
      if(v->cursor > 0) --v->cursor;
    } else if(m.key == "Home" || m.key == "End") {
      v->cursor = line_edge(*v, m.key == "End");
    } else return send(p, proto::s_error{cat("клавиша не обработана: ", m.key)});
    send_cursor(p, *v);
  }

  void handle(peer & p, proto::c_pointer const & m) {
    editor_view * v = find(p, m.view);
    if(!v) return send(p, proto::s_error{cat("нет представления ", m.view)});
    // The end of the piece is the place after its last symbol.
    for(uint32_t k = 0; k < v->places.size(); ++k) {
      caret const & c = v->places[k];
      if(c.node != m.node || c.piece != m.piece) continue;
      if(c.offset == m.offset || (m.offset && c.offset + 1 == m.offset && (k + 1 == v->places.size() || v->places[k + 1].node != c.node || v->places[k + 1].piece != c.piece))) {
        v->cursor = c.offset == m.offset ? k : k + 1;
        return send_cursor(p, *v);
      }
    }
    send(p, proto::s_error{cat("нет места #", m.node.value, "/", m.piece, "/", m.offset)});
  }

  editor_view * find(peer & p, uint32_t id) {
    for(auto & v: p.views)
      if(v.id == id && v.root) return &v;
    return nullptr;
  }

  void send_cursor(peer & p, editor_view const & v) {
    caret c = v.places.empty() ? caret{} : v.places[v.cursor];
    send(p, proto::s_cursor{v.id, c.node, c.piece, c.offset});
  }

  // --- layout ---

  void layout(editor_view & v) {
    printer pr(session.db);
    pr.layouts = &v.layouts;
    pr.print(v.root);
    std::vector<uint32_t> at(session.db.size(), ~uint32_t{0});
    for(uint32_t k = 0; k < v.layouts.size(); ++k) at[v.layouts[k].node.value] = k;
    v.places.clear();
    v.lead.clear();
    caret last{};
    walk(v, at, 0, last);
    v.places.push_back(last); // texture
    v.lead += '\n';
    v.cursor = 0;
  }

  // Places - before each code point; at the junction of pieces there is one place
  // (before the next symbol), the end of the piece is not considered a separate place.
  void walk(editor_view & v, std::vector<uint32_t> const & at, uint32_t k, caret & last) {
    node_layout const & l = v.layouts[k];
    for(uint32_t i = 0; i < l.pieces.size(); ++i) {
      if(auto const * t = std::get_if<std::string>(&l.pieces[i])) {
        uint32_t n = code_points(*t);
        for(uint32_t o = 0; o < n; ++o) v.places.push_back({l.node, i, o});
        for(char ch: *t)
          if((static_cast<unsigned char>(ch) & 0xC0) != 0x80) v.lead += ch;
        if(n) last = {l.node, i, n};
      } else walk(v, at, at[std::get<node_id>(l.pieces[i]).value], last);
    }
  }

  uint32_t line_edge(editor_view const & v, bool end) const {
    uint32_t k = v.cursor;
    if(end) {
      while(k + 1 < v.places.size() && v.lead[k] != '\n') ++k;
    } else {
      while(k > 0 && v.lead[k - 1] != '\n') --k;
    }
    return k;
  }

  vm_session & session;
  os::uring ring;
  int listen_fd = -1;
  std::deque<peer> peers; // Deque: Customer links are stable
};

} // namespace tz
