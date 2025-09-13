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

int check_error(const char* msg,int res)
{
    if(res==-1)
    {
        fmt::println("{}:{}",msg,strerror(errno));
        throw;
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
            fmt::println("getaddrinfo error:{},{}",gai_strerror(err),err);
            throw;
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
                        return c - 'A' + 'a';
                    }
                });
                m_header_keys[key] = value;
                if(key=="content-length"){
                    content_length = std::stoi(value);
                }
            }
            pos = next_pos;
    }
    }

    void push_chunk(std::string_view chunk){
        if(!m_header_finished){
            m_header.append(chunk);
            size_t header_len = m_header.find("\r\n\r\n");
            //cant find the end of header
            if(header_len!=std::string::npos){
                m_header_finished = true;
                //keep the body part in m_body
                m_body = m_header.substr(header_len + 4);
                m_header.resize(header_len);
                _extract_headers();
            }
        }

    }

    StringMap &headers(){
        return m_header_keys;
    }

    std::string &headline(){
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
struct http_request_parser{
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
    size_t _extract_content_length(){
        auto &headers = m_header_parser.headers();
        auto it = headers.find("content_length");
        if(it == headers.end()){
            return 0;
        }
        try
        {
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
};

std::vector<std::thread> pool;

int main(){
    setlocale(LC_ALL,"zh_CN.UTF-8");
    address_resolver resolver;

    fmt::println("listening:127.0.0.1:8080");

    auto entry = resolver.resolve("127.0.0.1","8080");
    
    int listenfd = entry.create_socket_and_bind();
    

    CHECK_CALL(listen,listenfd,SOMAXCONN);

    

    while(true)
    {
        socket_address_storage addr;
        int connid = CHECK_CALL(accept,listenfd,&addr.m_addr,&addr.m_addrlen);
        pool.emplace_back([connid]{
            char buf[1024];
            //size_t n = CHECK_CALL(read,connid,buf,sizeof(buf));
            http_request_parser req_parse;
            do{
                
                size_t n = CHECK_CALL(read,connid,buf,sizeof(buf));
                //fmt::println("read {} bytes",n);
                req_parse.push_chunk(std::string_view(buf,n));
            }while(!req_parse.request_finished());

            fmt::println("*****************");
            fmt::println("request headers:{}",req_parse.headers_raw());
            fmt::println(" ");
            fmt::println("request body:{}",req_parse.body());
            std::string body = req_parse.body();
            fmt::println(" ");

            std::string res = "HTTP/1.1 200 OK\r\nServer: co_http\r\nConnection: close\r\nContent-length: "
            +std::to_string(body.size()) +"\r\n\r\n"+body;

            fmt::println("response:{}",res);
            fmt::println("*****************");
            CHECK_CALL(write,connid,res.data(),res.size());
            close(connid);
        });
    }
    for(auto &t:pool){
        t.join();
    }
    return 0;

}