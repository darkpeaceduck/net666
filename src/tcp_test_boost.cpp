#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#define BOOST_TEST_MODULE MyTest
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "proto.h"
#include "transfer.h"
#include "tcp_engine.h"
#include "tcp_internal.h"
#include "tcp_proto_mem.h"
#include "tcp_proto_input.h"
#include "tcp_proto_internal.h"
#include "tcp_proto_handshaker.h"

#include <iostream>

namespace utf = boost::unit_test;

#if TRANSFER_COMPLETLY_NO_LOST
    static const bool transfer_no_lost = true;
#else
    static const bool transfer_no_lost = false;
#endif


static void test_mq_data(sock_mq &mq) {
       int data[INPUT_MQ_MAXMSG];

       for(int i = 0; i < INPUT_MQ_MAXMSG; i++)
           data[i] = i + 1;

       for(int i = 0; i < INPUT_MQ_MAXMSG; i++)
           mq.put_msg(data + i, sizeof(int), 1);
       for(int i = 0; i < INPUT_MQ_MAXMSG; i++) {
           int rec;
           unsigned int pr;
           mq.get_msg((void *)&rec, sizeof(int), pr);
           BOOST_REQUIRE_EQUAL(data[i], rec);
           BOOST_REQUIRE_EQUAL(pr, 1);
       }
}
BOOST_AUTO_TEST_CASE( mq_test )
{
    sock_mq::param param;
    fill_param("127.0.0.1", 0, param.id.in_addr);
    fill_param("127.0.0.1", 0, param.id.out_addr);
    param.create = true;
    default_mq_attr(param.created_attr);

    sock_mq::param rdonly_param = param;
    rdonly_param.create = false;

    for(int i = 0; i < 10; i++) {
        BOOST_REQUIRE_NO_THROW(sock_mq mq(param); sock_mq mq2(rdonly_param));
    }

    {
    BOOST_REQUIRE_THROW(sock_mq mq(rdonly_param), fuck);
    }

    BOOST_REQUIRE_NO_THROW(
    sock_mq::param int_param = param;
    int_param.created_attr.mq_msgsize = sizeof(int);
    sock_mq mq(int_param);
    test_mq_data(mq);
    );
}

/* DEFAULT MQ TCP PARAMS DEPENDS ON setrlimit on max mq size */
BOOST_AUTO_TEST_CASE( mq_test_long_msg)
{
    unsigned int no;
    sock_mq::param param;
    fill_param("127.0.0.1", 0, param.id.in_addr);
    fill_param("127.0.0.1", 0, param.id.out_addr);
    param.create = true;
    default_tcp_mq_attr(param.created_attr);

    sock_mq mq(param);
    size_t msg_sz = param.created_attr.mq_msgsize;
    void * data = malloc(msg_sz);

    mq.put_msg(data, msg_sz, 1);
    mq.get_msg(data, msg_sz, no);

    free(data);
}


BOOST_AUTO_TEST_CASE(sock_test)
{
    sock::sock_param param;
    fill_param("127.0.0.1", 0, param.id.in_addr);
    fill_param("127.0.0.1", 0, param.id.out_addr);
    default_mq_attr(param.mq_param);
    param.mq_param.mq_msgsize = sizeof(int);

    class _: public sock::sock_shmem_initializer {
        virtual void init(void * client_mem, sock::locks_shmem * locks) {}
               /* call from sock constructor under socket lock */
        virtual void load(void * client_mem, sock::locks_shmem * locks) {}
    } shmem_init;

    for(int iter = 0; iter < 2; iter++)
    {
            sock::sock_param loc_param = param;
            const size_t dsz = 100;
            int data[dsz];
            int clean_data[dsz];
            memset(clean_data, 0, sizeof(clean_data));

            for(int i = 0; i < dsz; i++)
                data[i] = i + 100;


            loc_param.shmem_sz = sizeof(data);

            sock sk(loc_param, shmem_init);
            test_mq_data(sk.get_mq());
            void * mem = sk.accquire_shmem(true);
            BOOST_CHECK_EQUAL(0, memcmp(mem, clean_data, loc_param.shmem_sz));
            memcpy(mem, data, loc_param.shmem_sz);
            BOOST_CHECK_EQUAL(0, memcmp(mem, data, loc_param.shmem_sz));
            sk.put_internal_lock();

            sock sk2(loc_param, shmem_init);
            BOOST_CHECK_EQUAL(0, memcmp(sk2.accquire_shmem(true), data, loc_param.shmem_sz));
            sk2.put_internal_lock();

            BOOST_CHECK_EQUAL(0, memcmp(sk.accquire_shmem(true), data, loc_param.shmem_sz));
            sk.put_internal_lock();

            /* simulate close sk */
            sock * sk3 = new sock(loc_param, shmem_init);
            delete sk3;

            BOOST_CHECK_THROW(sk.accquire_shmem(true), fuck);
            BOOST_CHECK_THROW(sk2.accquire_shmem(true), fuck);
    }
}

BOOST_AUTO_TEST_CASE(transfer_test,
        * utf::enable_if<transfer_no_lost>())
{
    for(int iter = 0; iter < 2; iter++) {
        socket_identify id;
        fill_param("127.0.0.1", 1234, id.in_addr);
        fill_param("127.0.0.1", 80, id.out_addr);
        socket_base sock(id);

        size_t bsz = 100;
        char outpu_buf[bsz];
        char input_buf[bsz];
        memset(input_buf, 0, bsz);

        for(int i = 0; i < bsz; i++)
            outpu_buf[i] = i;
        msg output_msg(outpu_buf, bsz);
        output_msg.set_dest_addr(id.out_addr);
        BOOST_REQUIRE_NO_THROW(transfer_send_to(sock.get_fd(), output_msg));

        output_msg.flush_to_orig_buf(input_buf);
        BOOST_REQUIRE_EQUAL(0, memcmp(outpu_buf, input_buf, bsz));
        memset(input_buf, 0, bsz);

        msg input_msg(input_buf, bsz);
        BOOST_REQUIRE_NO_THROW(transfer_recvfrom(sock.get_fd(), input_msg));

        input_msg.flush_to_orig_buf(input_buf);
        BOOST_REQUIRE_EQUAL(0, memcmp(outpu_buf, input_buf, bsz));

    }
}

BOOST_AUTO_TEST_CASE(socket_sock_base_test,
        * utf::enable_if<transfer_no_lost>())
{
    class _: public sock::sock_shmem_initializer {
            virtual void init(void * client_mem, sock::locks_shmem * locks)  {}
                   /* call from sock constructor under socket lock */
            virtual void load(void * client_mem, sock::locks_shmem * locks)  {}
    } shmem_init;
    for(int iter = 0; iter < 2; iter++) {
        socket_identify id;
        fill_param("127.0.0.1", 1234, id.in_addr);
        fill_param("127.0.0.1", 80, id.out_addr);
        socket_sock_base sock(id, &shmem_init);

        size_t bsz = 100;
       char outpu_buf[bsz];
       char input_buf[bsz];

       for(int i = 0; i < bsz; i++)
           outpu_buf[i] = i;

       msg output_msg(outpu_buf, bsz);

       sock.send(output_msg);

       while(1) {
           try {
              std::shared_ptr<msg> input = sock.next_msg(std::chrono::seconds(5));
              input->flush_to_orig_buf(input_buf);
              BOOST_REQUIRE_EQUAL(0, memcmp(outpu_buf, input_buf, bsz));
              break;
           } catch(timeout &){
           }
        }
    }
}

template<>
struct mem_area<MEM_TAG::RESERVED> {
    struct area{
        char data[100];
    }__attribute__((packed));
    void init(struct area * _) {}
};


BOOST_AUTO_TEST_CASE(mem_manager_test)
{
    BOOST_REQUIRE_EQUAL(mem_info<MEM_TAG::RESERVED>::sz(), sizeof(mem_area<MEM_TAG::RESERVED>::area));
    sock::sock_param param;
    fill_param("127.0.0.1", 123, param.id.in_addr);
    fill_param("127.0.0.1", 90, param.id.out_addr);
    default_mq_attr(param.mq_param);
    param.mq_param.mq_msgsize = sizeof(int);

    size_t dsz = 100;
    char data[100];
    for(int i = 0; i < dsz; i++)
           data[i] = i + 100;

    size_t iter = 3;
    struct _: public sock::sock_shmem_initializer {
        virtual void init(void * client_mem, sock::locks_shmem * locks) {
              locks->locks_num = 1;
              wrapper = sock::external_lock_wrapper(&locks->locks[0]);
              wrapper.init();
              times++;
        }
                   /* call from sock constructor under socket lock */
        virtual void load(void * client_mem, sock::locks_shmem * locks) {
            wrapper = sock::external_lock_wrapper(&locks->locks[0]);
        }
        _() : wrapper(NULL) {}
        sock::external_lock_wrapper wrapper;
        size_t times = 0;
    } shmem_init;

    for(size_t i = 0; i < iter; i++) {
        param.shmem_sz = mem_manager::mem_full_sz();
        sock sk (param, shmem_init);
        mem_manager mn(&sk);

        {
            mem_manager::mem_ctl<MEM_TAG::RESERVED> ctl(mn, shmem_init.wrapper);
            memcpy(ctl.get_info().ptr->data, data, dsz);
            return;
        }

        {
            mem_manager::mem_ctl<MEM_TAG::RESERVED> ctl(mn, shmem_init.wrapper);
            BOOST_REQUIRE_EQUAL(0, memcmp(ctl.get_info().ptr->data, data, dsz));
        }
    }
    BOOST_REQUIRE_EQUAL(iter, shmem_init.times);
}

BOOST_AUTO_TEST_CASE(receiver_window_test)
{
    BOOST_REQUIRE(receiver_window(0, 100).contains(receiver_window(10, 50)));
    BOOST_REQUIRE(!receiver_window(0, 100).contains(receiver_window(10, 110)));
    BOOST_REQUIRE(!receiver_window(50, 100).contains(receiver_window(40, 60)));
    BOOST_REQUIRE(!receiver_window(50, 100).contains(receiver_window(40, 110)));
    tcp_seq limit = std::numeric_limits<tcp_seq>().max() + 1;
    BOOST_REQUIRE(receiver_window(limit - 100, 200).contains(receiver_window(limit - 50, 60)));
    BOOST_REQUIRE(receiver_window(limit - 100, 200).contains(receiver_window(50, 40)));
    BOOST_REQUIRE(!receiver_window(limit - 100, 200).contains(receiver_window(limit - 50, 600)));
    BOOST_REQUIRE(!receiver_window(limit - 100, 200).contains(receiver_window(limit - 110, 40)));
    BOOST_REQUIRE(!receiver_window(limit - 100, 200).contains(receiver_window(limit - 110, 120)));
    BOOST_REQUIRE(!receiver_window(limit - 100, 200).contains(receiver_window(limit - 110, 600)));
    BOOST_REQUIRE(!receiver_window(limit - 100, 200).contains(receiver_window(110, 600)));
}

BOOST_AUTO_TEST_CASE(send_window_test)
{
    tcp_seq limit = (size_t)std::numeric_limits<tcp_seq>().max() + 1;
    {
        send_window win(1, 0, 0, 100);
        win.set_off_from_seq(1);
        BOOST_REQUIRE(win.offset() == 1);
        BOOST_REQUIRE(win.get_sz() == 99);
        BOOST_REQUIRE(win.sent_off == 0);
        BOOST_REQUIRE(win.start == 1);
    }
    {
        send_window win(limit - 1, 0, 0, 100);
        win.set_off_from_seq(1);
        BOOST_REQUIRE(win.offset() == 3);
        BOOST_REQUIRE(win.get_sz() == 97);
        BOOST_REQUIRE(win.sent_off == 0);
        BOOST_REQUIRE(win.start == limit - 1);
    }
}

BOOST_AUTO_TEST_CASE(chsum_test) {
    size_t bsz = 97;
    char input_buf[bsz];
    memset(input_buf, 0, bsz);

    for(int i = 0; i < bsz; i++)
      input_buf[i] = i;
    for(int i = 0; i < bsz; i++) {
        uint16_t old = chsum((uint16_t*)input_buf, i + 1);
        input_buf[i]++;
        uint16_t new_ = chsum((uint16_t*)input_buf, i + 1);
        input_buf[i]--;
        BOOST_REQUIRE_NE(old, new_);

    }
}

 template< MEM_TAG V>
    struct fake_controller_base {
         class _: public sock::sock_shmem_initializer {
                     virtual void init(void * client_mem, sock::locks_shmem * locks)  {
                         mem_manager::on_init(client_mem);
                     }
                            /* call from sock constructor under socket lock */
                     virtual void load(void * client_mem, sock::locks_shmem * locks)  {}
        };
        socket_sock_base sock;
        sock::external_lock_wrapper lock;
        std::unique_ptr<sock::sock_shmem_initializer> tmp;
        mem_manager::mem_ctl<V>  get_ctl() {
            return mem_manager::mem_ctl<V>(sock.get_manager(), lock);
        }
        fake_controller_base(socket_identify id) : sock(id, new _()),
                        lock(NULL, true),
                        tmp(sock.get_remembered_initializer())
                {}
        uint32_t generate_rand32() {
            return rand32();
        }

    };



BOOST_AUTO_TEST_CASE(established_handshaker_test) {
    socket_identify id;
    fill_param("127.0.0.1", 1234, id.in_addr);
    fill_param("127.0.0.1", 80, id.out_addr);
#if 0
    struct fake_controller_complete : public fake_controller_base<MEM_TAG::ESTABLISH_HANDSHAKER> {
            establish_handshaker<fake_controller_complete> handshaker;
            bool dropped;
            fake_controller_complete(socket_identify id) : fake_controller_base(id) {
                cleanup();
            }
            void send_ack(tcp_seq seq, tcp_seq ack_seq, int timeout) {
                msg msg_(NULL, 0);
                msg_.set_ack();
                msg_.set_seq(seq + dropped);
                msg_.set_ack_seq(ack_seq);
                handshaker.recv_on_ack(*this, msg_, get_ctl());
            }
            void send_syn_ack(tcp_seq seq, tcp_seq ack_seq, int timeout) {
                msg msg_(NULL, 0);
                msg_.set_syn();
                msg_.set_ack();
                msg_.set_seq(seq);
                msg_.set_ack_seq(ack_seq);
                handshaker.send_on_syn_ack(*this, msg_, get_ctl());
            }
            void send_syn(tcp_seq seq, tcp_seq ack_seq, int timeout) {
                msg msg_(NULL, 0);
                msg_.set_syn();
                msg_.set_seq(seq);
                msg_.set_ack_seq(ack_seq);
                handshaker.recv_on_syn(*this, msg_, get_ctl());
            }
            void recv_established() {
                est[0] = 1;
            }
            void send_established() {
                est[1] = 1;
            }

            void cleanup() {
                est[0] = est[1] = 0;
            }

            void start_test() {

                dropped = false;
                BOOST_REQUIRE_NO_THROW(handshaker.send_establish(*this, get_ctl()));
                BOOST_REQUIRE_EQUAL(est[0], 1);
                BOOST_REQUIRE_EQUAL(est[1], 1);

                dropped = true;
                cleanup();
                BOOST_REQUIRE_THROW(handshaker.send_establish(*this, get_ctl()), drop);
                BOOST_REQUIRE_EQUAL(est[0], 0);
                BOOST_REQUIRE_EQUAL(est[1], 1);

            }
            bool est[2];
     } obj(id);
     obj.start_test();
#endif
}

BOOST_AUTO_TEST_CASE(close_handshaker_test) {
#if 0
    socket_identify id;
    fill_param("127.0.0.1", 1234, id.in_addr);
    fill_param("127.0.0.1", 80, id.out_addr);

    struct fake_controller_complete : public fake_controller_base<MEM_TAG::CLOSE_HANDSHAKER> {
            close_handshaker<fake_controller_complete> handshaker;
            bool dropped;
            fake_controller_complete(socket_identify id) : fake_controller_base(id) {
                est[0] = est[1] = 0;
            }
            void send_ack(tcp_seq seq, tcp_seq ack_seq, int timeout=0) {
                msg msg_(NULL, 0);
                msg_.set_ack();

                msg_.set_seq(seq + dropped);

                msg_.set_ack_seq(ack_seq);
                handshaker.recv_on_ack(*this, msg_, get_ctl());
            }
            void send_fin_ack(tcp_seq seq, tcp_seq ack_seq, int timeout) {
                msg msg_(NULL, 0);
                msg_.set_fin();
                msg_.set_ack();


                msg_.set_seq(seq);
                msg_.set_ack_seq(ack_seq);
                handshaker.send_on_fin_ack(*this, msg_, get_ctl());
            }
            void send_fin(tcp_seq seq, tcp_seq ack_seq, int timeout) {
                msg msg_(NULL, 0);
                msg_.set_fin();


                msg_.set_seq(seq);
                msg_.set_ack_seq(ack_seq);
                handshaker.recv_on_fin(*this, msg_, get_ctl());
            }
            void recv_closed() {
                est[0] = 1;
            }
            void send_closed() {
                est[1] = 1;
            }

            void cleanup(bool dr) {
                dropped = dr;
                            est[0] = est[1] = 0;
                }

            void start_test() {
                cleanup(false);

                BOOST_REQUIRE_NO_THROW(handshaker.send_close(*this, get_ctl()));
                BOOST_REQUIRE_EQUAL(est[0], 1);
                BOOST_REQUIRE_EQUAL(est[1], 1);

                cleanup(true);
                BOOST_REQUIRE_THROW(handshaker.send_close(*this, get_ctl()), drop);
                BOOST_REQUIRE_EQUAL(est[0], 0);
                BOOST_REQUIRE_EQUAL(est[1], 1);

            }
            bool est[2];
     } obj(id);
     obj.start_test();
#endif
}

BOOST_AUTO_TEST_CASE(fast_retransmit_test) {
    socket_identify id;
    fill_param("127.0.0.1", 1234, id.in_addr);
    fill_param("127.0.0.1", 80, id.out_addr);

    struct fake_controller_complete : public fake_controller_base<MEM_TAG::OUTPUT_FAST_RETRANSMITION> {
            bool retry;
            msg tmp;
            fast_retransmit<fake_controller_complete> fr;
            fake_controller_complete(socket_identify id) : fake_controller_base(id), tmp(NULL, 0) {
                this->retry = false;
            }
            void output_on_retry(msg &msg) {
                BOOST_REQUIRE(retry);
                BOOST_REQUIRE_EQUAL(&tmp, &msg);
                retry = false;
            }
            void start_test() {
                msg ack(NULL, 0);
                ack.set_ack();

                for(int j = 0; j < 2; j++)  {
                    ack.set_seq(j);
                    for(int i = 0; i < TCP_FAST_RETRANSMIT_BOUND - 1 ; i++)
                        fr.on_ack(*this, ack, tmp, get_ctl());
                    retry = true;
                    fr.on_ack(*this, ack, tmp, get_ctl());
                    BOOST_REQUIRE(!retry);
                }

                for(int j = 0; j < TCP_FAST_RETRANSMIT_BOUND * 3; j++){
                    ack.set_seq(j);
                    fr.on_ack(*this, ack, tmp, get_ctl());
                }

            }
     } obj(id);
     obj.start_test();
}

BOOST_AUTO_TEST_CASE(output_control_flow_persist_timer_test) {

        struct fake_controller_complete  {
                output_control_flow_persist_timer<fake_controller_complete> timer;
                bool ping;
                bool ping_ready;
                msg ack;
                std::mutex m;
                std::condition_variable var;
                fake_controller_complete() : ping(false), ack(NULL, 0), ping_ready(false) {}
                void output_set_ping_only(bool enable) {
                    BOOST_REQUIRE(ping ^ enable);
                    ping = enable;
                    BOOST_REQUIRE(ping_ready);
                    ping_ready = false;

                    if (enable) {
                        std::thread thread  = std::thread{&fake_controller_complete::t1, this};
                        thread.detach();
                    }
                }
                void output_signal_win_sz_not_zero() {
                    var.notify_all();
                }
                void output_wait_win_sz_not_zero() {
                    std::unique_lock<std::mutex> lock(m);
                    std::cerr << "on waiting\n";
                    var.wait(lock, [&]{ return ack.get_window_sz() > 0;});
                    std::cerr << "out of waiting\n";
                }

                void t1() {
                    std::unique_lock<std::mutex> lock(m);
                    BOOST_REQUIRE(ping);
                    for(int i = 0; i < 100; i++) {
                        timer.on_ack(*this, ack, true);
                    }

                    ping_ready = true;
                    ack.set_window_sz(1);
                    timer.on_ack(*this, ack, true);
                    BOOST_REQUIRE(!ping_ready);
                    BOOST_REQUIRE(!ping);
                }

                void t2() {
                    /* on ack true msges*/
                    ping_ready = true;
                    timer.on_ack(*this, ack);
                    BOOST_REQUIRE(!ping_ready);
                    BOOST_REQUIRE(!ping);
                }
                void start_test() {
                    t2();
                }
         } obj;
         obj.start_test();
}

BOOST_AUTO_TEST_CASE(timeout_computing_test) {
    /* eyes printables test */
    socket_identify id;
    fill_param("127.0.0.1", 1234, id.in_addr);
    fill_param("127.0.0.1", 80, id.out_addr);


    struct fake_controller_complete : public fake_controller_base<MEM_TAG::OUTPUT_TIMEOUT_COMPUTING> {
                timeout_computing<fake_controller_complete> tim;
                size_t ts;
                fake_controller_complete(socket_identify id) : fake_controller_base(id), ts(0) {
                }
                size_t get_timestamp() {
                    return ts;
                }
                void start_test() {
                    msg ack(NULL, 0);
                    size_t a = 1000000;
                    size_t b = 3000000;
                       ack.set_recv_timestamp(a);
                       ack.set_sender_timestamp(a + 2000);

                       msg sender(NULL, 0);
                       sender.set_sender_timestamp(b);
                       ts = b + 2000;

                    size_t last = 0, now;
                    for(int i = 0; i < 10; i++) {
                        sender.set_recv_timestamp(ack.get_recv_timestamp() - 1000);
                        ts += 1000;
                        tim.refresh(*this, sender, ack, get_ctl());

                        now = tim.get_timeout(get_ctl());
                        std::string output =  (std::string("timeout (ms) = ") + std::to_string(now));
                        BOOST_TEST_MESSAGE( output.c_str() );
                        BOOST_REQUIRE_LE(last, now);
                        last = now;
                    }

                    for(int i = 0; i < 10; i++) {
                        sender.set_recv_timestamp(ack.get_recv_timestamp() + 1000);
                        ts -= 1000;
                        tim.refresh(*this, sender, ack, get_ctl());

                        now = tim.get_timeout(get_ctl());
                        std::string output =  (std::string("timeout (ms) = ") + std::to_string(now));
                        BOOST_TEST_MESSAGE( output.c_str() );
                    }
                    BOOST_REQUIRE_GE(last, now);
                    last = now;
                }
    } obj(id);
    obj.start_test();
}

BOOST_AUTO_TEST_CASE(congection_window_control_test) {
    /* eyes printables test */
    socket_identify id;
    fill_param("127.0.0.1", 1234, id.in_addr);
    fill_param("127.0.0.1", 80, id.out_addr);

#if 0
    struct fake_controller_complete : public fake_controller_base<MEM_TAG::OUTPUT_RECEIVER_WINDOW_CONTROL> {
                congection_window_control<fake_controller_complete, void> tim;

                size_t data_sz = 100;
                void * data_ptr = (void*)100500;
                size_t mtu = 1500;
                size_t check[3] = {
                       15100,
                       471,
                       12
                };
                off_t check_ptr = 0;
                fake_controller_complete(socket_identify id) : fake_controller_base(id) {
                }
                void output_send(void * data, size_t data_sz, size_t mtu, size_t winsz) {
                    BOOST_REQUIRE_EQUAL(this->data_sz, data_sz);
                    BOOST_REQUIRE_EQUAL(this->data_ptr, data);
                    BOOST_REQUIRE_EQUAL(winsz, check[check_ptr++]);
                    std::string msg = std::string ("mtu ") + std::to_string(mtu) +
                        std::string(" win_sz ") + std::to_string(winsz);
                    BOOST_TEST_MESSAGE(msg.c_str());
                }

                void start_test() {
                    msg ack(NULL, 0);
                    ack.set_ack();
                    ack.set_window_sz(65534);
                    tim = congection_window_control<fake_controller_complete, void>(mtu, get_ctl());


                    for(int i = 0; i < 10; i++)
                        tim.on_ack(ack, get_ctl());

                    tim.send(*this, (char*)data_ptr, data_sz, get_ctl());

                    for(int i = 0; i < 5; i++)
                        tim.on_retry(get_ctl());

                    tim.send(*this, (char*)data_ptr, data_sz, get_ctl());

                    ack.set_window_sz(12);
                    for(int i = 0; i < 10; i++)
                        tim.on_ack(ack, get_ctl());
                    tim.send(*this, (char*)data_ptr, data_sz, get_ctl());

                }
    } obj(id);
    obj.start_test();
#endif
}

BOOST_AUTO_TEST_CASE(nagles_test) {
    socket_identify id;
    fill_param("127.0.0.1", 1234, id.in_addr);
    fill_param("127.0.0.1", 80, id.out_addr);


#if 0
    struct fake_controller_complete : public fake_controller_base<MEM_TAG::OUTPUT_RECEIVER_WINDOW_NAGLES> {
                nagles<fake_controller_complete, void*> nag;

                char * data_ptr = (char*)100500;
                size_t mtu = 1500;
                size_t check[6] = {
                        500,
                        500,
                        1500,
                        30000,
                        30000,
                        5534
                };
                off_t check_ptr = 0;
                bool wait_ready = false;
                int wait_nm = 0;
                fake_controller_complete(socket_identify id) : fake_controller_base(id) {
                }

                void * output_send_as_single_package(void * data, size_t data_sz) {
                    BOOST_REQUIRE_EQUAL(data_sz, check[check_ptr++]);
                    BOOST_REQUIRE_EQUAL(data_ptr, data);
                    data_ptr += data_sz;
                    return NULL;
                }

                void output_wait_on_send_buf_empty() {
                    BOOST_REQUIRE(wait_ready);
                    wait_nm++;
                }

                void start_test() {
                    {
                        size_t data_sz = 1000;
                        wait_ready = true;
                        nag.send(*this, data_ptr, data_sz, mtu, 500);
                        BOOST_REQUIRE_EQUAL(wait_nm, 2);
                    }

                    {
                        size_t data_sz = 1500;
                        wait_ready = false;
                        wait_nm = 0;
                        nag.send(*this, data_ptr, data_sz, mtu, 2000);
                    }

                    {
                        size_t data_sz = 65534;
                        mtu = 30000;
                        wait_ready = true;
                        wait_nm = 0;
                        nag.send(*this, data_ptr, data_sz, mtu, 100500);
                        BOOST_REQUIRE_EQUAL(wait_nm, 1);
                    }
//                    nag.send(*this, data, data_sz, mtu, winsz);

                }

    } obj(id);
    obj.start_test();
#endif
}





BOOST_AUTO_TEST_CASE(send_output_buffer_test) {
    socket_identify id;
    fill_param("127.0.0.1", 1234, id.in_addr);
    fill_param("127.0.0.1", 80, id.out_addr);


    struct fake_controller_complete : public fake_controller_base<MEM_TAG::OUTPUT_SEND_OUTPUT_BUFFER> {
                struct my_msg {
                    size_t cnt;
                };
                send_output_buffer<fake_controller_complete, my_msg*, my_msg*> nag;

                char * data_ptr = (char*)100500;
                size_t mtu = 1500;
                size_t check[6] = {
                        500,
                        500,
                        1500,
                        30000,
                        30000,
                        5534
                };
                off_t check_ptr = 0;
                bool wait_ready = false;
                int wait_nm = 0;
                std::vector<my_msg*> rel;
                fake_controller_complete(socket_identify id) : fake_controller_base(id) {
                }

                size_t release_inplace_msg(void * buf) {
                    rel.push_back(( my_msg *)buf);
                    return sizeof(my_msg);
                }

                size_t put_msg_inplace(void * buf, msg &msg) {
                    my_msg * msg_ = ( my_msg *) buf;
                    msg_->cnt = msg.get_seq();

                    return sizeof(my_msg);
                }
                my_msg *  get_msg_inplace(void *buf)  {
                    return  ( my_msg *) buf;
                }

                my_msg *  get_msg_inplace_future(void * buf) {
                    return  ( my_msg *) buf;
                }

                void start_test() {
                    size_t b = 100;
                    my_msg * ptrs[b];
                    std::unique_ptr<msg> go[b];
                    for(int i = 0; i < b; i++)
                        go[i] = std::unique_ptr<msg>(new msg(NULL, 0));
                    my_msg * prev = NULL;
                    void * start = NULL;
                    for(int i = 0; i < 10; i++) {
                        go[i]->set_seq(i);
                        my_msg * now = ptrs[i] = nag.put_msg_at(*this, i, *go[i], get_ctl());
                        BOOST_REQUIRE_EQUAL(now->cnt, go[i]->get_seq());
                        if (prev) {
                            BOOST_REQUIRE_EQUAL(prev + 1, now);
                        } else
                            start = now;
                        prev = now;
                    }

                    for(int i = 0; i < 10; i++) {
                        BOOST_REQUIRE_EQUAL(ptrs[i], nag.get_msg_at(*this, i, get_ctl()));
                    }

                    std::vector<my_msg*> res;
                    for(int i = 5; i < 10; i++)
                        res.push_back(ptrs[i]);
                    for(int i = 0; i < 5; i++)
                        res.push_back(ptrs[i]);
                    nag.release_range(*this, 5, 9, get_ctl());
                    nag.release_range(*this, 0, 4, get_ctl());

                    BOOST_REQUIRE(res == rel);
                    nag.refresh(get_ctl());
                    BOOST_REQUIRE_EQUAL(start, nag.put_msg_at(*this, 0, *go[0], get_ctl()));



                }
    } obj(id);
    obj.start_test();
}

BOOST_AUTO_TEST_CASE(wtf_test) {
    socket_identify id;
        fill_param("127.0.0.1", 1234, id.in_addr);
        fill_param("127.0.0.1", 80, id.out_addr);
    struct fake_controller_complete : public fake_controller_base<MEM_TAG::OUTPUT_SEND_OUTPUT_BUFFER> {
        fake_controller_complete(socket_identify id) : fake_controller_base(id) {
                        }

        void opa(mem_manager::mem_ctl<MEM_TAG::OUTPUT_SEND_OUTPUT_BUFFER> ctl) {
            sem_t * go = (sem_t*)ctl.get_info().ptr->buf;
            sem_init(go ,1 ,1);
            sem_wait(go);
            sem_wait(go);
        }
        void don() {
            opa(get_ctl());
        }
    }suka(id);
    suka.don();
}


BOOST_AUTO_TEST_CASE(send_cumulative_window_test) {
    socket_identify id;
    fill_param("127.0.0.1", 1234, id.in_addr);
    fill_param("127.0.0.1", 80, id.out_addr);


    struct fake_controller_complete : public fake_controller_base<MEM_TAG::OUTPUT_CUMULATIVE_WINDOW_PROCESSING> {
                struct my_msg {
                    size_t cnt = 0;
                    bool used = false;
                    bool released = false;
                    msg * msg_;
                };

                typedef my_msg * fut;
                typedef my_msg * inp;

                bool signal_not_empty = false;
                bool signal_a = false;
                bool signal_b = false;
                bool signal_c = false;
                bool signal_d = false;


                std::map<off_t, std::shared_ptr<my_msg> >mp;
                send_cumulative_window_processing<fake_controller_complete, inp, fut> nag;
                fake_controller_complete(socket_identify id) : fake_controller_base(id) {
                }


                void mark_msg_used(inp msg_, bool flag) {
                    msg_->used = flag;
                }

                bool is_msg_used(inp in) {
                   return in->used || in->released;
               }

               size_t get_msg_data_sz(inp in) {
//                   return sizeof(*in);
                   return in->msg_->get_data_len();
               }



                void output_refresh_on_msg_pair_prev_window_matched(inp in, msg &ms_) {
                    BOOST_REQUIRE(signal_a);
                    signal_a = false;
                }

                void output_refresh_on_msg_pair(inp in, msg &ms_) {
                    BOOST_REQUIRE(signal_a);
                    signal_a = false;
                }

                fut output_sent_buf_put_msg_at(off_t off, msg &msg) {
                   mp[off].reset(new my_msg());
                   mp[off]->cnt = msg.get_seq();
                   mp[off]->msg_ = &msg;
                   mp[off]->used = mp[off]->released = false;
                   return mp[off].get();
               }

               inp output_sent_buf_get_msg_at(off_t off) {
                   return mp[off].get();
               }

                void output_sent_buf_release_range(off_t a, off_t b) {
                    for(auto pa : mp) {
                        if (a <= pa.first && pa.first <= b) {
                            BOOST_REQUIRE(!pa.second->released);
                            pa.second->released = true;
                        }
                    }
                }

                void output_sent_buf_refresh_contents() {
                    BOOST_REQUIRE(signal_d);
                    signal_d = false;
                }




                void output_sent_buf_wait_not_empty() {
                    BOOST_REQUIRE(signal_c);
                    signal_c = false;
                }

                void on_sent_buf_empty() {
                    BOOST_REQUIRE(signal_b);
                    signal_b = false;
                }

                void output_sent_buf_signal_not_empty() {
                    BOOST_REQUIRE(signal_not_empty);
                    signal_not_empty = false;
                }

                void start_test() {
                    tcp_seq start = 100500;
                    nag.force_move_start(start, get_ctl());

                    {
                        /* win exceed test */
                        msg m(NULL, 0);
                        m.set_data_len(OUTPUT_WINDOW_SZ);
                        signal_not_empty = true;
                        nag.send(*this, m, get_ctl());

                        msg ack(NULL, 0);
                       ack.set_ack();

                       signal_d = true;
                       signal_b = true;
                       signal_a = true;
                       ack.set_seq(OUTPUT_WINDOW_SZ + start - 1);
                       nag.on_ack(*this, ack, get_ctl());
                       BOOST_REQUIRE(!signal_a);
                       BOOST_REQUIRE(!signal_b);
                       BOOST_REQUIRE(!signal_d);
                       BOOST_REQUIRE(!signal_not_empty);

                       start += OUTPUT_WINDOW_SZ;

                    }

                    {
                        /* drop overflow test */
                        msg m(NULL, 0);
                        m.set_data_len(OUTPUT_WINDOW_SZ + 1);
                        BOOST_REQUIRE_THROW(nag.send(*this, m, get_ctl()), drop);
                    }

                    for(int iter = 0; iter < 2; iter++){
                        size_t b = 100;
                           std::unique_ptr<msg> go[b];
                           for(int i = 0; i < b; i++) {
                               go[i] = std::unique_ptr<msg>(new msg(NULL, 0));
                               go[i]->set_data_len(i + 1);
                           }

                          int n = 4;


                        for(int i = 0 ; i < n; i++) {
                            signal_not_empty = (i == 0);
                            nag.send(*this, *go[i], get_ctl());

                            if (!i)
                                BOOST_REQUIRE(!signal_not_empty);
                        }

                        for(int j = 0; j < 10; j++) {
                            for(int i = 0; i < n; i++) {
                                inp hu = nag.next_msg(*this, get_ctl());
                                BOOST_REQUIRE_EQUAL(go[i]->get_seq(), hu->cnt);
                            }
                            BOOST_REQUIRE_THROW(nag.next_msg(*this, get_ctl()), timeout);
                            for(int i = 0; i < n; i++) {
                                nag.retry(*this, *go[i], get_ctl());
                            }
                        }

                        msg ack(NULL, 0);
                        ack.set_ack();
                        ack.set_seq(start - 1);

                        signal_a = true;
                        nag.on_ack(*this, ack, get_ctl());
                        BOOST_REQUIRE(!signal_a);


                        size_t sum = 0;
                        for(int i = 0; i < n; i++) {
                            sum += go[i]->get_data_len();
                            ack.set_seq(start + sum - 1);

                            signal_a = true;
                            if (i == n -1)
                                signal_b = true;
                            nag.on_ack(*this, ack, get_ctl());
                            BOOST_REQUIRE(!signal_a);
                            if (i == n - 1)
                                BOOST_REQUIRE(!signal_b);
                        }
                        start += sum;
                    }

                }
    } obj(id);
    obj.start_test();
}



#if 0
BOOST_AUTO_TEST_CASE(worker_test) {
    int b = 3;
    struct suka : public worker {
        virtual void do_job() {
            q = a;
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        suka(int a) : a(a), q(0) {}
        ~suka() {
            join();
        }
        int a, q;
    } blya(b);
    blya.start();
//    std::this_thread::sleep_for(std::chrono::milliseconds(1));
//    BOOST_REQUIRE_EQUAL(b, blya.a);
//    BOOST_REQUIRE_EQUAL(b, blya.q);
}
#endif




#if 1
BOOST_AUTO_TEST_CASE(single_socket_test)
{
    socket_identify id;
    fill_param("127.0.0.1", 80, id.in_addr);
    fill_param("127.0.0.1", 80, id.out_addr);
    socket_full_impl sock(id);


#if 1
    if (1){
        size_t dsz = OUTPUT_WINDOW_SZ / 3;
        char data[dsz];
        char input_data[dsz];

       for(int iter = 0; iter < 10; iter++) {
           for(int i = 0; i < dsz; i++)
               data[i] = i + 100 * (iter + 1);
           memset(input_data, 0, sizeof(input_data));

           std::thread t1 = std::thread([&]{ sock.send(data, dsz);});
           sock.recv(input_data, dsz);
           t1.join();
           BOOST_REQUIRE_EQUAL(memcmp(data, input_data, dsz), 0);
       }
    }

    if (1){
        /* OVERFLOW TEST */
        size_t dsz = 2 * OUTPUT_WINDOW_SZ - 1;
        char data[dsz];
        char input_data[dsz];

       for(int iter = 0; iter < 2; iter++) {
           for(int i = 0; i < dsz; i++)
               data[i] = i + 100 * (iter + 1);
           memset(input_data, 0, sizeof(input_data));

           std::thread t1 = std::thread([&]{ sock.send(data, dsz);});
           sock.recv(input_data, dsz);
           t1.join();
           BOOST_REQUIRE_EQUAL(memcmp(data, input_data, dsz), 0);
       }
    }

       if (1){
               /* SEVERAL SENDS */
               size_t dsz = 2 * (OUTPUT_WINDOW_SZ - 1);
               char data[dsz];
               char input_data[dsz];

              for(int iter = 0; iter < 2; iter++) {
                  for(int i = 0; i < dsz; i++)
                      data[i] = i + 100 * (iter + 1);
                  memset(input_data, 0, sizeof(input_data));

                  std::thread t1 = std::thread([&]{ sock.send(data, dsz / 2);sock.send(data + dsz/2, dsz / 2);});
                  sock.recv(input_data, dsz);
                  t1.join();
                  BOOST_REQUIRE_EQUAL(memcmp(data, input_data, dsz), 0);
          }
       }
#endif

}
#endif

#if 1
BOOST_AUTO_TEST_CASE(several_mq_test) {
    unsigned int no;
    sock_mq::param param;
    fill_param("127.0.0.1", 0, param.id.in_addr);
    fill_param("127.0.0.1", 80, param.id.out_addr);
    param.create = true;
    default_tcp_mq_attr(param.created_attr);

    sock_mq mq(param);

    std::swap(param.id.in_addr, param.id.out_addr);
    sock_mq mq2(param);

}
#endif

#if 1
BOOST_AUTO_TEST_CASE(twice_socket_test)
{
    socket_identify id;
    fill_param("127.0.0.1", 80, id.in_addr);
    fill_param("127.0.0.1", 1234, id.out_addr);
    socket_full_impl send_sock_(id);

    socket_identify id2;
    fill_param("127.0.0.1", 1234, id2.in_addr);
    fill_param("127.0.0.1", 80, id2.out_addr);
    socket_full_impl recv_sock_(id2);

    socket_full_impl *send_sock = &send_sock_;
    socket_full_impl *recv_sock = &recv_sock_;

    for(int _ = 0; _ < 2; _++) {
#if 1
    if (1){
        size_t dsz = OUTPUT_WINDOW_SZ / 3;
        char data[dsz];
        char input_data[dsz];

       for(int iter = 0; iter < 10; iter++) {
           for(int i = 0; i < dsz; i++)
               data[i] = i + 100 * (iter + 1);
           memset(input_data, 0, sizeof(input_data));

           std::thread t1 = std::thread([&]{ (*send_sock).send(data, dsz);});
           (*recv_sock).recv(input_data, dsz);
           t1.join();
           BOOST_REQUIRE_EQUAL(memcmp(data, input_data, dsz), 0);
       }
    }

    if (1){
        /* OVERFLOW TEST */
        size_t dsz = 2 * OUTPUT_WINDOW_SZ - 1;
        char data[dsz];
        char input_data[dsz];

       for(int iter = 0; iter < 2; iter++) {
           for(int i = 0; i < dsz; i++)
               data[i] = i + 100 * (iter + 1);
           memset(input_data, 0, sizeof(input_data));

           std::thread t1 = std::thread([&]{ (*send_sock).send(data, dsz);});
           (*recv_sock).recv(input_data, dsz);
           t1.join();
           BOOST_REQUIRE_EQUAL(memcmp(data, input_data, dsz), 0);
       }
    }

       if (1){
               /* SEVERAL SENDS */
               size_t dsz = 2 * (OUTPUT_WINDOW_SZ - 1);
               char data[dsz];
               char input_data[dsz];

              for(int iter = 0; iter < 2; iter++) {
                  for(int i = 0; i < dsz; i++)
                      data[i] = i + 100 * (iter + 1);
                  memset(input_data, 0, sizeof(input_data));

                  std::thread t1 = std::thread([&]{ (*send_sock).send(data, dsz / 2);(*send_sock).send(data + dsz/2, dsz / 2);});
                  (*recv_sock).recv(input_data, dsz);
                  t1.join();
                  BOOST_REQUIRE_EQUAL(memcmp(data, input_data, dsz), 0);
          }
       }
#endif
       std::swap(send_sock, recv_sock);
    }

}
#endif

BOOST_AUTO_TEST_CASE(serv_socket_test) {

    socket_identify id2;
    fill_param("127.0.0.1", 1234, id2.out_addr);


    socket_serv serv(id2.out_addr);

    if (1) {
        for(int iter = 0; iter < 5; iter++) {

           socket_identify id;
           fill_param("127.0.0.1", 80 + iter, id.in_addr);
           fill_param("127.0.0.1", 1234, id.out_addr);
           socket_full_impl send_sock_(id);
           socket_full_impl *send_sock = &send_sock_;


            size_t dsz = 1;
            char data[dsz];
            char input_data[dsz];

           for(int i = 0; i < dsz; i++)
               data[i] = i + 100 ;
           memset(input_data, 0, sizeof(input_data));


           std::thread t1 = std::thread([&]{ send_sock->send(data, dsz);});

           while(1) {
               std::unique_ptr<stream_socket> recv_sock(serv.accept_one_client());
               if (((socket_full_impl *)recv_sock.get())->get_id().out_addr.sin_port == id.in_addr.sin_port) {
                   (*recv_sock).recv(input_data, dsz);
                   break;
               }
           }
           t1.join();
           BOOST_REQUIRE_EQUAL(memcmp(data, input_data, dsz), 0);

        }
    }
}

BOOST_AUTO_TEST_CASE(serv_client_socket_test) {
    socket_identify id2;
    fill_param("127.0.0.1", 1234, id2.out_addr);


    socket_serv serv(id2.out_addr);

    if (1) {
        for(int iter = 0; iter <5; iter++) {

           socket_identify id;
           fill_param("127.0.0.1", 80 + iter, id.in_addr);
           fill_param("127.0.0.1", 1234, id.out_addr);
           socket_client send_sock(id);


            size_t dsz = 1;
            char data[dsz];
            char input_data[dsz];

           for(int i = 0; i < dsz; i++)
               data[i] = i + 100 ;
           memset(input_data, 0, sizeof(input_data));


           std::thread t1 = std::thread([&]{
               send_sock.connect();
               send_sock.send(data, dsz);
           });

           while(1) {
               std::unique_ptr<stream_socket> recv_sock(serv.accept_one_client());
               if (((socket_full_impl *)recv_sock.get())->get_id().out_addr.sin_port == id.in_addr.sin_port) {
                   (*recv_sock).recv(input_data, dsz);
                   break;
               }
           }
           t1.join();
           BOOST_REQUIRE_EQUAL(memcmp(data, input_data, dsz), 0);

        }
    }
}


