#ifndef __IP_H
#define __IP_H

#define UDP  	0x11
#define ICMP    0x01
#define TCP	0x06


/*
 *	This structure defines an ip header.
 */

struct iphdr {

	unsigned char 	version:4,
  					ihl:4;
	unsigned char	tos;
	unsigned short	tot_len;
	unsigned short	id;
	unsigned short	frag_off;
	unsigned char	ttl;
	unsigned char	protocol;
	unsigned short	check;
	unsigned long	saddr;
	unsigned long	daddr;
	/*The options start here. */
}__attribute__ ((packed));

unsigned short in_csum(unsigned short *ptr, int nbytes);
int ip_init(unsigned long ip);
int ip_rcv_packet(sk_buff *skb);
int ip_send(sk_buff *skb, unsigned long ip, unsigned char proto);
void ip_skb_reserve(sk_buff *skb);
unsigned long ip_get_source_ip(sk_buff *skb);

#endif /* __IP_H */
