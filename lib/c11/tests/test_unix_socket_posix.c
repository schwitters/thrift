/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <thrift/c11/transport/thrift_unix_socket.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    result = EXIT_FAILURE; goto cleanup; \
} } while (0)

enum { PATH_CAPACITY = 256, IO_TIMEOUT_MS = 100, PORT_SENTINEL = 42,
       FILE_MODE = 0600 };

static int close_socket(struct thrift_socket **socket_value)
{
    enum thrift_status status = thrift_socket_close(*socket_value);
    *socket_value = NULL;
    return status == THRIFT_OK;
}

int main(void)
{
    char directory[] = "/tmp/thrift-unix-XXXXXX";
    char path[PATH_CAPACITY] = {0}, link_path[PATH_CAPACITY] = {0};
    char long_path[sizeof(((struct sockaddr_un *)0)->sun_path) + 1];
    char maximum_path[sizeof(((struct sockaddr_un *)0)->sun_path)] = {0};
    const char payload[] = "unix\0stream";
    char received[sizeof(payload)] = {0};
    struct thrift_socket *listener = NULL, *client = NULL, *accepted = NULL, *other = NULL;
    struct thrift_transport client_io = {0}, server_io = {0};
    struct stat before, after;
    uint16_t port = PORT_SENTINEL;
    int result = EXIT_SUCCESS, file = -1, count;
    bool directory_created = false;

    CHECK(mkdtemp(directory));
    directory_created = true;
    count = snprintf(path, sizeof(path), "%s/rpc.sock", directory);
    CHECK(count > 0 && (size_t)count < sizeof(path));
    count = snprintf(link_path, sizeof(link_path), "%s/link.sock", directory);
    CHECK(count > 0 && (size_t)count < sizeof(link_path));
    CHECK(thrift_socket_listen_unix(NULL, &other) == THRIFT_INVALID && !other);
    CHECK(thrift_socket_connect_unix("", &other) == THRIFT_INVALID && !other);
    CHECK(thrift_socket_connect_unix(path, NULL) == THRIFT_INVALID);
    CHECK(thrift_socket_listen_unix(path, NULL) == THRIFT_INVALID);
    memset(long_path, 'x', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    CHECK(thrift_socket_listen_unix(long_path, &other) == THRIFT_LIMIT && !other);
    CHECK(thrift_socket_connect_unix(long_path, &other) == THRIFT_LIMIT && !other);
    CHECK(thrift_socket_connect_unix(path, &other) == THRIFT_IO && !other);

    /* Binding never removes a regular file or follows/removes a symlink. */
    file = open(path, O_CREAT | O_EXCL | O_WRONLY, FILE_MODE);
    CHECK(file >= 0);
    CHECK(write(file, payload, sizeof(payload)) == (ssize_t)sizeof(payload));
    CHECK(fstat(file, &before) == 0);
    CHECK(close(file) == 0);
    file = -1;
    CHECK(thrift_socket_listen_unix(path, &other) == THRIFT_IO && !other);
    CHECK(stat(path, &after) == 0 && before.st_ino == after.st_ino && before.st_size == after.st_size);
    CHECK(symlink(path, link_path) == 0);
    CHECK(thrift_socket_listen_unix(link_path, &other) == THRIFT_IO && !other);
    CHECK(lstat(link_path, &after) == 0 && S_ISLNK(after.st_mode));
    CHECK(unlink(link_path) == 0);
    CHECK(unlink(path) == 0);

    CHECK(thrift_socket_listen_unix(path, &listener) == THRIFT_OK);
    CHECK(lstat(path, &before) == 0 && S_ISSOCK(before.st_mode));
    CHECK(thrift_socket_listen_unix(path, &other) == THRIFT_IO && !other);
    CHECK(lstat(path, &after) == 0 && before.st_ino == after.st_ino);
    CHECK(thrift_socket_transport(listener, &server_io) == THRIFT_INVALID);
    CHECK(thrift_socket_port(listener, &port) == THRIFT_INVALID && port == PORT_SENTINEL);
    CHECK(thrift_socket_connect_unix(path, &client) == THRIFT_OK);
    CHECK(thrift_socket_accept(listener, &accepted) == THRIFT_OK);
    CHECK(thrift_socket_port(client, &port) == THRIFT_INVALID && port == PORT_SENTINEL);
    CHECK(thrift_socket_port(accepted, &port) == THRIFT_INVALID && port == PORT_SENTINEL);
    CHECK(thrift_socket_timeout(accepted, IO_TIMEOUT_MS) == THRIFT_OK);
    CHECK(thrift_socket_timeout(client, IO_TIMEOUT_MS) == THRIFT_OK);
    CHECK(thrift_socket_transport(client, &client_io) == THRIFT_OK);
    CHECK(thrift_socket_transport(accepted, &server_io) == THRIFT_OK);
    CHECK(client_io.write(client_io.context, payload, sizeof(payload)) == THRIFT_OK);
    CHECK(server_io.read(server_io.context, received, sizeof(received)) == THRIFT_OK);
    CHECK(!memcmp(payload, received, sizeof(payload)));
    CHECK(server_io.write(server_io.context, received, sizeof(received)) == THRIFT_OK);
    memset(received, 0, sizeof(received));
    CHECK(client_io.read(client_io.context, received, sizeof(received)) == THRIFT_OK);
    CHECK(!memcmp(payload, received, sizeof(payload)));
    CHECK(server_io.read(server_io.context, received, 1) == THRIFT_TIMEOUT);
    CHECK(close_socket(&client));
    CHECK(server_io.read(server_io.context, received, 1) == THRIFT_EOF);
    CHECK(close_socket(&accepted));
    CHECK(lstat(path, &after) == 0 && before.st_ino == after.st_ino);
    CHECK(close_socket(&listener));
    CHECK(lstat(path, &after) == 0 && S_ISSOCK(after.st_mode));
    CHECK(thrift_socket_connect_unix(path, &other) == THRIFT_IO && !other);
    CHECK(thrift_socket_listen_unix(path, &other) == THRIFT_IO && !other);
    CHECK(unlink(path) == 0);
    CHECK(thrift_socket_listen_unix(path, &listener) == THRIFT_OK);
    CHECK(close_socket(&listener));
    CHECK(unlink(path) == 0);

    /* The longest terminated pathname fitting sun_path is accepted exactly. */
    count = snprintf(maximum_path, sizeof(maximum_path), "%s/", directory);
    CHECK(count > 0 && (size_t)count < sizeof(maximum_path) - 1);
    memset(maximum_path + count, 'x', sizeof(maximum_path) - (size_t)count - 1);
    maximum_path[sizeof(maximum_path) - 1] = '\0';
    CHECK(thrift_socket_listen_unix(maximum_path, &listener) == THRIFT_OK);
    CHECK(thrift_socket_connect_unix(maximum_path, &client) == THRIFT_OK);
    CHECK(thrift_socket_accept(listener, &accepted) == THRIFT_OK);
    CHECK(close_socket(&client));
    CHECK(close_socket(&accepted));
    CHECK(close_socket(&listener));
    CHECK(unlink(maximum_path) == 0);
    maximum_path[0] = '\0';
cleanup:
    if (!close_socket(&other)) result = EXIT_FAILURE;
    if (!close_socket(&client)) result = EXIT_FAILURE;
    if (!close_socket(&accepted)) result = EXIT_FAILURE;
    if (!close_socket(&listener)) result = EXIT_FAILURE;
    if (file >= 0 && close(file) != 0)
        result = EXIT_FAILURE;
    if (directory_created) {
        if (path[0] && unlink(path) != 0 && errno != ENOENT)
            result = EXIT_FAILURE;
        if (link_path[0] && unlink(link_path) != 0 && errno != ENOENT)
            result = EXIT_FAILURE;
        if (maximum_path[0] && unlink(maximum_path) != 0 && errno != ENOENT)
            result = EXIT_FAILURE;
        if (rmdir(directory) != 0)
            result = EXIT_FAILURE;
    }
    return result;
}
