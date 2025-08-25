#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ifaddrs.h>
#include <net/if.h>

#define PORT 2005
#define BUFFER_SIZE 1024
#define PASV_PORT_MIN 50000
#define PASV_PORT_MAX 51000

void send_response(int sock, const char* msg);
void handle_client(int client_sock);
int open_pasv_socket(int *port_out);
int get_local_ip(char *ip_buffer, size_t buflen);
void list_files(int data_sock);

int main() {
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    if (chdir("share") != 0) {
        perror("Failed to change to 'share' directory");
        return 1;
    }

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        return 1;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        return 1;
    }

    if (listen(server_sock, 5) < 0) {
        perror("Listen failed");
        return 1;
    }

    printf("Simple FTP server listening on port %d...\n", PORT);

    while (1) {
        client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock < 0) {
            perror("Accept failed");
            continue;
        }
        printf("Client connected\n");
        handle_client(client_sock);
        close(client_sock);
        printf("Client disconnected\n");
    }

    close(server_sock);
    return 0;
}

void send_response(int sock, const char* msg) {
    send(sock, msg, strlen(msg), 0);
}

/* 
int get_local_ip(char *ip_buffer, size_t buflen) {
    strncpy(ip_buffer, "127.0.0.1", buflen);
    return 0;
}
*/

// Get the local LAN IP address (non-loopback)
int get_local_ip(char *ip_buffer, size_t buflen) {
    struct ifaddrs *ifaddr, *ifa;
    int family;

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        family = ifa->ifa_addr->sa_family;

        // Only look at IPv4 addresses
        if (family == AF_INET &&
            (ifa->ifa_flags & IFF_LOOPBACK) == 0) { // Exclude loopback
            if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                            ip_buffer, buflen,
                            NULL, 0, NI_NUMERICHOST) == 0) {
                freeifaddrs(ifaddr);
                return 0;
            }
        }
    }

    freeifaddrs(ifaddr);
    return -1; // Fallback to default if none found
}

int open_pasv_socket(int *port_out) {
    int pasv_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (pasv_sock < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(pasv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in pasv_addr;
    pasv_addr.sin_family = AF_INET;
    pasv_addr.sin_addr.s_addr = INADDR_ANY;
    pasv_addr.sin_port = 0; // Let OS pick port

    if (bind(pasv_sock, (struct sockaddr*)&pasv_addr, sizeof(pasv_addr)) < 0) {
        perror("bind");
        close(pasv_sock);
        return -1;
    }

    if (listen(pasv_sock, 1) < 0) {
        perror("listen");
        close(pasv_sock);
        return -1;
    }

    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);
    if (getsockname(pasv_sock, (struct sockaddr*)&sin, &len) < 0) {
        perror("getsockname");
        close(pasv_sock);
        return -1;
    }
    *port_out = ntohs(sin.sin_port);
    printf("Passive mode listening on port %d\n", *port_out);

    return pasv_sock;
}

void list_files(int data_sock) {
    DIR *d = opendir(".");
    struct dirent *dir;
    struct stat st;
    char buf[BUFFER_SIZE];

    if (!d) {
        perror("opendir");
        return;
    }

    while ((dir = readdir(d)) != NULL) {
        if (stat(dir->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(buf, sizeof(buf), "-rw-r--r-- 1 user group %ld Jul 19 12:00 %s\r\n",
                     (long)st.st_size, dir->d_name);
            send(data_sock, buf, strlen(buf), 0);
        }
    }
    closedir(d);
}

void handle_client(int client_sock) {
    char buffer[BUFFER_SIZE];
    int bytes_read;
    int pasv_listen_sock = -1;
    int pasv_data_sock = -1;
    char local_ip[INET_ADDRSTRLEN];

    if (get_local_ip(local_ip, sizeof(local_ip)) != 0) {
        fprintf(stderr, "Failed to get local IP, using 127.0.0.1\n");
        strncpy(local_ip, "127.0.0.1", sizeof(local_ip));
    }

    send_response(client_sock, "220 Simple FTP Ready\r\n");

    while ((bytes_read = recv(client_sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_read] = '\0'; 
        printf("Received: %s", buffer);

        if (strncmp(buffer, "USER", 4) == 0) {
            send_response(client_sock, "331 Username OK, need password\r\n");
        } else if (strncmp(buffer, "PASS", 4) == 0) {
            send_response(client_sock, "230 User logged in\r\n");
        } else if (strncmp(buffer, "QUIT", 4) == 0) {
            send_response(client_sock, "221 Goodbye\r\n");
            break;
        } else if (strncmp(buffer, "SYST", 4) == 0) {
            send_response(client_sock, "215 UNIX Type: L8\r\n");
        } else if (strncmp(buffer, "PWD", 3) == 0) {
            send_response(client_sock, "257 \"/\" is the current directory\r\n");
        } else if (strncmp(buffer, "TYPE", 4) == 0) {
            send_response(client_sock, "200 Type set to I.\r\n");
        } else if (strncmp(buffer, "CWD", 3) == 0 || strncmp(buffer, "CDUP", 4) == 0) {
            send_response(client_sock, "250 Directory change OK\r\n");
        } else if (strncmp(buffer, "PASV", 4) == 0) {
            if (pasv_listen_sock != -1) close(pasv_listen_sock);
            int port;
            pasv_listen_sock = open_pasv_socket(&port);
            if (pasv_listen_sock < 0) {
                send_response(client_sock, "425 Can't open data connection\r\n");
                continue;
            }
            // Build PASV response with local IP and port
            unsigned int ip1, ip2, ip3, ip4;
            sscanf(local_ip, "%u.%u.%u.%u", &ip1, &ip2, &ip3, &ip4);
            int p1 = port / 256;
            int p2 = port % 256;

            char msg[100];
            snprintf(msg, sizeof(msg),
                "227 Entering Passive Mode (%u,%u,%u,%u,%d,%d).\r\n",
                ip1, ip2, ip3, ip4, p1, p2);
            send_response(client_sock, msg);

        } else if (strncmp(buffer, "LIST", 4) == 0) {
            if (pasv_listen_sock == -1) {
                send_response(client_sock, "425 Use PASV first.\r\n");
                continue;
            }
            send_response(client_sock, "150 Opening ASCII mode data connection for file list\r\n");
            pasv_data_sock = accept(pasv_listen_sock, NULL, NULL);
            if (pasv_data_sock < 0) {
                perror("accept");
                send_response(client_sock, "425 Can't open data connection\r\n");
                close(pasv_listen_sock);
                pasv_listen_sock = -1;
                continue;
            }
            list_files(pasv_data_sock);
            close(pasv_data_sock);
            pasv_data_sock = -1;
            close(pasv_listen_sock);
            pasv_listen_sock = -1;
            send_response(client_sock, "226 Transfer complete\r\n");

        } else if (strncmp(buffer, "RETR ", 5) == 0) {
            if (pasv_listen_sock == -1) {
                send_response(client_sock, "425 Use PASV first.\r\n");
                continue;
            }
            char *filename = buffer + 5;
            filename[strcspn(filename, "\r\n")] = 0;
            send_response(client_sock, "150 Opening data connection\r\n");
            pasv_data_sock = accept(pasv_listen_sock, NULL, NULL);
            if (pasv_data_sock < 0) {
                perror("accept");
                send_response(client_sock, "425 Can't open data connection\r\n");
                close(pasv_listen_sock);
                pasv_listen_sock = -1;
                continue;
            }
            FILE *file = fopen(filename, "rb");
            if (!file) {
                send_response(client_sock, "550 File not found\r\n");
            } else {
                size_t n;
                while ((n = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
                    send(pasv_data_sock, buffer, n, 0);
                }
                fclose(file);
                send_response(client_sock, "226 Transfer complete\r\n");
            }
            close(pasv_data_sock);
            pasv_data_sock = -1;
            close(pasv_listen_sock);
            pasv_listen_sock = -1;

        } else if (strncmp(buffer, "STOR ", 5) == 0) {
            if (pasv_listen_sock == -1) {
                send_response(client_sock, "425 Use PASV first.\r\n");
                continue;
            }
            char *filename = buffer + 5;
            filename[strcspn(filename, "\r\n")] = 0;
            send_response(client_sock, "150 Opening data connection\r\n");
            pasv_data_sock = accept(pasv_listen_sock, NULL, NULL);
            if (pasv_data_sock < 0) {
                perror("accept");
                send_response(client_sock, "425 Can't open data connection\r\n");
                close(pasv_listen_sock);
                pasv_listen_sock = -1;
                continue;
            }
            FILE *file = fopen(filename, "wb");
            if (!file) {
                send_response(client_sock, "550 Could not create file\r\n");
            } else {
                ssize_t n;
                while ((n = recv(pasv_data_sock, buffer, BUFFER_SIZE, 0)) > 0) {
                    fwrite(buffer, 1, n, file);
                }
                fclose(file);
                send_response(client_sock, "226 Transfer complete\r\n");
            }
            close(pasv_data_sock);
            pasv_data_sock = -1;
            close(pasv_listen_sock);
            pasv_listen_sock = -1;

        } else {
            send_response(client_sock, "502 Command not implemented\r\n");
        }
    }

    // Cleanup on client disconnect
    if (pasv_listen_sock != -1)
        close(pasv_listen_sock);
    if (pasv_data_sock != -1)
        close(pasv_data_sock);
}

