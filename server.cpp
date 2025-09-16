#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include<string.h>
#include<fmt/format.h>
#include<thread>
#include<vector>
#include<algorithm>
#include<map>
#include <string>
#include <sstream>
#include <vector>

int err;
std::error_category *cat;

std::error_category const &gai_category(){
    static struct final : std::error_category{
        char const *name() const noexcept override{
            return "getaddrinfo";
        }
        std::string message(int err) const override{
            return gai_strerror(err);
        }
    } instance;
    return instance;
}

int check_error(const char* msg,int res)
{
    if(res==-1)
    {
        fmt::println("{}:{}",msg,strerror(errno));
        auto ec = std::error_code(errno, std::system_category());
        throw std::system_error(ec, msg);
    }
    return res;
}

size_t check_error(const char* msg,ssize_t res){
    if(res==-1){
        fmt::println("{}:{}",msg,strerror(errno));
        throw;
    }
    return res;
}

# define CHECK_CALL(func,...) check_error(#func,func(__VA_ARGS__))

struct socket_address_fatptr{
    struct sockaddr *m_addr;
    socklen_t m_addrlen;
};

struct socket_address_storage{
    union{
        struct sockaddr m_addr;
        struct sockaddr_storage m_addr_storage;
    };

    socklen_t m_addrlen = sizeof(struct sockaddr_storage);

    operator socket_address_fatptr(){
        return{&m_addr,m_addrlen};
    }
};

struct address_resolved_entry {
    struct addrinfo *m_curr=nullptr;

    socket_address_fatptr get_address() const{
        return{m_curr->ai_addr,m_curr->ai_addrlen};
    }

    int create_socket() const{
        int sockfd=CHECK_CALL(socket,m_curr->ai_family,m_curr->ai_socktype,m_curr->ai_protocol);
        
        return sockfd;
    }

    int create_socket_and_bind() const{
        int sockfd=create_socket();
        CHECK_CALL(bind,sockfd,m_curr->ai_addr,m_curr->ai_addrlen);
        return sockfd;
    }

    [[nodiscard]] bool next_entry(){
        m_curr=m_curr->ai_next;
        if(m_curr==nullptr){
            return false;
        }
        return true;
    }


};

struct address_resolver{
    struct addrinfo *m_head=nullptr;

    address_resolved_entry  resolve(std::string const &name,std::string const &service){
        int err = getaddrinfo(name.c_str(),service.c_str(),NULL,&m_head);
        if (err!=0){
            //fmt::println("getaddrinfo error:{},{}",gai_strerror(err),err);
            auto ec = std::error_code(err, gai_category());
            throw std::system_error(ec, name + ":" + service);
        }
        return {m_head};
    }

    address_resolved_entry get_first_entry(){
        return{m_head};

    }

    address_resolver()=default;

    address_resolver(address_resolver &&that):m_head(that.m_head){
        that.m_head=nullptr;
    }

    ~address_resolver(){
        if(m_head){
            freeaddrinfo(m_head);
        }
    }

};

using StringMap = std::map<std::string, std::string>;

struct http11_header_parser{
    std::string m_header;
    std::string m_heading_line;//GET / HTTP/1.1
    StringMap m_header_keys;
    std::string m_body;
    size_t content_length = 0;
    bool m_header_finished = false;

    [[nodiscard]] bool header_finished(){
        return m_header_finished;
    }

    void _extract_headers(){
        size_t pos = m_header.find("\r\n");
        if (pos == std::string::npos) {
            throw std::runtime_error("Invalid HTTP request: no CRLF found");
        }
        m_heading_line = m_header.substr(0, pos);  // 截取第一行
        //fmt::println("my heading line:{}",m_heading_line);
        while(pos!=std::string::npos){
            //skip \r\n
            pos+=2;
            size_t next_pos = m_header.find("\r\n", pos);
            size_t line_len = std::string::npos;
            if(next_pos!=std::string::npos){
                line_len = next_pos - pos;
            }

            //goto next line
            std::string line = m_header.substr(pos, line_len);
            size_t colon = line.find(": ");
            if(colon!=std::string::npos){
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 2);
                //turn the keys to lower case
                std::transform(key.begin(),key.end(),key.begin(),[](char c){
                    if(c>='A' && c<='Z'){
                        return static_cast<char>(c - 'A' + 'a');
                    }
                    return static_cast<char>(c);
                    
                });
                m_header_keys[key] = value;
                //fmt::println("found header:{}:{}",key,m_header_keys[key]);
                if(key=="content-length"){
                    //fmt::println("found content length:{}",value);
                    content_length = std::stoi(value);
                }
            }
            pos = next_pos;
    }
    }

    void push_chunk(std::string_view chunk){
        if(!m_header_finished){
            //fmt::println("starting to push chunk to header");
            m_header.append(chunk);
            size_t header_len = m_header.find("\r\n\r\n");
            //cant find the end of header
            if(header_len!=std::string::npos){
                m_header_finished = true;
                //keep the body part in m_body
                m_body = m_header.substr(header_len + 4);
                m_header.resize(header_len);
                //fmt::println("starting to extract headers");
                _extract_headers();
            }
        }

    }

    StringMap &headers(){
        return m_header_keys;
    }

    std::string &headline(){
        fmt::println("heading line:{}",m_heading_line);
        return m_heading_line;
    }

    std::string &headers_raw(){
        return m_header;
    }

    std::string &extra_body(){
        return m_body;
    }

};


// http request parser
template<class HeaderParser = http11_header_parser>
struct _http_base_parser{
    HeaderParser m_header_parser;
    size_t m_content_length = 0;
    bool m_body_finished = false;

    [[nodiscard]] bool request_finished() const{
        return m_body_finished;//body is finished, no need more chunks
    }

    std::string &body(){
        return m_header_parser.extra_body();
    }

    std::string &headers(){
        return m_header_parser.headers();
    }

    std::string &headers_raw(){
        return m_header_parser.headers_raw();
    }

    std::string &headline(){
        return m_header_parser.headline();
    }




    size_t _extract_content_length(){
        auto &headers = m_header_parser.headers();
        auto it = headers.find("content-length");
        if(it == headers.end()){
            //fmt::println("no content length header, assume 0");
            return 0;
        }
        try
        {
            //fmt::println("found content length:{}",it->second);
            return std::stoi(it->second);
        }
        catch(std::invalid_argument const &)
        {
            return 0;
        }
    }
 
    void push_chunk(std::string_view chunk){
        
        if(!m_header_parser.header_finished()){
            m_header_parser.push_chunk(chunk);
            if(m_header_parser.header_finished()){
                m_content_length = _extract_content_length();
                if(body().size() >= m_content_length){
                    //fmt::println("body size {} >= content length {}",body().size(),m_content_length);
                    m_body_finished = true;
                    body().resize(m_content_length);
                }
            }
        }
        else{
            body().append(chunk);
            if(body().size() >= m_content_length){
                    m_body_finished = true;
                    body().resize(m_content_length);
            }
        }
    }

    std::string _headline_first(){
        //get / http/1.1 request
        //http/1.1 200 ok response
        auto &line = headline();
        size_t space = line.find(' ');
        if(space==std::string::npos){
            return "";
        }
        return line.substr(0,space);

    }

    std::string _headline_second(){
        auto &line = headline();
        size_t space1 = line.find(' ');
        if (space1 == std::string::npos) {
            return {};
        }
        size_t space2 = line.find(' ', space1 + 1);
        if (space2 == std::string::npos) {
            // 只找到一个空格，返回后半部分
            return line.substr(space1 + 1);
        }
        // 返回第一个空格和第二个空格之间的部分
        return line.substr(space1 + 1, space2 - space1 - 1);
    }
    std::string _headline_third() {
        auto &line = headline();
        size_t space1 = line.find(' ');
        if (space1 == std::string::npos) {
            return {};
        }
        size_t space2 = line.find(' ', space1 + 1);
        return line.substr(space2 + 1);
    }



};

template <class HeaderParser = http11_header_parser>
struct http_response_parser : _http_base_parser<HeaderParser>{
    
    std::string http_version(){
        return this->_headline_first();
    }

    int status() {
        auto s = this->_headline_second();
        try{
            return std::stoi(s);
        }catch(std::logic_error const &){
            return -1;
        }
    }

    std::string status_string(){
        return this->_headline_third();
    }
};

template<class HeaderParser = http11_header_parser>
struct http_request_parser : _http_base_parser<HeaderParser>{
    std::string method(){
        return this->_headline_first();
    }

    std::string url(){
        return this->_headline_second();
    }

    std::string http_version(){
        return this->_headline_third();
    }
};

struct http_response_writer {
    std::ostringstream oss;   // 用于拼接 header
    std::string buf;          // 保存 header 的 buffer
    bool header_finished = false;

    // 开始 header，写入状态行
    void begin_header(int status_code, const std::string& status_text = "OK") {
        oss << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    }

    // 写入单个 header
    void write_header(const std::string& key, const std::string& value) {
        oss << key << ": " << value << "\r\n";
    }

    // 结束 header，写入空行
    void end_headers() {
        oss << "\r\n";
        buf = oss.str();
        header_finished = true;
    }

    // 返回 header 缓冲区（只包含 header，不包含 body）
    std::string& buffer() {
        return buf;
    }
};

std::vector<std::thread> pool;

auto server(){
    address_resolver resolver;

    fmt::println("listening:127.0.0.1:8080");

    auto entry = resolver.resolve("-1","8080");
    
    int listenfd = entry.create_socket_and_bind();
    

    CHECK_CALL(listen,listenfd,SOMAXCONN);

    

    while(true)
    {
        socket_address_storage addr;
        int connid = CHECK_CALL(accept,listenfd,&addr.m_addr,&addr.m_addrlen);
        pool.emplace_back([connid]{
            while(true){
                char buf[1024];
                //size_t n = CHECK_CALL(read,connid,buf,sizeof(buf));
                http_request_parser req_parse;
                do{
                    
                    size_t n = CHECK_CALL(read,connid,buf,sizeof(buf));
                    //fmt::println("read {} bytes",n);
                    if(n==0){
                        //if eof is received
                        fmt::println("eof received from connid:{}",connid);
                        goto quit;
                    }
                    req_parse.push_chunk(std::string_view(buf,n));
                }while(!req_parse.request_finished());

                /*
                fmt::println("*****************");
                fmt::println("request headers:{}",req_parse.headers_raw());
                fmt::println(" ");
                fmt::println("request body:{}",req_parse.body());
                
                fmt::println(" ");*/
                std::string body = req_parse.body();
                if(body.empty()){
                    body = "<html><body><h1>your request is empty</h1></body></html>";
                }
                else{
                    body = "<html><body><h1>your request body is:</h1><p>" + body + "</p></body></html>";
                }



                //writer part
                http_response_writer res_writer;
                res_writer.begin_header(200);
                res_writer.write_header("Server","co_http");
                res_writer.write_header("Content-Type", "text/html;charset=utf-8");
                res_writer.write_header("Connection","keep-alive");
                res_writer.write_header("Content-length",std::to_string(body.size()));
                res_writer.end_headers();
                auto &buffer = res_writer.buffer();
                CHECK_CALL(write, connid, buffer.data(), buffer.size());
                CHECK_CALL(write, connid, body.data(), body.size());

                //fmt::println("response:{}",buffer);
                //fmt::println("*****************");
                fmt::println("handled request from connid:{}",connid);
            }
        quit:
            fmt::println("closing connid:{}",connid);
            close(connid);
        }
    );
    }
}

int main(){
    setlocale(LC_ALL,"zh_CN.UTF-8");
    try{
        server();
    } catch(std::system_error const &e){
        fmt::println("error:{}",e.what());
    }
    for(auto &t:pool){
        t.join();
    }
    return 0;

}