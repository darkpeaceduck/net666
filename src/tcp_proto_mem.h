#pragma once
#include <stddef.h>
#include <semaphore.h>

#include "tcp_engine.h"
#include "transfer.h"
#include "tcp_proto_internal.h"
#include "tcp_log.h"


enum MEM_TAG {
    MEM_TAG_BEGIN,
    INTERNAL_STATE,
    OUTPUT_RECEIVER_WINDOW_CONTROL,
    OUTPUT_RECEIVER_WINDOW_NAGLES,
    OUTPUT_CUMULATIVE_WINDOW_PROCESSING,
    OUTPUT_SEND_OUTPUT_BUFFER,
    OUTPUT_TIMEOUT_COMPUTING,
    OUTPUT_FAST_RETRANSMITION,
    INPUT_RECEIVER_WINDOW_PROCESSING,
    INPUT_RECEIVER_BUFFER_PROCESSING,
    ESTABLISH_HANDSHAKER,
    CLOSE_HANDSHAKER,
    MEM_TAG_END,
    RESERVED
};

template<MEM_TAG E>
struct mem_area {
    struct area{
    }__attribute__((aligned(8)));
    static void init(area * ptr) {
    }
};

template<>
struct mem_area<MEM_TAG::INPUT_RECEIVER_WINDOW_PROCESSING>  {
    struct area{
        size_t start;
        off_t off;
        size_t sz;

        off_t flushed_off;
    }__attribute__((aligned(8)));
    static void init(area * ptr) {
        ptr->start = ptr->off = 0;
        ptr->sz = INPUT_WINDOW_SZ;
    }
};

template<>
struct mem_area<MEM_TAG::INPUT_RECEIVER_BUFFER_PROCESSING>  {
    struct area{
        off_t off;
        char buf[INPUT_BUF_SZ];
    }__attribute__((aligned(8)));
    static void init(area * ptr) {
        ptr->off = 0;
    }
};


template<>
struct mem_area<MEM_TAG::OUTPUT_RECEIVER_WINDOW_CONTROL>  {
    struct area{
        size_t receiver_win_sz;
        size_t congection_win_sz;
        size_t mtu;
    }__attribute__((aligned(8)));
    static void init(area * ptr) {
        ptr->mtu = 0;
        ptr->receiver_win_sz = OUTPUT_WINDOW_SZ;
        ptr->congection_win_sz = OUTPUT_CONGECTION_START_SZ;
    }
};

template<>
struct mem_area<MEM_TAG::OUTPUT_RECEIVER_WINDOW_NAGLES>  {
    struct area{
    }__attribute__((aligned(8)));
    static void init(area * ptr) {
    }
};

template<>
struct mem_area<MEM_TAG::OUTPUT_CUMULATIVE_WINDOW_PROCESSING>  {
    struct area{
        size_t start;
        off_t off;
        off_t send_off;
        size_t sz;

    }__attribute__((aligned(8)));
    static void init(area * ptr) {
        ptr->start = ptr->off = ptr->send_off = 0;
        ptr->sz = OUTPUT_WINDOW_SZ;
    }
};

template<>
struct mem_area<MEM_TAG::OUTPUT_SEND_OUTPUT_BUFFER>  {
    struct area{
        char buf[OUTPUT_SEND_BUF_SZ];
        off_t off_map[OUTPUT_SEND_BUF_SZ];
        off_t end;
    }__attribute__((aligned(8)));
    static void init(area * ptr) {
    }
};


template<>
struct mem_area<MEM_TAG::OUTPUT_TIMEOUT_COMPUTING>  {
    struct area{
        size_t SRTT;
        size_t RTTVAR;
        size_t nm;
    }__attribute__((aligned(8)));
    static void init(area * ptr) {
    }
};

template<>
struct mem_area<MEM_TAG::OUTPUT_FAST_RETRANSMITION>  {
    struct area{
        tcp_seq last;
        int cnt;
    }__attribute__((aligned(8)));
    static void init(area * ptr) {
        ptr->cnt = 0;
    }
};

template<>
struct mem_area<MEM_TAG::ESTABLISH_HANDSHAKER> {
    struct area{
        tcp_seq send_remebered_seq;
        tcp_seq recv_remebered_seq;
        tcp_seq recv_remebered_ack_seq;
    }__attribute__((packed));
    static void init(area * ptr) {
    }
};


template<>
struct mem_area<MEM_TAG::CLOSE_HANDSHAKER> {
    struct area{
        tcp_seq send_remebered_seq;
        tcp_seq recv_remebered_seq;
    }__attribute__((aligned(8)));
    static void init(area * ptr) {
    }
};

template<>
struct mem_area<MEM_TAG::INTERNAL_STATE> {
    struct area{
        tcp_state state;
    }__attribute__((aligned(8)));

    static void init(area * ptr) {
        ptr->state = CLOSED;
    }
};




#if 0
class locks_intilizator : public sock::sock_shmem_initializer {
    sock::external_lock_wrapper send_lock;
    sock::external_lock_wrapper recv_lock;
public:
    virtual void init(void * client_mem, sock::locks_shmem * locks)  {

    }
           /* call from sock constructor under socket lock */
    virtual void load(void * client_mem, sock::locks_shmem * locks)  {

    }
};
#endif

template<MEM_TAG E>
struct mem_info{
    static constexpr MEM_TAG prev = (MEM_TAG)(E - 1);
    static constexpr size_t sz() {
        return sizeof(typename mem_area<E>::area);
    };
    static constexpr size_t summary_prefix_off() {
        return mem_info<prev>::summary_prefix_off() + mem_info<prev>::sz();
    };
    static void init(void * ptr) {
        mem_area<E>::init((typename mem_area<E>::area*)((char*)ptr + summary_prefix_off()));
        mem_info<prev>::init(ptr);
    }
    mem_info(typename mem_area<E>::area * ptr) : ptr(ptr) {}
    typename mem_area<E>::area * ptr;
};

template<>
struct mem_info<MEM_TAG_BEGIN>{
    static constexpr size_t sz() {
        return 0;
    }
    static constexpr size_t summary_prefix_off() {
        return 0;
    }
    static void init(void *ptr) {}
};


class mem_manager {
public:
    template<MEM_TAG E>
    struct mem_ctl {
        mem_ctl(mem_manager &manager, sock::external_lock_wrapper &lock) :
                lock_(lock),
                manager(manager),
                info(manager.mem_get<E>(lock)) {
            }
        ~mem_ctl() {
            manager.mem_put<E>(lock_);
        }
        mem_info<E> &get_info() {
            return info;
        }
        void lock() {
            lock_.lock();
        }
        void unlock() {
            lock_.unlock();
        }
        template<class T, typename Ftype, class ... Args>
        void call(T &t, Ftype f, Args ... args) {
           unlock();
           (t.*f)(args...);
           lock();
       }
    private:
      mem_manager &manager;
      mem_info<E> info;
      sock::external_lock_wrapper lock_;
    };
private:
    template<MEM_TAG E>
    typename mem_area<E>::area* area(void * global_mem) {
        LOG() << "mem manager : get  area for off " << mem_info<E>::summary_prefix_off() << "\n";
        return (typename mem_area<E>::area*)((off_t)global_mem + mem_info<E>::summary_prefix_off());
    }
public:
    mem_manager() : sk(NULL) {}
    mem_manager(sock * sk) :
        sk(sk){
    };
    template<MEM_TAG E>
    typename mem_area<E>::area * mem_get(sock::external_lock_wrapper &lock) {
        bool internal = false;
        if (lock.have())
            lock.lock();
        else
            internal = true;

        void * global_mem = sk->accquire_shmem(internal);
        LOG() << "mem manager : mem get for " << global_mem << "\n";
        return area<E>(global_mem);
    }
    template<MEM_TAG E>
    void mem_put(sock::external_lock_wrapper &lock) {
        if (lock.have())
            lock.unlock();
        else
            sk->put_internal_lock();
    }
    static size_t mem_full_sz() {
        size_t res = mem_info<MEM_TAG_END>::summary_prefix_off();
        LOG() << "mem manager : mem full sz is" << res<< "\n";
        return res;
    }
    static void on_init(void *global_mem) {
        mem_info<mem_info<MEM_TAG_END>::prev>::init(global_mem);
    }
private:
    sock * sk;
};
