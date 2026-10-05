#include<stdio.h>
#include<string.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>

#define DEST_PORT 8080
#define DEST_IP "127.0.0.1"

int main() {
    
    int sockfd;
    struct sockaddr_in dest_addr;
    char buf[1024];

    sockfd=socket(AF_INET,SOCK_DGRAM,0);
    dest_addr.sin_family=AF_INET;
    dest_addr.sin_port=htons(DEST_PORT);
    dest_addr.sin_addr.s_addr=inet_addr(DEST_IP);

    while(1) {
        memset(buf, 0, sizeof(buf));
        printf("Enter message to send: ");
        fgets(buf, sizeof(buf), stdin);

        sendto(sockfd, buf, strlen(buf), 0, (struct sockaddr *)&dest_addr, sizeof(struct sockaddr));
    }
    close(sockfd);

    return 0;
}