from struct import *
import socket
from datetime import *

def inet_ntoa(num):
    # python version wants the num to be packed
    return str(num & 0xff) + '.' + str((num >> 8) & 0xff) + '.' + str((num >> 16) & 0xff) + '.' + str((num >> 24) & 0xff)

def inet_aton(ip):
    i = map(int, ip.split('.'))
    return (i[3] << 24) | (i[2] << 16) | (i[1] << 8) | i[0]

def unix2str(tstamp):
    dt = datetime.fromtimestamp(tstamp)
    return dt.isoformat(' ')
    

class log_types(object):
    DEBUG = 0x2; #/* interpret as a string */
    CTRL = 0x4; #/* control messages */
    ERROR = 0x8; #/* strings */
    BITCOIN = 0x10; #/* general status information (strings) */
    BITCOIN_MSG = 0x20; #/* actual incoming/outgoing messages as encoded */

    str_mapping = {
        0x2 : 'DEBUG',
        0x4 : 'CTRL',
        0x8 : 'ERROR',
        0x10 : 'BITCOIN',
        0x20 : 'BITCOIN_MSG'
    }

class update_types(object):
    CONNECT_SUCCESS = 0x1;# // We initiated the connection 
    ACCEPT_SUCCESS = 0x2;# // They initiated the connection (i.e., the result of an accept)
    ORDERLY_DISCONNECT = 0x4;# // Attempt to read returns 0
    WRITE_DISCONNECT = 0x8;# // Attempt to write returns error, disconnected
    UNEXPECTED_ERROR = 0x10;# // We got some kind of other error, indicating potentially iffy state, so disconnected
    CONNECT_FAILURE = 0x20;# // We initiated a connection, but if failed.
    PEER_RESET = 0x40;# // connection reset by peer
    CONNECTOR_DISCONNECT = 0x80;# // we initiated a disconnect

    str_mapping = {
        0x1 : 'CONNECT_SUCCESS',
        0x2 : 'ACCEPT_SUCCESS',
        0x4 : 'ORDERLY_DISCONNECT',
        0x8 : 'WRITE_DISCONNECT',
        0x10 : 'UNEXPECTED_ERROR',
        0x20 : 'CONNECT_FAILURE',
        0x40 : 'PEER_RESET',
        0x80 : 'CONNECTOR_DISCONNECT',
    }

class log(object):
    def __init__(self, log_type, timestamp, rest):
        self.log_type = log_type;
        self.timestamp = timestamp
        self.rest = rest

    @staticmethod
    def deserialize_parts(serialization):
        log_type, timestamp = unpack('>BQ', serialization[:9])
        rest = serialization[9:]
        return (log_type, timestamp, rest);
        return log(log_type, timestamp, rest)

    @staticmethod
    def deserialize(log_type, timestamp, rest):
        return log(log_type, timestamp, rest)

    def __str__(self):
        return "[{0}] {1}: {2}".format(unix2str(self.timestamp), log_types.str_mapping[self.log_type], self.rest);

class debug_log(log):
    @staticmethod
    def deserialize(timestamp, rest):
        return debug_log(log_types.DEBUG, timestamp, rest)

class ctrl_log(log):
    @staticmethod
    def deserialize(timestamp, rest):
        return ctrl_log(log_types.CTRL, timestamp, rest)


class error_log(log):
    @staticmethod
    def deserialize(timestamp, rest):
        return ctrl_log(log_types.ERROR, timestamp, rest)

class bitcoin_log(log): # bitcoin connection/disconnection events
    def repack(self):
        first = pack('>II', self.handle_id, self.update_type)
        second = pack('=hHI8xhHI8x', socket.AF_INET, self.r_port_, self.r_addr_, socket.AF_INET, self.l_port_, self.l_addr_)
        third = pack('>Is', len(self.text), self.text)
        return first + second + third

    def __init__(self, timestamp, handle_id, update_type, remote_addr, remote_port, local_addr, local_port, text):
        self.handle_id = handle_id
        self.r_addr_ = inet_aton(remote_addr)
        self.r_port_ = socket.htons(remote_port);
        self.l_addr_ = inet_aton(local_addr)
        self.l_port_ = socket.htons(local_port);
        self.update_type = update_type 
        self.text = text
        rest = self.repack()
        super(bitcoin_log, self).__init__(log_types.BITCOIN, timestamp, rest)

    def __str__(self):
        return "[{0}] {1}: handle: {2} update_type: {3}, remote: {4}:{5}, local: {6}:{7}, text: {8}".format(unix2str(self.timestamp), log_types.str_mapping[self.log_type], self.handle_id, update_types.str_mapping[self.update_type], self.remote_addr, self.remote_port, self.local_addr, self.local_port, self.text);

    @staticmethod
    def deserialize(timestamp, rest):
        handle_id, update_type = unpack('>II', rest[:8])
        fam1, r_port_, r_addr_, fam2, l_port_, l_addr_ = unpack('=hHI8xhHI8x', rest[8:40])
        text_len, = unpack('>I', rest[40:44])
        if text_len > 0:
            text, = unpack('>{0}s'.format(text_len), rest[44:])
        else:
            text = ''
        return bitcoin_log(timestamp, handle_id, update_type, 
                           inet_ntoa(r_addr_), socket.ntohs(r_port_), 
                           inet_ntoa(l_addr_), socket.ntohs(l_port_), text)

    @property
    def remote_addr(self):
        return inet_ntoa(self.r_addr_)

    @property
    def remote_port(self):
        return socket.ntohs(self.r_port_)

    @property
    def local_addr(self):
        return inet_ntoa(self.l_addr_)

    @property
    def local_port(self):
        return socket.ntohs(self.l_port_)

class bitcoin_msg_log(log):
    def repack(self):
        return pack('>I?', self.handle_id, self.is_sender) + self.bitcoin_msg

    def __init__(self, timestamp, handle_id, is_sender, bitcoin_msg):
        self.handle_id = handle_id;
        self.is_sender = is_sender
        self.bitcoin_msg = bitcoin_msg
        rest = self.repack()
        super(bitcoin_msg_log, self).__init__(log_types.BITCOIN_MSG, timestamp, rest)

    @staticmethod
    def deserialize(timestamp, rest):
        handle_id, is_sender = unpack('>I?', rest[:5])
        return bitcoin_msg_log(timestamp, handle_id, is_sender, rest[6:])

    def __str__(self):
        return "[{0}] {1}: handle_id: {2}, is_sender: {3}, bitcoin_msg: (ommitted)".format(unix2str(self.timestamp), log_types.str_mapping[self.log_type], self.handle_id, self.is_sender)

type_to_obj = {
    log_types.DEBUG : debug_log,
	log_types.CTRL : ctrl_log,
	log_types.ERROR : error_log,
	log_types.BITCOIN : bitcoin_log,
	log_types.BITCOIN_MSG : bitcoin_msg_log
}
