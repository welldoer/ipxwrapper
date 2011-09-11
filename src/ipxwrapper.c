/* ipxwrapper - Library functions
 * Copyright (C) 2008 Daniel Collins <solemnwarning@solemnwarning.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <windows.h>
#include <winsock2.h>
#include <wsipx.h>
#include <nspapi.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

#include "ipxwrapper.h"
#include "common.h"
#include "interface.h"
#include "router.h"

#define DLL_UNLOAD(dll) \
	if(dll) {\
		FreeModule(dll);\
		dll = NULL;\
	}

ipx_socket *sockets = NULL;
ipx_host *hosts = NULL;
SOCKET send_fd = -1;
struct reg_global global_conf;

struct rclient g_rclient;

static CRITICAL_SECTION sockets_cs;
static CRITICAL_SECTION hosts_cs;

#define INIT_CS(cs) if(!init_cs(cs, &initialised_cs)) { return FALSE; }

static BOOL init_cs(CRITICAL_SECTION *cs, int *counter) {
	if(!InitializeCriticalSectionAndSpinCount(cs, 0x80000000)) {
		log_printf("Failed to initialise critical section: %s", w32_error(GetLastError()));
		return FALSE;
	}
	
	(*counter)++;
	return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE me, DWORD why, LPVOID res) {
	static int initialised_cs = 0;
	
	if(why == DLL_PROCESS_ATTACH) {
		log_open();
		
		if(!rclient_init(&g_rclient)) {
			return FALSE;
		}
		
		reg_open(KEY_QUERY_VALUE);
		
		if(reg_get_bin("global", &global_conf, sizeof(global_conf)) != sizeof(global_conf)) {
			global_conf.udp_port = DEFAULT_PORT;
			global_conf.w95_bug = 1;
			global_conf.bcast_all = 0;
			global_conf.filter = 1;
		}
		
		INIT_CS(&sockets_cs);
		INIT_CS(&hosts_cs);
		
		WSADATA wsdata;
		int err = WSAStartup(MAKEWORD(1,1), &wsdata);
		if(err) {
			log_printf("Failed to initialize winsock: %s", w32_error(err));
			return FALSE;
		}
		
		if(!rclient_start(&g_rclient)) {
			return FALSE;
		}
		
		if(g_rclient.router) {
			send_fd = g_rclient.router->udp_sock;
		}else{
			/* Create UDP socket for sending packets if not using a private router */
			
			if((send_fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
				log_printf("Failed to create UDP socket: %s", w32_error(WSAGetLastError()));
				return FALSE;
			}
			
			struct sockaddr_in addr;
			addr.sin_family = AF_INET;
			addr.sin_addr.s_addr = INADDR_ANY;
			addr.sin_port = 0;
			
			if(bind(send_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
				log_printf("Failed to bind UDP socket (send_fd): %s", w32_error(WSAGetLastError()));
				return FALSE;
			}
		}
	}else if(why == DLL_PROCESS_DETACH) {
		if(send_fd != -1 && !g_rclient.router) {
			closesocket(send_fd);
		}
		
		send_fd = -1;
		
		rclient_stop(&g_rclient);
		
		WSACleanup();
		
		switch(initialised_cs) {
			case 2: DeleteCriticalSection(&hosts_cs);
			case 1: DeleteCriticalSection(&sockets_cs);
			default: break;
		}
		
		initialised_cs = 0;
		
		reg_close();
		
		unload_dlls();
		
		log_close();
	}
	
	return TRUE;
}

/* Lock the mutex and search the sockets list for an ipx_socket structure with
 * the requested fd, if no matching fd is found, unlock the mutex
 *
 * TODO: Change this behaviour. It is almost as bad as the BKL.
*/
ipx_socket *get_socket(SOCKET fd) {
	lock_sockets();
	
	ipx_socket *ptr = sockets;
	
	while(ptr) {
		if(ptr->fd == fd) {
			break;
		}
		
		ptr = ptr->next;
	}
	
	if(!ptr) {
		unlock_sockets();
	}
	
	return ptr;
}

/* Lock the mutex */
void lock_sockets(void) {
	EnterCriticalSection(&sockets_cs);
}

/* Unlock the mutex */
void unlock_sockets(void) {
	LeaveCriticalSection(&sockets_cs);
}

/* Add a host to the hosts list or update an existing one */
void add_host(const unsigned char *net, const unsigned char *node, uint32_t ipaddr) {
	EnterCriticalSection(&hosts_cs);
	
	ipx_host *hptr = hosts;
	
	while(hptr) {
		if(memcmp(hptr->ipx_net, net, 4) == 0 && memcmp(hptr->ipx_node, node, 6) == 0) {
			hptr->ipaddr = ipaddr;
			hptr->last_packet = time(NULL);
			
			LeaveCriticalSection(&hosts_cs);
			return;
		}
		
		hptr = hptr->next;
	}
	
	hptr = malloc(sizeof(ipx_host));
	if(!hptr) {
		LeaveCriticalSection(&hosts_cs);
		
		log_printf("No memory for hosts list entry");
		return;
	}
	
	memcpy(hptr->ipx_net, net, 4);
	memcpy(hptr->ipx_node, node, 6);
	
	hptr->ipaddr = ipaddr;
	hptr->last_packet = time(NULL);
	
	hptr->next = hosts;
	hosts = hptr;
	
	LeaveCriticalSection(&hosts_cs);
}

/* Search the hosts list */
ipx_host *find_host(const unsigned char *net, const unsigned char *node) {
	EnterCriticalSection(&hosts_cs);
	
	ipx_host *hptr = hosts, *pptr = NULL;
	
	while(hptr) {
		if(memcmp(hptr->ipx_net, net, 4) == 0 && memcmp(hptr->ipx_node, node, 6) == 0) {
			if(hptr->last_packet+TTL < time(NULL)) {
				/* Host record has expired, delete */
				
				if(pptr) {
					pptr->next = hptr->next;
					free(hptr);
				}else{
					hosts = hptr->next;
					free(hptr);
				}
				
				hptr = NULL;
			}
			
			break;
		}
		
		pptr = hptr;
		hptr = hptr->next;
	}
	
	LeaveCriticalSection(&hosts_cs);
	return hptr;
}
