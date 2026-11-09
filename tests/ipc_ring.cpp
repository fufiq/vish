// IPC ring test (os::ipc_channel): two processes (fork) - order, ring rotation, large messages, time out, end closed,
// occupied roles, alien / corrupt file. Runs check.sh. The argument is the IPC directory.
#include "vish/os.hpp"
#include <cassert>
#include <cstdio>
using namespace vish;
int main(int argc, char ** argv) {
  assert(argc == 2);
  ::setenv("VISH_IPC_DIR", argv[1], 1);
  std::string name = cat("t", ::getpid());
  { // one process, both ends: order, empty, ring rotation
    os::ipc_channel w, r;
    assert(r.open(name, 'r', 256).empty());
    assert(w.open(name, 'w').empty());
    std::string m;
    assert(r.recv(m, 0) == 5); // empty
    assert(w.send("раз", 0) == 0 && w.send("", 0) == 0 && w.send("три", 0) == 0);
    assert(r.recv(m, 0) == 0 && m == "раз");
    assert(r.recv(m, 0) == 0 && m == "");
    assert(r.recv(m, 0) == 0 && m == "три");
    for(int round = 0; round < 1000; ++round) { // 256 byte ring: messages go over the edge
      std::string x(size_t(round % 90), char('a' + round % 26));
      assert(w.send(x, 0) == 0);
      assert(r.recv(m, 0) == 0 && m == x);
    }
    assert(w.send(std::string(253, 'x'), 0) == 7); // 253 + 4 = 257 > 256
    assert(w.send(std::string(252, 'x'), 0) == 0 && r.recv(m, 0) == 0 && m.size() == 252); // ring
    assert(w.send(std::string(300, 'x'), 0) == 7);
    for(int k = 0; k < 6; ++k) w.send(std::string(40, 'q'), 0); // ring
    assert(w.send(std::string(40, 'q'), 30) == 5); // time
    os::ipc_channel w2;
    assert(!w2.open(name, 'w').empty()); // finish
    assert(w.send("x", 0) != 4);
    r.close();
    assert(w.send(std::string(40, 'y'), 0) == 6); // The reader left (was alive) and there is no place: closed
  }
  { // two processes: 200,000 messages in order through the ring 4 KiB, the writer dies - the reader receives 6 after the remainder
    std::string n2 = cat(name, "b");
    os::ipc_channel r;
    assert(r.open(n2, 'r', 4096).empty());
    pid_t pid = ::fork();
    if(pid == 0) {
      os::ipc_channel w;
      if(!w.open(n2, 'w').empty()) ::_exit(2);
      for(int i = 0; i < 200000; ++i) {
        std::string m = cat(i, ":", std::string(size_t(i % 50), 'z'));
        if(w.send(m, 5000) != 0) ::_exit(3);
      }
      ::_exit(0); // ending
    }
    std::string m;
    int got = 0;
    for(;;) {
      int st = r.recv(m, 5000);
      if(st == 6) break;
      assert(st == 0);
      assert(m == cat(got, ":", std::string(size_t(got % 50), 'z')));
      ++got;
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(got == 200000);
    std::string m2;
    assert(r.recv(m2, 50) == 6); // further closed
  }
  { // Writer writes in 100 ms; reader waits up to 3 seconds and wakes up early
    std::string n3 = cat(name, "c");
    os::ipc_channel r;
    assert(r.open(n3, 'r').empty());
    pid_t pid = ::fork();
    if(pid == 0) {
      os::ipc_channel w;
      if(!w.open(n3, 'w').empty()) ::_exit(2);
      ::usleep(100000);
      w.send("привет", 1000);
      ::usleep(100000);
      ::_exit(0);
    }
    uint64_t t0 = os::ipc_channel::now_ms();
    std::string m;
    assert(r.recv(m, 3000) == 0 && m == "привет");
    assert(os::ipc_channel::now_ms() - t0 < 1500);
    ::waitpid(pid, nullptr, 0);
  }
  { // Not a channel: someone else's file by the same name
    std::string n4 = cat(name, "d"), path = cat(argv[1], "/", n4);
    std::FILE * f = std::fopen(path.c_str(), "wb");
    std::string junk(8192, 'j');
    std::fwrite(junk.data(), 1, junk.size(), f);
    std::fclose(f);
    os::ipc_channel r;
    assert(r.open(n4, 'r').empty()); // There is no other end - the contents of the file do not matter: the ring begins again
    os::ipc_channel bad;
    assert(!bad.open("../x", 'r').empty());
    assert(!bad.open("x", 'q').empty());
    assert(!bad.open("y", 'r', 100).empty()); // non-deucity
  }
  std::puts("ipc ok");
}
