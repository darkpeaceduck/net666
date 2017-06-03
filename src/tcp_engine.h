#pragma once
#include <netinet/in.h>
#include <exception>
#include <string>
#include <semaphore.h>
#include <memory>
#include <chrono>
#include <mqueue.h>
#include <string.h>
#include <string>
#include <mutex>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>

#include "proto.h"
#include "tcp_log.h"

#define MAXMQSYSTEM 10
void fill_param(const char * addr, uint16_t port, struct sockaddr_in &in);
void fill_param_self(uint16_t port, struct sockaddr_in &in);
bool addr_comparable(const sockaddr_in &id, const sockaddr_in &addr);
int mtu_if(const char * if_name, int sock);
int mtu_binded(int sock, const struct sockaddr_in &bind_addr, int max_mtu);
void convert_to_timeval(std::chrono::milliseconds ms, struct timeval& timeval);
size_t sys_time();
u_int16_t chsum(u_int16_t * buf, size_t sz);
uint32_t rand32();
void convert_to_timespec(std::chrono::milliseconds ms_, struct timespec& timeval);
std::string sock_label(const struct socket_identify &id);

class sock_mq {
public:
    struct param {
        struct socket_identify id;
        bool create;
        struct mq_attr created_attr;
    };
    sock_mq(const sock_mq::param &param);
    ~sock_mq();
    void getattrs(struct mq_attr &attr);
    void put_msg(void *msg, size_t sz, unsigned int priority);
    ssize_t get_msg(void * msg, size_t sz, unsigned int &priority);
    mqd_t mqd();
private:
    bool create;
    std::string label;
    mqd_t mq;
};


class sock {
public:
    static const size_t shmem_max_locks = 50;
    struct sock_param {
        struct socket_identify id;
        struct mq_attr mq_param;
        size_t shmem_sz;
    };
    enum lock_type{
        SEND,
        RECV,
    };
    struct locks_shmem {
        size_t locks_num;
        sem_t locks[shmem_max_locks];
    }__attribute__((aligned(8)));;
    struct sock_shmem_initializer {
        /* call from sock constructor under socket lock */
        virtual void init(void * client_mem, locks_shmem * locks)  = 0;
        /* call from sock constructor under socket lock */
        virtual void load(void * client_mem, locks_shmem * locks)  = 0;
    };
    struct external_lock_wrapper {
        external_lock_wrapper(sem_t * sem, bool ignored=false) : sem(sem), ignored(ignored){}
        void init()  {
            if (!sem || ignored)
                return;
            sem_init(sem, 1, 1);
        }
        void lock() {
            if (!sem || ignored)
                return;
            LOG() << "LOCK : trying get " << sem << "\n";
            if (sem_wait(sem) == -1)
               throw fuck(
                       "sock lock err " );
            LOG() << "LOCK : got " << sem << "\n";
        }
        void unlock() {
            if (!sem || ignored)
                return;
            LOG() << "UNLOCK : trying " << sem << "\n";
            if (sem_post(sem) == -1)
                throw fuck(
                        "sock unlock err " );
            LOG() << "UNLOCK : finished " << sem << "\n";
        }
        bool have() {
            if (ignored)
                return true;
            return sem != NULL;
        }
    private:
        sem_t * sem;
        bool ignored;
    };
    struct notificator_wrapper {
        notificator_wrapper() : sem(NULL) {}
        notificator_wrapper(sem_t * sem) : sem(sem) {}
            void init()  {
                    if (sem_init(sem, 1, 0) == -1)
                        throw fuck("sem init err");
            }
            void wait() {
                LOG() << "waiting sem " << (void*)sem << "\n";
                LOG() << "WAIT NOTIF : trying get " << sem << "\n";
                if (sem_wait(sem) == -1)
                   throw fuck(
                           "sock lock err " );
                LOG() << "WAIT NOTIF : got " << sem << "\n";
            }
            void wait_for(std::chrono::milliseconds ms) {
                struct timespec spec;
                ms += std::chrono::milliseconds(sys_time());
                convert_to_timespec(ms ,spec);
                LOG() << "WAIT NOTIF TIMED: trying get " << sem << "\n";
                if (sem_timedwait(sem, &spec) == -1) {
                    if (errno != ETIMEDOUT)
                       throw fuck(
                               "sock lock err " );
                }
                LOG() << "WAIT NOTIF TIMED: got " << sem << "\n";
            }
            void notify() {
                LOG() << "SIGNAL NOTIF: trying get " << sem << "\n";
                if (sem_post(sem) == -1)
                    throw fuck(
                            "sock unlock err " );
                LOG() << "SIGNAL  NOTIF : got " << sem << "\n";
            }
        public:
            sem_t * sem;
    };
private:
    struct sock_shmem {
        enum {
            UNITILIZATED = 0, OK, UNLINKED
        } __attribute__((aligned(8))) shm_state;
        locks_shmem locks;
        char client_shmem[];
    }__attribute__((aligned(8)));

    void __init_shmem(sock_shmem_initializer &initializer);
    void __load_shmem(sock_shmem_initializer &initializer);
    void __init_internal(sock_shmem_initializer &initializer);
    void __create_lock() ;
    void __lock();
    void __unlock();
    void reopen(sock_shmem_initializer &initializer);
public:
    sock(const sock::sock_param &param, sock_shmem_initializer &initializer);
    ~sock();
    /* must be called under region lock or with true param - need to put_lock than*/
    void * accquire_shmem(bool internal_lock);
    void put_internal_lock();
    sock_mq & get_mq() ;
private:
    socket_identify id;
    std::string label;
    sock_shmem * shmem;
    int shd;
    sem_t * lock;
    size_t client_shmem_sz;
    size_t shmem_sz;
    sock_mq::param mq_param;
    std::unique_ptr<sock_mq> mq;
};


class worker {
protected:
    std::mutex m;
    std::thread t;
    bool started = false;
    std::atomic<bool> active;
public:
    void start() {
        std::unique_lock<std::mutex> lock(m);
        if (!started) {
            t = std::thread{&worker::do_job, this};
            started = true;
        }
    }
    void join() {
        std::unique_lock<std::mutex> lock(m);
        LOG() << "worker joining\n";
        if (started) {
            if (t.joinable())
                t.join();
            started = false;
        }
        LOG() << "worker joined\n";
    }
    worker() : active(true) {}
    virtual ~worker() {
        join();
    }
    void set_passive() {
        active = false;
    }
    bool is_active() {
        return active.load();
    }
    virtual void do_job() = 0;
};
