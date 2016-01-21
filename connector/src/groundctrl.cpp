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
#include <fstream>

/* standard unix libraries */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <libgen.h>

/* third party libraries */
#include <ev++.h>

/* our libraries */
#include "autogen.hpp"
#include "bitcoin.hpp"
#include "bitcoin_handler.hpp"
#include "command_handler.hpp"
#include "iobuf.hpp"
#include "netwrap.hpp"
#include "logger.hpp"
#include "network.hpp"
#include "config.hpp"
#include "child_handler.hpp"
#include "main.hpp"

using namespace std;

namespace bc = bitcoin;

map<int, child_handler> g_childmap; /* idx -> child_handler */

void standup_getup_children(char *argv[], const int *which) { /* which is terminated by -1, a list of which children to spin up */
	vector<pair< pid_t, int> > children; /* vector location specifies index */
	const libconfig::Config *cfg(get_config());

	for(;*which >= 0; ++which) {
		assert(*which < (1<<TOM_FD_BITS)); /* currently only support TOM_FD bits in the handle id spread */
		int sv[2];
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
			throw network_error(strerror(errno), errno);
		}
		pid_t pid = fork();
		if (pid < 0) {
			throw network_error(strerror(errno), errno); 
		} else if(pid) { /* in parent */
			// talk to the child over sv[0]
			close(sv[1]);
			g_childmap[ *which ] = child_handler(pid, sv[0]);

		} else { /* in child */
			prctl(PR_SET_PDEATHSIG, SIGKILL);
		
			// We will assume file descriptor GC_FD is the channel for the
			// child, so map sv[1] to be the child descriptor
			if (dup2(sv[1], GC_FD) != GC_FD) {
				throw runtime_error(strerror(errno));
			}
			/* this is cludgy, but who cares */
			string path(dirname(argv[0]));
			path += "/main";
			string config_arg("--configfile="); config_arg += ((const char*)cfg->lookup("version").getSourceFile());
			string daemon_arg("--daemonize=0");
			string inst_arg("--instance=");
			inst_arg += '0' + *which;
			string tom_arg("--tom=1");
			execl(path.c_str(), path.c_str(), config_arg.c_str(), daemon_arg.c_str(), inst_arg.c_str(), tom_arg.c_str(), (char*)NULL);
			throw runtime_error(strerror(errno));
		}
	}
}

/* because we do fork/exec in here, the assumption is that everything
   is CLOEXEC or is appropriately handled in standup/getup children */
int main(int argc, char *argv[]) {


	if (startup_setup(argc, argv) != 0) {
		return EXIT_FAILURE;
	}

	const libconfig::Config *cfg(get_config());
	const char *config_file = cfg->lookup("version").getSourceFile();

	signal(SIGPIPE, SIG_IGN);

	cerr << "Connecting to log server" << endl;

	/* okay, stand up children now. Note: nothing but stdin, stdout,
	   and sterr are open right now. If this changes, cleanup in this
	   function. */
	int which[] = {0,1,-1};
	/* TODO: read WHICH out of netmine.cfg, and reap/watch children exits */
	/* TODO: check if any children are already running and just connect
	   on that channel. Note, you'd have to clear prctl in the
	   following function */
	standup_getup_children(argv,which);


	string root((const char*)cfg->lookup("logger.root"));
	string logpath = root + "servers";
	try {
		g_log_buffer = new log_buffer(unix_sock_client(logpath, true));
	} catch (const network_error &e) {
		cerr << "WARNING: Could not connect to log server! Will reattempt" << e.what() << endl;
	}


	ev::timer logwatch;
	logwatch.set<log_watcher>(&logpath);
	logwatch.set(10.0, 10.0);
	logwatch.start();

	const char *control_filename = cfg->lookup("ground_ctrl.control_path");
	unlink(control_filename);

	struct sockaddr_un control_addr;
	bzero(&control_addr, sizeof(control_addr));
	control_addr.sun_family = AF_UNIX;
	strcpy(control_addr.sun_path, control_filename);

	int control_sock = Socket(AF_UNIX, SOCK_STREAM, 0);
	fcntl(control_sock, F_SETFL, O_NONBLOCK);
	Bind(control_sock, (struct sockaddr*)&control_addr, strlen(control_addr.sun_path) + 
	     sizeof(control_addr.sun_family));
	Listen(control_sock, cfg->lookup("ground_ctrl.control_listen"));

	ev::default_loop loop;


	ctrl::accept_handler ctrl_handler(control_sock);


	{
		ifstream cfile(config_file);
		string s("");
		string line;
		for(string line; getline(cfile,line);) {
			s += line + "\n";
		}
		g_log<GROUND>("Initiating GC with commit: ", commit_hash);
		g_log<GROUND>("Full config: ", s);
		cfile.close();
	}
	
	while(true) {
		/* add timer to attempt recreation of lost control channel */
		loop.run();
	}
	
	g_log<GROUND>("Orderly shutdown");
	return EXIT_SUCCESS;
}
