#pragma once
#include <vector>
#include <algorithm>
#include "tcp_proto_mem.h"
#include "transfer.h"
#include "tcp_proto_internal.h"
#include "tcp_log.h"



template<class T, class V, class E>
class send_cumulative_window_processing  {
    typedef mem_manager::mem_ctl<MEM_TAG::OUTPUT_CUMULATIVE_WINDOW_PROCESSING> CTL_TYPE;
    send_window win;

    void __setup(CTL_TYPE * ctl) {
      auto ptr = ctl->get_info().ptr;
      win = send_window(
              ptr->start,
              ptr->off,
              ptr->send_off,
              ptr->sz);
    }
    void __flush(CTL_TYPE * ctl) {
      auto ptr = ctl->get_info().ptr;
      ptr->start = win.start;
      ptr->send_off = win.sent_off;
      ptr->off = win.off;
      ptr->sz = win.sz;
    }
    bool __msg_in_win(msg &msg) {
        return win.contains(receiver_window::SINGLE_WINDOW(msg.get_seq()));
    }
    bool __msg_in_prev_win(msg &msg) {
        return win.contains(receiver_window::SINGLE_WINDOW((size_t)msg.get_seq() + 1));
    }
    bool __sent_buf_empty() {
        return win.sent_off == win.off;
    }

    void __check_msg_overhead_throwable(msg &msg) {
        if (win.sent_sz() < msg.get_data_len()) {
            throw drop();
        }
    }
public:
    size_t get_sz(CTL_TYPE ctl) {
        __setup(&ctl);
        return win.get_sz();
    }
    /* called once */
    void force_move_start(tcp_seq start_seq, CTL_TYPE ctl) {
        __setup(&ctl);
        win.start = start_seq;
        win.off = win.sent_off = 0;
        __flush(&ctl);
    }
    bool is_sent_buf_empty(CTL_TYPE ctl) {
        __setup(&ctl);
           return __sent_buf_empty();
    }
    E send(T &controller, msg &msg, CTL_TYPE ctl) {
        __setup(&ctl);
        __check_msg_overhead_throwable(msg);


        msg.set_seq(win.sent_begin());

        LOG() << "cm send set msg seq : " << msg.get_seq() << "\n";
        if (__sent_buf_empty()) {
            LOG() << "cm send : signal not empty\n";
            controller.output_sent_buf_signal_not_empty();
        }

        E ret = controller.output_sent_buf_put_msg_at(win.sent_off, msg);


        win.inc_sent_off(msg.get_data_len());
        LOG() << "cm send : winow afrer send " << win;
        __flush(&ctl);
        return ret;
    }

    void retry(T &controller, msg &msg, CTL_TYPE ctl) {
        __setup(&ctl);
        LOG() << "cm retry: for seq " << msg.get_seq() << "\n";
        off_t msg_off = win.get_in_msg_off(msg.get_seq()) + win.offset();
        LOG() << "cm retry: in window off " << msg_off << "\n";
        V ret = controller.output_sent_buf_get_msg_at(msg_off);
        controller.mark_msg_used(ret, false);
        LOG() << "cm retry: mark msg as notused at " << msg_off << "\n";
        __flush(&ctl);
    }

    bool check_msg_complete(T &controller, msg &msg, CTL_TYPE ctl) {
        __setup(&ctl);
        bool ret = !__msg_in_win(msg);
        __flush(&ctl);
        return ret;
    }

    void on_ack(T &controller, msg &msg,  CTL_TYPE ctl) {
        __setup(&ctl);
        if (__msg_in_win(msg)) {
            LOG() << "cm on ack: msg "<< msg.get_seq() << " in window  "<<  win << "\n";
            off_t old_off = win.offset();
            win.set_off_from_seq(msg.get_seq());
            off_t new_off = win.offset();
            LOG() << "cm on ack: move off " << old_off<<" -> " << new_off <<  "\n";

            V ret = controller.output_sent_buf_get_msg_at(old_off);
            controller.output_refresh_on_msg_pair(ret, msg);
            controller.output_sent_buf_release_range(old_off, new_off - 1);
            if (__sent_buf_empty()) {
                LOG() << "cm on ack: signal on buf empty \n";
                controller.on_sent_buf_empty();
            }
            if (win.get_sz() == 0) {
                win.next_window();
                LOG() << "cm on ack : window exceeded -> go to next window : " << win;
                controller.output_sent_buf_refresh_contents();
            }
        } else if (__msg_in_prev_win(msg)) {
            V first_msg = controller.output_sent_buf_get_msg_at(win.offset());
            LOG() << "cm on ack: msg is in prev window, refresing pair at, winoff=" << win.offset() << ",  msg_seq = " << msg.get_seq() << "\n";
            controller.output_refresh_on_msg_pair_prev_window_matched(first_msg, msg);
        }
        __flush(&ctl);
    }

    V next_msg(T &controller, CTL_TYPE ctl) {
        __setup(&ctl);
        if (__sent_buf_empty()) {
            LOG() << "cm next msg : wait buf not empty\n";
            controller.output_sent_buf_wait_not_empty();
        }
        off_t off = win.offset();
        while(off < win.sent_off) {
            V ret = controller.output_sent_buf_get_msg_at(off);
            LOG() << "cm next msg : get msg at " << off << "\n";
            if (controller.is_msg_used(ret)) {
                off += controller.get_msg_data_sz(ret);
                LOG() << "cm next msg : msg is used -> go next " << off << "\n";
            } else {
                controller.mark_msg_used(ret, true);
                LOG() << "cm next msg : mark as used\n";
                __flush(&ctl);
                   return ret;
            }
        }
        throw timeout();
    }
};

template<class T, class V, class E>
class send_output_buffer {
    typedef mem_manager::mem_ctl<MEM_TAG::OUTPUT_SEND_OUTPUT_BUFFER> CTL_TYPE;
    char * buf;
    off_t * off_map;
    off_t * end;
    void __setup(CTL_TYPE * ctl) {
        auto ptr = ctl->get_info().ptr;
        buf = ptr->buf;
        off_map = ptr->off_map;
        end = &ptr->end;
    }
    off_t real_off(off_t off) {
        return off_map[off];
    }
    void put_real_off(off_t client_off, off_t real_off) {
        LOG() << "sent buf : add mapping client_off = " << client_off<< ", real_off " << real_off << "\n";
        off_map[client_off] = real_off;
    }
public:
    void refresh(CTL_TYPE ctl) {
        __setup(&ctl);
        LOG() << "sent buf : refreshing \n";
        *end = 0;
        put_real_off(0, 0);
    }
    V get_msg_at(T &controller, off_t off, CTL_TYPE ctl) {
        __setup(&ctl);
        LOG() << "sent buf : get msg at  = " << off<< ", real_off =" << real_off(off) << "\n";
        return controller.get_msg_inplace(buf + real_off(off));
    }
    E put_msg_at(T &controller, off_t off,  msg &msg, CTL_TYPE ctl) {
        __setup(&ctl);
        size_t final_sz  = controller.put_msg_inplace(buf + real_off(off), msg);
        LOG() << "sent buf : out msg at " << off << ", inplace_sz = " << final_sz << "\n";
        *end += final_sz;
        put_real_off(off + msg.get_data_len(), *end);
        /* on selective add here pair (off, final_sz) */
        return controller.get_msg_inplace_future(buf + real_off(off));
    }
    void release_range(T &controller, off_t begin_, off_t end_, CTL_TYPE ctl) {
        __setup(&ctl);
        size_t off = real_off(begin_);
        size_t real_end = real_off(end_ + 1);
        LOG() << "sent buf : release range " << begin_ << ", end = " << end_<< ", real_off = " << off << ", real_end=" << real_end << ",end = " << *end << "\n";
        while(off < real_end) {
            size_t sz  = controller.release_inplace_msg(buf + off);
            LOG() << "sent buf : released msg at " <<  off << ", sz= " << sz<< "\n";
            off += sz;
        }
        LOG() << "sent buf : released complated\n";
    }
};

template<class T, class E>
class nagles {
    typedef mem_manager::mem_ctl<MEM_TAG::OUTPUT_RECEIVER_WINDOW_NAGLES> CTL_TYPE;
public:
    std::vector<E> send(T &controller, char * data, size_t data_sz) {
        std::vector<E> ret;
        while(data_sz > 0) {
            size_t mtu = controller.output_get_mtu();
            size_t win_sz = controller.output_bounded_win_sz();
            if (win_sz > mtu && data_sz >= mtu) {
                try {
                    ret.push_back(controller.output_send_as_single_package(data, mtu));
                } catch(drop &) {
                    controller.output_wait_on_send_buf_empty();
                    continue;
                }
                data_sz -= mtu;
                data += mtu;
                LOG() << "nagles : send full mtu = " << mtu << " , left " << data_sz << "\n";
            } else {
                size_t segment = std::min(mtu, std::min(win_sz, data_sz));
                LOG() << "nagles : waiting while send buf empty \n";
                controller.output_wait_on_send_buf_empty();

                /* cant drop here (buffer default sz >mtu ) */
                ret.push_back(controller.output_send_as_single_package(data, segment));
                data_sz -= segment;
                data += segment;
                LOG() << "nagles : send " << segment << "  , left " << data_sz << "\n";
            }
        }
        return ret;
    }
};


/* AIMD (congection window) + receiver window control
 */
template<class T, class E>
class congection_window_control {

    static const size_t mult_arg = 2;
    static const size_t add_arg = 1;

    typedef mem_manager::mem_ctl<MEM_TAG::OUTPUT_RECEIVER_WINDOW_CONTROL> CTL_TYPE;
    size_t * receiver_win_sz;
    size_t  *congection_win_sz;
    size_t * mtu;
  void __setup(CTL_TYPE * ctl) {
      auto ptr = ctl->get_info().ptr;
      receiver_win_sz = &ptr->receiver_win_sz;
      congection_win_sz = &ptr->congection_win_sz;
      mtu = &ptr->mtu;
  }
public:
     congection_window_control () {}
     congection_window_control(size_t mtu, CTL_TYPE ctl) {
        __setup(&ctl);
        if (*this->mtu == 0) {
            LOG() << "mtu unsetted, applying " << mtu << "\n";
            *this->mtu = mtu;
        }
    }

     size_t get_mtu(CTL_TYPE ctl) {
         __setup(&ctl);
         return *mtu;
     }

     size_t bounded_win_sz(CTL_TYPE ctl) {
         __setup(&ctl);
         return std::min(*receiver_win_sz, *congection_win_sz);
     }

    void on_retry(CTL_TYPE ctl) {
        __setup(&ctl);
        LOG() << "congection_window_control : on retry, old_congection_win sz " << *congection_win_sz << " ";
        *congection_win_sz /= 2;
        *congection_win_sz = std::max(*congection_win_sz, (size_t)1);
        LOG() << ", new size  " << *congection_win_sz << "\n";
    }
    /* have allready accepted from low levels */
    void on_ack(msg &msg, CTL_TYPE ctl) {
        __setup(&ctl);
        LOG() << "congection_window_control : on ack, old_congection_win sz " << *congection_win_sz << " ";
        *receiver_win_sz = msg.get_window_sz();
        *congection_win_sz += add_arg * *mtu;
        LOG() << "setup receiver_win_sz    " << *receiver_win_sz << " , ";
        LOG() << "setup congection_win_sz   " << *congection_win_sz << "\n";

    }
};

template<class T>
class
 timeout_computing {
private:
    size_t * SRTT, *RTTVAR, *nm;
    constexpr static double alpha = 0.125;
    constexpr static double beta = 0.25;
    static const size_t Gkof = 300;
    const static size_t K = 4;
    typedef mem_manager::mem_ctl<MEM_TAG::OUTPUT_TIMEOUT_COMPUTING> CTL_TYPE;

    void __setup(CTL_TYPE *ctl) {
        auto ptr = ctl->get_info().ptr;
        SRTT = &ptr->SRTT;
        RTTVAR = &ptr->RTTVAR;
        nm = &ptr->nm;
    }

    void __clean_compute(size_t R) {
        *SRTT = R ;
        *RTTVAR = R/ 2;
        LOG() << "tcp timeout_computing(clean) for " << R << " SRTT = " << *SRTT << " RTTVAR =" << *RTTVAR << "\n";
    }

    void __noclean_compute(size_t R) {
        *RTTVAR = ((1-beta) * (double)(*RTTVAR) + beta * (double)std::abs((ssize_t)(*SRTT) - (ssize_t)R));
        *SRTT = (1-alpha) * (double)(*SRTT) + alpha * (double)R;
        LOG() << "tcp timeout_computing(noclean) for " << R << " SRTT = " << *SRTT << " RTTVAR =" << *RTTVAR << "\n";
    }

    size_t __get_rto() {
        return *SRTT + std::max((size_t)Gkof, K * (*RTTVAR));
    }

    void __compute_timeout(size_t R) {
        if (*nm == 0) {
           __clean_compute(R);
        } else {
            __noclean_compute(R);
        }
        (*nm)++;
    }
public:
    size_t get_timeout(CTL_TYPE ctl) {
        __setup(&ctl);
        size_t ret = 0;
        if (*nm == 0)
            ret = TCP_OUTPUT_START_TIMEOUT;
        else
            ret = __get_rto();
        LOG() << "send timeout is " << ret << "\n";
        return ret;
    }
    void refresh(T &controller, msg &sender, msg &ack, CTL_TYPE ctl) {
        __setup(&ctl);
        ssize_t sender_timestamp = sender.get_sender_timestamp();
        ssize_t current_timestamp = controller.get_timestamp();

        ssize_t conv_ack_recv_timestamp = ack.get_recv_timestamp();
        ssize_t conv_ack_sender_timestamp = ack.get_sender_timestamp();

        LOG() << "tcp timeout refreshing , send_ts=" << sender_timestamp <<
                " cur_ts= " << current_timestamp << " host recv = " << conv_ack_recv_timestamp <<
                "  host_send = " << conv_ack_sender_timestamp << "\n";

        ssize_t R = (conv_ack_recv_timestamp  - conv_ack_sender_timestamp) +
                (current_timestamp -  sender_timestamp);

        __compute_timeout(R);
    }
};

template<class T>
class output_control_flow_persist_timer {
    /* throws timeout exception */
public:
    void on_ack(T&controller, msg &msg, bool ping_only=false) {
            size_t winsz = msg.get_window_sz();
            if (winsz == 0) {
                if (!ping_only) {
                    LOG() << "set ping only(winsz == 0)\n";
                    controller.output_set_ping_only(true);
                    controller.output_wait_win_sz_not_zero();
                }
            } else {
                if (ping_only) {
                    LOG() << "unset ping only from ping worker\n";
                    controller.output_set_ping_only(false);
                    controller.output_signal_win_sz_not_zero();
                }
            }
    }
};



template<class T>
class fast_retransmit  {
    typedef mem_manager::mem_ctl<MEM_TAG::OUTPUT_FAST_RETRANSMITION> CTL_TYPE;
    tcp_seq * last;
    int * cnt;
    void __setup(CTL_TYPE *ctl) {
       auto ptr = ctl->get_info().ptr;
       last = &ptr->last;
       cnt = &ptr->cnt;
   }
public:
    void on_ack(T&controller, msg &ack, msg &sender, CTL_TYPE ctl) {
        __setup(&ctl);
        tcp_seq seq = ack.get_seq();
        if (*cnt == 0) {
            *last = seq;
            *cnt = 1;
        } else {
            if (seq == *last) {
                (*cnt)++;
                if (*cnt >= TCP_FAST_RETRANSMIT_BOUND) {
                    *cnt = 0;
                    LOG() << "fast_retransmit go retry\n";
                    controller.output_on_retry(sender);
                }
            } else {
                *cnt = 1;
                *last = seq;
            }
        }
        LOG() << "fast_retransmit : after cnt = " << *cnt << " last = " << *last << "\n" ;
    }
};
