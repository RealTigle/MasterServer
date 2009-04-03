/* $Id$ */

#include "shared/stdafx.h"
#include "shared/debug.h"
#include "shared/network/core/core.h"
#include "contentserver.h"

/**
 * @file contentserver/handler.cpp Handler of queries to the content server
 */

ContentServer::ContentServer(SQL *sql, NetworkAddress address) : Server(sql), first(NULL)
{
	this->listen_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (this->listen_socket == INVALID_SOCKET) {
		error("Could not bind to %s; cannot open socket\n", address.GetAddressAsString());
	}

	/* reuse the socket */
	int reuse = 1;
	/* The (const char*) cast is needed for windows!! */
	if (setsockopt(this->listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) == -1) {
		error("Could not bind to %s; setsockopt() failed\n", address.GetAddressAsString());
	}

	if (!SetNonBlocking(this->listen_socket)) {
		error("Could not bind to %s; could not make socket non-blocking failed\n", address.GetAddressAsString());
	}

	if (bind(this->listen_socket, (struct sockaddr*)address.GetAddress(), sizeof(*address.GetAddress())) != 0) {
		error("Could not bind to %s; bind() failed\n", address.GetAddressAsString());
	}

	if (listen(this->listen_socket, 1) != 0) {
		error("Could not bind to %s; listen() failed\n", address.GetAddressAsString());
	}
}

ContentServer::~ContentServer()
{
	closesocket(this->listen_socket);
	while (this->first != NULL) delete this->first;
}

void ContentServer::AcceptClients()
{
	for (;;) {
		struct sockaddr_storage sin;
		socklen_t sin_len = sizeof(sin);
		SOCKET s = accept(this->listen_socket, (struct sockaddr*)&sin, &sin_len);
		if (s == INVALID_SOCKET) return;

		if (!SetNonBlocking(s) || !SetNoDelay(s)) return;

		new ServerNetworkContentSocketHandler(this, s, NetworkAddress(sin, sin_len));
	}
}

void ContentServer::RealRun()
{
	while (!this->stop_server) {
		fd_set read_fd, write_fd;
		struct timeval tv;

		FD_ZERO(&read_fd);
		FD_ZERO(&write_fd);

		for (ServerNetworkContentSocketHandler *cs = this->first; cs != NULL; cs = cs->next) {
			if (!cs->IsPacketQueueEmpty() || cs->HasQueue()) {
				FD_SET(cs->sock, &write_fd);
			} else {
				FD_SET(cs->sock, &read_fd);
			}
		}

		/* take care of listener port */
		FD_SET(this->listen_socket, &read_fd);

		/* Wait for a second */
		tv.tv_sec = 1;
		tv.tv_usec = 0;
#if !defined(__MORPHOS__) && !defined(__AMIGA__)
		select(FD_SETSIZE, &read_fd, &write_fd, NULL, &tv);
#else
		WaitSelect(FD_SETSIZE, &read_fd, &write_fd, NULL, &tv, NULL);
#endif
		/* accept clients.. */
		if (FD_ISSET(this->listen_socket, &read_fd)) {
			this->AcceptClients();
		}

		/* read stuff from clients/write to them */
		for (ServerNetworkContentSocketHandler *cs = this->first; cs != NULL;) {
			if (FD_ISSET(cs->sock, &write_fd)) {
				cs->writable = true;
				while (cs->Send_Packets() && cs->IsPacketQueueEmpty() && cs->HasQueue()) {
					cs->SendQueue();
				}
			} else if (FD_ISSET(cs->sock, &read_fd)) {
				/* Only receive packets when our outgoing packet queue is empty. This
				 * way we prevent internal memory overflows when people start
				 * bombarding the server with enormous requests. */
				cs->Recv_Packets();
			}

			/* Check whether we should delete the socket */
			ServerNetworkContentSocketHandler *cur_cs = cs;
			cs = cs->next;
			if (cur_cs->HasClientQuit()) delete cur_cs;
		}

		CSleep(1);
	}
}

ServerNetworkContentSocketHandler::ServerNetworkContentSocketHandler(ContentServer *cs, SOCKET s, NetworkAddress sin) : NetworkContentSocketHandler(s, sin), cs(cs)
{
	this->next = cs->first;
	cs->first = this;

	this->contentQueue = NULL;
}

ServerNetworkContentSocketHandler::~ServerNetworkContentSocketHandler()
{
	ServerNetworkContentSocketHandler **prev = &cs->first;
	while (*prev != this) {
		assert(*prev != NULL);
		prev = &(*prev)->next;
	}

	*prev = this->next;

	delete [] this->contentQueue;
}
