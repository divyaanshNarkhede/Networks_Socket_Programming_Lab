#!/usr/bin/env python3
# oracle.py - Oracle Node (ON) of the link-state emulator.
#
# Usage: python3 oracle.py <config-file>

import os
import select
import socket
import struct
import sys

ON_PORT = 5000        # TCP port fixed by the problem statement
MAX_NODES = 26        # nodes are named A..Z

# Message types (first byte of every TCP message)
MSG_CONNECT = 1
MSG_LINK_STATE = 2

# Binary layouts. "!" = network byte order (big-endian), no padding.
HEADER = struct.Struct("!BH")         # type (1 byte), body length (2 bytes)
CONNECT_BODY = struct.Struct("!4sH")  # IPv4 address (4 raw bytes), UDP port
TUPLE = struct.Struct("!c4sHI")       # letter, IPv4 address, UDP port, cost


def log(msg):
    print("[ON] " + msg, flush=True)


def letter(i):
    return chr(ord("A") + i)


def find_my_ip():
    # connect() on a UDP socket sends nothing; it only makes the kernel pick
    # the outgoing interface, whose address getsockname() then tells us.
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("8.8.8.8", 80))
    ip = s.getsockname()[0]
    s.close()
    return ip


def parse_config(path):
    """Read the config file and return the full N x N cost matrix.

    cost[i][j] is the link cost between node i and node j, -1 if there is
    no link, and 0 on the diagonal. Raises ValueError if the file is bad.
    """
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()       # drops leading tabs and the newline
            if line == "" or line.startswith("#"):
                continue
            try:
                rows.append([int(tok) for tok in line.split()])
            except ValueError:
                log(f"warning: ignoring non-numeric line {line!r}")

    if len(rows) == 0:
        raise ValueError("no rows of costs found")
    n = len(rows[0]) + 1              # first row has one entry per other node
    if n > MAX_NODES:
        raise ValueError(f"{n} nodes given, at most {MAX_NODES} allowed")
    if len(rows) != n - 1:
        raise ValueError(f"{n} nodes need {n - 1} rows, found {len(rows)}")

    cost = [[-1] * n for _ in range(n)]
    for i in range(n):
        cost[i][i] = 0
    for i, row in enumerate(rows):
        if len(row) != n - 1 - i:
            raise ValueError(f"row of node {letter(i)} needs {n - 1 - i} "
                             f"entries, found {len(row)}")
        for k, c in enumerate(row):
            j = i + 1 + k             # row i starts at column i+1
            if c != -1 and c <= 0:
                raise ValueError(f"cost {letter(i)}-{letter(j)} is {c}, "
                                 f"must be -1 or a positive integer")
            cost[i][j] = c
            cost[j][i] = c            # links are symmetric
    return cost


def recv_exact(sock, n):
    """Receive exactly n bytes (TCP may deliver them in pieces)."""
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:                 # recv() returned 0 bytes: peer closed
            raise ConnectionError("connection closed")
        data += chunk
    return data


class Node:
    """What the ON knows about one virtual node."""

    def __init__(self, sock):
        self.sock = sock      # TCP socket to the VN, None after it disconnects
        self.ip = None        # 4 raw bytes, from the CONNECT message
        self.udp_port = None  # from the CONNECT message


# ---- global state ----
config_path = None
config_mtime = 0.0
cost = []             # N x N matrix from parse_config()
nodes = []            # nodes[0] is A, nodes[1] is B, ... in connection order
started = False       # True once the first N nodes have all sent CONNECT


def send_link_state(i):
    nd = nodes[i]
    if nd.sock is None or nd.ip is None:
        return                        # only to connected nodes that sent CONNECT
    n = len(cost)
    tuples = []
    text = []
    for j, other in enumerate(nodes):
        if other.ip is None:
            continue                  # address of node j not known
        if j == i:
            c = 0                     # the zero-cost tuple tells i who it is
        elif i < n and j < n and cost[i][j] > 0:
            c = cost[i][j]            # j is a neighbour of i
        else:
            continue
        tuples.append(TUPLE.pack(letter(j).encode(), other.ip, other.udp_port, c))
        text.append(f"{letter(j)}={c}")
    body = struct.pack("!B", len(tuples)) + b"".join(tuples)
    try:
        nd.sock.sendall(HEADER.pack(MSG_LINK_STATE, len(body)) + body)
        log(f"LINK-STATE -> {letter(i)}: {','.join(text)}")
    except OSError:
        disconnect(i)


def send_all_link_states():
    for i in range(len(nodes)):
        send_link_state(i)


def accept_new(listener):
    sock, peer = listener.accept()
    if len(nodes) == MAX_NODES:
        log(f"rejecting {peer[0]}:{peer[1]}: already {MAX_NODES} nodes")
        sock.close()
        return
    nodes.append(Node(sock))          # first connection is A, next B, ...
    i = len(nodes) - 1
    extra = " (extra node: disconnected island)" if i >= len(cost) else ""
    log(f"accepted {peer[0]}:{peer[1]} as node {letter(i)}{extra}")


def disconnect(i):
    log(f"node {letter(i)} disconnected")
    nodes[i].sock.close()
    nodes[i].sock = None


def handle_message(i):
    global started
    nd = nodes[i]
    try:
        mtype, length = HEADER.unpack(recv_exact(nd.sock, HEADER.size)) 
        body = recv_exact(nd.sock, length)
    except OSError:                   # closed or reset by the VN
        disconnect(i)
        return
    if mtype != MSG_CONNECT or length != CONNECT_BODY.size:
        log(f"ignoring bad message from node {letter(i)}")
        return

    nd.ip, nd.udp_port = CONNECT_BODY.unpack(body)
    log(f"CONNECT from {letter(i)} = {socket.inet_ntoa(nd.ip)}:{nd.udp_port}")
    n = len(cost)
    if started:
        send_link_state(i)            # an extra node joined late
    elif len(nodes) >= n and all(nodes[k].ip is not None for k in range(n)):
        started = True
        log(f"all {n} nodes connected, sending LINK-STATE messages")
        send_all_link_states()


def check_config():
    global cost, config_mtime
    try:
        mtime = os.path.getmtime(config_path)
    except OSError:
        return                        # file is being replaced by an editor
    if mtime == config_mtime:
        return
    config_mtime = mtime

    try:
        new_cost = parse_config(config_path)
    except (OSError, ValueError) as e:
        log(f"config file changed but is invalid ({e}); keeping old topology")
        return
    if len(new_cost) != len(cost):
        log("config file changed the number of nodes; ignoring it")
        return
    if new_cost == cost:
        return
    log("config file changed")
    cost = new_cost
    if started:
        send_all_link_states()


def main():
    global config_path, config_mtime, cost

    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <config-file>")
        sys.exit(1)
    config_path = sys.argv[1]
    try:
        cost = parse_config(config_path)
        config_mtime = os.path.getmtime(config_path)
    except (OSError, ValueError) as e:
        print(f"error: cannot use config file: {e}")
        sys.exit(1)
    log(f"config has {len(cost)} nodes")

    try:
        my_ip = find_my_ip()
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        # lets a restarted ON bind port 5000 again at once
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((my_ip, ON_PORT))
        listener.listen(MAX_NODES)
    except OSError as e:
        print(f"error: cannot open TCP port {ON_PORT}: {e}")
        sys.exit(1)
    log(f"listening on TCP {my_ip}:{ON_PORT}")

    while True:
        socks = [listener] + [nd.sock for nd in nodes if nd.sock is not None]
        # wait until some socket is readable, or at most 1 second
        readable, _, _ = select.select(socks, [], [], 1.0)
        for s in readable:
            if s is listener:
                accept_new(listener)
            else:
                for i, nd in enumerate(nodes):
                    if nd.sock is s:
                        handle_message(i)
                        break
        check_config()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
