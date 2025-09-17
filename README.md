# coHttp

一个用 **C++11** 编写的简易 HTTP/1.1 服务器，支持基本的请求解析与响应构建。

## 功能特性

- 基于 socket 的 HTTP 服务器
- 请求解析：
  - 请求行 (method / url / version)
  - 请求头 (支持 `Content-Length` 等)
  - 请求体（可选）
- 响应构建：
  - 支持自定义状态码与 reason
  - 自定义响应头
  - 写入响应体
- 基础错误处理（使用 `std::error_code` 和 `std::system_error`）
- 可复用的 request/response writer

## 架构说明

- `http11_header_parser`  
  解析 HTTP 请求头与请求行
- `http_response_parser`  
  解析 HTTP 响应
- `http11_header_writer`  
  构建 HTTP 报文头
- `http_response_writer` / `http_request_writer`  
  高层封装，简化 header 与 body 的写入

## 使用方法

### 构建
```bash
mkdir build && cd build
cmake ..
make
