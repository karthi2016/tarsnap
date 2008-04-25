#include "bsdtar_platform.h"

#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "netpacket_internal.h"
#include "netproto.h"
#include "sysendian.h"
#include "warnp.h"

#include "netpacket.h"

static int reconnect(NETPACKET_CONNECTION * NPC);
static network_callback callback_connect;
static network_callback callback_reconnect;
static int callback_getbuf(void *, uint8_t, uint8_t **, size_t);
static network_callback callback_packetreceived;

/* Maximum number of times we'll try to reconnect. */
#define MAXRECONNECTS	10

/* Time to wait between each attempt. */
static int reconnect_wait[MAXRECONNECTS + 1] = {
    0, 0, 1, 2, 4, 8, 15, 30, 60, 90, 90
};

/**
 * netpacket_open(void):
 * Return a netpacket connection cookie.
 */
NETPACKET_CONNECTION *
netpacket_open(void)
{
	struct netpacket_internal * NPC;

	/* Construct cookie structure. */
	if ((NPC = malloc(sizeof(struct netpacket_internal))) == NULL)
		goto err0;
	NPC->NC = NULL;
	NPC->packetbuf = NULL;

	/* We're not connected yet. */
	NPC->state = 0;

	/* We haven't lost any connections yet. */
	NPC->ndrops = 0;

	/* We've never successfully connected to the server. */
	NPC->serveralive = 0;

	/* We're not reading a packet yet. */
	NPC->reading = 0;

	/* We're using the default getbuf function. */
	NPC->getbuf = callback_getbuf;

	/* The queue is empty. */
	NPC->pending_head = NPC->pending_tail = NPC->pending_current = NULL;

	/* Success! */
	return (NPC);

err0:
	/* Failure! */
	return (NULL);
}

static int
callback_connect(void * cookie, int status)
{
	struct netpacket_internal * NPC = cookie;

	/* If we're being cancelled, return. */
	if (status == NETWORK_STATUS_CANCEL)
		goto done;

	/* Did we successfully connect? */
	if (status == NETWORK_STATUS_OK) {
		/* We're connected. */
		NPC->state = 2;
		NPC->serveralive = 1;

		/* If there are pending operation(s), do them now. */
		for (NPC->pending_current = NPC->pending_head;
		    NPC->pending_current != NULL;
		    NPC->pending_current = NPC->pending_current->next) {
			if ((NPC->pending_current->writepacket)(
			    NPC->pending_current->cookie, NPC))
				goto err0;
		}
	} else {
		/* Try again... */
		if (reconnect(NPC))
			goto err0;
	}

done:
	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

/**
 * netpacket_op(NPC, writepacket, cookie):
 * Call ${writepacket} to send a request to the server over the provided
 * netpacket connection.
 */
int
netpacket_op(NETPACKET_CONNECTION * NPC,
    sendpacket_callback * writepacket, void * cookie)
{
	struct netpacket_op * op;

	/* Allocate memory for cookie. */
	if ((op = malloc(sizeof(struct netpacket_op))) == NULL)
		goto err0;

	/* Store parameters for request. */
	op->writepacket = writepacket;
	op->cookie = cookie;

	/* Add operation to queue. */
	op->next = NULL;
	if (NPC->pending_tail != NULL) {
		NPC->pending_tail->next = op;
		NPC->pending_tail = op;
	} else {
		NPC->pending_head = op;
		NPC->pending_tail = op;
	}

	switch (NPC->state) {
	case 0:
		/* We need to connect to the server. */
		if ((NPC->NC = netproto_connect(callback_connect,
		    NPC)) == NULL)
			goto err0;
		NPC->state = 1;
		break;
	case 1:
		/*
		 * Do nothing: The packet will be sent from callback_connect
		 * once the connection is established.
		 */
		break;
	case 2:
		/* We're already connected: Send the packet. */
		NPC->pending_current = op;
		if ((op->writepacket)(op->cookie, NPC))
			goto err0;
		break;
	}

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

static int
callback_reconnect(void * cookie, int status)
{
	struct netpacket_internal * NPC = cookie;

	/* If we're being cancelled, return. */
	if (status == NETWORK_STATUS_CANCEL)
		goto done;

	/* The status should be NETWORK_STATUS_TIMEOUT. */
	if (status != NETWORK_STATUS_TIMEOUT) {
		warn0("Bad status in callback_reconnect: %d", status);
		goto err0;
	}

	/* Close the (dead) connection. */
	if (netproto_close(NPC->NC))
		goto err1;
	NPC->NC = NULL;

	/* Open a new connection. */
	if ((NPC->NC = netproto_connect(callback_connect, NPC)) == NULL)
		goto err0;

done:
	/* Success! */
	return (0);

err1:
	NPC->NC = NULL;
err0:
	/* Failure! */
	return (-1);
}

static int
reconnect(NETPACKET_CONNECTION * NPC)
{
	int nseconds;

	/* Flush any pending activity on the socket. */
	if (netproto_flush(NPC->NC))
		goto err0;

	/* We're trying to reconnect. */
	NPC->state = 1;

	/* We're not reading a packet any more, if we ever were. */
	NPC->reading = 0;

	/* Have we lost out connection / failed to connect too many times? */
	NPC->ndrops += 1;
	if ((NPC->ndrops > 10) ||
	    (NPC->serveralive == 0 && NPC->ndrops > 1)) {
		warn0("Too many network failures");
		goto err0;
	}

	/* Figure out how long we ought to wait before reconnecting. */
	nseconds = reconnect_wait[NPC->ndrops];

	/* Warn the user that we're waiting. */
	if (nseconds != 0)
		warn0("Connection lost, "
		    "waiting %d seconds before reconnecting", nseconds);

	/* Set a callback to reconnect. */
	if (netproto_sleep(NPC->NC, nseconds, callback_reconnect, NPC))
		goto err0;

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

int
netpacket_op_packetsent(void * cookie, int status)
{
	struct netpacket_internal * NPC = cookie;
	int rc = 0;

	/* If we're being cancelled, return. */
	if (status == NETWORK_STATUS_CANCEL)
		goto done;

	/* Try to reconnect if there was an error. */
	if (status != NETWORK_STATUS_OK) {
		if (reconnect(NPC))
			goto err0;
		goto done;
	}

	/* We want to read a response packet if we're not already doing so. */
	if (NPC->reading == 0) {
		if (netproto_readpacket(NPC->NC, NPC->getbuf,
		    callback_packetreceived, NPC))
			goto err0;
		NPC->reading = 1;
	}

done:
	/* Return success or the return code from the callback. */
	return (rc);

err0:
	/* Failure! */
	return (-1);
}

static int
callback_getbuf(void * cookie, uint8_t type, uint8_t ** buf,
    size_t buflen)
{
	struct netpacket_internal * NPC = cookie;
	int status = NETWORK_STATUS_OK;

	/* Store packet type and length for future reference. */
	NPC->packettype = type;
	NPC->packetlen = buflen;

	/* Sanity-check the type and length. */
	switch (type) {
	case NETPACKET_REGISTER_CHALLENGE:
		if (buflen != 32 + CRYPTO_DH_PUBLEN)
			status = NETPROTO_STATUS_PROTERR;
		break;
	case NETPACKET_REGISTER_RESPONSE:
		if (buflen != 41)
			status = NETPROTO_STATUS_PROTERR;
		break;
	case NETPACKET_TRANSACTION_START_RESPONSE:
		if (buflen != 33)
			status = NETPROTO_STATUS_PROTERR;
		break;
	case NETPACKET_TRANSACTION_GETNONCE_RESPONSE:
	case NETPACKET_TRANSACTION_COMMIT_RESPONSE:
		if (buflen != 32)
			status = NETPROTO_STATUS_PROTERR;
		break;
	case NETPACKET_WRITE_FEXIST_RESPONSE:
	case NETPACKET_WRITE_FILE_RESPONSE:
	case NETPACKET_DELETE_FILE_RESPONSE:
		if (buflen != 66)
			status = NETPROTO_STATUS_PROTERR;
		break;
	case NETPACKET_READ_FILE_RESPONSE:
		if ((buflen < 70) || (buflen > 70 + 262144))
			status = NETPROTO_STATUS_PROTERR;
		break;
	case NETPACKET_DIRECTORY_RESPONSE:
		if ((buflen < 70) || (buflen > 70 + 8000 * 32) ||
		    (buflen % 32 != 70 % 32))
			status = NETPROTO_STATUS_PROTERR;
		break;
	default:
		/* Unknown packet type. */
		status = NETPROTO_STATUS_PROTERR;
		break;
	}

	/* If the length is valid, we need to allocate a buffer. */
	if (status == NETWORK_STATUS_OK) {
		*buf = NPC->packetbuf = malloc(buflen);
		if (*buf == NULL)
			status = NETWORK_STATUS_ERR;
	}

	return (status);
}

static int
callback_packetreceived(void * cookie, int status)
{
	struct netpacket_internal * NPC = cookie;
	struct netpacket_op * head = NPC->pending_head;
	handlepacket_callback * handlepacket = head->handlepacket;
	int rc = 0;

	/* If we're being cancelled, return. */
	if (status == NETWORK_STATUS_CANCEL)
		goto done;

	/* On non-protocol errors, try to reconnect. */
	if ((status != NETWORK_STATUS_OK) &&
	    (status != NETPROTO_STATUS_PROTERR)) {
		if (reconnect(NPC))
			goto err0;
		goto done;
	}

	/* Call upstream callback. */
	head->handlepacket = NULL;
	NPC->pending_current = head;
	rc = handlepacket(head->cookie, NPC, status, NPC->packettype,
	    NPC->packetbuf, NPC->packetlen);

	/* If we didn't send a follow-up packet, this operation is done. */
	if (head->handlepacket == NULL) {
		if (NPC->pending_tail == head)
			NPC->pending_tail = NULL;
		NPC->pending_head = head->next;
		free(head);

		/* We have successfully performed an operation. */
		NPC->ndrops = 0;
	}

	/* Read another packet if appropriate. */
	if ((NPC->pending_head != NULL) &&
	    (NPC->pending_head->handlepacket != NULL)) {
		if (netproto_readpacket(NPC->NC, NPC->getbuf,
		    callback_packetreceived, NPC))
			goto err0;
	} else {
		NPC->reading = 0;
	}

done:
	/* Free the packet buffer. */
	free(NPC->packetbuf);
	NPC->packetbuf = NULL;

	/* Return value from callback. */
	return (rc);

err0:
	/* Failure! */
	return (-1);
}

/**
 * netpacket_close(NPC):
 * Close a netpacket connection.
 */
int
netpacket_close(NETPACKET_CONNECTION * NPC)
{
	struct netpacket_op * next;

	/* Close the network protocol layer connection if we have one. */
	if (NPC->NC != NULL)
		if (netproto_close(NPC->NC))
			goto err1;

	/* Free any queued operations. */
	NPC->pending_current = NPC->pending_head;
	while (NPC->pending_current != NULL) {
		next = NPC->pending_current->next;
		free(NPC->pending_current);
		NPC->pending_current = next;
	}

	/* Free the cookie. */
	free(NPC);

	/* Success! */
	return (0);

err1:
	NPC->pending_current = NPC->pending_head;
	while (NPC->pending_current != NULL) {
		next = NPC->pending_current->next;
		free(NPC->pending_current);
		NPC->pending_current = next;
	}

	free(NPC);

	/* Failure! */
	return (-1);
}
