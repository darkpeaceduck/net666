#include "tcp_internal.h"
#include "stream_socket.h"
#include <iostream>
#include <map>

class transf {
    struct raw {
        size_t len;
        char data[];
    }__attribute__((packed));
    stream_socket * sock;
public :
    transf(  stream_socket * sock) : sock(sock) {}
    void send(std::string msg) {
        raw * data = (raw*)malloc(sizeof(raw) + msg.length());
        data->len = msg.length();
        memcpy(data->data, msg.c_str(), data->len);
        sock->send(data, sizeof(*data) + data->len);
        free(data);
    }
    std::string recv() {
        size_t sz;
        sock->recv(&sz, sizeof(sz));
        char * data = new char[sz + 1];
        sock->recv(data, sz);
        data[sz] = 0;
        std::string ret(data);
        delete data;
        return ret;
    }
};

const std::string EOFSTR = "fin";

void client(socket_identify id) {
    socket_client client(id);
    client.connect();
    transf tr(&client);
    std::string next = "";
    while(1) {
        std::cin >> next;
        tr.send(next);
        std::cout << "=================== CLIENT : SENT STRING " << next << "\n";
        std::string ret = tr.recv();
        std::cout << "=================== CLIENT : GOT STRING " << ret << "\n";
        if (next == EOFSTR)
              break;
    }
    std::cout << "CLIENT EXITS\n";
}

std::mutex m;
std::map<uint32_t, std::shared_ptr<stream_socket>> serv_map;

void serv_thread(stream_socket * sk) {
    std::cout << "SERV THREAD STARTED\n";
    transf tr(sk);
    while(1) {
        std::string ret = tr.recv();
        std::cout << "=================== SERV : GOT STRING " << ret << "\n";
        std::string ans = ret;

        std::unique_lock<std::mutex> lock(m);
        for(auto &item : serv_map) {
            transf(item.second.get()).send(ans);
            std::cout << "=================== SERV : SNT STRING " << ans << ", to " << item.first <<  "\n";
        }
        if (ret == EOFSTR)
             break;
    }
}
void serv_accept(socket_serv &serv) {
    while(1) {
        stream_socket * sk = serv.accept_one_client();
        std::unique_lock<std::mutex> lock(m);
        bool next = false;
        uint32_t port = ntohs(((socket_full_impl*)sk)->get_id().out_addr.sin_port);
        std::cout << "ACCEPTING CLIENT AT " << port << "\n";
        if (!serv_map.count(port)) {
            next = true;
            serv_map[port] = std::shared_ptr<stream_socket>(sk);
        }
        lock.unlock();
        if (next) {
            std::thread t(serv_thread, sk);
            t.detach();
        }

    }
}

void serv(const sockaddr_in & id) {
    socket_serv serv(id);
    serv_accept(serv);
}

int main(int argc, char* argv[]) {
    std::string role(argv[1]);
    if (role == "serv") {
        int port = atoi(argv[2]);
        std::cout << "role serv, port = "<< port << "\n";

        socket_identify id;
        fill_param("127.0.0.1", port, id.in_addr);
        serv(id.in_addr);
    } else {
        int local_port = atoi(argv[2]);
        int remote_port = atoi(argv[3]);
        std::cout << "role client, local port = " << local_port << " remote port = " << remote_port << "\n";

        socket_identify id;
        fill_param("127.0.0.1", local_port, id.in_addr);
        fill_param("127.0.0.1", remote_port, id.out_addr);
        client(id);
    }
}
