#include <cstring>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>

#include "network.hpp"
#include "netwrap.hpp"

int unix_sock_setup(const char *path, struct sockaddr_un *addr, bool nonblocking) {
	bzero(addr, sizeof(*addr));
	addr->sun_family = AF_UNIX;
	strcpy(addr->sun_path, path);
	int sock = Socket(AF_UNIX, SOCK_STREAM, 0);
	if (nonblocking) {
		fcntl(sock, F_SETFD, O_NONBLOCK);
	}
	return sock;
}

int unix_sock_server(const char *path, int listen, bool nonblocking) {
	unlink(path);

	struct sockaddr_un addr;
	int sock = unix_sock_setup(path, &addr, nonblocking);
	Bind(sock, (struct sockaddr*)&addr, strlen(addr.sun_path) + 
	     sizeof(addr.sun_family));
	Listen(sock, listen);
	return sock;
}

int unix_sock_client(const char *path, bool nonblocking) {
	sockaddr_un addr;
	int sock = unix_sock_setup(path, &addr, nonblocking);
	Connect(sock, (struct sockaddr *)&addr, strlen(addr.sun_path) + sizeof(addr.sun_family));
	return sock;
}
