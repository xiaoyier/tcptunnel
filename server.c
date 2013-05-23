#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "sha1.h"
#include "b64.h"

#define BUFF_LEN 32768

int log(int priority, const char *format, ...){
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}

int calc_ws_protocol_ret(const char *challenge, char *response){
    const char *magic_string = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    int len = strlen(challenge) + strlen(magic_string);
    char input[len + 1];
    sprintf(input, "%s%s", challenge, magic_string);

    uint8_t sha1_digest[SHA1HashSize];
    SHA1Context s;
    SHA1Reset(&s);
    SHA1Input(&s, input, len);
    SHA1Result(&s, sha1_digest);

    b64_encode(sha1_digest, SHA1HashSize, response, 64);

    return 0;
}

int main(int argc, char *argv[]){

    struct sockaddr_in conn_addr;
    conn_addr.sin_family = AF_INET;
    conn_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    conn_addr.sin_port = htons(1080);

    struct sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    listen_addr.sin_port = htons(8278); // TCPT

    int listenfd = -1;

    for(;;){
        int connfd = socket(PF_INET, SOCK_STREAM, 0);
        if(connect(
            connfd,
            (struct sockaddr *)&conn_addr,
            sizeof(conn_addr)
        ) < 0){
            fputs("Cannot connect to remote server\n", stderr);
            return -1;
        }

        if(listenfd == -1){
            listenfd = socket(PF_INET, SOCK_STREAM, 0);
            bind(
                listenfd,
                (const struct sockaddr *)&listen_addr,
                sizeof(listen_addr)
            );
            listen(listenfd, 10);

        }
        struct sockaddr_in sin;
        int len = sizeof(struct sockaddr_in);
        int acceptfd = accept(
            listenfd,
            (struct sockaddr *)&sin,
            &len
        );

        if(!fork()){
            char buffer[BUFF_LEN];
            if(fork()){
                for(;;){
                    int len = recv(connfd, buffer, BUFF_LEN, 0);
                    if(len < 0){
                        log(LOG_NOTICE, "recv from connfd failed");
                        shutdown(connfd,SHUT_RD);
                        shutdown(acceptfd,SHUT_WR);
                    }
                    if(len == 0)
                        break;
                    len = send(acceptfd, buffer, len, 0);
                    if(len < 0){
                        log(LOG_NOTICE, "send to acceptfd failed");
                        shutdown(acceptfd,SHUT_WR);
                        shutdown(connfd,SHUT_RD);
                    }
                }
                shutdown(connfd,SHUT_RD);
                shutdown(acceptfd,SHUT_WR);
            }else{
                for(;;){
                    int len = recv(acceptfd, buffer, BUFF_LEN, 0);
                    if(len < 0){
                        log(LOG_NOTICE, "recv from acceptfd failed");
                        shutdown(acceptfd,SHUT_RD);
                        shutdown(connfd,SHUT_WR);
                    }
                    if(len == 0)
                        break;
                    len = send(connfd, buffer, len, 0);
                    if(len < 0){
                        log(LOG_NOTICE, "send to connfd failed");
                        shutdown(connfd,SHUT_WR);
                        shutdown(acceptfd,SHUT_RD);
                    }
                }
                shutdown(acceptfd,SHUT_RD);
                shutdown(connfd,SHUT_WR);
            }
            return 0;
        }
    }
    return 0;
}

