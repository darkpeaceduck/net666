#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <iostream>

#include "transfer.h"
#include "proto.h"
#include "tcp_engine.h"
#include "tcp_log.h"

void transfer_send_to(int raw, struct msg &msg) {
    sockaddr_in addr;
    msg.get_dest_addr(addr);

    LOG() << "transfer send_to sz " << msg.tcp_data_sz() << "\n";
    LOG() << "sizeof(TCPHDR) " << sizeof(TCPHDR) << "\n";
    int ret = sendto (raw, msg.tcp_data_begin(),
         msg.tcp_data_sz(), 0,
         (const struct sockaddr *) &addr,
         sizeof(addr));
    LOG() << "sendto : transfered bytes: " << ret << "\n";
    if (ret != msg.tcp_data_sz()) {
        if (ret < 0)
            throw fuck("tranfer_send_to : internal error");
        else
            throw fuck("tranfer_send_to : sent size  != accepted sent size");
    }
}

void transfer_recvfrom(int raw, struct msg &msg) {
    LOG() << "recfrom maxsize " << msg.get_data_len() << "\n";
    /* here maybe more than raw_data_sz ? */
    sockaddr_in addr;
//    addr.sin_addr.s_addr = inet_addr("123.23.234.55");
    socklen_t _ = sizeof(addr);
    int ret = recvfrom(raw, msg.packet_begin(),
            msg.packet_sz(), 0,
            (struct sockaddr *)&addr, &_);

    LOG() << "recfrom read " << ret << " bytes \n";

//    sockaddr_in tmp;
//    msg.get_src_addr(tmp);
//    std::cerr << inet_ntoa(addr.sin_addr) << " " << inet_ntoa(tmp.sin_addr) << "\n";
      if (ret < 0)
          throw fuck("tranfer_recvfrom : internal error");

      msg.set_data_len(ret - msg.header_len());
      LOG() << "recvfrom : set data len to  " << msg.get_data_len() << " bytes \n";
}
