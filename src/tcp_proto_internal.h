#pragma once
#include <iostream>
#include "tcp_log.h"

#define INPUT_WINDOW_SZ 1000
#define INPUT_BUF_SZ 300000
#define OUTPUT_WINDOW_SZ 1000
#define OUTPUT_CONGECTION_START_SZ 100
#define OUTPUT_NAGLES_BUF_SZ 100
#define OUTPUT_SEND_BUF_SZ 300000
#define TCP_OUTPUT_START_TIMEOUT 3000
#define TCP_FAST_RETRANSMIT_BOUND 4
#define TCP_HANDSHAKE_TIMEOUT 3000
#define TCP_HANDSHAKE_CLIENT_SYN_SEQ 0
#define TCP_HANDSHAKE_SERVER_SYN_SEQ 1000

enum tcp_state {
    CLOSED,
    SYN_SENT,
    SYN_RECEIVED,
    ESTABLISHED
};

struct receiver_window {
    size_t start;
    off_t off;
    size_t sz;
    static const size_t modulo = (size_t)std::numeric_limits<tcp_seq>().max() + 1;
    receiver_window(size_t start, off_t off, size_t sz) : start(start), off(off), sz(sz) {}
    receiver_window() : receiver_window(0, 0, 0){}
    receiver_window(size_t start, size_t sz) : receiver_window(start, 0, sz){}
    virtual ~receiver_window() {}


    off_t offset() {
        return off;
    }

    virtual void next_window() {
        off = 0;
        start += sz;
    }

    size_t end() {
        return start + sz;
    }

    size_t begin() {
        return start + off;
    }

    size_t before_begin() {
        size_t b = begin();
        if (b == 0)
            return modulo - 1;
        return b - 1;
    }


    void inc(off_t inc) {
        off += inc;
    }

    size_t get_start_sz() {
        return sz;
    }

    size_t get_sz() {
        return sz - off;
    }

    void extend_end(size_t add) {
        sz += add;
    }



    bool contains(receiver_window win) {
       size_t begin_v = begin();
       size_t end_v = end();
       size_t start_win = win.begin();
       size_t end_win = win.end();

       if (((tcp_seq) end_v) < begin_v && end_win < (tcp_seq)end_v) {
           if ((tcp_seq)end_win > start_win)
               start_win += modulo;
           end_win += modulo;
       }

       LOG() << "receiver win contains : for " << *this;
       LOG() << "with " << win;
       LOG() << "numbers "<< begin_v << " " << end_v <<
               " " << start_win << " " << end_win << "\n";
       LOG() << "receiver win contains end\n" ;
       return begin_v <= start_win && end_v >= end_win;
   }

    static receiver_window SINGLE_WINDOW(size_t start) {
        return receiver_window(start, 1);
    }

    friend std::ostream& operator<<(std::ostream& os, receiver_window& a);
};

inline std::ostream& operator<<(std::ostream& os, receiver_window & i) {
        return os << "receive window print : start " << i.start << ", off " << i.off
                << ", sz " << i.sz <<", \n";
}


struct send_window : public receiver_window{
    off_t sent_off;
    send_window() : receiver_window() {
        sent_off = 0;
    }
    send_window(size_t start, off_t off, off_t sent_off, size_t sz) :
        receiver_window(start, off, sz),
        sent_off(sent_off) {}

    void inc_sent_off(off_t add) {
        sent_off += add;
    }
    size_t sent_begin() {
        return start + sent_off;
    }

    size_t sent_sz() {
        return sz - sent_off;
    }

    size_t get_in_msg_off(tcp_seq seq){
        size_t add =0 ;
        size_t start_modulo = ((tcp_seq)begin());
                if (start_modulo <= seq)
                    add = seq - start_modulo;
                else
                    add = modulo - start_modulo + seq;
        return add;
    }
    void set_off_from_seq(tcp_seq seq) {
        receiver_window::inc(get_in_msg_off(seq) + 1);
    }
    virtual void next_window() {
        receiver_window::next_window();
        sent_off = 0;
    }
    friend std::ostream& operator<<(std::ostream& os, send_window& a);
};

inline std::ostream& operator<<(std::ostream& os, send_window & i) {
        return os << "send window print : start " << i.start << ", off " << i.off << ", sent_off =  " << i.sent_off
                << ", sz " << i.sz <<", \n";
}
