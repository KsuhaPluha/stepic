#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

#define THREAD_POOL_SIZE 128
#define HEADER_SIZE 8192


static int logf = -1;
static char lbuf[1024];

void makelog(const char *str, int size){
    if (logf>=0){
        if (size == 0){
            size = strlen(str);
        }
        write(logf, str, size);
        write(logf, "\n", 1);
                
    }
}

void zerror(const char *msg){
    if (logf >=0){
        makelog("ERROR", 0);
        makelog(msg,0);
    }
    perror(msg);
    exit(EXIT_FAILURE);
}


int pid_store(const char *pid_path){
    int fd = open( pid_path, O_CREAT | O_WRONLY, 0644 );
    if (fd >= 0){
        char buffer[8];
        int bytes = snprintf(buffer, sizeof(buffer), "%d", getpid());
        if (bytes != write(fd, buffer, bytes))
            zerror("pid.write");

        if (close(fd) < 0)
            zerror("pid.close");
    }
    else {
        zerror("pid.open");
    }
}
int check_status(ssize_t r, int *shutdown){
    if (r>0)
        return r;
    if (r<0 && ( errno = EAGAIN || errno == EWOULDBLOCK ))
        return 0;
    *shutdown = 1;
    return -1;
}



int min(int a, int b){ return a < b ? a :b; }
int max(int a, int b){ return a > b ? a :b; }
int check_header(const char *msg, int read, int *url_start, int *url_end){
    int start = 0;
    int method = 0;
    makelog("check_header", 0);
    makelog(msg, read);
    if (msg[0] == 'G' && msg[1] == 'E' && msg[2] == 'T' && msg[3] == ' ' && msg[4] == '/'){
        method = 1;
        if (read > 5){
            *url_start = 4;
            for(int i = 4; i < read; ++i){
                if (msg[i] == ' '){
                    *url_end = i;
                    if  ( 0 == strncmp(" HTTP/1.", msg+i, min(read-i, 8))){
                        i+=8;
                        if (i<read && msg[i] >= '0' && msg[i] <= '9'){
                            i+=1;
                            while(i<read && msg[i] >= '0' && msg[i] <='9') ++i;
                            if (i<read && msg[i] == '\r') ++i;
                            if (i<read && msg[i] == '\n'){
                                i+=1;
                                while(i<read){
                                    if (msg[i] == '\n'){
                                        return method;
                                    }
                                    if (msg[i] == '\r' && i + 1 < read && msg[i+1] == '\n'){
                                        return method;
                                    }

                                    while(i<read){
                                        while(i<read && (msg[i] > '\n')) ++i;
                                        if (i<read && msg[i] == '\r' ) {
                                            ++i;
                                        }
                                        if (i==read)
                                            return 0;
                                        if (msg[i] == '\n'){
                                            i+=1;
                                            break;
                                        }
                                        if (msg[i] == '\0'){
                                            return -2;
                                        }
                                        i+=1;
                                    }
                                }
                            }
                            else if (i == read){
                                return 0;
                            }
                            else {
                                return -2;
                            }
                        }

                    }
                    else {
                        return -2;
                    }
                }
            }
            return 0;
        }
        else {
            return 0;
        }
    }
    else if (strncmp(msg, "GET /", min(read,5)) != 0){
        return -1;
    }
    return 0;
}

void send_back(int fd, const char *message, int read, int *shutdown){
    size_t sended = 0;
    while(sended < read){
        ssize_t r = send(fd, message+sended, read - sended, MSG_NOSIGNAL);
        if (check_status(r, shutdown) >= 0){
            if (r>=0)
                sended += r;
        }
        else {
            break;
        }
    }
}
static const char error_404[] = "HTTP/1.0 404 Not Found\r\nContent-type: text/html\r\n\r\n<h1>Not Found</h1>";
static const char error_200[] = "HTTP/1.0 200 Ok\r\nContent-type: text/plain\r\n\r\nOK";
static const char error_200_1[] = "HTTP/1.0 200 Ok\r\nContent-type: %s\r\n\r\nURL=%.*s\r\n";
static const char error_200_2[] = "HTTP/1.0 200 Ok\r\nContent-type: %s\r\nContent-Length: %ld\r\n\r\n";

static int hex(char c){
    if ('0' <= c && c <= '9')
        return c - '0';
    if ('a' <= c && c <= 'f')
        return c-'a';
    if ('A' <= c && c <= 'F')
        return c-'A';
    return 0;
}

static const char *mime_types[] = {
"application/octet-stream","text/html","text/css","text/xml","image/gif","image/jpeg","application/x-javascript","application/atom+xml","text/mathml","text/plain","text/vnd.sun.j2me.app-descriptor","text/vnd.wap.wml","text/x-component","image/png","image/tiff","image/vnd.wap.wbmp","image/x-icon","image/x-jng","image/x-ms-bmp","image/svg+xml","application/java-archive","application/json","application/mac-binhex40","application/msword","application/pdf","application/postscript","application/rtf","application/vnd.ms-excel","application/vnd.ms-powerpoint","application/vnd.wap.wmlc","application/vnd.google-earth.kml+xml","application/vnd.google-earth.kmz","application/x-7z-compressed","application/x-cocoa","application/x-java-archive-diff","application/x-java-jnlp-file","application/x-makeself","application/x-perl","application/x-pilot","application/x-rar-compressed","application/x-redhat-package-manager","application/x-sea","application/x-shockwave-flash","application/x-stuffit","application/x-tcl","application/x-x509-ca-cert","application/x-xpinstall","application/xhtml+xml","application/zip","application/octet-stream","application/ogg","audio/midi","audio/mpeg","audio/ogg","audio/x-realaudio","audio/webm","video/3gpp","video/mp4","video/mpeg","video/ogg","video/quicktime","video/webm","video/x-flv","video/x-mng","video/x-ms-asf","video/x-ms-wmv","video/x-msvideo"
};

static char mime_char[] = {
0,97,114,120,100,50,107,103,102,105,116,101,110,51,118,109,115,108,52,99,112,98,122,111,0,0,101,52,114,98,103,0,0,115,0,0,46,0,0,0,0,109,0,0,46,0,0,0,0,46,0,0,0,0,101,0,0,119,0,0,46,0,0,0,0,101,112,111,0,0,112,0,0,109,0,0,46,0,0,0,0,109,0,0,46,0,0,0,0,46,0,0,0,0,101,97,0,0,100,0,0,46,0,0,0,0,101,119,107,114,106,0,0,46,0,0,0,0,46,0,0,0,0,46,0,0,0,0,46,0,0,0,0,46,0,0,0,0,112,103,113,115,0,0,115,0,0,46,0,0,0,0,111,0,0,46,0,0,0,0,104,0,0,46,0,0,0,0,97,0,0,46,0,0,0,0,97,105,0,0,106,0,0,46,0,0,0,0,109,0,0,46,0,0,0,0,112,0,0,109,0,0,46,0,0,0,0,116,0,0,46,0,0,0,0,101,110,112,103,109,118,0,0,112,0,0,109,106,0,0,46,0,0,0,0,46,0,0,0,0,112,109,106,0,0,46,0,0,0,0,46,0,0,0,0,46,0,0,0,0,109,106,0,0,46,0,0,0,0,46,0,0,0,0,111,0,0,46,0,0,0,0,100,105,0,0,46,0,0,0,0,46,0,0,0,0,115,0,0,46,0,0,0,0,119,100,115,102,116,105,0,0,115,0,0,46,0,0,0,0,112,0,0,46,0,0,0,0,97,0,0,46,0,0,0,0,105,0,0,100,116,0,0,114,0,0,97,0,0,106,0,0,46,0,0,0,0,46,0,0,0,0,114,0,0,46,0,0,0,0,103,116,0,0,46,0,0,0,0,46,0,0,0,0,112,97,118,100,115,0,0,120,0,0,46,0,0,0,0,46,0,0,0,0,97,0,0,46,0,0,0,0,105,0,0,109,0,0,46,0,0,0,0,109,0,0,46,0,0,0,0,112,114,120,111,105,0,0,112,0,0,46,0,0,0,0,99,0,0,46,0,0,0,0,116,0,0,46,0,0,0,0,101,0,0,46,0,0,0,0,115,0,0,46,0,0,0,0,112,120,0,0,109,0,0,46,0,0,0,0,101,0,0,46,0,0,0,0,117,105,111,0,0,114,0,0,46,0,0,0,0,98,0,0,46,0,0,0,0,115,0,0,106,0,0,46,0,0,0,0,112,0,0,109,0,0,46,0,0,0,0,108,103,109,111,0,0,102,0,0,46,0,0,0,0,111,0,0,46,0,0,0,0,119,0,0,46,0,0,0,0,109,0,0,46,0,0,0,0,101,112,98,115,111,116,0,0,112,0,0,46,0,0,0,0,114,46,0,0,46,0,0,0,0,0,0,101,0,0,119,0,0,46,0,0,0,0,109,0,0,46,0,0,0,0,116,0,0,97,0,0,46,0,0,0,0,104,0,0,46,0,0,0,0,108,112,115,106,0,0,120,0,0,46,0,0,0,0,101,46,0,0,46,0,0,0,0,0,0,99,114,0,0,46,0,0,0,0,46,0,0,0,0,46,0,0,0,0,108,99,112,109,0,0,100,0,0,46,0,0,0,0,116,0,0,46,0,0,0,0,46,0,0,0,0,119,107,109,120,116,0,0,46,0,0,0,0,46,0,0,0,0,46,0,0,0,0,46,0,0,0,0,104,0,0,46,120,115,0,0,0,0,46,0,0,0,0,46,0,0,0,0,112,0,0,109,0,0,46,0,0,0,0,108,114,111,116,0,0,109,0,0,119,0,0,46,0,0,0,0,112,0,0,46,0,0,0,0,100,0,0,46,0,0,0,0,104,0,0,46,0,0,0,0,108,112,103,109,115,105,0,0,110,0,0,106,0,0,46,0,0,0,0,103,0,0,51,0,0,46,0,0,0,0,51,0,0,46,0,0,0,0,98,0,0,119,46,0,0,46,0,0,0,0,0,0,109,0,0,46,0,0,0,0,122,0,0,46,0,0,0,0,101,100,0,0,100,0,0,46,0,0,0,0,112,0,0,46,0,0,0,0,103,55,109,0,0,118,0,0,115,0,0,46,0,0,0,0,46,0,0,0,0,107,0,0,46,0,0,0,0,99,115,0,0,99,105,0,0,46,0,0,0,0,46,0,0,0,0,105,0,0,46,0,0,0
};
static int mime_ptr[]   = {
0,25,93,137,175,195,206,214,303,383,430,477,497,529,540,578,643,687,761,772,813,881,901,930,0,0,32,40,48,53,64,0,0,35,0,0,38,0,-41,-41,0,43,0,0,46,0,-52,-52,0,51,0,-54,-54,0,56,0,0,59,0,0,62,0,-55,-55,0,69,80,88,0,0,72,0,0,75,0,0,78,0,-52,-52,0,83,0,0,86,0,-52,-52,0,91,0,-53,-53,0,97,105,0,0,100,0,0,103,0,-45,-45,0,112,117,122,127,132,0,0,115,0,-20,-20,0,120,0,-20,-20,0,125,0,-51,-51,0,130,0,-39,-39,0,135,0,-20,-20,0,143,151,159,167,0,0,146,0,0,149,0,-53,-53,0,154,0,0,157,0,-50,-50,0,162,0,0,165,0,-22,-22,0,170,0,0,173,0,-64,-64,0,179,187,0,0,182,0,0,185,0,-10,-10,0,190,0,0,193,0,-51,-51,0,198,0,0,201,0,0,204,0,-52,-52,0,209,0,0,212,0,-44,-44,0,222,239,259,273,281,295,0,0,225,0,0,229,234,0,0,232,0,-58,-58,0,237,0,-5,-5,0,244,249,254,0,0,247,0,-13,-13,0,252,0,-63,-63,0,257,0,-17,-17,0,263,268,0,0,266,0,-58,-58,0,271,0,-5,-5,0,276,0,0,279,0,-53,-53,0,285,290,0,0,288,0,-49,-49,0,293,0,-49,-49,0,298,0,0,301,0,-19,-19,0,311,319,327,335,361,369,0,0,314,0,0,317,0,-42,-42,0,322,0,0,325,0,-24,-24,0,330,0,0,333,0,-64,-64,0,338,0,0,342,356,0,0,345,0,0,348,0,0,351,0,0,354,0,-34,-34,0,359,0,-14,-14,0,364,0,0,367,0,-26,-26,0,373,378,0,0,376,0,-4,-4,0,381,0,-14,-14,0,390,398,403,411,422,0,0,393,0,0,396,0,-46,-46,0,401,0,-25,-25,0,406,0,0,409,0,-66,-66,0,414,0,0,417,0,0,420,0,-51,-51,0,425,0,0,428,0,-49,-49,0,437,445,453,461,469,0,0,440,0,0,443,0,-28,-28,0,448,0,0,451,0,-45,-45,0,456,0,0,459,0,-9,-9,0,464,0,0,467,0,-49,-49,0,472,0,0,475,0,-43,-43,0,481,489,0,0,484,0,0,487,0,-58,-58,0,492,0,0,495,0,-49,-49,0,502,510,518,0,0,505,0,0,508,0,-36,-36,0,513,0,0,516,0,-49,-49,0,521,0,0,524,0,0,527,0,-21,-21,0,532,0,0,535,0,0,538,0,-52,-52,0,546,554,562,570,0,0,549,0,0,552,0,-62,-62,0,557,0,0,560,0,-59,-59,0,565,0,0,568,0,-65,-65,0,573,0,0,576,0,-60,-60,0,586,594,605,616,624,635,0,0,589,0,0,592,0,-45,-45,0,598,603,0,0,601,0,-40,-40,-37,-37,0,608,0,0,611,0,0,614,0,-61,-61,0,619,0,0,622,0,-49,-49,0,627,0,0,630,0,0,633,0,-7,-7,0,638,0,0,641,0,-1,-1,0,649,657,668,682,0,0,652,0,0,655,0,-27,-27,0,661,666,0,0,664,0,-25,-25,-25,-25,0,672,677,0,0,675,0,-2,-2,0,680,0,-3,-3,0,685,0,-6,-6,0,693,701,709,714,0,0,696,0,0,699,0,-49,-49,0,704,0,0,707,0,-44,-44,0,712,0,-37,-37,0,721,726,731,736,741,0,0,724,0,-11,-11,0,729,0,-30,-30,0,734,0,-8,-8,0,739,0,-3,-3,0,744,0,0,749,751,756,0,-1,-1,0,754,0,-47,-47,0,759,0,-1,-1,0,764,0,0,767,0,0,770,0,-57,-57,0,778,789,797,805,0,0,781,0,0,784,0,0,787,0,-29,-29,0,792,0,0,795,0,-38,-38,0,800,0,0,803,0,-23,-23,0,808,0,0,811,0,-12,-12,0,821,832,843,851,865,873,0,0,824,0,0,827,0,0,830,0,-35,-35,0,835,0,0,838,0,0,841,0,-56,-56,0,846,0,0,849,0,-56,-56,0,854,0,0,858,863,0,0,861,0,-15,-15,-18,-18,0,868,0,0,871,0,-49,-49,0,876,0,0,879,0,-48,-48,0,885,893,0,0,888,0,0,891,0,-49,-49,0,896,0,0,899,0,-38,-38,0,906,917,922,0,0,909,0,0,912,0,0,915,0,-19,-19,0,920,0,-32,-32,0,925,0,0,928,0,-31,-31,0,934,948,0,0,938,943,0,0,941,0,-33,-33,0,946,0,-16,-16,0,951,0,0,954,0,-49,-49
};
static int convert_path(char *path, int size, const char *&content_type){
    int dest=0;
    for(int i=1; i< size; ++i){
        switch(path[i]){
            case '%':
                if (i+2<size){
                    path[dest] = hex(path[i+1]) * 16 + hex(path[i+2]);
                    i+=2;
                    if (path[dest] == '/'){
                        if (path[dest-1] == '.' && path[dest-2] == '.' && path[dest-3] == '/'){
                            dest -= 2;
                            while(dest > 0 && path[dest] != '/'){
                                --dest;
                            }
                            path[dest] = '/';
                        }
                    }
                    dest+=1;
                }
                else {
                    path[dest++] = '%';
                }
                break;
            case '?':
                i = size;
                break;
            case '/':
                path[dest] = '/';
                if (path[dest-1] == '.' && path[dest-2] == '.' && path[dest-3] =='/'){
                    dest -= 2;
                    while(dest > 0 && path[dest] != '/'){
                        --dest;
                    }
                    path[dest] = '/';
                }
                dest += 1;
            case '\0':
                i = size;
                break;
        default:
                path[dest++] = path[i];
        }
    }
    path[dest] = '\0';
    content_type = "application/octet-stream";
    int state = 0;
    dest -= 1;
    int found = 0;
    //fprintf(stderr, "start detect\n");
    while(1){
        if (mime_ptr[state])
            found = mime_ptr[state];
        state += 1;
        while(mime_char[state] && mime_char[state] != path[dest]) ++state;
        if (mime_char[state]){
            state = mime_ptr[state];
            --dest;
        }
        else {
            break;
        }
    }
    if (state){
        if (mime_ptr[state])
            found = mime_ptr[state];
    }
    if (found)
        content_type = mime_types[-found];
    //fprintf(stderr, "end detect=%s\n", content_type);
    return dest;
}
void print_debug(int fd, char * buffer, int &url_start, int &url_end, int &shutdown){
    char obur[1024];
    int r=0;
    ////fprintf(stderr, "r=%d,us=%d, ue=%d\n", r, url_start, url_end);
    r = snprintf(obur, sizeof(obur), error_200_1, "text/plain", url_end-url_start, buffer+url_start); 
    //fprintf(stderr, "r=%d,us=%d, ue=%d\n", r, url_start, url_end);
    send_back(fd, obur, r, &shutdown);
    shutdown = 1;
}

void * start_worker(void *in){
    int *pipefd = (int *)in;
    int commands[2];
    while(1){
        int bytes = read(pipefd[0], commands, sizeof(commands));
        if (bytes == sizeof(commands)){
            if (commands[0] == 0){
                //fprintf(stderr, "Get fd=%d\n", commands[1]);
                int fd = commands[1];
                int read = 0;
                int shutdown = 0;
                char buffer[HEADER_SIZE];
                while(!shutdown){
                    size_t bytes = recv(fd, buffer+read, sizeof(buffer)-read, 0);
                    if (check_status(bytes, &shutdown) > 0){
                        read+=bytes;
                    }
                    int status;
                    int url_start;
                    int url_end;
                    if ((status = check_header(buffer, read, &url_start, &url_end )) >= 0){
                        if (status > 0){
                            // print_debug(fd, buffer, url_start, url_end, shutdown);
                            const char *content_type;
                            makelog(buffer, url_end+9);
                            convert_path(buffer + url_start, url_end - url_start, content_type);
                            ////fprintf(stderr, "openning %s\n", 1+buffer + url_start);
                            int filefd = open(buffer + url_start, O_RDONLY);
                            //fprintf(stderr, "open %d\n", filefd);
                            if (filefd >=0){
                                int r;
                                char obur[1024];
                                off_t content_length = 0;
                                struct stat filestat;
                                fstat(filefd, &filestat);
                                r = snprintf(obur, sizeof(obur), error_200_2, content_type, (long)filestat.st_size); 
                                //fprintf(stderr, "%s=URL=%s\n", obur, buffer + url_start);
                                send_back(fd, obur, r, &shutdown);
                                sendfile(fd, filefd, 0, filestat.st_size);
                                shutdown = 1;
                                close(filefd);
                            }
                            else {
                                send_back(fd, error_404, sizeof(error_404) - 1, &shutdown);
                                shutdown = 1;
                            }
                            //fprintf(stderr, "closing %d\n", fd);
                        }
                    }
                    else {
                        send_back(fd, error_404, sizeof(error_404)-1, &shutdown);
                        shutdown = 1;
                    }
                }
                //fprintf(stderr, "close(fd=%d)\n", commands[1]);
                close(fd);
            }
            else if (commands[0] == 1){
                return NULL;
            }
        }
        else {
            zerror("partly reading");
        }
    }
    return NULL;
}

int main(int argc, char **argv){
    char *c;
    int opt;
    char *host = NULL;
    char *root = NULL;
    int port = 0;
    struct sockaddr_in bind_addr;

    while(-1 != (opt = getopt(argc, argv, "h:p:d:"))){
        switch(opt){
            case 'h':
                if (host)
                    free(host);
                host = strdup(optarg);
                if (inet_aton(host, (struct in_addr *) &bind_addr.sin_addr)==0){
                    fprintf(stderr, "%s: Invalid address\n", host);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case 'd':
                if (root)
                    free(root);
                root = strdup(optarg);
                break;
            default:
                fprintf(stderr, "Usage: %s -h host -p port -d directory =%d\n", argv[0],opt);
                exit(EXIT_FAILURE);
        }
    }
    
    if (port ==0 || root == NULL){
        fprintf(stderr, "Usage: %s -h host -p port -d directory\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    in_addr_t any = INADDR_ANY;
    if (!host)
        memcpy(&bind_addr.sin_addr, &any, sizeof(bind_addr.sin_addr));
    logf = open("/home/box/final.log", O_CREAT | O_APPEND | O_WRONLY, 0666);
    if (logf < 0)
        logf = open("/home/gtoly/final.log", O_CREAT | O_APPEND | O_WRONLY, 0666);
    snprintf( lbuf, sizeof(lbuf), "start -p %d -h %s -d %s", port, host, root );
    makelog( lbuf, 0 );

    int master_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (master_socket < 0)
        zerror("socket");
    long opts = 1;

    if (-1==setsockopt(master_socket, SOL_SOCKET, SO_REUSEADDR, &opts, sizeof(&opts)))
        zerror("setsockopt");

    int br = bind(master_socket, (struct sockaddr  *) &bind_addr, sizeof(bind_addr));
    if (br <= -1)
        zerror("master.bind");

    if ( 0 > listen(master_socket, SOMAXCONN))
        zerror("listen");
    int pipefd[2];
    if ( 0 != pipe(pipefd))
        zerror("pipe");
    makelog("forking           ....", 0);
    pid_t child = fork();
    if (child == -1)
        zerror("daemonize.child");

    if (!child){
        makelog("floseing           ....", 0);
        fclose(stdin);
        fclose(stdout);
        fclose(stderr);
        close(logf);
        daemon(1,0);
        logf = open("/home/box/final.log", O_CREAT | O_APPEND | O_WRONLY, 0666);
        if (logf < 0)
            logf = open("/home/gtoly/final.log", O_CREAT | O_APPEND | O_WRONLY, 0666);
        // pid_store("http.pid");

        /* переходим в html root */
        makelog("chdiring           ....", 0);
        if ( 0 != chdir(root) )
            zerror("chdir");
        /* Стартуем наши треды */
        makelog("threading           ....", 0);
        int thread_total = 0;
        pthread_attr_t thread_attr;
        pthread_attr_init(&thread_attr);
        pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
        for(int i=0; i < THREAD_POOL_SIZE; ++i){
            pthread_t thr;
            if ( 0 > pthread_create(&thr, &thread_attr, start_worker, pipefd)){
                perror("thread_create");
                pthread_attr_destroy(&thread_attr);
                break;
            }
            ++thread_total;
        }
        pthread_attr_destroy(&thread_attr);
        makelog("threading finish          ....", 0);

        struct sockaddr_in addr;
        socklen_t addrsize;
        int sock;
        makelog("accepting           ....", 0);
        while(1){
            addrsize = sizeof(addr);
            if ((sock = accept4(master_socket, (struct sockaddr *)&addr, &addrsize, SOCK_CLOEXEC)) >= 0){
                makelog("accept ....", 0);
                int buffer[2];
                buffer[0] = 0;
                buffer[1] = sock;
                int bytes = write(pipefd[1], buffer, sizeof(buffer));
                if (bytes < 0)
                    zerror("send fd");
            }
            else {
                if (errno == EAGAIN || errno == EWOULDBLOCK){
                    continue;
                }
                else {
                    zerror("chld.accept");
                }
            }
        }
    }
    else {
        sleep(1);
    };

    close(master_socket);
    free(host);
    free(root);
    exit(EXIT_SUCCESS);
}