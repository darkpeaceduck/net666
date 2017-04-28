#pragma once
#include <mqueue.h>
#include <string>
#include <stdexcept>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <string.h>

struct socket_identify {
   struct sockaddr_in in_addr;
   struct sockaddr_in out_addr;
   socket_identify(const sockaddr_in &in) {
       in_addr = in;
       memset(&out_addr, 0, sizeof(out_addr));
   }
   socket_identify() {}
};


#define INPUT_MQ_MAXMSG 100
#define INPUT_MQ_CURMSGS 0
#define INPUT_MQ_FLAGS 0
#define INPUT_MQ_MSGSIZE 100


struct TCPHDR {
    struct tcphdr hdr;
    uint64_t sender_timestamp;
    uint64_t recv_timestamp;
} __attribute__((packed));

struct TCP_PACKET {
        struct iphdr iph;
        struct TCPHDR tcph;
        char buf[];
} __attribute__((packed));

#define TCP_INPUT_MQ_MAXMSG 100
#define TCP_INPUT_MQ_CURMSGS 0
#define TCP_INPUT_MQ_FLAGS 0
#define TCP_INPUT_MQ_MSGSIZE (65534 + sizeof(TCP_PACKET))

#define TCP_OUTPUT_MAXWINDOW (65534 + sizeof(TCP_PACKET))

class fuck : public std::runtime_error {
public:
    fuck(const std::string &msg) : std::runtime_error(msg + " " + std::string(strerror(errno))) {}
};

class timeout : public std::exception {
};

class drop : public std::exception {

};


static inline void default_mq_attr(mq_attr & attr) {
    attr.mq_flags = INPUT_MQ_FLAGS;
    attr.mq_maxmsg = INPUT_MQ_MAXMSG;
    attr.mq_msgsize = INPUT_MQ_MSGSIZE;
    attr.mq_curmsgs = INPUT_MQ_CURMSGS;
}

static inline void default_tcp_mq_attr(mq_attr & attr) {
    attr.mq_flags = TCP_INPUT_MQ_FLAGS;
    attr.mq_maxmsg = TCP_INPUT_MQ_MAXMSG;
    attr.mq_msgsize = TCP_INPUT_MQ_MSGSIZE;
    attr.mq_curmsgs = TCP_INPUT_MQ_CURMSGS;
}

static inline size_t max_packet_data_size(int mtu) {
    return mtu -  sizeof(TCP_PACKET);
}
