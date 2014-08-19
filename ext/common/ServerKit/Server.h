/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2014 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_SERVER_KIT_SERVER_H_
#define _PASSENGER_SERVER_KIT_SERVER_H_

#include <Utils/sysqueue.h>

#include <boost/cstdint.hpp>
#include <oxt/system_calls.hpp>
#include <vector>
#include <new>
#include <ev++.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>

#include <Logging.h>
#include <SafeLibev.h>
#include <ServerKit/Context.h>
#include <ServerKit/Client.h>
#include <ServerKit/ClientRef.h>
#include <Utils.h>
#include <Utils/SmallVector.h>
#include <Utils/ScopeGuard.h>
#include <Utils/IOUtils.h>

namespace Passenger {
namespace ServerKit {

using namespace std;
using namespace boost;
using namespace oxt;


/*
start main server
start admin server
main loop

upon receiving exit signal on admin server:
	set all servers to shutdown mode
		- stop accepting new clients
		- allow some clients to finish their request
		- disconnect other clients
	wait until servers are done shutting down (client count == 0)
		exit main loop
*/

template<typename DerivedServer, typename Client = Client>
class Server: public HooksImpl {
public:
	/***** Types *****/
	typedef Client ClientType;
	typedef ClientRef<DerivedServer, Client> ClientRef;
	STAILQ_HEAD(FreeClientList, Client);
	TAILQ_HEAD(ClientList, Client);

	enum State {
		ACTIVE,
		TOO_MANY_FDS,
		STOPPING,
		STOPPED
	};

	/***** Configuration *****/
	unsigned int acceptBurstCount: 7;
	bool startReadingAfterAccept: 1;
	unsigned int minSpareClients: 12;
	unsigned int freelistLimit: 12;

	/***** Working state and statistics (do not modify) *****/
	State serverState;
	FreeClientList freeClients;
	ClientList activeClients, disconnectedClients;
	unsigned int freeClientCount, activeClientCount, disconnectedClientCount;

private:
	static const unsigned int MAX_ENDPOINTS = 4;

	Context *ctx;
	uint8_t nEndpoints;
	bool accept4Available;
	ev::timer acceptResumptionWatcher;
	ev::io endpoints[MAX_ENDPOINTS];


	/***** Private methods *****/

	static void _onAcceptable(EV_P_ ev_io *io, int revents) {
		static_cast<Server *>(io->data)->onAcceptable(io, revents);
	}

	void onAcceptable(ev_io *io, int revents) {
		Client *lastActiveClient = TAILQ_FIRST(&activeClients);
		unsigned int acceptCount = 0;
		bool error = false;
		int errcode;

		assert(serverState == ACTIVE);

		for (unsigned int i = 0; i < acceptBurstCount; i++) {
			int fd = acceptNonBlockingSocket(io->fd);
			if (fd == -1) {
				error = true;
				errcode = errno;
				break;
			}

			FdGuard guard(fd);
			Client *client = checkoutClientObject();
			TAILQ_INSERT_HEAD(&activeClients, client, nextClient.activeOrDisconnectedClient);
			activeClientCount++;
			acceptCount++;
			client->setConnState(Client::ACTIVE);
			client->fdnum = fd;
			client->input.reinitialize(fd);
			client->output.reinitialize(fd);
			client->reinitialize(fd);
			guard.clear();
		}

		if (acceptCount > 0) {
			P_DEBUG("[Server] " << acceptCount << " new clients accepted (" <<
				(activeClientCount - acceptCount) << " -> " << acceptCount << " clients)");
		}
		if (error && errcode != EAGAIN && errcode != EWOULDBLOCK) {
			P_ERROR("[Server] Cannot accept client: " << strerror(errcode) <<
				" (errno=" << errcode << "). " <<
				"Stop accepting clients for 3 seconds. " <<
				"Current client count: " << activeClientCount);
			serverState = TOO_MANY_FDS;
			acceptResumptionWatcher.start();
			for (uint8_t i = 0; i < nEndpoints; i++) {
				ev_io_stop(ctx->libev->getLoop(), &endpoints[i]);
			}
		}

		onClientsAccepted(lastActiveClient);
	}

	void onAcceptResumeTimeout(ev::timer &timer, int revents) {
		assert(serverState == TOO_MANY_FDS);
		P_WARN("[Server] Resuming accepting new clients");
		serverState = ACTIVE;
		for (uint8_t i = 0; i < nEndpoints; i++) {
			ev_io_start(ctx->libev->getLoop(), &endpoints[i]);
		}
		acceptResumptionWatcher.stop();
	}

	int acceptNonBlockingSocket(int serverFd) {
		union {
			struct sockaddr_in inaddr;
			struct sockaddr_un unaddr;
		} u;
		socklen_t addrlen = sizeof(u);

		if (accept4Available) {
			int fd = callAccept4(serverFd,
				(struct sockaddr *) &u,
				&addrlen,
				O_NONBLOCK);
			// FreeBSD returns EINVAL if accept4() is called with invalid flags.
			if (fd == -1 && (errno == ENOSYS || errno == EINVAL)) {
				accept4Available = false;
				return acceptNonBlockingSocket(serverFd);
			} else {
				return fd;
			}
		} else {
			int fd = syscalls::accept(serverFd,
				(struct sockaddr *) &u,
				&addrlen);
			FdGuard guard(fd);
			if (fd == -1) {
				return -1;
			} else {
				try {
					setNonBlocking(fd);
				} catch (const SystemException &e) {
					errno = e.code();
					return -1;
				}
				guard.clear();
				return fd;
			}
		}
	}

	Client *checkoutClientObject() {
		// Try to obtain client object from freelist.
		if (!STAILQ_EMPTY(&freeClients)) {
			return checkoutClientObjectFromFreelist();
		} else {
			return createNewClientObject();
		}
	}

	Client *checkoutClientObjectFromFreelist() {
		assert(freeClientCount > 0);
		Client *client = STAILQ_FIRST(&freeClients);
		assert(client->getConnState() == Client::IN_FREELIST);
		freeClientCount--;
		STAILQ_REMOVE_HEAD(&freeClients, nextClient.freeClient);
		return client;
	}

	Client *createNewClientObject() {
		Client *client;
		try {
			client = new Client(this);
		} catch (const std::bad_alloc &) {
			return NULL;
		}
		onClientObjectCreated(client);
		return client;
	}

	void clientReachedZeroRefcount(Client *client) {
		assert(disconnectedClientCount > 0);
		assert(!TAILQ_EMPTY(&disconnectedClients));

		TAILQ_REMOVE(&disconnectedClients, client, nextClient.activeOrDisconnectedClient);
		disconnectedClientCount--;

		if (!addClientToFreelist(client)) {
			delete client;
		}

		if (serverState == STOPPING
		 && activeClientCount == 0
		 && disconnectedClientCount == 0)
		{
			stopCompleted();
		}
	}

	bool addClientToFreelist(Client *client) {
		if (freeClientCount < freelistLimit) {
			STAILQ_INSERT_HEAD(&freeClients, client, nextClient.freeClient);
			freeClientCount++;
			int prevref = client->refcount.fetch_add(1, boost::memory_order_relaxed);
			assert(prevref == 0);
			(void) prevref;
			client->setConnState(Client::IN_FREELIST);
			return true;
		} else {
			return false;
		}
	}

	void passClientToEventLoopThread(Client *client) {
		// The shutdown procedure waits until all ACTIVE and DISCONNECTED
		// clients are gone before destroying a Server, so we know for sure
		// that this async callback outlives the Server.
		ctx->libev->runLater(boost::bind(&Server::passClientToEventLoopThreadCallback,
			this, ClientRef(client)));
	}

	void passClientToEventLoopThreadCallback(ClientRef clientRef) {
		// Do nothing. Once this method returns, the reference count of the
		// client drops to 0, and clientReachedZeroRefcount() is called.
	}

	void stopCompleted() {
		serverState = STOPPED;
	}

	static int _onClientDataReceived(FdChannel *channel, const MemoryKit::mbuf &buffer,
		int errcode)
	{
		Client *client = static_cast<Client *>(channel->getHooks()->userData);
		Server *server = static_cast<Server *>(client->getServer());
		return server->onClientDataReceived(client, buffer, errcode);
	}

	static void _onClientOutputError(FileBufferedFdOutputChannel *channel, int errcode) {
		Client *client = static_cast<Client *>(channel->getHooks()->userData);
		Server *server = static_cast<Server *>(client->getServer());
		server->onClientOutputError(client, errcode);
	}

protected:
	/***** Protected API *****/

	/** Get a thread-safe reference to the client. As long as the client
	 * has a reference, it will never be added to the freelist.
	 */
	ClientRef getClientRef(Client *client) {
		return ClientRef(client);
	}

	/** Increase client reference count. */
	void refClient(Client *client) {
		client->refcount.fetch_add(1, boost::memory_order_relaxed);
	}

	/** Decrease client reference count. Adds client to the
	 * freelist if reference count drops to 0.
	 */
	void unrefClient(Client *client) {
		int oldRefcount = client->refcount.fetch_sub(1, boost::memory_order_release);
		assert(oldRefcount >= 1);

		if (oldRefcount == 1) {
			boost::atomic_thread_fence(boost::memory_order_acquire);

			if (pthread_equal(pthread_self(), ctx->libev->getCurrentThread())) {
				assert(client->getConnState() != Client::IN_FREELIST);
				/* As long as the client is still in the ACTIVE state, it has at least
				 * one reference, namely from the Server itself. Therefore it's impossible
				 * to get to a zero reference count without having disconnected a client. */
				assert(client->getConnState() == Client::DISCONNECTED);
				clientReachedZeroRefcount(client);
			} else {
				// Let the event loop handle the client reaching the 0 refcount.
				passClientToEventLoopThread(client);
			}
		}
	}


	/***** Hooks *****/

	virtual void onClientObjectCreated(Client *client) {
		client->hooks.impl        = this;
		client->hooks.userData    = client;

		client->input.setContext(ctx);
		client->input.setHooks(&client->hooks);
		client->input.callback    = _onClientDataReceived;

		client->output.setContext(ctx);
		client->output.setHooks(&client->hooks);
		client->output.errorCallback = _onClientOutputError;
	}

	virtual void onClientsAccepted(Client *lastActiveClient) {
		ClientRef client(TAILQ_FIRST(&activeClients));

		while (client.get() != NULL && client.get() != lastActiveClient) {
			ClientRef nextClient(TAILQ_NEXT(client.get(), nextClient.activeOrDisconnectedClient));
			onClientAccepted(client.get());
			if (client.get()->connected() && startReadingAfterAccept) {
				client.get()->input.startReading();
			}
			client = boost::move(nextClient);
		}
	}

	virtual void onClientAccepted(Client *client) {
		// Do nothing.
	}

	virtual void onClientDisconnected(Client *client) {
		// Do nothing.
	}

	virtual bool shouldDisconnectClientOnStop(Client *client) {
		return true;
	}

	virtual int onClientDataReceived(Client *client, const MemoryKit::mbuf &buffer,
		int errcode)
	{
		return -1;
	}

	virtual void onClientOutputError(Client *client, int errcode) {
		disconnect(&client);
	}

public:
	/***** Public methods *****/

	Server(Context *context)
		: acceptBurstCount(32),
		  startReadingAfterAccept(true),
		  minSpareClients(128),
		  freelistLimit(1024),
		  serverState(ACTIVE),
		  freeClientCount(0),
		  activeClientCount(0),
		  disconnectedClientCount(0),
		  ctx(context),
		  nEndpoints(0),
		  accept4Available(true)
	{
		STAILQ_INIT(&freeClients);
		TAILQ_INIT(&activeClients);
		TAILQ_INIT(&disconnectedClients);
		acceptResumptionWatcher.set(context->libev->getLoop());
		acceptResumptionWatcher.set(0, 3);
		acceptResumptionWatcher.set<
			Server<DerivedServer, Client>,
			&Server<DerivedServer, Client>::onAcceptResumeTimeout>(this);
	}

	~Server() {
		assert(serverState == STOPPED);
	}

	// Pre-create multiple client objects so that they get allocated
	// near each other in memory. Hopefully increases CPU cache locality.
	void createSpareClients() {
		for (unsigned int i = 0; i < minSpareClients; i++) {
			Client *client = createNewClientObject();
			client->setConnState(Client::IN_FREELIST);
			STAILQ_INSERT_HEAD(&freeClients, client, nextClient.freeClient);
			freeClientCount++;
		}
	}

	void listen(int fd) {
		assert(nEndpoints < MAX_ENDPOINTS);
		setNonBlocking(fd);
		ev_io_init(&endpoints[nEndpoints], _onAcceptable, fd, EV_READ);
		endpoints[nEndpoints].data = this;
		ev_io_start(ctx->libev->getLoop(), &endpoints[nEndpoints]);
		nEndpoints++;
	}

	void stop() {
		if (serverState != ACTIVE) {
			return;
		}

		vector<Client *> clients;
		Client *client;
		typename vector<Client *>::iterator v_it, v_end;

		serverState = STOPPING;

		// Stop listening on all endpoints.
		acceptResumptionWatcher.stop();
		for (uint8_t i = 0; i < nEndpoints; i++) {
			ev_io_stop(ctx->libev->getLoop(), &endpoints[i]);
		}

		if (activeClientCount == 0 && disconnectedClientCount == 0) {
			stopCompleted();
			return;
		}

		// Once we've set serverState to STOPPING, `activeClientCount` will no
		// longer grow, but may change due to hooks and callbacks.
		// So we make a copy of the client list here and operate on that.
		clients.reserve(activeClientCount);
		TAILQ_FOREACH (client, &activeClients, nextClient.activeOrDisconnectedClient) {
			assert(client->getConnState() == Client::ACTIVE);
			refClient(client);
			clients.push_back(client);
		}

		// Disconnect each active client.
		v_end = clients.end();
		for (v_it = clients.begin(); v_it != v_end; v_it++) {
			client = (Client *) *v_it;
			if (shouldDisconnectClientOnStop(client)) {
				Client *c = client;
				disconnect(&client);
				unrefClient(c);
			}
		}

		// When all active and disconnected clients are gone,
		// stopCompleted() will be called to set state to STOPPED.
	}

	Client *lookupClient(int fd) {
		Client *client;

		TAILQ_FOREACH (client, &activeClients, nextClient.activeOrDisconnectedClient) {
			assert(client->getConnState() == Client::ACTIVE);
			if (client->fd == fd) {
				return client;
			}
		}
		return NULL;
	}

	bool disconnect(int fd) {
		assert(serverState != STOPPED);
		Client *client = lookupClient(fd);
		if (client != NULL) {
			return disconnect(&client);
		} else {
			return false;
		}
	}

	bool disconnect(Client **client) {
		Client *c = *client;
		if (c->getConnState() != Client::ACTIVE) {
			return false;
		}

		c->setConnState(ServerKit::Client::DISCONNECTED);
		TAILQ_REMOVE(&activeClients, c, nextClient.activeOrDisconnectedClient);
		activeClientCount--;
		TAILQ_INSERT_HEAD(&disconnectedClients, c, nextClient.activeOrDisconnectedClient);
		disconnectedClientCount++;

		c->input.deinitialize();
		c->output.deinitialize();
		c->deinitialize();
		// TODO: handle exception
		safelyClose(c->fdnum);

		// TODO:
		//RH_DEBUG(client, "Disconnected; new client count = " << clients.size());

		*client = NULL;
		onClientDisconnected(c);
		unrefClient(c);
		return true;
	}

	Context *getContext() {
		return ctx;
	}

	void configure(void *json) { }
	void inspectStateAsJson() { }


	/***** Friend-public methods and hook implementations *****/

	void _refClient(Client *client) {
		refClient(client);
	}

	void _unrefClient(Client *client) {
		unrefClient(client);
	}

	virtual bool hook_isConnected(Hooks *hooks, void *source) {
		Client *client = static_cast<Client *>(hooks->userData);
		return client->connected();
	}

	virtual void hook_ref(Hooks *hooks, void *source) {
		Client *client = static_cast<Client *>(hooks->userData);
		refClient(client);
	}

	virtual void hook_unref(Hooks *hooks, void *source) {
		Client *client = static_cast<Client *>(hooks->userData);
		unrefClient(client);
	}
};


} // namespace ServerKit
} // namespace Passenger

#endif /* _PASSENGER_SERVER_KIT_SERVER_H_ */
