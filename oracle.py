import socket
import select
import sys
import os

ON_PORT = 5000
MAX_NODES = 26
POLL_TIME = 1.0

def letter(i):
    return chr(ord("A") + i)

def log(msg):
    print(f"[ON] {msg}", flush=True)

def find_my_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("8.8.8.8", 80))
    ip = s.getsockname()[0]
    s.close()
    return ip

def parse_config_file(path):
    ## Reads config file and returns the N x N matrix
    rows = []
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip() ## drops leading tab and newline
            if line == "" or line.startswith("#"):
                continue
            try:
                rows.append([int(val) for val in line.split()])
            except ValueError:
                log(f"warning: {path}:{lineno}: ignoring non-numeric line {line!r}")
            rows.append([int(val) for val in line.split()])

    if len(rows) == 0:
        raise ValueError("no rows of costs found")

    n = len(rows[0]) + 1
    if n > MAX_NODES:
        raise ValueError(f"{n} nodes given, atmost {MAX_NODES} allowed")

    if len(rows) != n - 1:
        raise ValueError(f"{n} nodes need {n - 1} rows, found {len(rows)}")

    cost = [[-1] * n for _ in range(n)]
    for i in range(n):
        cost[i][i] = 0

    for i, row in enumerate(rows):
        if len(row) != n - 1 - i:
            raise ValueError(f"row of node {letter(i)} needs {n - i - 1} entries, found {len(row)}")

        for k, c in enumerate(row):
            j = i + k + 1
            if c != -1 and c <= 0:
                raise ValueError(f":cost {letter(i)}-{letter(j)} is {c}, must be -1 or a positive integer")

            cost[i][j] = c
            cost[j][i] = c

    return cost

class Node:
    ## What ON knows about VN
    def __init__(self, sock):
        self.sock = sock
        self.ip = None
        self.udp_port = None

config_path = None
config_mtime = 0.0
cost = []
nodes = []
started = False

def accept_new(sock):
    new_sock, addr = sock.accept()
    if len(nodes) >= MAX_NODES:
        log(f"refusing connection from {addr[0]}:{addr[1]}: max nodes reached")
        new_sock.close()
        return
    nodes.append(Node(new_sock))
    log(f"accepted connection from {addr[0]}:{addr[1]}")

def main():
    global config_path, config_mtime, cost

    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <config-file>")
        sys.exit(1)

    config_path = sys.argv[1]
    try:
        cost = parse_config_file(config_path)
        config_mtime = os.path.getmtime(config_path)
    except (OSError, ValueError) as e:
        print(f"error: {e}")
        sys.exit(1)
    log(f"config has {len(cost)} nodes")

    try:
        my_ip = find_my_ip()
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((my_ip, ON_PORT))
        sock.listen(MAX_NODES)
    except OSError as e:
        print(f"error: {e}")
        sys.exit(1)

    log(f"listening on TCP {my_ip}:{ON_PORT}")

    while True:
        socks = [sock] + [nd.sock for nd in nodes if nd.sock is not None]
        readable, _, _ = select.select(socks, [], [], POLL_TIME)
        for s in readable:
            if s is sock:
                accept_new(sock)
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