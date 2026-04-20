#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "../include/http_conn.h"

void http_conn_init(HttpConn* conn, int fd) {
    conn->fd      = fd;
    conn->buf_len = 0;
    conn->state   = PARSE_REQUESTLINE;
    conn->request.keep_alive = false;
    memset(conn->buf, 0, sizeof(conn->buf));
}

// 从fd读数据追加到buf
int http_conn_read(HttpConn* conn) {
    int n = read(conn->fd,
                 conn->buf + conn->buf_len,
                 sizeof(conn->buf) - conn->buf_len);
    if (n <= 0) return n;
    conn->buf_len += n;
    return n;
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
        sscanf(line.c_str(), "%s %s %s", method, path, version);
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

        if (line.find("Connection:") != std::string::npos) {
            if (line.find("keep-alive") != std::string::npos) {
                conn->request.keep_alive = true;
            }
        }
    }

    return (conn->state == PARSE_DONE) ? 1 : 0;
}

// 发送响应
int http_conn_respond(HttpConn* conn) {
    std::string path = "resources" + conn->request.path;

    // 请求/时默认返回index.html
    if (conn->request.path == "/") {
        path = "resources/index.html";
    }

    // 打开文件
    int file_fd = open(path.c_str(), O_RDONLY);
    if (file_fd < 0) {
        // 404
        const char* resp =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 9\r\n\r\n"
            "Not Found";
        write(conn->fd, resp, strlen(resp));
        return 0;
    }

    // 获取文件大小
    struct stat st;
    fstat(file_fd, &st);
    int file_size = st.st_size;

    // 发送响应头
    char header[256];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Length: %d\r\n\r\n",
             file_size);
    write(conn->fd, header, strlen(header));

    // mmap映射文件内容，零拷贝发送
    void* file_data = mmap(NULL, file_size,
                           PROT_READ, MAP_PRIVATE, file_fd, 0);
    write(conn->fd, file_data, file_size);
    munmap(file_data, file_size);
    close(file_fd);

    return 1;
}