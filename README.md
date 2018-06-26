## Toy tcp proto implementation details

- Linux raw sockets
- Linux userland interprocess-sync : shmem, semaphores, mqueue
- AIMD congetion avoidance
- TCP cumulative Ack
- TCP Nagle's algorithm
- Retransmission timeout computation based on estimated RTT between hosts (TCP timestamps)
- TCP fast retransmit
- TCP flow control 
- Duplex handshake
- Reuseport feature support
- User chat on implemented sockets