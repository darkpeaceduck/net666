#pragma once

#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <memory>
#include <string.h>
#include <stddef.h>
#include <iostream>

#include "proto.h"

struct msg {
    struct raw_msg_t {
        std::size_t buf_len;
        std::size_t data_len;
        TCP_PACKET packet;
    } __attribute__((packed)) * raw;
    msg(void *inplace) {
        raw = (raw_msg_t*)inplace;
        is_inplace = true;
    }
    msg(msg &other, void * inplace=NULL) {
        if (!inplace)
           raw = (raw_msg_t *)malloc(other.full_sz());
        else {
           raw = (raw_msg_t*)inplace;
           is_inplace = true;
        }
        memcpy(this->raw, other.raw, other.full_sz() - other.get_data_len());
        memcpy(this->raw->packet.buf, other.get_data(), other.get_data_len());
    }
    msg(void * buf, size_t buf_len) {
        size_t fsz  = full_sz_no_obj(buf_len);
        raw = (raw_msg_t *)malloc(fsz);
        if (!raw)
            throw fuck("");
        memset(raw, 0, fsz);
        this->raw->buf_len = this->raw->data_len = buf_len;
        if (buf) {
            memcpy(get_data(), buf, buf_len);
        }
   }
   virtual  ~msg() {
       if (!is_inplace)
           free(raw);
   }
   static size_t full_sz_no_obj(size_t buf_len) {
       return sizeof(struct raw_msg_t) + buf_len;
   }
   size_t full_sz() {
       return full_sz_no_obj(raw->buf_len);
   }
   size_t full_sz_updated_data() {
       return full_sz_no_obj(0) + get_data_len();
   }
   void * packet_begin() {
       return &this->raw->packet;
   }
   size_t header_len() {
       return sizeof(iphdr) + sizeof(TCPHDR);
   }
   void set_data_len(size_t len) {
       raw->data_len = len;
   }
   virtual void * get_data() {
       return ((TCP_PACKET*)packet_begin())->buf;
   }
   size_t get_data_len() {
       return raw->data_len;
   }
   void * tcp_data_begin() {
       return &((TCP_PACKET*)packet_begin())->tcph;
   }
   size_t tcp_data_sz() {
      return sizeof(((TCP_PACKET*)packet_begin())->tcph) + get_data_len();
   }
   size_t packet_sz() {
       return sizeof(raw->packet) + get_data_len();
   }
   void flush_to_orig_buf(void * buf) {
       memcpy(buf, get_data(), get_data_len());
   }
   tcp_seq get_seq() {
       return ((TCP_PACKET*)packet_begin())->tcph.hdr.seq;
   }
   void set_seq(tcp_seq seq) {
       ((TCP_PACKET*)packet_begin())->tcph.hdr.seq = seq;
   }
   tcp_seq get_ack_seq() {
       return ((TCP_PACKET*)packet_begin())->tcph.hdr.ack_seq;
   }
   void set_ack_seq(tcp_seq ack_seq) {
       ((TCP_PACKET*)packet_begin())->tcph.hdr.ack_seq = ack_seq;
   }
   size_t get_window_sz() {
       return ((TCP_PACKET*)packet_begin())->tcph.hdr.window;
   }
   void set_window_sz(uint16_t val) {
       ((TCP_PACKET*)packet_begin())->tcph.hdr.window = val;
   }
   void cleanup_data_sz() {
       raw->data_len = raw->buf_len;
   }
   void set_dest_addr(const struct sockaddr_in &addr) {
       ((TCP_PACKET*)packet_begin())->iph.daddr = addr.sin_addr.s_addr;
       ((TCP_PACKET*)packet_begin())->tcph.hdr.dest = addr.sin_port;
   }
   void get_dest_addr(sockaddr_in &ret) {
       memset(&ret, 0, sizeof(ret));
       ret.sin_family = AF_INET;
       ret.sin_addr.s_addr = ((TCP_PACKET*)packet_begin())->iph.daddr;
       ret.sin_port = ((TCP_PACKET*)packet_begin())->tcph.hdr.dest;
   }
   void set_src_addr(const struct sockaddr_in &addr) {
       ((TCP_PACKET*)packet_begin())->iph.saddr = addr.sin_addr.s_addr;
       ((TCP_PACKET*)packet_begin())->tcph.hdr.source = addr.sin_port;
  }
  void get_src_addr(sockaddr_in &ret) {
      memset(&ret, 0, sizeof(ret));
      ret.sin_family = AF_INET;
      ret.sin_addr.s_addr = ((TCP_PACKET*)packet_begin())->iph.saddr;
      ret.sin_port = (((TCP_PACKET*)packet_begin())->tcph.hdr.source);
  }
  bool is_ack() {
      return ((TCP_PACKET*)packet_begin())->tcph.hdr.ack > 0;
  }
  bool is_syn() {
      return ((TCP_PACKET*)packet_begin())->tcph.hdr.syn > 0;
  }
  bool is_usual() {
      return !is_ack() && !is_syn() && !is_fin() && !is_rst();
  }
  bool is_fin() {
      return ((TCP_PACKET*)packet_begin())->tcph.hdr.fin > 0;
  }
  bool is_rst() {
      return ((TCP_PACKET*)packet_begin())->tcph.hdr.rst > 0;
  }
  size_t get_sender_timestamp() {
      return ((TCPHDR *)tcp_data_begin())->sender_timestamp;
  }
  void set_sender_timestamp(size_t val) {
      ((TCPHDR *)tcp_data_begin())->sender_timestamp = val;
  }
  size_t get_recv_timestamp() {
      return ((TCPHDR *)tcp_data_begin())->recv_timestamp;
  }
  void set_recv_timestamp(size_t val) {
      ((TCPHDR *)tcp_data_begin())->recv_timestamp = val;
  }

  void set_syn() {
      ((TCP_PACKET*)packet_begin())->tcph.hdr.syn = 1;
  }
  void set_ack() {
      ((TCP_PACKET*)packet_begin())->tcph.hdr.ack = 1;
  }
  void set_fin() {
      ((TCP_PACKET*)packet_begin())->tcph.hdr.fin = 1;
  }
  void set_rst() {
        ((TCP_PACKET*)packet_begin())->tcph.hdr.rst = 1;
  }
  uint16_t get_chsum() {
      return ((TCP_PACKET*)packet_begin())->tcph.hdr.check;
  }
  void set_chsum(uint16_t sum) {
        ((TCP_PACKET*)packet_begin())->tcph.hdr.check = sum;
  }
  bool is_inplace = false;
};

struct tmp_msg : public msg{
    void *saved_data;
    tmp_msg(void * data, size_t sz) : msg(NULL, 0) {
        saved_data = data;
        raw->data_len = raw->buf_len = sz;
    }
    virtual void* get_data() {
        return saved_data;
    }
};

void transfer_send_to(int raw, struct msg &msg);
void transfer_recvfrom(int raw, struct msg &msg);
