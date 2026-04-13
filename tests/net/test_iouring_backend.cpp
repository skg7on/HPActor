#include <hpactor/net/iouring_backend.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#if !defined(__linux__)
int main() { return 0; }
#else
int main() {
    // Test 1: Constructor/destructor
    {
        hpactor::net::IoUringBackend backend;
        // Destructs without crash
    }

    // Test 2: start()
    {
        hpactor::net::IoUringBackend backend;
        bool result = backend.start();
        assert(result && "start() should return true");
        // backend is now running and will stop on destruction
    }

    // Test 3: register_buffer() after start
    {
        hpactor::net::IoUringBackend backend;
        backend.start();
        char buffer[64];
        int buffer_id = backend.register_buffer(buffer, sizeof(buffer));
        assert(buffer_id >= 0 && "register_buffer should return valid buffer_id >= 0");
    }

    // Test 4: unregister_buffer()
    {
        hpactor::net::IoUringBackend backend;
        backend.start();
        char buffer[64];
        int buffer_id = backend.register_buffer(buffer, sizeof(buffer));
        assert(buffer_id >= 0);
        bool result = backend.unregister_buffer(buffer_id);
        assert(!result && "unregister_buffer should return false (liburing doesn't support individual buffer unregistration)");
    }

    // Test 5: add_fd() / update_fd() / remove_fd()
    {
        hpactor::net::IoUringBackend backend;
        backend.start();

        int fds[2];
        int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(r == 0 && "socketpair should succeed");

        bool added = backend.add_fd(fds[0], hpactor::net::IoEvent::Read);
        assert(added && "add_fd should return true");

        bool updated = backend.update_fd(fds[0], hpactor::net::IoEvent::Write);
        assert(updated && "update_fd should return true");

        bool removed = backend.remove_fd(fds[0]);
        assert(removed && "remove_fd should return true");

        ::close(fds[0]);
        ::close(fds[1]);
    }

    // Test 6: run_after()
    {
        hpactor::net::IoUringBackend backend;
        backend.start();

        uint64_t handle = backend.run_after(hpactor::ActorId(1), 100);
        assert(handle > 0 && "run_after should return handle > 0");
    }

    // Test 7: async_accept()
    {
        hpactor::net::IoUringBackend backend;
        backend.start();

        int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(listen_fd >= 0 && "socket should succeed");

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // let kernel assign port

        int r = ::bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
        assert(r == 0 && "bind should succeed");

        socklen_t addrlen = sizeof(addr);
        r = ::getsockname(listen_fd, (struct sockaddr*)&addr, &addrlen);
        assert(r == 0 && "getsockname should succeed");

        r = ::listen(listen_fd, 5);
        assert(r == 0 && "listen should succeed");

        bool added = backend.add_fd(listen_fd, hpactor::net::IoEvent::Read);
        assert(added && "add_fd should succeed for listen socket");

        // Submit async_accept - it should queue without error
        backend.async_accept(listen_fd, hpactor::ActorId(1));

        ::close(listen_fd);
    }

    // Test 8: async_connect()
    {
        hpactor::net::IoUringBackend backend;
        backend.start();

        int fds[2];
        int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(r == 0 && "socketpair should succeed");

        // Submit async_connect on one end of the socketpair
        // Since it's already connected, this will succeed immediately
        backend.async_connect(fds[0], nullptr, 0, hpactor::ActorId(1));

        ::close(fds[0]);
        ::close(fds[1]);
    }

    return 0;
}
#endif
