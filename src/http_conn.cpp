#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "../include/http_conn.h"

#pragma GCC diagnostic ignored "-Wunused-result"

// ---- 静态文件缓存 + 预构建响应 ----
// 启动时将 header+body 合并为单块，响应只需一次 write()
static char*  g_resp_ka;    // Connection: keep-alive response
static size_t g_resp_ka_len;
static char*  g_resp_close; // no keep-alive response
static size_t g_resp_close_len;

void http_cache_init(const char* resource_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/index.html", resource_dir);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    fstat(fd, &st);
    if (st.st_size <= 0) { close(fd); return; }

    char* body = new char[st.st_size];
    (void)read(fd, body, st.st_size);
    close(fd);

    char hdr[256];
    int hlen;

    // 预构建 keep-alive 响应
    hlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\n"
                    "Connection: keep-alive\r\n"
                    "Content-Length: %zu\r\n\r\n", (size_t)st.st_size);
    g_resp_ka_len = hlen + st.st_size;
    g_resp_ka = new char[g_resp_ka_len];
    memcpy(g_resp_ka, hdr, hlen);
    memcpy(g_resp_ka + hlen, body, st.st_size);

    // 预构建 close 响应
    hlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Length: %zu\r\n\r\n", (size_t)st.st_size);
    g_resp_close_len = hlen + st.st_size;
    g_resp_close = new char[g_resp_close_len];
    memcpy(g_resp_close, hdr, hlen);
    memcpy(g_resp_close + hlen, body, st.st_size);

    delete[] body;
}

void http_conn_init(HttpConn* conn, int fd) {
    conn->fd      = fd;
    conn->buf_len = 0;
    conn->state   = PARSE_REQUESTLINE;
    conn->request.keep_alive = false;
    // buf is NOT zeroed — buf_len guards all reads/writes into it
}

// ET模式必须循环读到EAGAIN，否则缓冲区里残留数据不会再触发通知
int http_conn_read(HttpConn* conn) {
    int total = 0;
    while (1) {
        int remain = sizeof(conn->buf) - conn->buf_len;
        if (remain == 0) break;  // buf满了，先处理已有数据
        int n = read(conn->fd, conn->buf + conn->buf_len, remain);
        if (n > 0) {
            conn->buf_len += n;
            total += n;
        } else if (n == 0) {
            return total > 0 ? total : 0;   // 对端关闭
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 读干净了
            return -1;
        }
    }
    return total;
}

// 解析一行：找到\r\n返回行内容，没找到返回空
static std::string get_line(const char* buf, int len, int* pos) {
    for (int i = *pos; i < len - 1; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n') {
            std::string line(buf + *pos, i - *pos);
            *pos = i + 2;  // 跳过\r\n
            return line;
        }
    }
    return "";  // 还没收完整一行
}

int http_conn_parse(HttpConn* conn) {
    int pos = 0;

    // 解析请求行
    if (conn->state == PARSE_REQUESTLINE) {
        std::string line = get_line(conn->buf, conn->buf_len, &pos);
        if (line.empty()) return 0;

        char method[16], path[256], version[16];
        sscanf(line.c_str(), "%15s %255s %15s", method, path, version);
        conn->request.method  = method;
        conn->request.path    = path;
        conn->request.version = version;
        conn->state = PARSE_HEADERS;
    }

    // 解析请求头
    while (conn->state == PARSE_HEADERS) {
        int old_pos = pos;
        std::string line = get_line(conn->buf, conn->buf_len, &pos);

        // pos没变说明没找到\r\n，数据还没到齐
        if (pos == old_pos) return 0;

        // 空行说明头部结束
        if (line.empty()) {
            conn->state = PARSE_DONE;
            break;
        }

        if (line.find("Connection:") != std::string::npos ||
            line.find("connection:") != std::string::npos) {
            // case-insensitive: ab 发 "Keep-Alive"，curl 发 "keep-alive"
            std::string lower = line;
            for (char& c : lower) c = tolower(c);
            if (lower.find("keep-alive") != std::string::npos)
                conn->request.keep_alive = true;
        }
    }

    return (conn->state == PARSE_DONE) ? 1 : 0;
}

// 发送响应
int http_conn_respond(HttpConn* conn) {
    std::string path = "resources" + conn->request.path;

    // 命中缓存：预构建响应，单次 write() 无额外内存分配
    if ((conn->request.path == "/" || conn->request.path == "/index.html")
        && g_resp_ka) {
        if (conn->request.keep_alive) {
            (void)write(conn->fd, g_resp_ka, g_resp_ka_len);
        } else {
            (void)write(conn->fd, g_resp_close, g_resp_close_len);
        }
        return 1;
    }

    // 请求/时默认返回index.html
    if (conn->request.path == "/") {
        path = "resources/index.html";
    }

    const char* conn_hdr = conn->request.keep_alive
                           ? "Connection: keep-alive\r\n" : "";

    // 打开文件
    int file_fd = open(path.c_str(), O_RDONLY);
    if (file_fd < 0) {
        const char* resp =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 9\r\n\r\n"
            "Not Found";
        (void)write(conn->fd, resp, strlen(resp));
        return 0;
    }

    struct stat st;
    fstat(file_fd, &st);
    int file_size = st.st_size;

    char header[256];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "%s"
             "Content-Length: %d\r\n\r\n",
             conn_hdr, file_size);
    (void)write(conn->fd, header, strlen(header));

    if (file_size > 0) {
        void* file_data = mmap(NULL, file_size,
                               PROT_READ, MAP_PRIVATE, file_fd, 0);
        if (file_data != MAP_FAILED) {
            (void)write(conn->fd, file_data, file_size);
            munmap(file_data, file_size);
        }
    }
    close(file_fd);

    return 1;
}