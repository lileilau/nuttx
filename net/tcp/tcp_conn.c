/****************************************************************************
 * net/tcp/tcp_conn.c
 *
 *   Copyright (C) 2007-2011, 2013-2015 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Large parts of this file were leveraged from uIP logic:
 *
 *   Copyright (c) 2001-2003, Adam Dunkels.
 *   All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#if defined(CONFIG_NET) && defined(CONFIG_NET_TCP)

#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <arch/irq.h>

#include <nuttx/net/netconfig.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/ip.h>
#include <nuttx/net/tcp.h>

#include "devif/devif.h"
#include "tcp/tcp.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define IPv4 ((struct net_iphdr_s *)&dev->d_buf[NET_LL_HDRLEN(dev)])
#define IPv6 ((struct net_ipv6hdr_s *)&dev->d_buf[NET_LL_HDRLEN(dev)])

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The array containing all uIP TCP connections. */

static struct tcp_conn_s g_tcp_connections[CONFIG_NET_TCP_CONNS];

/* A list of all free TCP connections */

static dq_queue_t g_free_tcp_connections;

/* A list of all connected TCP connections */

static dq_queue_t g_active_tcp_connections;

/* Last port used by a TCP connection connection. */

static uint16_t g_last_tcp_port;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tcp_listener
 *
 * Description:
 *   Given a local port number (in network byte order), find the TCP
 *   connection that listens on this this port.
 *
 *   Primary uses: (1) to determine if a port number is available, (2) to
 *   To identify the socket that will accept new connections on a local port.
 *
 ****************************************************************************/

#ifdef CONFIG_NETDEV_MULTINIC
static FAR struct tcp_conn_s *tcp_listener(in_addr_t ipaddr, uint16_t portno)
#else
static FAR struct tcp_conn_s *tcp_listener(uint16_t portno)
#endif
{
  FAR struct tcp_conn_s *conn;
  int i;

  /* Check if this port number is in use by any active UIP TCP connection */

  for (i = 0; i < CONFIG_NET_TCP_CONNS; i++)
    {
      conn = &g_tcp_connections[i];

      /* Check if this connection is open and the local port assignment
       * matches the requested port number.
       */

      if (conn->tcpstateflags != TCP_CLOSED && conn->lport == portno)
        {
#ifdef CONFIG_NETDEV_MULTINIC
          /* If there are multiple interface devices, then the local IP
           * address of the connection must also match.  INADDR_ANY is a
           * special case:  There can only be instance of a port number
           * with INADDR_ANY.
           */

          if (net_ipv4addr_cmp(conn->u.ipv4.laddr, ipaddr) ||
#ifdef CONFIG_NET_IPv6
              net_ipv4addr_cmp(conn->u.ipv4.laddr, g_ipv4_allzeroaddr))
#else
              net_ipv4addr_cmp(conn->u.ipv4.laddr, INADDR_ANY))
#endif
#endif
            {
              /* The port number is in use, return the connection */

              return conn;
            }
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: tcp_selectport
 *
 * Description:
 *   If the port number is zero; select an unused port for the connection.
 *   If the port number is non-zero, verify that no other connection has
 *   been created with this port number.
 *
 * Input Parameters:
 *   portno -- the selected port number in host order. Zero means no port
 *     selected.
 *
 * Return:
 *   0 on success, negated errno on failure:
 *
 *   EADDRINUSE
 *     The given address is already in use.
 *   EADDRNOTAVAIL
 *     Cannot assign requested address (unlikely)
 *
 * Assumptions:
 *   Interrupts are disabled
 *
 ****************************************************************************/

#ifdef CONFIG_NETDEV_MULTINIC
static int tcp_selectport(in_addr_t ipaddr, uint16_t portno)
#else
static int tcp_selectport(uint16_t portno)
#endif
{
  if (portno == 0)
    {
      /* No local port assigned. Loop until we find a valid listen port number
       * that is not being used by any other connection. NOTE the following loop
       * is assumed to terminate but could not if all 32000-4096+1 ports are
       * in used (unlikely).
       */

      do
        {
          /* Guess that the next available port number will be the one after
           * the last port number assigned.
           */
          portno = ++g_last_tcp_port;

          /* Make sure that the port number is within range */

          if (g_last_tcp_port >= 32000)
            {
              g_last_tcp_port = 4096;
            }
        }
#ifdef CONFIG_NETDEV_MULTINIC
      while (tcp_listener(ipaddr, htons(g_last_tcp_port)));
#else
      while (tcp_listener(htons(g_last_tcp_port)));
#endif
    }
  else
    {
      /* A port number has been supplied.  Verify that no other TCP/IP
       * connection is using this local port.
       */

#ifdef CONFIG_NETDEV_MULTINIC
      if (tcp_listener(ipaddr, portno))
#else
      if (tcp_listener(portno))
#endif
        {
          /* It is in use... return EADDRINUSE */

          return -EADDRINUSE;
        }
    }

  /* Return the selected or verified port number */

  return portno;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tcp_initialize
 *
 * Description:
 *   Initialize the TCP/IP connection structures.  Called only once and only
 *   from the UIP layer at start-up in normal user mode.
 *
 ****************************************************************************/

void tcp_initialize(void)
{
  int i;

  /* Initialize the queues */

  dq_init(&g_free_tcp_connections);
  dq_init(&g_active_tcp_connections);

  /* Now initialize each connection structure */

  for (i = 0; i < CONFIG_NET_TCP_CONNS; i++)
    {
      /* Mark the connection closed and move it to the free list */

      g_tcp_connections[i].tcpstateflags = TCP_CLOSED;
      dq_addlast(&g_tcp_connections[i].node, &g_free_tcp_connections);
    }

  g_last_tcp_port = 1024;
}

/****************************************************************************
 * Name: tcp_alloc
 *
 * Description:
 *   Find a free TCP/IP connection structure and allocate it
 *   for use.  This is normally something done by the implementation of the
 *   socket() API but is also called from the interrupt level when a TCP
 *   packet is received while "listening"
 *
 ****************************************************************************/

FAR struct tcp_conn_s *tcp_alloc(uint8_t domain)
{
  FAR struct tcp_conn_s *conn;
  net_lock_t flags;

  /* Because this routine is called from both interrupt level and
   * and from user level, we have not option but to disable interrupts
   * while accessing g_free_tcp_connections[];
   */

  flags = net_lock();

  /* Return the entry from the head of the free list */

  conn = (FAR struct tcp_conn_s *)dq_remfirst(&g_free_tcp_connections);

#ifndef CONFIG_NET_SOLINGER
  /* Is the free list empty? */

  if (!conn)
    {
      /* As a fall-back, check for connection structures which can be stalled.
       *
       * Search the active connection list for the oldest connection
       * that is about to be closed anyway.
       */

      FAR struct tcp_conn_s *tmp =
        (FAR struct tcp_conn_s *)g_active_tcp_connections.head;

      while (tmp)
        {
          nllvdbg("conn: %p state: %02x\n", tmp, tmp->tcpstateflags);

          /* Is this connection in a state we can sacrifice. */

          /* REVISIT: maybe we could check for SO_LINGER but it's buried
           * in the socket layer.
           */

          if (tmp->tcpstateflags == TCP_CLOSING    ||
              tmp->tcpstateflags == TCP_FIN_WAIT_1 ||
              tmp->tcpstateflags == TCP_FIN_WAIT_2 ||
              tmp->tcpstateflags == TCP_TIME_WAIT  ||
              tmp->tcpstateflags == TCP_LAST_ACK)
            {
              /* Yes.. Is it the oldest one we have seen so far? */

              if (!conn || tmp->timer > conn->timer)
                {
                  /* Yes.. remember it */

                  conn = tmp;
                }
            }

          /* Look at the next active connection */

          tmp = (FAR struct tcp_conn_s *)tmp->node.flink;
        }

      /* Did we find a connection that we can re-use? */

      if (conn != NULL)
        {
          nlldbg("Closing unestablished connection: %p\n", conn);

          /* Yes... free it.  This will remove the connection from the list
           * of active connections and release all resources held by the
           * connection.
           *
           * REVISIT:  Could there be any higher level, socket interface
           * that needs to be informed that we did this to them?
           *
           * Actually yes. When CONFIG_NET_SOLINGER is enabled there is a
           * pending callback in netclose_disconnect waiting for getting
           * woken up.  Otherwise there's the callback too, but no one is
           * waiting for it.
           */

          tcp_free(conn);

          /* Now there is guaranteed to be one free connection.  Get it! */

          conn = (FAR struct tcp_conn_s *)dq_remfirst(&g_free_tcp_connections);
        }
    }
#endif

  net_unlock(flags);

  /* Mark the connection allocated */

  if (conn)
    {
      memset(conn, 0, sizeof(struct tcp_conn_s));
      conn->tcpstateflags = TCP_ALLOCATED;
#if defined(CONFIG_NET_IPv4) && defined(CONFIG_NET_IPv6)
      conn->domain        = domain;
#endif
    }

  return conn;
}

/****************************************************************************
 * Name: tcp_free
 *
 * Description:
 *   Free a connection structure that is no longer in use. This should be
 *   done by the implementation of close()
 *
 ****************************************************************************/

void tcp_free(FAR struct tcp_conn_s *conn)
{
  FAR struct devif_callback_s *cb;
  FAR struct devif_callback_s *next;
#ifdef CONFIG_NET_TCP_WRITE_BUFFERS
  FAR struct tcp_wrbuffer_s *wrbuffer;
#endif
  net_lock_t flags;

  /* Because g_free_tcp_connections is accessed from user level and interrupt
   * level, code, it is necessary to keep interrupts disabled during this
   * operation.
   */

  DEBUGASSERT(conn->crefs == 0);
  flags = net_lock();

  /* Free remaining callbacks, actually there should be only the close callback
   * left.
   */

  for (cb = conn->list; cb; cb = next)
    {
      next = cb->flink;
      tcp_callback_free(conn, cb);
    }

  /* TCP_ALLOCATED means that that the connection is not in the active list
   * yet.
   */

  if (conn->tcpstateflags != TCP_ALLOCATED)
    {
      /* Remove the connection from the active list */

      dq_rem(&conn->node, &g_active_tcp_connections);
    }

#ifdef CONFIG_NET_TCP_READAHEAD
  /* Release any read-ahead buffers attached to the connection */

  iob_free_queue(&conn->readahead);
#endif

#ifdef CONFIG_NET_TCP_WRITE_BUFFERS
  /* Release any write buffers attached to the connection */

  while ((wrbuffer = (struct tcp_wrbuffer_s *)sq_remfirst(&conn->write_q)) != NULL)
    {
      tcp_wrbuffer_release(wrbuffer);
    }

  while ((wrbuffer = (struct tcp_wrbuffer_s *)sq_remfirst(&conn->unacked_q)) != NULL)
    {
      tcp_wrbuffer_release(wrbuffer);
    }
#endif

#ifdef CONFIG_NET_TCPBACKLOG
  /* Remove any backlog attached to this connection */

  if (conn->backlog)
    {
      tcp_backlogdestroy(conn);
    }

  /* If this connection is, itself, backlogged, then remove it from the
   * parent connection's backlog list.
   */

  if (conn->blparent)
    {
      tcp_backlogdelete(conn->blparent, conn);
    }
#endif

  /* Mark the connection available and put it into the free list */

  conn->tcpstateflags = TCP_CLOSED;
  dq_addlast(&conn->node, &g_free_tcp_connections);
  net_unlock(flags);
}

/****************************************************************************
 * Name: tcp_active
 *
 * Description:
 *   Find a connection structure that is the appropriate
 *   connection to be used with the provided TCP/IP header
 *
 * Assumptions:
 *   This function is called from UIP logic at interrupt level
 *
 ****************************************************************************/

FAR struct tcp_conn_s *tcp_active(FAR struct net_driver_s *dev,
                                  FAR struct tcp_hdr_s *tcp)
{
  FAR struct net_iphdr_s *ip = IPv4;
  FAR struct tcp_conn_s *conn;
  in_addr_t srcipaddr;
#ifdef CONFIG_NETDEV_MULTINIC
  in_addr_t destipaddr;
#endif

  conn       = (FAR struct tcp_conn_s *)g_active_tcp_connections.head;
  srcipaddr  = net_ip4addr_conv32(ip->srcipaddr);
#ifdef CONFIG_NETDEV_MULTINIC
  destipaddr = net_ip4addr_conv32(ip->destipaddr);
#endif

  while (conn)
    {
      /* Find an open connection matching the TCP input. The following
       * checks are performed:
       *
       * - The local port number is checked against the destination port
       *   number in the received packet.
       * - The remote port number is checked if the connection is bound
       *   to a remote port.
       * - If multiple network interfaces are supported, then the local
       *   IP address is available and we will insist that the
       *   destination IP matches the bound address. If a socket is
       *   bound to INADDRY_ANY, then it should receive all packets
       *   directed to the port.
       * - Finally, if the connection is bound to a remote IP address,
       *   the source IP address of the packet is checked.
       *
       * If all of the above are true then the newly received TCP packet
       * is destined for this TCP connection.
       */

      if (conn->tcpstateflags != TCP_CLOSED &&
          tcp->destport == conn->lport &&
          tcp->srcport  == conn->rport &&
#ifdef CONFIG_NETDEV_MULTINIC
          (net_ipv4addr_cmp(conn->u.ipv4.laddr, g_ipv4_allzeroaddr) ||
           net_ipv4addr_cmp(destipaddr, conn->u.ipv4.laddr)) &&
#endif
          net_ipv4addr_cmp(srcipaddr, conn->u.ipv4.raddr))
        {
          /* Matching connection found.. break out of the loop and return a
           * reference to it.
           */

          break;
        }

      /* Look at the next active connection */

      conn = (FAR struct tcp_conn_s *)conn->node.flink;
    }

  return conn;
}

/****************************************************************************
 * Name: tcp_nextconn
 *
 * Description:
 *   Traverse the list of active TCP connections
 *
 * Assumptions:
 *   This function is called from UIP logic at interrupt level (or with
 *   interrupts disabled).
 *
 ****************************************************************************/

FAR struct tcp_conn_s *tcp_nextconn(FAR struct tcp_conn_s *conn)
{
  if (!conn)
    {
      return (FAR struct tcp_conn_s *)g_active_tcp_connections.head;
    }
  else
    {
      return (FAR struct tcp_conn_s *)conn->node.flink;
    }
}

/****************************************************************************
 * Name: tcp_alloc_accept
 *
 * Description:
 *    Called when driver interrupt processing matches the incoming packet
 *    with a connection in LISTEN. In that case, this function will create
 *    a new connection and initialize it to send a SYNACK in return.
 *
 * Assumptions:
 *   This function is called from UIP logic at interrupt level
 *
 ****************************************************************************/

FAR struct tcp_conn_s *tcp_alloc_accept(FAR struct net_driver_s *dev,
                                        FAR struct tcp_hdr_s *tcp)
{
  FAR struct net_iphdr_s *ip = IPv4;
  FAR struct tcp_conn_s *conn;

#ifdef CONFIG_NET_IPv4
  conn = tcp_alloc(PF_INET);
#else
  conn = tcp_alloc(PF_INET6);
#endif
  if (conn)
    {
      /* Fill in the necessary fields for the new connection. */

      conn->rto           = TCP_RTO;
      conn->timer         = TCP_RTO;
      conn->sa            = 0;
      conn->sv            = 4;
      conn->nrtx          = 0;
      conn->lport         = tcp->destport;
      conn->rport         = tcp->srcport;
      conn->mss           = TCP_IPv4_INITIAL_MSS(dev);
      net_ipv4addr_copy(conn->u.ipv4.raddr, net_ip4addr_conv32(ip->srcipaddr));
#ifdef CONFIG_NETDEV_MULTINIC
      net_ipv4addr_copy(conn->u.ipv4.laddr, net_ip4addr_conv32(ip->destipaddr));
#endif
      conn->tcpstateflags = TCP_SYN_RCVD;

      tcp_initsequence(conn->sndseq);
      conn->unacked       = 1;
#ifdef CONFIG_NET_TCP_WRITE_BUFFERS
      conn->expired       = 0;
      conn->isn           = 0;
      conn->sent          = 0;
#endif

      /* rcvseq should be the seqno from the incoming packet + 1. */

      memcpy(conn->rcvseq, tcp->seqno, 4);

#ifdef CONFIG_NET_TCP_READAHEAD
      /* Initialize the list of TCP read-ahead buffers */

      IOB_QINIT(&conn->readahead);
#endif

#ifdef CONFIG_NET_TCP_WRITE_BUFFERS
      /* Initialize the write buffer lists */

      sq_init(&conn->write_q);
      sq_init(&conn->unacked_q);
#endif

      /* And, finally, put the connection structure into the active list.
       * Interrupts should already be disabled in this context.
       */

      dq_addlast(&conn->node, &g_active_tcp_connections);
    }

  return conn;
}

/****************************************************************************
 * Name: tcp_bind
 *
 * Description:
 *   This function implements the lower level parts of the standard TCP
 *   bind() operation.
 *
 * Return:
 *   0 on success or -EADDRINUSE on failure
 *
 * Assumptions:
 *   This function is called from normal user level code.
 *
 ****************************************************************************/

#ifdef CONFIG_NET_IPv6
int tcp_bind(FAR struct tcp_conn_s *conn,
             FAR const struct sockaddr_in6 *addr)
#else
int tcp_bind(FAR struct tcp_conn_s *conn,
             FAR const struct sockaddr_in *addr)
#endif
{
  net_lock_t flags;
  int port;
#ifdef CONFIG_NETDEV_MULTINIC
  in_addr_t ipaddr;
#endif

  /* Verify or select a local port and address */

  flags = net_lock();

#ifdef CONFIG_NETDEV_MULTINIC
#ifdef CONFIG_NET_IPv6
  /* Get the IPv6 address that we are binding to */

  ipaddr = addr->sin6_addr.in6_u.u6_addr16;

#else
  /* Get the IPv4 address that we are binding to */

  ipaddr = addr->sin_addr.s_addr;

#endif

  port = tcp_selectport(ipaddr, ntohs(addr->sin_port));
#else
  /* Verify or select a local port */

  port = tcp_selectport(ntohs(addr->sin_port));
#endif

  net_unlock(flags);

  if (port < 0)
    {
      return port;
    }

  /* Save the local address in the connection structure. */

  conn->lport = addr->sin_port;

#ifdef CONFIG_NETDEV_MULTINIC
  net_ipv4addr_copy(conn->u.ipv4.laddr, ipaddr);
#endif

  return OK;
}

/****************************************************************************
 * Name: tcp_connect
 *
 * Description:
 *   This function implements the lower level parts of the standard
 *   TCP connect() operation:  It connects to a remote host using TCP.
 *
 *   This function is used to start a new connection to the specified
 *   port on the specified host. It uses the connection structure that was
 *   allocated by a preceding socket() call.  It sets the connection to
 *   the SYN_SENT state and sets the retransmission timer to 0. This will
 *   cause a TCP SYN segment to be sent out the next time this connection
 *   is periodically processed, which usually is done within 0.5 seconds
 *   after the call to tcp_connect().
 *
 * Assumptions:
 *   This function is called from normal user level code.
 *
 ****************************************************************************/

int tcp_connect(FAR struct tcp_conn_s *conn, FAR const struct sockaddr *addr)
{
  net_lock_t flags;
  int port;

  /* The connection is expected to be in the TCP_ALLOCATED state.. i.e.,
   * allocated via up_tcpalloc(), but not yet put into the active connections
   * list.
   */

  if (!conn || conn->tcpstateflags != TCP_ALLOCATED)
    {
      return -EISCONN;
    }

  /* If the TCP port has not already been bound to a local port, then select
   * one now.  We assume that the IP address has been bound to a local device,
   * but the port may still be INPORT_ANY.
   */

  flags = net_lock();
#ifdef CONFIG_NETDEV_MULTINIC
  port = tcp_selectport(conn->u.ipv4.laddr, ntohs(conn->lport));
#else
  port = tcp_selectport(ntohs(conn->lport));
#endif
  net_unlock(flags);

  if (port < 0)
    {
      return port;
    }

  /* Initialize and return the connection structure, bind it to the port
   * number.  At this point, we do not know the size of the initial MSS We
   * know the total size of the packet buffer, but we don't yet know the
   * size of link layer header.
   */

  conn->tcpstateflags = TCP_SYN_SENT;
  tcp_initsequence(conn->sndseq);

  conn->unacked    = 1;    /* TCP length of the SYN is one. */
  conn->nrtx       = 0;
  conn->timer      = 1;    /* Send the SYN next time around. */
  conn->rto        = TCP_RTO;
  conn->sa         = 0;
  conn->sv         = 16;   /* Initial value of the RTT variance. */
  conn->lport      = htons((uint16_t)port);
#ifdef CONFIG_NET_TCP_WRITE_BUFFERS
  conn->expired    = 0;
  conn->isn        = 0;
  conn->sent       = 0;
#endif

  /* Save values that are specific to the IP address domain */

#ifdef CONFIG_NET_IPv4
#ifdef CONFIG_NET_IPv6
  if (conn->domain == PF_INET)
#endif
    {
      FAR const struct sockaddr_in *inaddr =
        (FAR const struct sockaddr_in *)addr;

      /* Save MSS and the port from the sockaddr (already in network order) */

      conn->mss    = MIN_IPv4_TCP_INITIAL_MSS;
      conn->rport  = inaddr->sin_port;

      /* The sockaddr address is 32-bits in network order. */

      net_ipv4addr_copy(conn->u.ipv4.raddr, inaddr->sin_addr.s_addr);
    }
#endif /* CONFIG_NET_IPv4 */

#ifdef CONFIG_NET_IPv6
#ifdef CONFIG_NET_IPv4
  else
#endif
    {
      FAR const struct sockaddr_in6 *inaddr =
        (FAR const struct sockaddr_in6 *)addr;

      /* Save MSS and the port from the sockaddr (already in network order) */

      conn->mss     = MIN_IPv6_TCP_INITIAL_MSS;
      conn->rport   = inaddr->sin_port;

      /* The sockaddr address is 32-bits in network order. */

      net_ipv6addr_copy(conn->u.ipv6.raddr, inaddr->sin6_addr.s6_addr16);
    }
#endif /* CONFIG_NET_IPv6 */

#ifdef CONFIG_NET_TCP_READAHEAD
  /* Initialize the list of TCP read-ahead buffers */

  IOB_QINIT(&conn->readahead);
#endif

#ifdef CONFIG_NET_TCP_WRITE_BUFFERS
  /* Initialize the TCP write buffer lists */

  sq_init(&conn->write_q);
  sq_init(&conn->unacked_q);
#endif

  /* And, finally, put the connection structure into the active
   * list. Because g_active_tcp_connections is accessed from user level and
   * interrupt level, code, it is necessary to keep interrupts disabled during
   * this operation.
   */

  flags = net_lock();
  dq_addlast(&conn->node, &g_active_tcp_connections);
  net_unlock(flags);

  return OK;
}

#endif /* CONFIG_NET && CONFIG_NET_TCP */
