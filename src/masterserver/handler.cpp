/* $Id$ */

#include "shared/stdafx.h"
#include "shared/debug.h"
#include "shared/mysql.h"
#include "masterserver.h"
#include <time.h>

/**
 * @file masterserver/handler.cpp Handler of retries and updating the server list packet sent to clients
 */

/* Requerying of game servers */

MSQueriedServer::MSQueriedServer(NetworkAddress query_address, NetworkAddress reply_address, uint64 session_key, uint frame) : QueriedServer(query_address, frame)
{
	this->reply_address = reply_address;
	this->session_key = session_key;
}

void MSQueriedServer::DoAttempt(UDPServer *server)
{
	/* Not yet waited long enough for a next attempt */
	if (this->frame + SERVER_QUERY_TIMEOUT > server->GetFrame()) return;

	/* The server did not respond in time, retry */
	this->attempts++;

	if (this->attempts > SERVER_QUERY_ATTEMPTS) {
		/* We tried too many times already */
		DEBUG(net, 4, "[retry] too many server query attempts for %s", this->server_address.GetAddressAsString());

		server->RemoveQueriedServer(this);
		delete this;
		return;
	}

	DEBUG(net, 4, "[retry] querying %s", this->server_address.GetAddressAsString());

	/* Resend query */
	this->SendFindGameServerPacket(server->GetQuerySocket());

	this->frame = server->GetFrame();
}

MasterServer::MasterServer(SQL *sql, NetworkAddressList &addresses) : UDPServer(sql, addresses, new QueryNetworkUDPSocketHandler(this))
{
	this->serverlist_packet        = NULL;
	this->update_serverlist_packet = true;
	this->next_serverlist_frame    = 0;
	this->session_key              = time(NULL) << 16;
	srandom(this->session_key);

	this->master_socket = new MasterNetworkUDPSocketHandler(this);

	/* Bind master socket*/
	if (!this->master_socket->Listen(addresses[0], false)) {
		error("Could not bind to %s\n", addresses[0].GetAddressAsString());
	}
}

MasterServer::~MasterServer()
{
	delete this->master_socket;
	delete this->serverlist_packet;
}

void MasterServer::ReceivePackets()
{
	UDPServer::ReceivePackets();
	this->master_socket->ReceivePackets();
}

uint64 MasterServer::NextSessionKey()
{
	this->session_key += 1 + (random() & 0xFF);
	return this->session_key;
}

/**
 * Returns the packets with the game server list. This packet will
 * be updated/regenerated whenever needed, i.e. when we know a server
 * went online/offline or after a timeout as the updater can change
 * the state of a server too.
 * @return the serverlist packet
 * @post return != NULL
 */
Packet *MasterServer::GetServerListPacket()
{
	/* Rebuild the packet only once in a while */
	if (this->update_serverlist_packet || this->next_serverlist_frame < this->frame) {
		uint16 count;

		/*
		 * Due to the limited size of the packet, and the fact that we only send
		 * one packet with advertised servers, we have to limit the amount of
		 * servers we can put into the packet.
		 *
		 * For this we use the maximum size of the packet, substract the bytes
		 * needed for the PacketSize, PacketType information (as in all packets)
		 * and the count of servers. This gives the number of bytes we can use
		 * to place the advertised servers in. Which is then divided by the
		 * amount of bytes needed to encode the IP address and the port of a
		 * single server.
		 */
		static const uint16 max_count = (sizeof(this->serverlist_packet->buffer) - sizeof(PacketSize) - sizeof(PacketType) - sizeof(count)) / (sizeof(uint32) + sizeof(uint16));

		DEBUG(net, 4, "[server list] rebuilding the server list");

		delete this->serverlist_packet;
		this->serverlist_packet = NetworkSend_Init(PACKET_UDP_MASTER_RESPONSE_LIST);
		this->serverlist_packet->Send_uint8(NETWORK_MASTER_SERVER_VERSION);

		NetworkAddress servers[max_count];
		count = this->sql->GetActiveServers(servers, max_count);

		/* Fill the packet */
		this->serverlist_packet->Send_uint16(count);
		for (int i = 0; i < count; i++) {
			this->serverlist_packet->Send_uint32(((sockaddr_in*)servers[i].GetAddress())->sin_addr.s_addr);
			this->serverlist_packet->Send_uint16(servers[i].GetPort());
		}

		/* Schedule the next retry */
		this->next_serverlist_frame    = this->frame + GAME_SERVER_LIST_AGE;
		this->update_serverlist_packet = false;
	}

	return this->serverlist_packet;
}
