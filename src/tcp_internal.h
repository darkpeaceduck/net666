#pragma once

#include "stream_socket.h"
#include "transfer.h"
#include "tcp_engine.h"
#include "tcp_proto_input.h"
#include "tcp_proto_mem.h"
#include "tcp_proto_output.h"
#include "tcp_proto_handshaker.h"

class socket_base {
public:
    socket_base(const socket_identify &id) ;
    virtual ~socket_base();
    int get_fd();
    socket_identify get_id();
protected:
    int raw;
    const socket_identify id;
    size_t max_packet_sz;
    size_t max_data_sz;
};

class socket_sock_base : public socket_base {
public:
    socket_sock_base(const socket_identify &id, sock::sock_shmem_initializer *initializer);
    std::shared_ptr<msg> next_msg(std::chrono::milliseconds timeout_);
    void enqueue_msg(msg &msg);
    void send(msg &msg);
    sock::sock_shmem_initializer * get_remembered_initializer();
    mem_manager & get_manager();
protected:
    std::unique_ptr<sock> sk;
    size_t start_timestamp;
    size_t get_timestamp();
    size_t get_mtu();
private:
    sock::sock_shmem_initializer *initializer;
protected:
    mem_manager manager;
};


class socket_full_impl : public socket_sock_base, public stream_socket {
    class input_client_worker;
    class output_send_buf_worker;
    class input_recv_worker;
    class sock_mem_guard ;
    struct inplace_msg;
    struct close_hadnshaker_keeper;
    typedef sock::notificator_wrapper output_future;

    void input_on_regular_msg(msg &msg);
    void input_buf_put_data(off_t off, void * data, size_t sz);
    void input_win_signal_not_empty();
    void input_win_wait_not_empty();
    void input_win_signal_completely_flushed();
    void input_win_wait_completely_flushed();
    void input_buf_flush(off_t off, void * client_data, size_t sz);

    void recv_proc(char * data, size_t sz);


    size_t output_bounded_win_sz();
    size_t output_get_mtu();
    output_future output_send_as_single_package(void *data, size_t sz);
    output_future output_sent_buf_put_msg_at(off_t off, msg &msg);

    void *output_sent_buf_get_msg_at(off_t);


    void output_sent_buf_release_range(off_t begin, off_t end);
    void output_send_finally(msg &msg, std::chrono::milliseconds answer_timeout);
    void output_send_finally(msg &msg);
    void output_on_retry(msg &msg);
    void* output_send_buf_next_msg();
    bool output_check_msg_complete(msg &msg);
    void output_sent_buf_refresh_contents();
    void output_refresh_on_msg_pair(void * inplace_msg, msg &ack);
    void output_refresh_on_msg_pair_prev_window_matched(void *inplace_msg, msg &ack);

    void on_sent_buf_empty();
    void output_wait_on_send_buf_empty();

    void output_sent_buf_signal_not_empty();
    void output_sent_buf_wait_not_empty();

    void output_signal_msg_complete(msg &msg);

    void output_send_ack(tcp_seq seq, size_t winsz);

    size_t put_msg_inplace(void * buf, msg & msg);
    size_t get_inplace_msg_size(void*  inp);
    size_t release_inplace_msg(void * buf);
    void* get_msg_inplace(void * buf);
    output_future get_msg_inplace_future(void * buf);
    bool is_msg_used(void * buf);
    void mark_msg_used(void * buf, bool used);
    size_t get_msg_data_sz(void * buf);

    void output_on_ack(msg &msg);

    friend class receiver_window_processing<socket_full_impl>;
    friend class receiver_buffer_processing<socket_full_impl>;
    friend class send_output_buffer<socket_full_impl, void*, output_future>;
    friend class send_cumulative_window_processing<socket_full_impl, void*, output_future>;
    friend class nagles<socket_full_impl, output_future>;
    friend class congection_window_control<socket_full_impl, std::vector<output_future> >;
    friend class timeout_computing<socket_full_impl>;
    friend class fast_retransmit<socket_full_impl>;
    friend class establish_handshaker<socket_full_impl>;
public:
    socket_full_impl(const socket_identify &id);
    ~socket_full_impl() ;
    void send(void * buf, size_t bsz);
    void recv(void * buf, size_t bsz);
    void handshake(bool recv);
    void disconnect(bool recv, msg * input);
private:
    std::unique_ptr<input_recv_worker> recv_worker;
    std::unique_ptr<output_send_buf_worker> output_send_worker;
    std::unique_ptr<sock_mem_guard> guard;


    receiver_window_processing<socket_full_impl> input_window_proc;
    receiver_buffer_processing<socket_full_impl> input_buf_proc;

    send_output_buffer<socket_full_impl, void*, output_future> output_send_buf;
    send_cumulative_window_processing<socket_full_impl, void*, output_future> output_send_window_proc;
    nagles<socket_full_impl, output_future> output_splitter;
    congection_window_control<socket_full_impl, std::vector<output_future>> output_congection_win_control;
    timeout_computing<socket_full_impl> output_timeout_computing;
    fast_retransmit<socket_full_impl> output_fast_retransmit;
    internal_state recv_socket_state;
    internal_state send_socket_state;
};


class socket_serv  : public socket_base, public stream_server_socket{
public:
    socket_serv(const sockaddr_in &bind_addr);
    ~socket_serv();
    stream_socket * accept_one_client();
};

class socket_client : public stream_client_socket {
    std::unique_ptr<socket_full_impl> ptr;
    socket_identify id;
public:
    socket_client(socket_identify id);
    virtual void send(void *buf, size_t size) ;
    virtual void recv(void *buf, size_t size) ;
    virtual void connect() ;
};
