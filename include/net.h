#ifndef __CR_NET_H__
#define __CR_NET_H__
struct cr_fdset;
int dump_net_ns(int pid, struct cr_fdset *);
int prepare_net_ns(int pid);
#endif
