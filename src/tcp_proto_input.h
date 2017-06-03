#pragma once

#include <stddef.h>
#include <semaphore.h>
#include <iostream>

#include "tcp_engine.h"
#include "transfer.h"
#include "tcp_proto_mem.h"
#include "tcp_proto_internal.h"
#include "tcp_log.h"


template<class T>
class receiver_window_processing {
    typedef mem_manager::mem_ctl<MEM_TAG::INPUT_RECEIVER_WINDOW_PROCESSING> CTL_TYPE;
    receiver_window win;
    off_t * flushed_off;
    void __setup(CTL_TYPE * ctl) {
        auto ptr = ctl->get_info().ptr;
        win = receiver_window(
                ptr->start,
                ptr->off,
                ptr->sz);
        flushed_off = &ptr->flushed_off;
    }
    void __flush(CTL_TYPE * ctl) {
        auto ptr = ctl->get_info().ptr;
        ptr->start = win.start;
        ptr->off = win.off;
        ptr->sz = win.sz;
    }
    void __on_buffer_flushed(CTL_TYPE * ctl) {
          win.next_window();
          *flushed_off = 0;
          std::cerr << "buf flsuhed\n";
          __flush(ctl);
    }

    size_t __wrote_win_sz() {
        return win.offset() - *flushed_off;
    }

    void __move(T &controller, void * data, size_t sz, CTL_TYPE * ctl) {
        controller.input_buf_put_data(win.offset(), data, sz);
        bool need_not_empty_signal = __wrote_win_sz() == 0;

        win.inc(sz);
        __flush(ctl);

        if (need_not_empty_signal) {
            (*ctl).call(controller, &T::input_win_signal_not_empty);
            __setup(ctl);
        }


        if (win.get_sz() == 0) {
            (*ctl).call(controller, &T::input_win_wait_completely_flushed);
            __setup(ctl);
            __on_buffer_flushed(ctl);
        }

    }
public:
    receiver_window_processing() {}
    void on_msg(T &controller, msg & msg, CTL_TYPE ctl) {
        __setup(&ctl);
        tcp_seq seq = msg.get_seq();
        void * data = msg.get_data();
        size_t data_sz = msg.get_data_len();

        LOG() << "receiver on msg :  seq = " << seq << ", win_begin = " << win.begin() << "\n";

        if (win.contains(receiver_window(seq, data_sz))) {
            if (seq == win.begin()) {
                __move(controller, data, data_sz, &ctl);
                LOG() << "receiver on msg :  msg in window, updated window =  " << win;
            } else {
                LOG() << "receiver on msg :  msg out of window -> skipped\n";
//                controller.enqueue(msg);
            }
        }
        controller.output_send_ack(win.before_begin(), win.get_sz());
    }

    /* called once */
   void force_move_start(tcp_seq start_seq, CTL_TYPE ctl) {
       __setup(&ctl);
       win.start = start_seq;
       win.off = *flushed_off =  0;
       __flush(&ctl);
   }

    size_t client_flush_request(T &controller, void * client_data, size_t req_sz, CTL_TYPE ctl) {
        __setup(&ctl);

        LOG() << "client flush request : entered\n";
        if (__wrote_win_sz() == 0) {
            LOG() << "client flush request : win wait not empty\n";
            ctl.call(controller, &T::input_win_wait_not_empty);
            LOG() << "client flush request : win completed wait not empty\n";
            __setup(&ctl);
        }

        size_t sz = std::min(__wrote_win_sz(), req_sz);

        controller.input_buf_flush(*flushed_off, client_data, sz);
        LOG() << "client flush request : flushed buf\n";

        *flushed_off += sz;
        __flush(&ctl);

        if (__wrote_win_sz() == 0 && win.get_sz() == 0) {
            LOG() << "client flush request : signal compeletely flushed\n";
            ctl.call(controller, &T::input_win_signal_completely_flushed);
            LOG() << "client flush request : end signal compeletely flushed\n";
        }
        LOG() << "client flush request : exit\n";
        return sz;
    }
};

template<class T>
class receiver_buffer_processing {
    typedef mem_manager::mem_ctl<MEM_TAG::INPUT_RECEIVER_BUFFER_PROCESSING> CTL_TYPE;
    void * buf;
    void __setup(CTL_TYPE * ctl) {
        buf = ctl->get_info().ptr->buf;
    }
public:
    void put_data(off_t pos, void * data, size_t sz, CTL_TYPE ctl) {
        __setup(&ctl);
        LOG() << "receiver_buffer_processing : put , pos = " <<pos << " , sz = " << sz << " \n";
        memcpy((char*)buf + pos, data, sz);
    }

    void flush(off_t flushed_off, void * client_data, size_t sz, CTL_TYPE ctl) {
        __setup(&ctl);
        LOG() << "receiver_buffer_processing : flush,  flushed_off = " <<flushed_off << ", sz =" << sz << " \n";
        memcpy(client_data, (char*)buf+flushed_off, sz);
    }
};



