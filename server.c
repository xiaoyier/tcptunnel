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


            // Wait for websocket HTTP Handshake
            int len = recv(connfd, buffer, BUFF_LEN, 0);
            if(len < 0){
                log(LOG_NOTICE, "recv from connfd failed");
                shutdown(connfd,SHUT_RDWR);
                shutdown(acceptfd,SHUT_RDWR);
            }
            log(LOG_DEBUG, "Got http handshake packet");

            // Parse the request
            const char *bad_req = 
                "HTTP/1.1 400 Bad Request\r\n"
                "\r\n";

            const char *not_found = 
                "HTTP/1.1 404 Not Found\r\n"
                "\r\n";

            int ws_checker = 0;
            char ws_protocol_ret[64] = "";

            for(char *p = buffer; ; p=NULL){
                char *line = strtok(p, "\n");
                if(line[0] == '\r'){
                    log(LOG_DEBUG, "reach http eoh");
                    break;
                }
                if(line[0] == '\0'){
                    log(LOG_INFO, "reached EOP, but no http EOH found");
                        send(acceptfd, bad_req, strlen(bad_req), 0);
                        shutdown(connfd,SHUT_RDWR);
                        shutdown(acceptfd,SHUT_RDWR);
                        return -1;
                    }
                }
                line[strlen(line)-2] = '\0'; // trim trailing '\r'

                if(p){
                    // We are at the first line
                    // Simply ignore it
                }else{
                    // optional headers
                    char *key   = strtok(line, ":");
                    char *value = strtok(NULL, ":");
                    // strip the leading spaces
                    while(*value == " ")
                        value++;

                    if(!key || !value){
                        log(LOG_ERR, "malformed optional header");
                        send(acceptfd, bad_req, strlen(bad_req), 0);
                        shutdown(connfd,SHUT_RDWR);
                        shutdown(acceptfd,SHUT_RDWR);
                    }

#define match(s) if(!strcasecmp(key,(s)))
                    match("Connection"){
                        if(strcasecmp(value, "upgrade")){
                            log(LOG_ERR, "Connection is not upgrade");
                            send(acceptfd, bad_req, strlen(bad_req), 0);
                            shutdown(connfd,SHUT_RDWR);
                            shutdown(acceptfd,SHUT_RDWR);
                            return -1;
                        }
                        ws_checker++;
                    }
                    match("Upgrade"){
                        if(strcasecmp(value, "websocket")){
                            log(LOG_ERR, "Upgrade is not websocket");
                            send(acceptfd, bad_req, strlen(bad_req), 0);
                            shutdown(connfd,SHUT_RDWR);
                            shutdown(acceptfd,SHUT_RDWR);
                            return -1;
                        }
                        ws_checker++;
                    }
                    match("Sec-WebSocket-Version"){
                        if(strcmp(value, "13")){
                            log(LOG_ERR, "Sec-WebSocket-Version is not 13");
                            send(acceptfd, bad_req, strlen(bad_req), 0);
                            shutdown(connfd,SHUT_RDWR);
                            shutdown(acceptfd,SHUT_RDWR);
                            return -1;
                        }
                        ws_checker++;
                    }
                    match("Sec-WebSocket-Protocol"){
                        if(!calc_ws_protocol_ret(value, ws_protocol_ret)){
                            log(LOG_ERR, "Malformed Sec-WebSocket-Protocol");
                            send(acceptfd, bad_req, strlen(bad_req), 0);
                            shutdown(connfd,SHUT_RDWR);
                            shutdown(acceptfd,SHUT_RDWR);
                            return -1;
                        }
                        ws_checker++;
                    }
#undef match
                }

            
            }
            if(!iswebsocket){
                log(LOG_INFO, "http request, send 404");
                send(acceptfd, bad_req, strlen(not_found), 0);
                shutdown(connfd,SHUT_RDWR);
                shutdown(acceptfd,SHUT_RDWR);
                return -1;
            }
            if(ws_checker != 4){
                log(LOG_ERR, "websocket protocol missing mandatory headers");
                send(acceptfd, bad_req, strlen(bad_req), 0);
                shutdown(connfd,SHUT_RDWR);
                shutdown(acceptfd,SHUT_RDWR);
                return -1;
            }
            log(LOG_DEBUG, "Finish parsing handshake headers, write response");

            // Form handshake response
            const char *ret =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Sec-WebSocket-Protocol: chat\r\n"
                "Sec-WebSocket-Accept: %s\r\n"
                "\r\n";

            len = snprintf(buffer, BUFF_LEN, ret, ws_protocol_ret);
            len = send(acceptfd, buffer, len, 0);
            if(len < 0){
                log(LOG_NOTICE, "send to connfd failed");
                shutdown(connfd,SHUT_RDWR);
                shutdown(acceptfd,SHUT_RDWR);
                return -1;
            }
            log(LOG_DEBUG, "sent handshake response, entering full duplex");

            if(fork()){
                log(LOG_DEBUG, "read from connfd process started");
                for(;;){
                    len = recv(connfd, buffer, BUFF_LEN, 0);
                    if(len < 0){
                        log(LOG_NOTICE, "recv from connfd failed");
                        shutdown(connfd,SHUT_RD);
                        shutdown(acceptfd,SHUT_WR);
                        return -1;
                    }
                    if(len == 0)
                        break;
                    len = send(acceptfd, buffer, len, 0);
                    if(len < 0){
                        log(LOG_NOTICE, "send to acceptfd failed");
                        shutdown(acceptfd,SHUT_WR);
                        shutdown(connfd,SHUT_RD);
                        return -1;
                    }
                }
                shutdown(connfd,SHUT_RD);
                shutdown(acceptfd,SHUT_WR);
            }else{
                log(LOG_DEBUG, "write to connfd process started");
                for(;;){
                    len = recv(acceptfd, buffer, BUFF_LEN, 0);
                    if(len < 0){
                        log(LOG_NOTICE, "recv from acceptfd failed");
                        shutdown(acceptfd,SHUT_RD);
                        shutdown(connfd,SHUT_WR);
                        return -1;
                    }
                    if(len == 0)
                        break;
                    len = send(connfd, buffer, len, 0);
                    if(len < 0){
                        log(LOG_NOTICE, "send to connfd failed");
                        shutdown(connfd,SHUT_WR);
                        shutdown(acceptfd,SHUT_RD);
                        return -1;
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

