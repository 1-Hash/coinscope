#include "bitcoin.hpp"

#include <cassert>
#include <cstring>

#include <string>
#include <random>

#include "crypto.hpp"

using namespace std;


namespace bitcoin {

int32_t g_last_block(0);



uint8_t get_varint_size(const uint8_t *bytes) {
	uint8_t rv(0);
	if (bytes[0] < 0xfd) {
		rv = 1;//uint8_t
	} else if (bytes[0] == 0xfd) {
		rv = 3; //uint16_t
	} else if (bytes[0] == 0xfe) {
		rv = 5; //uint32_t
	} else if (bytes[0] == 0xff) {
		rv = 9; //uint64_t
	} else {
		assert(false);
	}
	return rv;
}

void set_address(struct packed_net_addr *dest, struct in_addr src, uint16_t port) {
	dest->time = time(NULL); /* NOTE: this time is uint32_t, elsewhere in the spec uint64_6 */
	dest->services = SERVICES;
	bzero(dest->addr.ipv6.as.bytes, 12);
	dest->addr.ipv4.as.number = src.s_addr;
	dest->port = port;
}

uint64_t get_varint(const uint8_t *buf) {
	uint8_t size = get_varint_size(buf);
	switch(size) {
	case 1:
		return buf[0];
		break;
	case 3:
		return *((uint16_t*) (buf+1));
		break;
	case 5:
		return *((uint32_t*) (buf+1));
		break;
	case 9:
		return *((uint64_t*) (buf+1));
		break;
	default:
		assert(false);
		break;
	}
}

uint8_t to_varint(uint8_t *buf, uint64_t val) {
	uint8_t size;
	if (val < 0xfd) {
		size = 1;
		buf[0] = val;
	} else if (val <= 0xffff) {
		size = 3;
		buf[0] = 0xfd;
		*((uint16_t*) (buf+1)) = val;
	} else if (val <= 0xffffffff) {
		size = 5;
		buf[0] = 0xfe;
		*((uint32_t*) (buf+1)) = val;
	} else {
		size = 9;
		buf[0] = 0xff;
		*((uint64_t*) (buf+1)) = val;
	}
	return size;
}

struct combined_version get_version(const string &user_agent,
                                    struct in_addr from, uint16_t from_port,
                                    struct in_addr recv, uint16_t recv_port) {
	uint8_t buf[9];
	uint8_t varint_size = to_varint(buf, user_agent.length());
	struct combined_version rv(varint_size + user_agent.length());
	rv.version(MAX_VERSION);
	rv.services(SERVICES);
	rv.timestamp(time(NULL));
	set_address(&rv.prefix->recv, recv, recv_port);
	set_address(&rv.prefix->from, from, from_port);
	rv.nonce(nonce_gen64());

	/* copy user agent...gross. Obviously has to have allocation large
	   enough to handle this */
	copy(buf, buf + varint_size, rv.user_agent());
	copy(user_agent.cbegin(), user_agent.cend(), rv.user_agent() + varint_size);

	rv.start_height(g_last_block);
	rv.relay(true);
	return rv;
}

uint32_t compute_checksum(const vector<uint8_t> &payload) { 
	return compute_checksum(payload.data(), payload.size());
}

uint32_t compute_checksum(const uint8_t *payload, size_t len) {
	unique_ptr<unsigned char[]> digest(sha256(sha256(payload, len), 32));
	/* only works on little-endian */
	uint32_t rv = *((uint32_t*) digest.get());
	return rv;
}

unique_ptr<struct packed_message, void(*)(void*)> get_message(const char *command, const vector<uint8_t> &payload) {
	return get_message(command, payload.data(), payload.size());
}

unique_ptr<struct packed_message, void(*)(void*)> get_message(const char *command, const uint8_t *payload, size_t len) {
	/* TODO: special version for zero payload for faster allocation */
	unique_ptr<struct packed_message, void(*)(void*)> rv((struct packed_message *) malloc(sizeof(struct packed_message) + len), free);
	rv->magic = 0xD9B4BEF9;
	bzero(rv->command, sizeof(rv->command));
	strncpy(rv->command, command, sizeof(rv->command));
	rv->length = len;
	if (rv->length) {
		copy(payload, payload + len, rv->payload);
		rv->checksum = compute_checksum(payload, len);
	} else {
		rv->checksum = 0x5df6e0e2;
	}

	return rv;
}
   
};

