// vn.cpp - Virtual Node (VN) of the link-state emulator.
//
// Build: g++ -std=c++17 -Wall -Wextra -O2 -o vn vn.cpp
// Usage: ./vn <oracle-ip> <my-udp-port> <my-ip>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

const int ON_PORT = 5000;       // oracle's TCP port (fixed by the problem)
const int MAX_NODES = 26;       // nodes are named A..Z
const int LSP_PERIOD = 10;      // seconds between periodic LSPs
const int PRINT_PERIOD = 10;    // seconds between graph printouts
const int LSP_COPIES = 3;       // send each LSP this many times, in case UDP drops one

// TCP messages: 3-byte header = type (1) + body length (2), then the body
const uint8_t MSG_CONNECT = 1;
const uint8_t MSG_LINK_STATE = 2;
const int HEADER_SIZE = 3;
const int CONNECT_BODY_SIZE = 6;    // IPv4 (4) + UDP port (2)
const int TUPLE_SIZE = 11;          // letter (1) + IPv4 (4) + port (2) + cost (4)
const int MAX_BODY_SIZE = 1 + MAX_NODES * TUPLE_SIZE;

// UDP LSP: type (1) | origin (1) | seq (4) | count (1) | count x [letter (1) | cost (4)]
const uint8_t MSG_LSP = 3;
const int LSP_HEADER_SIZE = 7;
const int LSP_ENTRY_SIZE = 5;
const int MAX_LSP_SIZE = LSP_HEADER_SIZE + MAX_NODES * LSP_ENTRY_SIZE;

struct Neighbour {
    char id;          // 'A'..'Z'
    uint32_t ip;      // network byte order (copied straight into sin_addr)
    uint16_t port;    // host byte order
    uint32_t cost;
};

struct LinkState {    // latest LSP heard from one origin node
    bool known;
    uint32_t seq;
    int count;
    char ids[MAX_NODES];
    uint32_t costs[MAX_NODES];
};

// ---- global state: fixed-size arrays only, no new/malloc ----
char my_id = '?';             // learnt from the zero-cost tuple of LINK-STATE
uint32_t my_ip;               // network byte order
uint16_t my_port;             // host byte order
uint32_t my_seq = 0;          // sequence number of my last LSP
Neighbour nbrs[MAX_NODES];
int nbr_count = 0;
LinkState db[MAX_NODES];      // db[k] = link state of node 'A' + k
int udp_fd = -1;
int tcp_fd = -1;

// ---------- small helpers ----------

// Write / read integers in network byte order at any position of a buffer.
void put_u16(uint8_t *p, uint16_t v) { v = htons(v); memcpy(p, &v, 2); }
void put_u32(uint8_t *p, uint32_t v) { v = htonl(v); memcpy(p, &v, 4); }
uint16_t get_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return ntohs(v); }
uint32_t get_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return ntohl(v); }

sockaddr_in make_addr(uint32_t ip, uint16_t port) {
    sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;     // already network byte order
    a.sin_port = htons(port);
    return a;
}

// The assignment forbids 127.x.x.x and 0.0.0.0.
bool is_bad_ip(uint32_t ip) {
    return (ntohl(ip) >> 24) == 127 || ip == 0;
}

// Keep calling send() until every byte is out (Beej 7.4).
bool send_all(int fd, const uint8_t *buf, int len) {
    int total = 0;
    while (total < len) {
        int n = send(fd, buf + total, len - total, MSG_NOSIGNAL);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

// Keep calling recv() until exactly len bytes arrived (Beej 7.6).
// Returns false if the other side closed the connection or on error.
bool recv_all(int fd, uint8_t *buf, int len) {
    int total = 0;
    while (total < len) {
        int n = recv(fd, buf + total, len - total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

// ---------- oracle (TCP) side ----------

void oracle_lost() {
    printf("[%c] lost the connection to the oracle\n", my_id);
    close(tcp_fd);
    tcp_fd = -1;                      // stop watching it; keep running LSPs
}

// Read one message from the oracle. Returns true if it was a valid
// LINK-STATE, i.e. my neighbourhood was (re)set.
bool handle_oracle_message() {
    uint8_t hdr[HEADER_SIZE];
    uint8_t body[MAX_BODY_SIZE];
    if (!recv_all(tcp_fd, hdr, HEADER_SIZE)) {
        oracle_lost();
        return false;
    }
    int len = get_u16(hdr + 1);
    if (len < 1 || len > MAX_BODY_SIZE || !recv_all(tcp_fd, body, len)) {
        oracle_lost();
        return false;
    }
    if (hdr[0] != MSG_LINK_STATE || len != 1 + body[0] * TUPLE_SIZE) {
        printf("[%c] ignoring bad message from the oracle\n", my_id);
        return false;
    }

    char new_id = '?';
    nbr_count = 0;
    for (int i = 0; i < body[0]; i++) {
        const uint8_t *t = body + 1 + i * TUPLE_SIZE;
        Neighbour nb;
        nb.id = t[0];
        memcpy(&nb.ip, t + 1, 4);     // stays in network byte order
        nb.port = get_u16(t + 5);
        nb.cost = get_u32(t + 7);
        if (nb.cost == 0) new_id = nb.id;   // the tuple describing me
        else nbrs[nbr_count++] = nb;
    }
    if (new_id < 'A' || new_id > 'Z') {
        printf("[%c] LINK-STATE without a valid own entry, ignored\n", my_id);
        return false;
    }
    my_id = new_id;

    // Required output, e.g. "E=0,C=1,D=7"
    printf("LINK-STATE: %c=0", my_id);
    for (int i = 0; i < nbr_count; i++) printf(",%c=%u", nbrs[i].id, nbrs[i].cost);
    printf("\n");
    return true;
}

// ---------- link-state protocol ----------

// Send a packet to every neighbour.
void flood(const uint8_t *pkt, int len) {
    for (int c = 0; c < LSP_COPIES; c++) {
        for (int i = 0; i < nbr_count; i++) {
            sockaddr_in to = make_addr(nbrs[i].ip, nbrs[i].port);
            sendto(udp_fd, pkt, len, 0, (sockaddr *)&to, sizeof to);
        }
    }
}

// Build a new LSP from my current neighbours, store it, and flood it.
void send_my_lsp() {
    my_seq++;
    LinkState &me = db[my_id - 'A'];
    me.known = true;
    me.seq = my_seq;
    me.count = nbr_count;
    for (int i = 0; i < nbr_count; i++) {
        me.ids[i] = nbrs[i].id;
        me.costs[i] = nbrs[i].cost;
    }

    uint8_t pkt[MAX_LSP_SIZE];
    pkt[0] = MSG_LSP;
    pkt[1] = my_id;
    put_u32(pkt + 2, my_seq);
    pkt[6] = nbr_count;
    for (int i = 0; i < nbr_count; i++) {
        uint8_t *e = pkt + LSP_HEADER_SIZE + i * LSP_ENTRY_SIZE;
        e[0] = nbrs[i].id;
        put_u32(e + 1, nbrs[i].cost);
    }
    flood(pkt, LSP_HEADER_SIZE + nbr_count * LSP_ENTRY_SIZE);
}

// An LSP arrived on the UDP socket.
void handle_lsp() {
    uint8_t pkt[MAX_LSP_SIZE + 1];    // +1 so an oversized packet is noticed
    int n = recvfrom(udp_fd, pkt, sizeof pkt, 0, NULL, NULL);
    if (n < LSP_HEADER_SIZE) return;

    // Anything can arrive on a UDP port: check before using it.
    char origin = pkt[1];
    uint32_t seq = get_u32(pkt + 2);
    int count = pkt[6];
    if (pkt[0] != MSG_LSP || origin < 'A' || origin > 'Z' ||
        n != LSP_HEADER_SIZE + count * LSP_ENTRY_SIZE)
        return;
    for (int i = 0; i < count; i++) {
        char id = pkt[LSP_HEADER_SIZE + i * LSP_ENTRY_SIZE];
        if (id < 'A' || id > 'Z') return;
    }

    // Drop my own LSP and anything not newer than what I already have.
    LinkState &ls = db[origin - 'A'];
    if (origin == my_id || (ls.known && seq <= ls.seq)) return;

    // New information: store it, then pass it on (flooding).
    ls.known = true;
    ls.seq = seq;
    ls.count = count;
    for (int i = 0; i < count; i++) {
        const uint8_t *e = pkt + LSP_HEADER_SIZE + i * LSP_ENTRY_SIZE;
        ls.ids[i] = e[0];
        ls.costs[i] = get_u32(e + 1);
    }
    flood(pkt, n);
}

// ---------- printing the known graph ----------

// True if node j's latest LSP lists node k as a neighbour.
bool reports(int j, int k) {
    if(!db[j].known) return false;
    for(int e=0; e<db[j].count; e++) {
        if(db[j].ids[e] == 'A' + k) return true;
    }
    return false;
}

// Fill g with the graph known from all LSPs (-1 = no link) and return the
// number of nodes to show (1 + highest node letter heard of).
int build_graph(long g[MAX_NODES][MAX_NODES]) {
    for (int i = 0; i < MAX_NODES; i++)
        for (int j = 0; j < MAX_NODES; j++) g[i][j] = -1;
    int n = 0;
    for (int k = 0; k < MAX_NODES; k++) {
        if (!db[k].known) continue;
        if (k + 1 > n) n = k + 1;
        for (int e = 0; e < db[k].count; e++) {
            int j = db[k].ids[e] - 'A';
            if(j + 1 > n) n = j+1;
            if(!reports(j,k)) continue; // two-way check
            g[k][j] = g[j][k] = db[k].costs[e];
            if (j + 1 > n) n = j + 1;
        }
    }
    return n;
}

// Print the upper-right triangle, like the oracle's config file.
void print_graph() {
    long g[MAX_NODES][MAX_NODES];
    int n = build_graph(g);
    printf("---- [%c] known graph (-1 = no link) ----\n   ", my_id);
    for (int j = 1; j < n; j++) printf("%4c", 'A' + j);
    printf("\n");
    for (int i = 0; i < n - 1; i++) {
        printf("%c: ", 'A' + i);
        for (int j = 1; j <= i; j++) printf("    ");
        for (int j = i + 1; j < n; j++) printf("%4ld", g[i][j]);
        printf("\n");
    }
}

// ---------- main ----------

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IOLBF, 0);   // flush every line, even into a file

    // 1. Command-line arguments
    if (argc != 4) {
        fprintf(stderr, "usage: %s <oracle-ip> <my-udp-port> <my-ip>\n", argv[0]);
        return 1;
    }
    in_addr oracle_addr, me_addr;
    int port = atoi(argv[2]);
    if (inet_aton(argv[1], &oracle_addr) == 0 || inet_aton(argv[3], &me_addr) == 0 ||
        is_bad_ip(oracle_addr.s_addr) || is_bad_ip(me_addr.s_addr) ||
        port < 1024 || port > 65535) {
        fprintf(stderr, "error: give real IPs (not 127.x.x.x or 0.0.0.0) and a port in 1024..65535\n");
        return 1;
    }
    my_ip = me_addr.s_addr;
    my_port = port;

    // 2. UDP socket for LSPs, bound to my own IP and port
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in me = make_addr(my_ip, my_port);
    if (udp_fd < 0 || bind(udp_fd, (sockaddr *)&me, sizeof me) < 0) {
        perror("error: cannot bind UDP socket");
        return 1;
    }

    // 3. TCP connection to the oracle, then CONNECT
    sockaddr_in on = make_addr(oracle_addr.s_addr , ON_PORT);
    while(true) {
        tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
        if(tcp_fd < 0) {
            perror("error: cannot create TCP socket");
            return 1;
        }
        if(connect(tcp_fd, (sockaddr *)&on, sizeof on) == 0) break;
        perror("error: cannot connect to the oracle retrying in 2s");
        close(tcp_fd);
        sleep(2);
    }
    uint8_t msg[HEADER_SIZE + CONNECT_BODY_SIZE];
    msg[0] = MSG_CONNECT;
    put_u16(msg + 1, CONNECT_BODY_SIZE);
    memcpy(msg + 3, &my_ip, 4);           // already network byte order
    put_u16(msg + 7, my_port);
    if (!send_all(tcp_fd, msg, sizeof msg)) {
        perror("error: cannot send CONNECT");
        return 1;
    }
    printf("VN %s:%d connected to the oracle\n", argv[3], my_port);

    // 4. Event loop: select() on both sockets with a 1 s timeout
    time_t last_lsp = time(NULL);
    time_t last_print = time(NULL);
    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(udp_fd, &readfds);
        int maxfd = udp_fd;
        if (tcp_fd >= 0) {
            FD_SET(tcp_fd, &readfds);
            if (tcp_fd > maxfd) maxfd = tcp_fd;
        }
        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        if (select(maxfd + 1, &readfds, NULL, NULL, &tv) < 0) {
            perror("select");
            return 1;
        }

        time_t now = time(NULL);
        if (tcp_fd >= 0 && FD_ISSET(tcp_fd, &readfds)) {
            if (handle_oracle_message()) {
                send_my_lsp();            // link state changed: tell everyone
                last_lsp = now;
            }
        }
        if (FD_ISSET(udp_fd, &readfds)) handle_lsp();

        if (my_id != '?' && now - last_lsp >= LSP_PERIOD) {
            send_my_lsp();                // periodic refresh
            last_lsp = now;
        }
        if (my_id != '?' && now - last_print >= PRINT_PERIOD) {
            print_graph();
            last_print = now;
        }
    }
}
