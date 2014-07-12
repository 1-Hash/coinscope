/* standard C libraries */
#include <cstdlib>
#include <cstring>
#include <cassert>


/* standard C++ libraries */
#include <vector>
#include <set>
#include <random>
#include <utility>
#include <iostream>

/* standard unix libraries */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

/* third party libraries */
#include <ev++.h>

/* our libraries */
#include "bitcoin.hpp"
#include "bitcoin_handler.hpp"
#include "command_handler.hpp"
#include "iobuf.hpp"
#include "netwrap.hpp"
#include "logger.hpp"
#include "network.hpp"
#include "config.hpp"

using namespace std;

namespace bc = bitcoin;


int main(int argc, const char *argv[]) {

	cout.sync_with_stdio(false);
	cerr.sync_with_stdio(false);
	if (argc == 2) {
		load_config(argv[1]);
	} else {
		load_config("../netmine.cfg");
	}

	const libconfig::Config *cfg(get_config());

	g_log<DEBUG>("Starting up");

	const char *control_filename = cfg->lookup("connector.control_path");
	unlink(control_filename);

	struct sockaddr_un control_addr;
	bzero(&control_addr, sizeof(control_addr));
	control_addr.sun_family = AF_UNIX;
	strcpy(control_addr.sun_path, control_filename);

	int control_sock = Socket(AF_UNIX, SOCK_STREAM, 0);
	fcntl(control_sock, F_SETFL, O_NONBLOCK);
	Bind(control_sock, (struct sockaddr*)&control_addr, strlen(control_addr.sun_path) + 
	     sizeof(control_addr.sun_family));
	Listen(control_sock, cfg->lookup("connector.control_listen"));

	ev::default_loop loop;


	vector<unique_ptr<bc::accept_handler> > bc_accept_handlers; /* form is to get around some move semantics with ev::io I don't want to muck it up */

	libconfig::Setting &list = cfg->lookup("connector.bitcoin.listeners");
	for(int index = 0; index < list.getLength(); ++index) {
		libconfig::Setting &setting = list[index];
		string family((const char*)setting[0]);
		string ipv4((const char*)setting[1]);
		uint16_t port((int)setting[2]);
		int backlog(setting[3]);

		g_log<BITCOIN>("Attempting to instantiate listener on ", family, 
		               ipv4, port, "with backlog", backlog);

		if (family != "AF_INET") {
			g_log<ERROR>("Family", family, "not supported. Skipping");
			continue;
		}

		struct sockaddr_in bitcoin_addr;
		bzero(&bitcoin_addr, sizeof(bitcoin_addr));
		bitcoin_addr.sin_family = AF_INET;
		bitcoin_addr.sin_port = htons(port);
		if (inet_pton(AF_INET, ipv4.c_str(), &bitcoin_addr.sin_addr) != 1) {
			g_log<ERROR>("Bad address format on address", index, strerror(errno));
			continue;
		}

		bitcoin_addr.sin_addr.s_addr = htonl(INADDR_ANY); 
		int bitcoin_sock = Socket(AF_INET, SOCK_STREAM, 0);
		fcntl(bitcoin_sock, F_SETFL, O_NONBLOCK);
		int optval = 1;
		setsockopt(bitcoin_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
		Bind(bitcoin_sock, (struct sockaddr*)&bitcoin_addr, sizeof(bitcoin_addr));
		Listen(bitcoin_sock, backlog);

		bc_accept_handlers.emplace_back(new bc::accept_handler(bitcoin_sock, bitcoin_addr.sin_addr, bitcoin_addr.sin_port));

	}


	ctrl::accept_handler ctrl_handler(control_sock);

	

	string root((const char*)cfg->lookup("logger.root"));
	g_log_buffer = new log_buffer(unix_sock_client(root + "servers", true));

	g_log<DEBUG>("Entering event loop");
	while(true) {
		/* add timer to clean destruction queues */
		/* add timer to attempt recreation of lost control channel */
		/* add timer to recreate lost logging channel */
		loop.run();
	}
	
	g_log<DEBUG>("Orderly shutdown of connector");
	return EXIT_SUCCESS;
}
