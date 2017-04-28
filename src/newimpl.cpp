#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <future>
#include <thread>
#include <mutex>
#include <chrono>
#include <iostream>
#include "proto.h"
#include "tcp_internal.h"
#include "transfer.h"
#include "tcp_proto_output.h"
#include "tcp_proto_handshaker.h"

socket_base::socket_base(const socket_identify &id) : id(id) {
    const int hdrincl= 0;

    raw = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    if (raw == -1)
        throw std::runtime_error("socket createt err");

    if ( (setsockopt(raw, IPPROTO_IP, IP_HDRINCL, &hdrincl, sizeof(hdrincl))) < 0) {
        throw std::runtime_error("setsocktopt err");
    };

    if (bind(raw, (struct sockaddr *) &id.in_addr, sizeof(id.in_addr)) < 0) {
        throw std::runtime_error("bind socket err");
    }

    this->max_packet_sz = mtu_binded(raw, this->id.in_addr, TCP_INPUT_MQ_MSGSIZE);
    this->max_data_sz = max_packet_data_size(this->max_packet_sz);

};
socket_base::~socket_base() {
    close(raw);
}

int socket_base::get_fd() {
    return raw;
}

socket_identify socket_base::get_id() {
    return id;
}

socket_sock_base::socket_sock_base(const socket_identify& id, sock::sock_shmem_initializer *initializer) : socket_base(id), initializer(initializer){
    sock::sock_param param;
    param.id = id;
    param.shmem_sz = mem_manager::mem_full_sz();
    default_tcp_mq_attr(param.mq_param);
    param.mq_param.mq_msgsize = this->max_packet_sz;

    this->start_timestamp = sys_time();

    sk = std::unique_ptr<sock>(new sock(param, *initializer));
    manager = mem_manager(sk.get());
}

sock::sock_shmem_initializer * socket_sock_base::get_remembered_initializer() {
    return initializer;
}

size_t socket_sock_base::get_timestamp() {
    return sys_time() - start_timestamp;
}

bool check_msg_corruption(msg &msg) {
    uint16_t chsum_req = msg.get_chsum();
    msg.set_chsum(0);
    uint16_t chsum_has = chsum((uint16_t *)msg.tcp_data_begin(), msg.tcp_data_sz());
    msg.set_chsum(chsum_req);
    LOG() << "socket base : check_msg_corruption : msg seq = " << msg.get_seq()
            << ", full_sz = " << msg.tcp_data_sz() << ", msg chsum = " << chsum_req << ", chsum_calc =  " << chsum_has <<  "\n";
    return chsum_has != chsum_req;
}


void put_msg_to_q(msg & input_msg) {
     struct mq_attr attr;

     sock_mq::param param;
     param.create = 0;
     input_msg.get_src_addr(param.id.out_addr);
     input_msg.get_dest_addr(param.id.in_addr);

     try {
         sock_mq sock_mq(param);
         LOG() << "next_msg : q to " << sock_label(param.id) << "\n";
         sock_mq.getattrs(attr);
         if (attr.mq_msgsize < input_msg.packet_sz()) {
             throw fuck("can't enqueue message : received msg len > sock mq max msg ");
         }
         sock_mq.put_msg(input_msg.packet_begin(), input_msg.packet_sz(),
                 1);
     } catch (fuck &) {
     }
}

std::shared_ptr<msg> socket_sock_base::next_msg(std::chrono::milliseconds timeout_) {

   fd_set fdset;
   struct timeval timeval;
   convert_to_timeval(timeout_, timeval);

   mqd_t mqd = sk->get_mq().mqd();

   FD_ZERO(&fdset);
   FD_SET(raw, &fdset);
   FD_SET(mqd, &fdset);
   int nfsd = std::max(raw, (int)mqd) + 1;

   LOG() << "next_msg : ready to select " << "\n";
   if (select(nfsd, &fdset, NULL, NULL, &timeval) == -1) {
       throw fuck("select");
   }

   LOG() << "next_msg : select completed " << "\n";

   if (FD_ISSET(raw, &fdset) || FD_ISSET(mqd, &fdset)) {
       std::shared_ptr<msg> input_msg(new msg(NULL, max_data_sz));
       if (FD_ISSET(raw, &fdset)) {
          transfer_recvfrom(raw, *input_msg);
          if (!check_msg_corruption(*input_msg))
              put_msg_to_q(*input_msg);
      }

      input_msg->cleanup_data_sz();
      if (FD_ISSET(mqd, &fdset)) {
          unsigned int priority;
          ssize_t rc = sk->get_mq().get_msg(input_msg->packet_begin(), input_msg->packet_sz(), priority);
          input_msg->set_data_len(rc - input_msg->header_len());
          return input_msg;
      }
   }
   throw timeout();
}

void socket_sock_base::enqueue_msg(msg& msg) {
    sk->get_mq().put_msg(msg.packet_begin(), msg.packet_sz(), 1);
}

void socket_sock_base::send(msg& msg) {
    msg.set_src_addr(id.in_addr);
    msg.set_dest_addr(id.out_addr);

    size_t ts = get_timestamp();
    msg.set_recv_timestamp(ts);
    msg.set_sender_timestamp(ts);


    msg.set_chsum(0);
    uint16_t chsum_get = chsum((uint16_t *)msg.tcp_data_begin(), msg.tcp_data_sz());
    msg.set_chsum(chsum_get);

    LOG() << "socket base : send " << msg.raw << ", : set chsum for seq = " << msg.get_seq() << ", full_sz = " << msg.tcp_data_sz() << ", chsum = " << chsum_get << "\n";
    transfer_send_to(raw, msg);
}

size_t socket_sock_base::get_mtu() {
    return max_data_sz;
}

mem_manager & socket_sock_base::get_manager() {
    return manager;
}

class impl_worker : public worker{
public:
    impl_worker(socket_full_impl &impl) : impl(impl) {
    }
    virtual void do_job() = 0;
    virtual ~impl_worker() {
        join();
    };
protected:
    socket_full_impl &impl;
};


/* ==================         SOCKET_FULL_IMPL                 =================================== */
class socket_full_impl::sock_mem_guard  : public sock::sock_shmem_initializer {
    void __load(sock::locks_shmem * locks) {
        send_lock = sock::external_lock_wrapper(&locks->locks[0]);
        recv_lock = sock::external_lock_wrapper(&locks->locks[1]);
        recv_worker_lock =  sock::external_lock_wrapper(&locks->locks[2]);
        send_worker_lock =  sock::external_lock_wrapper(&locks->locks[3]);

        recv_worker_complete_notif = sock::notificator_wrapper(&locks->locks[4]);
        recv_worker_buf_not_empty_notif = sock::notificator_wrapper(&locks->locks[5]);
        send_buf_empty_notif = sock::notificator_wrapper(&locks->locks[6]);
        send_buf_not_empty_notif = sock::notificator_wrapper(&locks->locks[7]);
    }
    static inline int __locks_num() {
        return 4;
    }
    void __init_all() {
        send_lock.init();
        recv_lock.init();
        recv_worker_lock.init();
        send_worker_lock.init();
        recv_worker_complete_notif.init();
        recv_worker_buf_not_empty_notif.init();
        send_buf_empty_notif.init();
        send_buf_not_empty_notif.init();
    }
public:
    sock_mem_guard() :
        send_lock(NULL),
        recv_lock(NULL),
        recv_worker_lock(NULL),
        send_worker_lock(NULL),
        no_lock(NULL, true) {
    }
    virtual void init(void * client_mem, sock::locks_shmem * locks) {
        mem_manager::on_init(client_mem);
        locks->locks_num = __locks_num();
        __load(locks);
        __init_all();
    }
    virtual void load(void * client_mem, sock::locks_shmem * locks) {
        __load(locks);
    }
    template<MEM_TAG E>
    mem_manager::mem_ctl<E> send_protected_area(mem_manager &mn) {
        return mem_manager::mem_ctl<E>(mn, send_lock);
    }
    template<MEM_TAG E>
    mem_manager::mem_ctl<E> recv_protected_area(mem_manager &mn) {
        return mem_manager::mem_ctl<E>(mn, recv_lock);
    }
    template<MEM_TAG E>
    mem_manager::mem_ctl<E> no_lock_area(mem_manager &mn) {
        return mem_manager::mem_ctl<E>(mn, no_lock);
    }
    sock::external_lock_wrapper send_lock;
    sock::external_lock_wrapper recv_lock;
    sock::external_lock_wrapper recv_worker_lock;
    sock::external_lock_wrapper send_worker_lock;
    sock::external_lock_wrapper no_lock;

    sock::notificator_wrapper recv_worker_complete_notif;
    sock::notificator_wrapper recv_worker_buf_not_empty_notif;

    sock::notificator_wrapper send_buf_empty_notif;
    sock::notificator_wrapper send_buf_not_empty_notif;
};


/* =================================           INPUT           ================================ */

/* 1 worker on 1 process
 * we accuire recv_lock in recv() - there are no concurent client_data,
 * when we call wait_while_completed and there are no threads in other processes
 * waiting on wait_while_completed.
 * But there are maybe concurent workers in processes - need to recv_worker_lock
 * diff from recv_lock
 * next_msg call is process-saved-concurency
 *
 * Why we need async worker like this ? 1) it works in  requests in background
 * 2) theoreticaly we can implement non-blocking recv()
 */
class socket_full_impl::input_recv_worker : public impl_worker{
public:
    input_recv_worker(socket_full_impl &impl) : impl_worker(impl) {}
    virtual void do_job() {
        std::chrono::milliseconds timeout_(1000);
        LOG() << "recv worker : started\n";
        while(active) {
            try {
                std::shared_ptr<msg> msg = impl.next_msg(timeout_);
                if (msg->is_usual()) {
                    LOG() << "recv worker : got usual msg\n";
                    impl.input_on_regular_msg(*msg);
                } else if(msg->is_fin() || msg->is_rst()) {
                    LOG() << "recv worker : got fin or rst msg\n";
                    impl.disconnect(false, msg.get());
                } else if (msg->is_ack()){
                    LOG() << "recv worker : got ack msg\n";
                    impl.output_on_ack(*msg);
                } else {
                    LOG() << "recv worker : got wtf msg -> enqueue again\n";
                    impl.enqueue_msg(*msg);
                }
            } catch(timeout &e) {
            }
        }
        LOG() << "recv worker : quited\n";
    }
};

/* ENTER POINT */
void socket_full_impl::input_on_regular_msg(msg &msg) {
    guard->recv_worker_lock.lock();
    tcp_state state = recv_socket_state.get_state(guard->no_lock_area<MEM_TAG::INTERNAL_STATE>(manager));
    if (state == tcp_state::ESTABLISHED) {
        LOG() << " input_on_regular_msg : entered = " << "\n";
        input_window_proc.on_msg(*this,
                        msg,
                        guard->recv_protected_area<INPUT_RECEIVER_WINDOW_PROCESSING>(manager));
    }
    guard->recv_worker_lock.unlock();
}

void socket_full_impl::input_buf_put_data(off_t off, void* data, size_t sz) {
    input_buf_proc.put_data(off, data, sz,
            guard->no_lock_area<INPUT_RECEIVER_BUFFER_PROCESSING>(manager));
}

void socket_full_impl::input_buf_flush(off_t off, void* client_data,
        size_t sz) {
    LOG() << " input_buf_flush : client_data = " << client_data << ", sz = " << sz << "\n";
    input_buf_proc.flush(off, client_data, sz,
                guard->no_lock_area<INPUT_RECEIVER_BUFFER_PROCESSING>(manager));
}

void socket_full_impl::input_win_signal_not_empty() {
    LOG() << " input_win_signal_not_empty : entered " << "\n";
    guard->recv_worker_buf_not_empty_notif.notify();
}

void socket_full_impl::input_win_wait_not_empty() {
    LOG() << " input_win_wait_not_empty : entered " << "\n";
    guard->recv_worker_buf_not_empty_notif.wait();
}

void socket_full_impl::input_win_signal_completely_flushed() {
    LOG() << " input_win_signal_completely_flushed : entered " << "\n";
    guard->recv_worker_complete_notif.notify();
}

void socket_full_impl::input_win_wait_completely_flushed() {
    LOG() << " input_win_wait_completely_flushed : entered " << "\n";
    guard->recv_worker_complete_notif.wait();
}



void socket_full_impl::recv_proc(char * data, size_t sz) {
    while(sz > 0)  {
        size_t add = input_window_proc.client_flush_request(*this,
                data, sz,
                guard->recv_protected_area<INPUT_RECEIVER_WINDOW_PROCESSING>(manager));
        sz -= add;
        data += add;
        LOG() << " recv_proc : flush req completed, sz =  " << sz  << "\n";
    }
}

void socket_full_impl::recv(void* buf, size_t bsz) {
    handshake(true);
    recv_worker->start();
    recv_proc((char*)buf, bsz);
}


/*=================        OUTPUT            ======================== */

/* ======== SENT WIN */

bool socket_full_impl::output_check_msg_complete(msg& msg) {
    return output_send_window_proc.check_msg_complete(*this, msg,
                guard->no_lock_area<MEM_TAG::OUTPUT_CUMULATIVE_WINDOW_PROCESSING>(manager));
}
/* ============= CONG WIN */
size_t socket_full_impl::output_bounded_win_sz() {
    return output_congection_win_control.bounded_win_sz(guard->no_lock_area<MEM_TAG::OUTPUT_RECEIVER_WINDOW_CONTROL>(manager));
}

size_t socket_full_impl::output_get_mtu() {
    return output_congection_win_control.get_mtu(guard->no_lock_area<MEM_TAG::OUTPUT_RECEIVER_WINDOW_CONTROL>(manager));
}
/* ========= SENT BUF */

void *socket_full_impl::output_sent_buf_get_msg_at(off_t off) {
    return output_send_buf.get_msg_at(*this, off,
            guard->no_lock_area<MEM_TAG::OUTPUT_SEND_OUTPUT_BUFFER>(manager));
}

void * socket_full_impl::output_send_buf_next_msg() {
    return output_send_window_proc.next_msg(*this,
            guard->no_lock_area<MEM_TAG::OUTPUT_CUMULATIVE_WINDOW_PROCESSING>(manager));
}

sock::notificator_wrapper socket_full_impl::output_sent_buf_put_msg_at(off_t off, msg& msg) {
    return output_send_buf.put_msg_at(*this, off, msg,
                guard->no_lock_area<MEM_TAG::OUTPUT_SEND_OUTPUT_BUFFER>(manager));
}

void socket_full_impl::output_sent_buf_release_range(off_t begin, off_t end) {
    output_send_buf.release_range(*this, begin, end,
            guard->no_lock_area<MEM_TAG::OUTPUT_SEND_OUTPUT_BUFFER>(manager));
}

void socket_full_impl::output_sent_buf_refresh_contents() {
    output_send_buf.refresh(
                guard->no_lock_area<MEM_TAG::OUTPUT_SEND_OUTPUT_BUFFER>(manager));
}

/* ============== UPPER LEVEL ROUTINES*/


sock::notificator_wrapper socket_full_impl::output_send_as_single_package(void* data, size_t sz) {
    LOG() << " output_send_as_single_package : sz = " << sz << "\n";
    tmp_msg package(data, sz);
    return output_send_window_proc.send(*this, package,
            guard->no_lock_area<MEM_TAG::OUTPUT_CUMULATIVE_WINDOW_PROCESSING>(manager));
}

/*=========================  FINALLY TIMER LOGIC */

void socket_full_impl::output_send_finally(msg &msg) {
    std::chrono::milliseconds timeout_ (output_timeout_computing.get_timeout(
            guard->no_lock_area<MEM_TAG::OUTPUT_TIMEOUT_COMPUTING>(manager)));
    output_send_finally(msg, timeout_);
}

void socket_full_impl::output_send_finally(msg& msg,
        std::chrono::milliseconds timeout_) {
    guard->send_worker_lock.unlock();
    LOG() << " output_send_finally : msg seq = " << msg.get_seq()<< ", timeout ms=  " << timeout_.count()<< " \n";
    socket_sock_base::send(msg);
    std::chrono::milliseconds small(100), summary(0);

    guard->send_worker_lock.lock();
    while(summary.count() < timeout_.count()) {
        if (output_check_msg_complete(msg))
            break;
        guard->send_worker_lock.unlock();
        std::this_thread::sleep_for(small);
        summary += small;
        guard->send_worker_lock.lock();
    }
}

/* ==============  INPLACE MSG LOGIC*/

/* WARNING ! sem_t addr must be aligned to 4 byte boundary */
struct socket_full_impl::inplace_msg {
    sem_t sem;
    bool used;
    char msg_data[];
};

size_t socket_full_impl::get_inplace_msg_size(void*  inp_) {
    inplace_msg * inp = (inplace_msg *)inp_;
    size_t align = 8;

    msg in_msg((void*)inp->msg_data);
    size_t fsz = sizeof(*inp) + in_msg.full_sz();
    return ((fsz / align) + 1) * align;
}


size_t socket_full_impl::put_msg_inplace(void* buf, msg& msg_) {
    LOG() << " put_msg_inplace : entered : msg seq = " << msg_.get_seq()<< ", buf =" << buf << "\n";
    struct inplace_msg * inplace_mem = (inplace_msg * )(buf);
    msg in_msg(msg_, inplace_mem->msg_data);
    sock::notificator_wrapper wrapper(&inplace_mem->sem);

    wrapper.init();
    LOG() << " put_msg_inplace : completed : msg seq = " << msg_.get_seq()<< "\n";
    return get_inplace_msg_size(inplace_mem);
}

void* socket_full_impl::get_msg_inplace(void * buf) {
    struct inplace_msg * inplace_mem = (inplace_msg * )buf;
    msg msg_(inplace_mem->msg_data);
    LOG() << " get_msg_inplace : msg seq = " << msg_.get_seq()<< "\n";
    return inplace_mem->msg_data;
}


sock::notificator_wrapper socket_full_impl::get_msg_inplace_future(void * buf) {
    struct inplace_msg * inplace_mem = (inplace_msg * )buf;
    sock::notificator_wrapper wrapper(&inplace_mem->sem);

    msg msg_(inplace_mem->msg_data);

    LOG() << " get_msg_inplace_future : msg seq = " << msg_.get_seq()<< ", sem addr = " << (&inplace_mem->sem) << "\n";
    return wrapper;
}


size_t socket_full_impl::release_inplace_msg(void* buf) {
    msg tmp(get_msg_inplace(buf));
    output_signal_msg_complete(tmp);
    LOG() << " release_inplace_msg : msg seq = " << tmp.get_seq()<< "\n";
    return get_inplace_msg_size((struct inplace_msg *  )buf);
}

bool socket_full_impl::is_msg_used(void * buf) {
    struct inplace_msg * inplace_mem = (inplace_msg * )buf;
    bool ret = inplace_mem->used;


    msg msg_(inplace_mem->msg_data);
    LOG() << " is_msg_used : msg seq = " << msg_.get_seq()<< ", used = " << ret << "\n";
    return ret;
}

void socket_full_impl::mark_msg_used(void * buf, bool used) {
    struct inplace_msg * inplace_mem = (inplace_msg * )buf;
    inplace_mem->used = used;

    msg msg_(inplace_mem->msg_data);
    LOG() << " mark_msg_used : msg seq = " << msg_.get_seq()<< ", used = " << used << "\n";
}

size_t socket_full_impl::get_msg_data_sz(void * buf) {
    struct inplace_msg * inplace_mem = (inplace_msg * )buf;
    msg msg_(inplace_mem->msg_data);
    size_t ret =  msg_.get_data_len();
    LOG() << "get_msg_data_sz : data_sz = " << ret << "\n";
    return ret;
}

/* =======================  SYNC LOGIC */
class socket_full_impl::output_send_buf_worker: public impl_worker{
public:
    output_send_buf_worker(socket_full_impl &impl) : impl_worker(impl) {}
    void do_job() {
        LOG() << "output_send_buf_worker : entered" << "\n";
        while(active) {
            /* return ref to msg and mark unused in buffer to not to lock than in worker */
            impl.guard->send_worker_lock.lock();
            tcp_state state = impl.send_socket_state.get_state(
                    impl.guard->no_lock_area<MEM_TAG::INTERNAL_STATE>(impl.manager));

            if ( state == tcp_state::ESTABLISHED) {
                try {
                    LOG() << "output_send_buf_worker : ready to take next msg" << "\n";
                    msg msg_ (impl.output_send_buf_next_msg());
                    LOG() << "output_send_buf_worker : send (finnaly) msg" << "\n";
                    impl.output_send_finally(msg_);
                    if (!impl.output_check_msg_complete(msg_)) {
                        LOG() << "output_send_buf_worker : out of send (finnaly) : retrying" << "\n";
                        impl.output_on_retry(msg_);
                    } else {
    //                    LOG() << "output_send_buf_worker : out of send (finnaly) : signal complete" << "\n";
    //                    impl.output_signal_msg_complete(msg_);
                    }
                    impl.guard->send_worker_lock.unlock();
                } catch(...) {
                    LOG() << "output_send_buf_worker : all messaged is on timeout -> go to sleep" << "\n";
                    impl.guard->send_worker_lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            } else {
                impl.guard->send_worker_lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        LOG() << "output_send_buf_worker : exited" << "\n";
    }
};

void socket_full_impl::output_signal_msg_complete(msg &msg) {
    LOG() << "output_signal_msg_complete : entered" << "\n";
    get_msg_inplace_future((char*)msg.raw - offsetof(inplace_msg, msg_data)).notify();
    LOG() << "output_signal_msg_complete : finished " << "\n";
}

void socket_full_impl::on_sent_buf_empty() {
    /* NUM WAITERS = 1 */
    LOG() << "on_sent_buf_empty : entered" << "\n";
    guard->send_buf_empty_notif.notify();
}

void socket_full_impl::output_wait_on_send_buf_empty() {
    LOG() << "output_wait_on_send_buf_empty : entered" << "\n";
    while(!output_send_window_proc.is_sent_buf_empty(guard->no_lock_area<MEM_TAG::OUTPUT_CUMULATIVE_WINDOW_PROCESSING>(manager))) {
        guard->send_worker_lock.unlock();
        guard->send_buf_empty_notif.wait();
//        guard->send_buf_empty_notif.wait_for(std::chrono::milliseconds(3000));
//        if (!output_send_worker->active)
//            throw
        guard->send_worker_lock.lock();
    }
    LOG() << "output_wait_on_send_buf_empty : quit" << "\n";
}

void socket_full_impl::output_sent_buf_signal_not_empty() {
    /* NUM WAITERS = 1 */
    LOG() << "output_sent_buf_signal_not_empty : entered" << "\n";
    guard->send_buf_not_empty_notif.notify();
}
void socket_full_impl::output_sent_buf_wait_not_empty() {
    LOG() << "output_sent_buf_wait_not_empty : entered" << "\n";
    while(output_send_window_proc.is_sent_buf_empty(guard->no_lock_area<MEM_TAG::OUTPUT_CUMULATIVE_WINDOW_PROCESSING>(manager))) {
           guard->send_worker_lock.unlock();
           guard->send_buf_not_empty_notif.wait_for(std::chrono::milliseconds(3000));
           if (!output_send_worker->is_active())
              throw timeout();
           guard->send_worker_lock.lock();
    }
    LOG() << "output_sent_buf_wait_not_empty : quit" << "\n";
}


void socket_full_impl::output_refresh_on_msg_pair(void * inplace_msg_, msg &ack) {
    LOG() << "output_refresh_on_msg_pair : entered" << "\n";
    msg msg_(inplace_msg_);

    output_timeout_computing.refresh(*this, msg_, ack,
                guard->no_lock_area<MEM_TAG::OUTPUT_TIMEOUT_COMPUTING>(manager));
    LOG() << "output_refresh_on_msg_pair for " << msg_.raw << ", seq = " << msg_.get_seq() << ": refreshed" << "\n";
}

void socket_full_impl::output_refresh_on_msg_pair_prev_window_matched(void *inplace_msg_, msg &ack) {
    LOG() << "output_refresh_on_msg_pair_prev_window_matched : entered" << "\n";
//    struct inplace_msg * inplace_mem = (inplace_msg * )inplace_msg_;
    msg msg_(inplace_msg_);
    /* maybe call retry inside*/
    output_fast_retransmit.on_ack(*this, ack, msg_,
                guard->no_lock_area<MEM_TAG::OUTPUT_FAST_RETRANSMITION>(manager));
}

void socket_full_impl::output_on_retry(msg& msg) {
    LOG() << "output_on_retry : entered" << "\n";
    output_congection_win_control.on_retry(
            guard->no_lock_area<MEM_TAG::OUTPUT_RECEIVER_WINDOW_CONTROL>(manager));
    output_send_window_proc.retry(*this, msg,
            guard->no_lock_area<MEM_TAG::OUTPUT_CUMULATIVE_WINDOW_PROCESSING>(manager));
}

/* OUTPUT ENTER POINT */
void socket_full_impl::send(void* buf, size_t bsz) {
    handshake(false);



    output_send_worker->start();
    recv_worker->start();
    guard->send_lock.lock();


    while(bsz > 0) {
        guard->send_worker_lock.lock();
        size_t bound = output_send_window_proc.get_sz(guard->no_lock_area<MEM_TAG::OUTPUT_CUMULATIVE_WINDOW_PROCESSING>(manager));
        bound = std::min(bound, bsz);

        LOG() << "send : ready to get futures" << "\n";

        std::vector<sock::notificator_wrapper> futures =  output_splitter.send(*this, (char*)buf, bound);
        guard->send_worker_lock.unlock();
        LOG() << "send : got futures" << "\n";
        for(auto future : futures) {
            LOG() << "wait future " << future.sem << "\n";
            future.wait();
            LOG() << "future exit " << future.sem << "\n";
        }

        bsz -= bound;
        buf = ((char*)buf) + bound;
        LOG() << "send : iteration complete for bound = " << bound << ", left = " << bsz << "\n";
    }
    guard->send_lock.unlock();
    LOG() << "send : quit" << "\n";
}


/* OUTPUT ENTER POINT */
void socket_full_impl::output_on_ack(msg &msg) {
    guard->send_worker_lock.lock();
    tcp_state state = send_socket_state.get_state(guard->no_lock_area<MEM_TAG::INTERNAL_STATE>(manager));
    if (state == tcp_state::ESTABLISHED) {
        output_congection_win_control.on_ack(msg,
                guard->no_lock_area<MEM_TAG::OUTPUT_RECEIVER_WINDOW_CONTROL>(manager));
        output_send_window_proc.on_ack(*this, msg,
                guard->no_lock_area<MEM_TAG::OUTPUT_CUMULATIVE_WINDOW_PROCESSING>(manager));
    }
    guard->send_worker_lock.unlock();
}

/* OUT OF ROUTINES */
void socket_full_impl::output_send_ack(tcp_seq seq, size_t winsz) {
    LOG() << "output send ack : seq=" << seq << " , winsz = " << winsz << "\n";
    msg msg_(NULL, 0);
    msg_.set_ack();
    msg_.set_seq(seq);
    msg_.set_window_sz(winsz);
    socket_sock_base::send(msg_);
}

/*=========== HANDSHAKE */
void socket_full_impl::handshake(bool recv) {
        struct go {
            establish_handshaker<go> handshaker;
            socket_full_impl & impl;

            go(socket_full_impl & impl) : impl(impl) {
            }

            mem_manager::mem_ctl<MEM_TAG::ESTABLISH_HANDSHAKER> get_ctl() {
                return impl.guard->no_lock_area<MEM_TAG::ESTABLISH_HANDSHAKER>(impl.manager);
            }

            uint32_t generate_rand32() {
                return rand32();
            }
            void output_send_ack(tcp_seq seq, tcp_seq ack_seq) {
                LOG() << "send ack\n";
                msg msg_(NULL, 0);
                msg_.set_ack();
                msg_.set_seq(seq);
                msg_.set_ack_seq(ack_seq);
                msg_.set_window_sz(OUTPUT_WINDOW_SZ);
                impl.::socket_sock_base::send(msg_);
            }
            void send_syn_ack(tcp_seq seq, tcp_seq ack_seq) {
                LOG() << "send syn ack\n";
                msg msg_(NULL, 0);
                msg_.set_syn();
                msg_.set_ack();
                msg_.set_seq(seq);
                msg_.set_ack_seq(ack_seq);
                msg_.set_window_sz(INPUT_WINDOW_SZ);
                impl.::socket_sock_base::send(msg_);
            }

            void send_syn(tcp_seq seq) {
                LOG() << "send syn\n";
               msg msg_(NULL, 0);
               msg_.set_syn();
               msg_.set_seq(seq);
               impl.::socket_sock_base::send(msg_);
           }

            std::shared_ptr<msg> __next_msg() {
                while(1) {
                    try {
                        return impl.::socket_sock_base::next_msg(std::chrono::milliseconds(100));
                    }catch(timeout &) {
                    }
                }
            }
            void next_syn_ack() {
                LOG() << "next syn ack\n";
                while(1) {
                    std::shared_ptr<msg> msg = __next_msg();
                    if (msg->is_syn() && msg->is_ack()) {
                        try {
                            handshaker.send_on_syn_ack(*this, *msg, get_ctl());
                LOG() << "next syn ack exit\n";
                            return;
                        } catch(drop &){
                        }
                    } else {
//                        impl.::socket_sock_base::enqueue_msg(*msg);
//                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }
            }

            void next_syn() {
                LOG() << "next syn\n";
                while(1) {
                    std::shared_ptr<msg> msg = __next_msg();
                    if (msg->is_syn() && !msg->is_ack()) {
                        try {
                            handshaker.recv_on_syn(*this, *msg, get_ctl());
                LOG() << "next syn exit\n";
                            return;
                        } catch(drop &){
                        }
                    } else {
//                        impl.::socket_sock_base::enqueue_msg(*msg);
//                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }
            }

            void next_ack() {
                LOG() << "next ack\n";
               while(1) {
                   std::shared_ptr<msg> msg = __next_msg();
                   if (msg->is_ack() && !msg->is_syn()) {
                       try {
                           handshaker.recv_on_ack(*this, *msg, get_ctl());
               LOG() << "next ack exit\n";
                           return;
                       } catch(drop &){
                       }
                   } else {
//                       impl.::socket_sock_base::enqueue_msg(*msg);
//                       std::this_thread::sleep_for(std::chrono::milliseconds(100));
                   }
               }
           }


            void recv_established(tcp_seq input) {
                impl.input_window_proc.force_move_start(input,  impl.guard->no_lock_area<MEM_TAG::INPUT_RECEIVER_WINDOW_PROCESSING>(impl.manager));
                impl.recv_socket_state.set_state(tcp_state::ESTABLISHED, impl.guard->no_lock_area<MEM_TAG::INTERNAL_STATE>(impl.manager));
                LOG() << "recv_established\n";
            }

            void send_established(tcp_seq output) {
                impl.output_send_window_proc.force_move_start(output,  impl.guard->no_lock_area<MEM_TAG::OUTPUT_CUMULATIVE_WINDOW_PROCESSING>(impl.manager));
                impl.send_socket_state.set_state(tcp_state::ESTABLISHED, impl.guard->no_lock_area<MEM_TAG::INTERNAL_STATE>(impl.manager));
                LOG() << "send_established\n";
            }


            void send_establish() {
                handshaker.send_establish(*this, get_ctl());
            }
            void recv_establish() {
                next_syn();
            }
        } obj(*this);

        if (!recv) {
            guard->send_lock.lock();
            guard->send_worker_lock.lock();
            if (send_socket_state.get_state(guard->no_lock_area<MEM_TAG::INTERNAL_STATE>(manager)) == tcp_state::CLOSED)
                obj.send_establish();
           guard->send_worker_lock.unlock();
           guard->send_lock.unlock();
           LOG() << "send establish out\n";
        }

        if (recv) {
            guard->recv_lock.lock();
            guard->recv_worker_lock.lock();
            if (recv_socket_state.get_state(guard->no_lock_area<MEM_TAG::INTERNAL_STATE>(manager)) == tcp_state::CLOSED) {
                obj.recv_establish();
            }
            guard->recv_worker_lock.unlock();
            guard->recv_lock.unlock();
            LOG() << "recv establish out\n";
        }
}

struct socket_full_impl::close_hadnshaker_keeper {
    close_handshaker<close_hadnshaker_keeper> handshaker;
    socket_full_impl & impl;

    close_hadnshaker_keeper(socket_full_impl & impl) : impl(impl) {
    }

    mem_manager::mem_ctl<MEM_TAG::CLOSE_HANDSHAKER> get_ctl() {
        return impl.guard->no_lock_area<MEM_TAG::CLOSE_HANDSHAKER>(impl.manager);
    }

    uint32_t generate_rand32() {
        return rand32();
    }
    void send_ack(tcp_seq seq) {
        LOG() << "send ack\n";
        msg msg_(NULL, 0);
        msg_.set_ack();
        msg_.set_seq(seq);
        impl.::socket_sock_base::send(msg_);
    }
    void send_fin_ack(tcp_seq seq, tcp_seq ack_seq) {
        LOG() << "send fin ack\n";
        msg msg_(NULL, 0);
        msg_.set_fin();
        msg_.set_ack();
        msg_.set_seq(seq);
        msg_.set_ack_seq(ack_seq);
        impl.::socket_sock_base::send(msg_);
    }

    void send_fin(tcp_seq seq) {
        LOG() << "send fin\n";
       msg msg_(NULL, 0);
       msg_.set_fin();
       msg_.set_seq(seq);
       for(int i = 0; i < 10; i++)
           impl.::socket_sock_base::send(msg_);
   }

    void send_rst(tcp_seq seq) {
        LOG() << "send rst\n";
        msg msg_(NULL, 0);
        msg_.set_rst();
        msg_.set_seq(seq);
        impl.::socket_sock_base::send(msg_);
    }

    void send_rst_ack(tcp_seq seq, tcp_seq ack_seq) {
        LOG() << "send rst ack\n";
        msg msg_(NULL, 0);
        msg_.set_rst();
        msg_.set_ack();
        msg_.set_seq(seq);
        msg_.set_ack_seq(ack_seq);
        impl.::socket_sock_base::send(msg_);
    }


    std::shared_ptr<msg> __next_msg() {
        while(1) {
            try {
                return impl.::socket_sock_base::next_msg(std::chrono::milliseconds(100));
            }catch(timeout &) {
            }
        }
    }



    template<class Pred, class NextFunc>
    void next_msg(Pred msg_predicate, NextFunc next) {
        while(1) {
            std::shared_ptr<msg> msg = __next_msg();
            if (msg_predicate(*msg)) {
                try {
                    next(*msg);
                    return;
                } catch(drop &){
                }
            } else {
//                        impl.::socket_sock_base::enqueue_msg(*msg);
//                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    template<class T>
    auto next_gen(T &&arg_) {
            return [&, f = std::move(arg_)](msg &msg_){
                return (handshaker.*(f))(*this, msg_, get_ctl());
            };
    }

    void next_fin_ack() {
        next_msg([&](msg &msg_) {return msg_.is_fin() && msg_.is_ack();},
                next_gen(&close_handshaker<close_hadnshaker_keeper>::send_on_fin_ack));
    }

    void recv_next_ack() {
        next_msg([&](msg &msg_) {return  !msg_.is_rst() && !msg_.is_fin() && msg_.is_ack();},
                        next_gen(&close_handshaker<close_hadnshaker_keeper>::recv_on_ack));
    }

    void send_next_ack() {
            next_msg([&](msg &msg_) {return !msg_.is_rst() && !msg_.is_fin() && msg_.is_ack();},
                            next_gen(&close_handshaker<close_hadnshaker_keeper>::send_on_ack));
    }

    void next_rst_ack() {
        next_msg([&](msg &msg_) {return !msg_.is_fin() && msg_.is_ack();},
                    next_gen(&close_handshaker<close_hadnshaker_keeper>::recv_on_rst_ack));
    }


    void send_close() {
        handshaker.send_close(*this, get_ctl());
    }

    void recv_close() {
        handshaker.recv_close(*this, get_ctl());
    }

    void recv_on_closing(msg &input) {
        handshaker.recv_on_fin(*this, input, get_ctl());
    }

    void send_on_closing(msg &input) {
        handshaker.send_on_rst(*this, input, get_ctl());
    }


    void recv_direct_closed() {
        recv_closed();
        LOG() << "recv direct closed\n";
    }

    void recv_reverse_closed() {
       recv_closed();
       LOG() << "recv reverse closed\n";
   }
    void send_direct_closed() {
        send_closed();
        LOG() << "send direct closed\n";
    }

    void send_reverse_closed() {
       send_closed();
       LOG() << "send reverse closed\n";
   }

    void recv_closed() {
        impl.recv_socket_state.set_state(tcp_state::CLOSED,
                impl.guard->no_lock_area<MEM_TAG::INTERNAL_STATE>(impl.manager));
    }

    void send_closed() {
        impl.send_socket_state.set_state(tcp_state::CLOSED,
                impl.guard->no_lock_area<MEM_TAG::INTERNAL_STATE>(impl.manager));
    }
};


void socket_full_impl::disconnect(bool recv, msg * input) {
    close_hadnshaker_keeper obj(*this);
    if (input) {
            if (input->is_fin() && !input->is_ack()) {
                guard->recv_lock.lock();
                guard->recv_worker_lock.lock();
                if (recv_socket_state.
                        get_state(guard->no_lock_area<MEM_TAG::INTERNAL_STATE>(manager)) == tcp_state::ESTABLISHED)
                    obj.recv_on_closing(*input);
                guard->recv_worker_lock.unlock();
                guard->recv_lock.unlock();
            }
            if (input->is_rst() && !input->is_ack()) {
                guard->send_lock.lock();
                guard->send_worker_lock.lock();
                if (send_socket_state.
                        get_state(guard->no_lock_area<MEM_TAG::INTERNAL_STATE>(manager)) == tcp_state::ESTABLISHED)
                    obj.send_on_closing(*input);
                guard->send_worker_lock.unlock();
                guard->send_lock.unlock();
            }
    } else {
        if (recv) {
            guard->recv_lock.lock();
            guard->recv_worker_lock.lock();
            if (recv_socket_state.
                   get_state(guard->no_lock_area<MEM_TAG::INTERNAL_STATE>(manager)) == tcp_state::ESTABLISHED)
                obj.recv_close();
            guard->recv_worker_lock.unlock();
            guard->recv_lock.unlock();
        }
        else {
            guard->send_lock.lock();
            guard->send_worker_lock.lock();
            if (send_socket_state.
                   get_state(guard->no_lock_area<MEM_TAG::INTERNAL_STATE>(manager)) == tcp_state::ESTABLISHED)
                obj.send_close();
            guard->send_worker_lock.unlock();
            guard->send_lock.unlock();
        }
    }
}


/* =====================              CONSTRUCTOR / DESTRUCTOR      ======================== */
socket_full_impl::socket_full_impl(const socket_identify& id) :
        socket_sock_base(id, new socket_full_impl::sock_mem_guard()),
        guard(static_cast<socket_full_impl::sock_mem_guard*>(socket_sock_base::get_remembered_initializer())),
        input_window_proc(),
        recv_worker(new socket_full_impl::input_recv_worker(*this)),
        output_send_worker(new socket_full_impl::output_send_buf_worker(*this)) {

    output_congection_win_control = congection_window_control<socket_full_impl, std::vector<output_future>>
            (get_mtu(),  guard->no_lock_area<MEM_TAG::OUTPUT_RECEIVER_WINDOW_CONTROL>(manager));
}

socket_full_impl::~socket_full_impl() {
    recv_worker->set_passive();
    output_send_worker->set_passive();

    recv_worker->join();
    output_send_worker->join();

#if USE_SAFETY_CLOSE
    disconnect(false, NULL);
    disconnect(true, NULL);
#endif
    LOG()  << "socket_full_impl closed\n";
}


/* ========================== SERV ============= */
socket_serv::socket_serv(const sockaddr_in &bind_addr) : socket_base(socket_identify(bind_addr)){
}

socket_serv::~socket_serv() {
}

stream_socket * socket_serv::accept_one_client() {
      std::shared_ptr<msg> input_msg(new msg(NULL, max_data_sz));

      while(1) {
          input_msg->cleanup_data_sz();
          transfer_recvfrom(raw, *input_msg);
          if (!check_msg_corruption(*input_msg)) {
              socket_identify id;
              input_msg->get_src_addr(id.out_addr);
              input_msg->get_dest_addr(id.in_addr);

              std::string label = sock_label(id);
              LOG() << "accept_one_client : trying " << label << "\n";
              if (addr_comparable(id.in_addr, get_id().in_addr)) {
                  socket_full_impl * ret = new socket_full_impl(id);
                  ret->enqueue_msg(*input_msg);
                  LOG() << "accept_one_client : completed " << label << "\n";
                  return ret;
              } else {
                  put_msg_to_q(*input_msg);
                  LOG() << "accept_one_client : enqueud " << label << "\n";
              }
          }
      }
}

/* ========================== CLIENT ============= */

void socket_client::send(void* buf, size_t size) {
    ptr->send(buf, size);
}

void socket_client::recv(void* buf, size_t size) {
    ptr->recv(buf, size);
}

void socket_client::connect() {
    ptr.reset(new socket_full_impl(id));
    ptr->handshake(false);
}

socket_client::socket_client(socket_identify id) : id(id){
}
