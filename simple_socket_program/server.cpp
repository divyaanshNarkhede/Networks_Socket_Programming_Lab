#include<stdio.h>
#include<string.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>

// #define MYPORT 8080

int main() {
    
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    int addr_len,numbytes;
    char buf[1024];

    int port;
    printf("Enter port number: ");
    scanf("%d", &port);
    getchar();

    sockfd=socket(AF_INET, SOCK_DGRAM, 0);

    server_addr.sin_family=AF_INET;
    server_addr.sin_port=htons(port);
    server_addr.sin_addr.s_addr=INADDR_ANY;
    memset(&(server_addr.sin_zero), '\0', 8);

    bind(sockfd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr));

    printf("Server is waiting\n");

    while(1) {
        addr_len=sizeof(struct sockaddr);
        numbytes=recvfrom(sockfd, buf, 1024-1, 0, (struct sockaddr *)&client_addr, (socklen_t *)&addr_len);

        buf[numbytes]='\0';
        printf("IP Address of Client: %s\n", inet_ntoa(client_addr.sin_addr));
        printf("Received: %s\n", buf);
    }

    close(sockfd);

    return 0;
}
