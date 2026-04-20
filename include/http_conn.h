#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include <string>

// HTTP解析状态机的状态
enum ParseState {
    PARSE_REQUESTLINE,  // 正在解析请求行
    PARSE_HEADERS,      // 正在解析请求头
    PARSE_DONE,         // 解析完成
    PARSE_ERROR         // 解析出错
};

struct HttpRequest {
    std::string method;   // GET / POST
    std::string path;     // /index.html
    std::string version;  // HTTP/1.1
    bool keep_alive;      // Connection: keep-alive?
};

struct HttpConn {
    int         fd;
    char        buf[4096];
    int         buf_len;
    ParseState  state;
    HttpRequest request;
};

void http_conn_init(HttpConn* conn, int fd);
int  http_conn_read(HttpConn* conn);       // 从fd读数据到buf
int  http_conn_parse(HttpConn* conn);      // 解析buf里的HTTP请求
int  http_conn_respond(HttpConn* conn);    // 发送响应

#endif