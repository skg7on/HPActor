// Copyright 2026 HPActor Contributors
#include <hpactor/net/reactor_dispatcher.hpp>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <optional>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    printf("=== ReactorDispatcher Tests ===\n");

    // Test 1: register_fd and unregister_fd basic mapping
    {
        printf("Test 1: register_fd/unregister_fd... ");
        ReactorDispatcher disp;
        disp.register_fd(42, ActorId(1));
        // No direct way to verify map, just check no crash
        disp.unregister_fd(42);
        printf("PASS\n");
    }

    // Test 2: register_recv and has_pending
    {
        printf("Test 2: register_recv and has_pending... ");
        ReactorDispatcher disp;
        char buf[64];
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        disp.register_recv(10, ActorId(5), OpType::Recv, &iov, 1);
        assert(disp.has_pending(10) && "should have pending op");
        disp.unregister_fd(10);
        assert(!disp.has_pending(10) && "pending should be removed");
        printf("PASS\n");
    }

    // Test 3: register_send and has_pending
    {
        printf("Test 3: register_send and has_pending... ");
        ReactorDispatcher disp;
        struct iovec iov;
        char data[] = "hello";
        iov.iov_base = data;
        iov.iov_len = 5;
        disp.register_send(10, ActorId(5), OpType::Send, &iov, 1);
        assert(disp.has_pending(10));
        disp.unregister_io(10);
        assert(!disp.has_pending(10));
        printf("PASS\n");
    }

    // Test 4: on_readiness triggers recv from socketpair
    {
        printf("Test 4: on_readiness triggers recv... ");
        ReactorDispatcher disp;
        std::optional<OpCompletion> captured;
        disp.set_completion_handler([&captured](OpCompletion c) { captured = c; });

        int fds[2];
        int r = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(r == 0);

        char recv_buf[64];
        struct iovec iov;
        iov.iov_base = recv_buf;
        iov.iov_len = sizeof(recv_buf);
        disp.register_recv(fds[0], ActorId(7), OpType::Recv, &iov, 1);

        // Write data to the other end
        const char* msg = "hello reactor";
        ssize_t written = write(fds[1], msg, 14);
        assert(written == 14);

        // Simulate readiness notification
        disp.on_readiness(fds[0], IoEvent::Read);

        assert(captured.has_value() && "completion should be captured");
        assert(captured->type == OpType::Recv && "should be Recv completion");
        assert(captured->actor == ActorId(7) && "actor should match");
        assert(captured->result == 14 && "should have read 14 StreamBuffer");
        assert(memcmp(recv_buf, "hello reactor", 14) == 0 && "data should "
                                                             "match");

        close(fds[0]);
        close(fds[1]);
        printf("PASS\n");
    }

    // Test 5: on_readiness triggers send via socketpair
    {
        printf("Test 5: on_readiness triggers send... ");
        ReactorDispatcher disp;
        std::optional<OpCompletion> captured;
        disp.set_completion_handler([&captured](OpCompletion c) { captured = c; });

        int fds[2];
        int r = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        assert(r == 0);

        struct iovec iov;
        char send_buf[] = "world";
        iov.iov_base = send_buf;
        iov.iov_len = 5;
        disp.register_send(fds[0], ActorId(3), OpType::Send, &iov, 1);

        // Simulate write readiness
        disp.on_readiness(fds[0], IoEvent::Write);

        assert(captured.has_value() && "completion should be captured");
        assert(captured->type == OpType::Send && "should be Send completion");
        assert(captured->actor == ActorId(3) && "actor should match");
        assert(captured->result == 5 && "should have sent 5 StreamBuffer");

        // Verify data arrived on other end
        char verify_buf[16] = {};
        ssize_t n = read(fds[1], verify_buf, sizeof(verify_buf));
        assert(n == 5 && "should read 5 StreamBuffer");
        assert(memcmp(verify_buf, "world", 5) == 0 && "data should match");

        close(fds[0]);
        close(fds[1]);
        printf("PASS\n");
    }

    // Test 6: on_readiness error on closed fd
    {
        printf("Test 6: error on closed fd... ");
        ReactorDispatcher disp;
        std::optional<OpCompletion> captured;
        disp.set_completion_handler([&captured](OpCompletion c) { captured = c; });

        int fds[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

        char recv_buf[64];
        struct iovec iov;
        iov.iov_base = recv_buf;
        iov.iov_len = sizeof(recv_buf);
        disp.register_recv(fds[0], ActorId(1), OpType::Recv, &iov, 1);

        // Close the fd
        close(fds[0]);

        disp.on_readiness(fds[0], IoEvent::Read);

        assert(captured.has_value() && "completion should be captured");
        assert(captured->result < 0 && "should report error for closed fd");
        // EBADF = 9

        close(fds[1]);
        printf("PASS\n");
    }

    // Test 7: on_readiness with no pending op is a no-op
    {
        printf("Test 7: no-op for unregistered fd... ");
        ReactorDispatcher disp;
        bool called = false;
        disp.set_completion_handler([&called](OpCompletion) { called = true; });

        disp.on_readiness(99, IoEvent::Read);
        assert(!called && "handler should not be called for unknown fd");
        printf("PASS\n");
    }

    // Test 8: unregister_io removes pending op, leaves fd mapping
    {
        printf("Test 8: unregister_io... ");
        ReactorDispatcher disp;
        char buf[64];
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        disp.register_recv(20, ActorId(2), OpType::Recv, &iov, 1);
        assert(disp.has_pending(20) && "should have pending op");
        disp.unregister_io(20);
        assert(!disp.has_pending(20) && "should not have pending op");
        // fd should still be in base map (unregister_io doesn't remove fd)
        printf("PASS\n");
    }

    // Test 9: accept readiness
    {
        printf("Test 9: accept readiness... ");
        ReactorDispatcher disp;
        std::optional<OpCompletion> captured;
        disp.set_completion_handler([&captured](OpCompletion c) { captured = c; });

        // Create a listening socket
        int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        assert(listen_fd >= 0);

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path),
                 "/tmp/hpactor_test_accept_%ld", static_cast<long>(getpid()));
        unlink(addr.sun_path);

        int r = bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr),
                     sizeof(addr));
        if (r != 0 && (errno == EPERM || errno == EACCES)) {
            close(listen_fd);
            unlink(addr.sun_path);
            printf("SKIP (AF_UNIX bind not permitted)\n");
        } else {
            if (r != 0) {
                perror("bind");
            }
            assert(r == 0 && "bind should succeed");

            r = listen(listen_fd, 1);
            assert(r == 0 && "listen should succeed");

            disp.register_accept(listen_fd, ActorId(10));

            // Connect a client to trigger readiness
            int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            assert(client_fd >= 0);
            connect(client_fd, reinterpret_cast<struct sockaddr*>(&addr),
                    sizeof(addr));

            // Wait briefly for connection to establish
            usleep(50000);

            // Trigger readiness
            disp.on_readiness(listen_fd, IoEvent::Read);

            assert(captured.has_value() && "accept completion should be "
                                           "captured");
            assert(captured->type == OpType::Accept && "should be Accept "
                                                       "completion");
            assert(captured->result >= 0 && "accept should succeed");

            if (captured->result >= 0) {
                close(captured->result); // close accepted fd
            }
            close(client_fd);
            close(listen_fd);
            unlink(addr.sun_path);
            printf("PASS\n");
        }
    }

    // Test 10: register_sendto and on_readiness
    {
        printf("Test 10: register_sendto... ");
        ReactorDispatcher disp;
        std::optional<OpCompletion> captured;
        disp.set_completion_handler([&captured](OpCompletion c) { captured = c; });

        int fds[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

        struct iovec iov;
        char data[] = "sendto test";
        iov.iov_base = data;
        iov.iov_len = 10;

        struct sockaddr_un dummy;
        memset(&dummy, 0, sizeof(dummy));
        dummy.sun_family = AF_UNIX;

        disp.register_sendto(fds[0], ActorId(4), OpType::SendTo, &iov, 1,
                             reinterpret_cast<struct sockaddr*>(&dummy),
                             sizeof(dummy));

        disp.on_readiness(fds[0], IoEvent::Write);

        assert(captured.has_value() && "completion should be captured");
        assert(captured->type == OpType::SendTo && "should be SendTo");
        assert(captured->result == 10 && "should send 10 StreamBuffer");

        close(fds[0]);
        close(fds[1]);
        printf("PASS\n");
    }

    // Test 11: register_accept followed by unregister_io prevents delivery
    {
        printf("Test 11: unregister_io cancels pending accept... ");
        ReactorDispatcher disp;
        bool called = false;
        disp.set_completion_handler([&called](OpCompletion) { called = true; });

        disp.register_accept(50, ActorId(99));
        disp.unregister_io(50);
        disp.on_readiness(50, IoEvent::Read);
        assert(!called && "handler should not be called after unregister_io");
        printf("PASS\n");
    }

    printf("=== All ReactorDispatcher Tests Passed ===\n");
    return 0;
}
