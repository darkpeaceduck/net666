#pragma once
#include "tcp_proto_mem.h"
#include "tcp_proto_internal.h"
#include "transfer.h"

class internal_state {
    typedef mem_manager::mem_ctl<MEM_TAG::INTERNAL_STATE> CTL_TYPE;
    tcp_state * state;
    void __setup(CTL_TYPE * ctl) {
          auto ptr = ctl->get_info().ptr;
          state = &ptr->state;
    }
public:
    void set_state(tcp_state state_set, CTL_TYPE ctl) {
        __setup(&ctl);
        *state = state_set;
    }
    tcp_state get_state(CTL_TYPE ctl) {
            __setup(&ctl);
            return *state ;
    }
};

template<class T>
class establish_handshaker {
    typedef mem_manager::mem_ctl<MEM_TAG::ESTABLISH_HANDSHAKER> CTL_TYPE;
    tcp_seq * send_remebered_seq;
    tcp_seq * recv_remebered_seq;
    tcp_seq * recv_remebered_ack_seq;

    void __setup(CTL_TYPE * ctl) {
      auto ptr = ctl->get_info().ptr;
      send_remebered_seq = &ptr->send_remebered_seq;
      recv_remebered_seq = &ptr->recv_remebered_seq;
      recv_remebered_ack_seq = &ptr->recv_remebered_ack_seq;
    }

public:
    void recv_on_syn(T &controller, msg &syn, CTL_TYPE ctl) {
        __setup(&ctl);
        *recv_remebered_seq = controller.generate_rand32();
        *recv_remebered_ack_seq =  syn.get_seq() + 1;
        controller.send_syn_ack(*recv_remebered_seq, *recv_remebered_ack_seq);
        controller.next_ack();
    }
    void recv_on_ack( T &controller, msg &ack, CTL_TYPE ctl) {
        __setup(&ctl);
        if (ack.get_seq() == *recv_remebered_ack_seq &&
                ack.get_ack_seq() == *recv_remebered_seq + 1) {
            controller.recv_established(ack.get_ack_seq());
        } else
            throw drop();
    }
    void send_on_syn_ack(T &controller, msg &ack, CTL_TYPE ctl) {
        __setup(&ctl);
        if (ack.get_ack_seq() ==  *send_remebered_seq + 1) {
            controller.output_send_ack(ack.get_ack_seq(), ack.get_seq() + 1);
            controller.send_established(ack.get_seq() + 1);
        } else {
            throw drop();
        }
    }
    void send_establish(T & controller, CTL_TYPE ctl) {
        __setup(&ctl);
        *send_remebered_seq = controller.generate_rand32();
        controller.send_syn(*send_remebered_seq);
        controller.next_syn_ack();
    }
};

/* 3 - way fin_ack handshaker without full duplex support */
template<class T>
class close_handshaker {
    typedef mem_manager::mem_ctl<MEM_TAG::CLOSE_HANDSHAKER> CTL_TYPE;
    tcp_seq * send_remebered_seq;
    tcp_seq * recv_remebered_seq;
    void __setup(CTL_TYPE * ctl) {
         auto ptr = ctl->get_info().ptr;
         send_remebered_seq = &ptr->send_remebered_seq;
         recv_remebered_seq = &ptr->recv_remebered_seq;
   }
public:
    /* === DIRECT WAY */
    void recv_on_fin(T &controller, msg &fin, CTL_TYPE ctl) {
        __setup(&ctl);
        *recv_remebered_seq = controller.generate_rand32();
        controller.send_fin_ack(*recv_remebered_seq, fin.get_seq());
        controller.recv_next_ack();
    }
    void recv_on_ack(T &controller, msg &ack, CTL_TYPE ctl) {
        __setup(&ctl);
        if (ack.get_seq() == *recv_remebered_seq ) {
            controller.recv_direct_closed();
        } else
            throw drop();
    }
    void send_on_fin_ack(T &controller, msg &ack, CTL_TYPE ctl) {
        __setup(&ctl);
        if (ack.get_ack_seq() ==  *send_remebered_seq ) {
            controller.send_ack(ack.get_seq());
            controller.send_direct_closed();
        } else
            throw drop();
    }
    void send_close(T & controller, CTL_TYPE ctl) {
        __setup(&ctl);
        *send_remebered_seq = controller.generate_rand32();
        controller.send_fin(*send_remebered_seq);
        controller.next_fin_ack();
    }

    /* ==== REVERSE WAY */

    void send_on_rst(T &controller, msg &fin, CTL_TYPE ctl) {
        __setup(&ctl);
        *recv_remebered_seq = controller.generate_rand32();
        controller.send_rst_ack(*recv_remebered_seq, fin.get_seq());
        controller.send_next_ack();
    }
    void send_on_ack(T &controller, msg &ack, CTL_TYPE ctl) {
        __setup(&ctl);
        if (ack.get_seq() == *recv_remebered_seq ) {
            controller.send_reverse_closed();
        } else
            throw drop();
    }
    void recv_on_rst_ack(T &controller, msg &ack, CTL_TYPE ctl) {
        __setup(&ctl);
        if (ack.get_ack_seq() ==  *send_remebered_seq ) {
            controller.send_ack(ack.get_seq());
            controller.send_reverse_closed();
        } else
            throw drop();
    }
    void recv_close(T & controller, CTL_TYPE ctl) {
        __setup(&ctl);
        *send_remebered_seq = controller.generate_rand32();
        controller.send_rst(*send_remebered_seq);
        controller.next_rst_ack();
    }
};
