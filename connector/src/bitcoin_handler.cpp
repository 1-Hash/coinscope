#include "bitcoin_handler.hpp"

#include <cassert>

#include <iostream>
#include <sstream>

#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "netwrap.hpp"
#include "logger.hpp"

using namespace std;

namespace bitcoin {

handler_set g_active_handlers;

uint32_t handler::id_pool = 0;

accept_handler::accept_handler(int fd, struct in_addr a_local_addr, uint16_t a_local_port) 
	: local_addr(a_local_addr), local_port(a_local_port), io()
{
	g_log<DEBUG>("bitcoin accept initializer initiated, awaiting incoming client connections");
	io.set<accept_handler, &accept_handler::io_cb>(this);
	io.set(fd, ev::READ);
	io.start();
}

accept_handler::~accept_handler() {
	g_log<DEBUG>("bitcoin accept handler destroyed");
	io.stop();
	close(io.fd);
	io.fd = -1;
}

void accept_handler::io_cb(ev::io &watcher, int /*revents*/) {
	struct sockaddr_in addr;
	socklen_t len;
	int client;
	try {
		client = Accept(watcher.fd, (struct sockaddr*)&addr, &len);
		fcntl(client, F_SETFL, O_NONBLOCK);		
	} catch (network_error &e) {
		if (e.error_code() != EWOULDBLOCK && e.error_code() != EAGAIN && e.error_code() != EINTR) {
			g_log<ERROR>(e.what(), "(bitcoin_handler)");
			/* trigger destruction of self via some kind of queue and probably recreate channel! */
		}
		return;
	}

	g_log<BITCOIN>("accepted connection to client on fd", client, "at" , *((struct sockaddr*)&addr));

	/* TODO: if can be converted to smarter pointers sensibly, consider, but
	   since libev doesn't use them makes it hard */
	g_active_handlers.emplace(new handler(client, RECV_VERSION_REPLY_HDR, addr.sin_addr, addr.sin_port, local_addr, local_port));
}

handler::handler(int fd, uint32_t a_state, struct in_addr a_remote_addr, uint16_t a_remote_port, in_addr a_local_addr, uint16_t a_local_port) 
	: read_queue(), to_read(0),
	  write_queue(), to_write(0),
	  remote_addr(a_remote_addr), remote_port(a_remote_port),
	  local_addr(a_local_addr), local_port(a_local_port), 
	  state(a_state), 
	  io(),
	  id(id_pool++) 
{
	char local_str[16];
	char remote_str[16];
	inet_ntop(AF_INET, &a_remote_addr, remote_str, sizeof(remote_str));
	inet_ntop(AF_INET, &a_local_addr, local_str, sizeof(local_str));
	ostringstream oss;
	
	oss << "Initiating handler with state " << state << " on " 
	    << local_str << ":" << ntoh(a_local_port) 
	    << " with " << remote_str << ":" << ntoh(a_remote_port) 
	    << " with id " << id << endl;
	g_log<BITCOIN>(oss.str());

	io.set<handler, &handler::io_cb>(this);
	if (a_state == SEND_VERSION_INIT) {
		io.set(fd, ev::WRITE);
		/* TODO: profile to see if extra copies are worth optimizing away */
		struct combined_version vers(get_version(USER_AGENT, local_addr, local_port, remote_addr, remote_port));
		append_for_write(get_message("version", vers.as_buffer(), vers.size));
	} else if (a_state == RECV_VERSION_REPLY_HDR) {
		io.set(fd, ev::READ);
		to_read = sizeof(struct packed_message);
	}
	assert(io.fd > 0);
	io.start();
}


void handler::handle_message_recv(const struct packed_message *msg) { 
	g_log<BITCOIN_MSG>(id, false, msg);
	if (strcmp(msg->command, "ping") == 0) {
		struct packed_message *pong = static_cast<struct packed_message*>(alloca(sizeof(pong) + msg->length));
		memcpy(pong, msg, sizeof(pong) + msg->length);
		pong->command[1] = 'o';
		append_for_write(pong);
		state |= SEND_MESSAGE;
	} else if (strcmp(msg->command, "getblocks") == 0) {
		vector<uint8_t> payload(get_inv(vector<inv_vector>()));
		append_for_write(get_message("inv", payload));
		state |= SEND_MESSAGE;
		to_write += sizeof(struct packed_message) + msg->length;
	}
}

handler::~handler() { 
	if (io.fd >= 0) {
		g_log<BITCOIN>("Shutting down via destructor", id);
		io.stop();
		close(io.fd);
	}
}

void handler::suicide() {
	g_log<BITCOIN>("Shutting down", id);
	io.stop();
	close(io.fd);
	io.fd = -1;
	g_active_handlers.erase(this);
	delete this;
}

size_t handler::append_for_write(const struct packed_message *m) {
	g_log<BITCOIN_MSG>(id, true, m);
	write_queue.append(m);
	to_write += m->length + sizeof(*m);
	return m->length + sizeof(*m);
}

size_t handler::append_for_write(unique_ptr<struct packed_message, void(*)(void*)> m) {
	return append_for_write(m.get());
}

void handler::do_read(ev::io &watcher, int /* revents */) {
	assert(watcher.fd >= 0);
	ssize_t r(1);
	while(r > 0) { /* do all reads we can in this event handler */
		while (r > 0 && to_read > 0) {
			read_queue.grow(read_queue.location() + to_read);
			g_log<DEBUG>("Calling receive for", to_read, "bytes");
			r = recv(watcher.fd, read_queue.offset_buffer(), to_read, 0);
			g_log<DEBUG>("Got",r);
			if (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) { 
				/* 
				   most probably a disconnect of some sort, though I
				   think with reads on a socket this should just come
				   across as a zero byte read, not an error... Anyway,
				   log error and queue object for deletion
				*/
				if (errno == ECONNRESET) {
					g_log<BITCOIN>("Connection reset by peer", id);
				} else {
					g_log<ERROR>("Got unexpected error on handler. ", id, strerror(errno));
				}
				suicide();
				return;

			}
			if (r > 0) {
				to_read -= r;
				read_queue.seek(read_queue.location() + r);
			}
			if (r == 0) { /* got disconnected! */
				/* LOG disconnect */
				g_log<BITCOIN>("Orderly disconnect", id);
				suicide();
				return;
			}
		}

		if (to_read == 0) {
			/* item needs to be handled */
			switch(state & RECV_MASK) {
			case RECV_HEADER:
				/* interpret data as message header and get length, reset remaining */ 
				to_read = ((struct packed_message*) read_queue.raw_buffer())->length;
				if (to_read == 0) { /* payload is packed message */
					handle_message_recv((struct packed_message*) read_queue.raw_buffer());
					read_queue.seek(0);
					to_read = sizeof(struct packed_message);
				} else {
					state = (state & SEND_MASK) | RECV_PAYLOAD;
				}
				break;
			case RECV_PAYLOAD:
				handle_message_recv((struct packed_message*) read_queue.raw_buffer());
				read_queue.seek(0);
				to_read = sizeof(struct packed_message);
				state = (state & SEND_MASK) | RECV_HEADER;
				break;
			case RECV_VERSION_INIT: // we initiated handshake, we expect ack
				// next message should be zero length header with verack command
				g_log<BITCOIN_MSG>(id, false, (struct packed_message*) read_queue.raw_buffer());
				state = (state & SEND_MASK) | RECV_HEADER;
				to_read = sizeof(struct packed_message);
				break;
			case RECV_VERSION_REPLY_HDR: // they initiated the handshake, but we've only read the header
				to_read = ((struct packed_message*) read_queue.raw_buffer())->length;
				state = (state & SEND_MASK) | RECV_VERSION_REPLY;
				break;
			case RECV_VERSION_REPLY: // they initiated handshake, send our version and verack
				g_log<BITCOIN_MSG>(id, false, (struct packed_message*)read_queue.raw_buffer());
				read_queue.seek(0);
				to_read = sizeof(struct packed_message);
					
				struct combined_version vers(get_version(USER_AGENT, remote_addr, remote_port, local_addr, local_port));
				unique_ptr<struct packed_message, void(*)(void*)> vmsg(get_message("version", vers.as_buffer(), vers.size));

				size_t start = write_queue.location();

				write_queue.seek(start + append_for_write(move(vmsg)));
				append_for_write(get_message("verack"));
				write_queue.seek(start);
				state = (state & SEND_MASK) | SEND_VERSION_REPLY | RECV_HEADER;
				break;
			}

		}
	}
         

}

void handler::do_write(ev::io &watcher, int /*revents*/) {

	ssize_t r(1);
	while (to_write && r > 0) { 
		g_log<DEBUG>("Calling write for", to_write, "bytes");
		assert(write_queue.location() + to_write <= write_queue.end());
		r = write(watcher.fd, write_queue.offset_buffer(), to_write);
		g_log<DEBUG>("Got", r);

		if (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) { 
			/* most probably a disconnect of some sort, log error and queue object for deletion */
			g_log<BITCOIN>("Received error on write:", id, strerror(errno));
			suicide();
			return;
		} 
		if (r > 0) {
			to_write -= r;
			write_queue.seek(write_queue.location() + r);
		}
	}

	if (to_write == 0) {
		switch(state & SEND_MASK) {
		case SEND_VERSION_INIT:
			state = RECV_VERSION_INIT;
			break;
		default:
			/* we actually do no special handling here so we can
			   buffer up writes. Beyond SEND_VERSION_INIT, we don't
			   care why we are sending */
			break;
		}

		state &= RECV_MASK;
		write_queue.seek(0);

	}
         

}

void handler::io_cb(ev::io &watcher, int revents) {
	if ((state & RECV_MASK) && (revents & ev::READ)) {
		do_read(watcher, revents);
	}
        
	if (revents & ev::WRITE) {
		do_write(watcher, revents);
		
	}


	int events = 0;
	if (state & SEND_MASK) {
		events |= ev::WRITE;
	}
	if (state & RECV_MASK) {
		events |= ev::READ;
	}
	io.set(events);
}

};

