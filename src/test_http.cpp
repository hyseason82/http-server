#include <stdio.h>
#include <string.h>
#include "../include/http_conn.h"

int main() {
    HttpConn conn;
    http_conn_init(&conn, -1);

    // 模拟收到一个HTTP请求
    const char* fake_request =
        "GET /index.html HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    // 手动填入buf
    memcpy(conn.buf, fake_request, strlen(fake_request));
    conn.buf_len = strlen(fake_request);

    // 解析
    int ret = http_conn_parse(&conn);
    if (ret == 1) {
        printf("Parse OK\n");
        printf("Method:     %s\n", conn.request.method.c_str());
        printf("Path:       %s\n", conn.request.path.c_str());
        printf("Version:    %s\n", conn.request.version.c_str());
        printf("Keep-alive: %s\n", conn.request.keep_alive ? "yes" : "no");
    } else {
        printf("Parse FAILED\n");
    }

    return 0;
}
