#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <exception>
#include <stdexcept>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/types.h>
#include <mqueue.h>
#include <algorithm>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <chrono>
#include <random>
#include <ratio>
#include <future>
#include <thread>
#include <ifaddrs.h>
#include <sys/resource.h>
#include "tcp_engine.h"
#include "tcp_log.h"

static const std::string SOCK_SHMEM_PREFIX = "/sock-";

int mtu_if(const char * if_name, int sock) {
    struct ifreq req;
    memset(&req, 0, sizeof(req));
    strcpy(req.ifr_ifrn.ifrn_name, if_name);
    if (ioctl(sock, SIOCGIFMTU, &req) == -1) {
        throw fuck("failed SIOCGIFMTU to sock ");
    }
    return req.ifr_ifru.ifru_mtu;
}

int mtu_binded(int sock, const struct sockaddr_in &bind_addr, int max_mtu) {
    struct ifaddrs* ifaddr;
    struct ifaddrs* ifa;
    getifaddrs(&ifaddr);

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr) {
            if (bind_addr.sin_family == ifa->ifa_addr->sa_family) {
                struct sockaddr_in* inaddr = (struct sockaddr_in*) ifa->ifa_addr;
                if (bind_addr.sin_addr.s_addr == INADDR_ANY
                        || inaddr->sin_addr.s_addr
                                == bind_addr.sin_addr.s_addr) {
                    if (ifa->ifa_name) {

                        max_mtu = std::min(max_mtu,
                                mtu_if(ifa->ifa_name, sock));
                    }
                }
            }
        }
    }
    freeifaddrs(ifaddr);
    return max_mtu;
}

bool addr_comparable(const sockaddr_in &addr, const sockaddr_in &addr2) {
    return addr.sin_addr.s_addr == addr2.sin_addr.s_addr &&
            addr.sin_port == addr2.sin_port;
}

void fill_param(const char * addr, uint16_t port, struct sockaddr_in &in) {
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = htons(port);
    in.sin_addr.s_addr = inet_addr(addr);
}

void fill_param_self(uint16_t port, struct sockaddr_in& in) {
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = htons(port);
    in.sin_addr.s_addr = htonl(INADDR_ANY);
}

std::string sock_label(const struct socket_identify &id) {
    return SOCK_SHMEM_PREFIX + std::string(inet_ntoa(id.in_addr.sin_addr)) + "."
            + std::to_string(ntohs(id.in_addr.sin_port)) + "-"
            + std::string(inet_ntoa(id.out_addr.sin_addr)) + "."
            + std::to_string(ntohs(id.out_addr.sin_port));
}

static void get_mq_rlimit(struct rlimit &rlim){
    int res = RLIMIT_MSGQUEUE;
    if (getrlimit(res, &rlim) != 0)
        throw fuck("get mq rlimit fail");
}

static void set_mq_rlimit(struct rlimit &rlim) {
    int res = RLIMIT_MSGQUEUE;
    if (setrlimit(res, &rlim) != 0)
        throw fuck("set mq rlimit fail");
}

#include <iostream>
sock_mq::sock_mq(const sock_mq::param &param) {
    label = sock_label(param.id);
    create = param.create;

    rlimit rlim;
    get_mq_rlimit(rlim);
    rlim_t required = (rlim_t) param.created_attr.mq_maxmsg * (rlim_t)param.created_attr.mq_msgsize;
    /* for internal data *2: ttr.mq_maxmsg * sizeof(struct msg_msg) +
                              min(attr.mq_maxmsg, MQ_PRIO_MAX) *
                                    sizeof(struct posix_msg_tree_node
    */
    required += rlim.rlim_cur * MAXMQSYSTEM;

    if (rlim.rlim_cur < required) {
        rlim.rlim_cur = rlim.rlim_max = required;
        set_mq_rlimit(rlim);
    }

    if (param.create) {

        mq = mq_open(label.c_str(), O_RDWR | O_CREAT, 0700,
                &param.created_attr);
    } else {
        mq = mq_open(label.c_str(), O_RDWR);
    }
    if (mq == -1)
        throw fuck("sock mq open failed ");
}

sock_mq::~sock_mq() {
    if (create) {
        mq_unlink(label.c_str());
    }
    mq_close(mq);
}

void sock_mq::getattrs(struct mq_attr &attr) {
    if (mq_getattr(mq, &attr) == -1) {
        throw fuck( "getaatrs failed ");
    }
}

void sock_mq::put_msg(void *msg, size_t sz, unsigned int priority) {
    if (mq_send(mq, (const char *) msg, sz, priority) == -1)
        throw fuck( "put msg failed " );
}

ssize_t sock_mq::get_msg(void * msg, size_t sz, unsigned int &priority) {
    ssize_t rc = mq_receive(mq, (char*) msg, sz, &priority);
    if (rc == -1)
        throw fuck("get msg failed ");
    return rc;
}

mqd_t sock_mq::mqd() {
    return mq;
}

void sock::__create_lock() {
    lock = sem_open(label.c_str(), O_CREAT | O_RDWR, 0777, 1);
    if (lock == SEM_FAILED)
        throw fuck("create sock lock ");
}
void sock::__lock() {
    if (!lock) {
        __create_lock();
    }
    if (sem_wait(lock) == -1)
        throw fuck(
                "sock lock err " );
}

void sock::__unlock() {
    if (sem_post(lock) == -1)
        throw fuck(
                "sock unlock err " );
}


void sock::__init_shmem(sock_shmem_initializer &initializer) {
    LOG() << "init shmem : client " << (void*)shmem->client_shmem << ", locks "
              << (void*)&shmem->locks << "\n";
    initializer.init((void*)shmem->client_shmem, &shmem->locks);
    shmem->shm_state = sock_shmem::OK;
}

void sock::__init_internal(sock_shmem_initializer &initializer) {

    __init_shmem(initializer);
    mq = std::unique_ptr<sock_mq>(new sock_mq(mq_param));
}

void sock::__load_shmem( sock_shmem_initializer &initializer) {
    LOG() << "load shmem : client " << (void*)shmem->client_shmem << ", locks "
            << (void*)&shmem->locks << "\n";
    initializer.load((void*)shmem->client_shmem, &shmem->locks);
}

void sock::reopen(sock_shmem_initializer &initializer) {
    reopen:
    __lock();
    shd = shm_open(label.c_str(), O_RDWR | O_CREAT, 0777);
    if (shd == -1) {
        throw fuck(
                "shm_open err " );
    }
    LOG() << "sock reopem: shmem_sz " << this->shmem_sz << "\n";

    if (ftruncate(shd, this->shmem_sz) == -1)
        throw fuck(
                "shm truncate err " );

    shmem = (sock_shmem *) mmap(0, this->shmem_sz,
    PROT_READ | PROT_WRITE, MAP_SHARED, shd, 0);

    if (shmem == MAP_FAILED)
        throw fuck(
                "shm mmap err " );
    // if ftruncate on new file - struct have been filled with 0
    if (shmem->shm_state == sock_shmem::UNLINKED) {
        munmap(shmem, this->shmem_sz);
        close(shd);
        __unlock();
        goto reopen;
    }
    if (shmem->shm_state == sock_shmem::UNITILIZATED) {
        /* init here */
        __init_internal(initializer);
    } else {
        __load_shmem(initializer);
    }
    __unlock();
}

sock::sock(const sock::sock_param &param, sock_shmem_initializer &initializer) {
    id = param.id;

    mq_param.create = true;
    mq_param.created_attr = param.mq_param;
    mq_param.id = param.id;

    label = sock_label(id);
    lock = NULL;
    client_shmem_sz = param.shmem_sz;
    this->shmem_sz = client_shmem_sz + sizeof(sock_shmem);
    reopen(initializer);
}

sock::~sock() {
    LOG() << "~sock : entered\n";
    __lock();
    LOG() << "~sock : got init lock\n";
    for(size_t i = 0; i < shmem->locks.locks_num; i++) {
        try {
            LOG() << "~sock : trying got " << i << " internal lock\n";
            external_lock_wrapper(&shmem->locks.locks[i]).lock();
            LOG() << "~sock : got " << i << " internal lock\n";
        } catch(...) {
            return;
        }
    }
    LOG() << "~sock : got internals locks\n";
    sem_unlink(label.c_str());
    shm_unlink(label.c_str());
    shmem->shm_state = sock_shmem::UNLINKED;
    for(size_t i = 0; i < shmem->locks.locks_num; i++) {
        try {
            external_lock_wrapper(&shmem->locks.locks[i]).unlock();
        } catch (...) {
            return;
        }
    }
    munmap(shmem, this->shmem_sz);
    __unlock();
    sem_close(lock);
    close(shd);
    LOG() << "~sock : out\n";
}

void * sock::accquire_shmem(bool internal_lock) {
    if (internal_lock)
        __lock();
    if (shmem->shm_state == sock_shmem::UNLINKED) {
        /* socket closed by concurent context */
        if (internal_lock)
            __unlock();
        throw fuck("socket has been already closed");
    }
    return shmem->client_shmem;
}

void sock::put_internal_lock() {
    __unlock();
}

sock_mq & sock::get_mq() {
    return *mq;
}

void convert_to_timeval(std::chrono::milliseconds ms_, struct timeval& timeval) {
    timeval.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(ms_).count();
    timeval.tv_usec = std::chrono::duration_cast<std::chrono::microseconds>(ms_).count() -
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::seconds(timeval.tv_sec)).count();
}

void convert_to_timespec(std::chrono::milliseconds ms_, struct timespec& timeval) {
    timeval.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(ms_).count();
    timeval.tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(ms_).count() -
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(timeval.tv_sec)).count();
}

size_t sys_time() {
    auto now = std::chrono::system_clock::now();
   return (std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())).count();
}

u_int16_t chsum(u_int16_t * buf, size_t sz) {
    u_int64_t chksum=0;
    while(sz > 1) {
        chksum +=*buf++;
        sz -= sizeof(u_int16_t);
    }
    if (sz)
        chksum += *(u_int8_t*)buf;

    chksum = (chksum >> 16) + (chksum & 0xffff);
    chksum += (chksum >>16);
    return (u_int16_t)(~chksum);
}

uint32_t rand32() {
    std::random_device rd;
    std::mt19937 mt(rd());
    std::uniform_int_distribution<uint32_t> dist(0, std::numeric_limits<uint32_t>::max());
    return dist(mt);
}
