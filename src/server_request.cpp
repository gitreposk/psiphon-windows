/*
 * Copyright (c) 2012, Psiphon Inc.
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
The purpose of ServerRequest is to wrap the various ways to make a HTTPS request
to the server, which will depend on what state we're in, what transports are 
available, etc.

Two design statements:

1. If a transport can connect without first making an extra-transport request,
   then it should.
    - In order to connect with VPN, an initial handshake is required in order to 
      get server credentials. So that doesn't qualify.
    - SSH and OSSH, on the other hand, do not, in theory, require an initial 
      handshake. So we will embed those credentials and then connect without an
      initial handshake.

2. Any extra-transport requests should try HTTPS (8080, then 443) and then fail
   over to setting up and making request through any available transports that
   don't require an extra-transport request to connect.
    - Failure requests, post-disconnect stats request, and VPN handshake requests
      all must be done extra-transport. Until now, those depended on HTTPS being
      available. This change will make it so that those requests succeed if HTTPS
      *or* SSH *or* OSSH are available.

Design assumptions:
- All transports will run a local proxy.
  - This is true at this time, but it's imaginable that it could change in the
    future. For now, though, when a transport is up we will alway route requests
    through the local proxy.

There are two basic states we can be in: 1) a transport is connected; 
and 2) no transport is connected. 

If a transport is connected, the request method is simple:
- Connect via the local proxy, using HTTPS on port 8080.

If a transport is not connected, the request method fails over among multiple
methods:

1. Direct to server
Connect directly with HTTPS. Fail over among specific ports (right now those
are 8080 and 443).

2. Via transport
Some transports (e.g., SSH) have all necessary connection information 
contained in their local ServerEntry; no separate handshake 
(i.e., extra-transport connection) is required to connect with these 
transports. If direct connection attempts fail, we will fail over to 
attempting to connect each of these types of transports and proxying our 
request through them.
*/

#include "stdafx.h"
#include "server_request.h"
#include "transport.h"
#include "transport_registry.h"
#include "httpsrequest.h"
#include "transport_connection.h"
#include "psiclient.h"


ServerRequest::ServerRequest()
{
}

ServerRequest::~ServerRequest()
{
}

bool ServerRequest::MakeRequest(
        bool adhocIfNeeded,
        const ITransport* currentTransport,
        const SessionInfo& sessionInfo,
        const TCHAR* requestPath,
        string& response,
        const StopInfo& stopInfo,
        LPCWSTR additionalHeaders/*=NULL*/,
        LPVOID additionalData/*=NULL*/,
        DWORD additionalDataLength/*=0*/)
{
    // See comments at the top of this file for full discussion of logic.

    // Throws if signaled
    stopInfo.stopSignal->CheckSignal(stopInfo.stopReasons, true);

    assert(requestPath);

    response.clear();

    bool transportConnected = currentTransport && currentTransport->IsConnected();

    if (transportConnected)
    {
        // This is the simple case: we just connect through the transport
        HTTPSRequest httpsRequest;
        bool requestSuccess = 
            httpsRequest.MakeRequest(
                NarrowToTString(sessionInfo.GetServerAddress()).c_str(),
                sessionInfo.GetWebPort(),
                sessionInfo.GetWebServerCertificate(),
                requestPath,
                response,
                stopInfo,
                currentTransport->IsServerRequestTunnelled(), // use local proxy?
                additionalHeaders,
                additionalData,
                additionalDataLength);
        return requestSuccess;
    }
    else if (!adhocIfNeeded)
    {
        // If ad hoc/temporary connections aren't allowed, and the transport
        // isn't currently connected, bail.
        return false;
    }

    // We don't have a connected transport. 
    // We'll fail over between a bunch of methods.

    // The ports we'll try to connect to directly, in order.
    vector<int> ports;
    ports.push_back(sessionInfo.GetWebPort());
    ports.push_back(443); // Also try the standard HTTPS port.
    vector<int>::const_iterator port_iter;
    for (port_iter = ports.begin(); port_iter != ports.end(); port_iter++)
    {
        HTTPSRequest httpsRequest;
        if (httpsRequest.MakeRequest(
                NarrowToTString(sessionInfo.GetServerAddress()).c_str(),
                *port_iter,
                sessionInfo.GetWebServerCertificate(),
                requestPath,
                response,
                stopInfo,
                false, // don't use local proxy -- there's no transport, and there may be bad/remnant system proxy settings
                additionalHeaders,
                additionalData,
                additionalDataLength))
        {
            return true;
        }

        my_print(true, _T("%s: HTTPS:%d failed"), __TFUNCTION__, *port_iter);
    }

    // Connecting directly via HTTPS failed. 
    // Now we'll try don't-need-handshake transports.

    vector<auto_ptr<ITransport>> tempTransports;
    GetTempTransports(sessionInfo, tempTransports);

    bool success = false;

    vector<auto_ptr<ITransport>>::iterator transport_iter;
    for (transport_iter = tempTransports.begin(); 
         transport_iter != tempTransports.end(); 
         transport_iter++)
    {
        TransportConnection connection;

        try
        {
            // Note that it's important that we indicate that we're not 
            // collecting stats -- otherwise we could end up with a loop of
            // final /status request attempts.

            // Throws on failure
            connection.Connect(
                stopInfo,
                (*transport_iter).get(),
                NULL, // not collecting stats
                sessionInfo, 
                NULL, // no handshake allowed
                tstring()); // splitTunnelingFilePath -- not providing it

            HTTPSRequest httpsRequest;
            if (httpsRequest.MakeRequest(
                    NarrowToTString(sessionInfo.GetServerAddress()).c_str(),
                    sessionInfo.GetWebPort(),
                    sessionInfo.GetWebServerCertificate(),
                    requestPath,
                    response,
                    stopInfo,
                    (*transport_iter).get()->IsServerRequestTunnelled(), // use local proxy?
                    additionalHeaders,
                    additionalData,
                    additionalDataLength))
            {
                success = true;
                break;
            }

            my_print(true, _T("%s: transport:%s failed"), __TFUNCTION__, (*transport_iter)->GetTransportProtocolName().c_str());

            // Note that when we leave this scope, the TransportConnection will
            // clean up the transport connection.
        }
        catch (TransportConnection::TryNextServer&)
        {
            // pass and continue
        }
    }
    
    // We've tried everything we can.

    return success;
}

/*
Returns a vector of eligible temporary transports -- that is, ones that can
connect with the available SessionInfo (with no preliminary handshake).
o_tempTransports will be empty if there are no eligible transports.
All elements of o_tempTransports are heap-allocated and must be delete'd by 
the caller.
NOTE: If you look at TransportConnection::Connect() you'll see that this logic
isn't strictly necessary. If a null handshake is passed, TryNextServer is 
thrown, so we could just iterate over all transports sanely. But this makes
our logic more explicit. And not dependent on the internals of another function.
*/
void ServerRequest::GetTempTransports(
                            const SessionInfo& sessionInfo,
                            vector<auto_ptr<ITransport>>& o_tempTransports)
{
    o_tempTransports.clear();

    vector<ITransport*> all_transports;
    TransportRegistry::NewAll(all_transports);

    ITransport* tempTransport = 0;
    vector<ITransport*>::iterator it;
    for (it = all_transports.begin(); it != all_transports.end(); it++)
    {
        // Only try transports that aren't the same as the current 
        // transport (because there's a reason it's not connected) 
        // and doesn't require a handshake.
        if (!(*it)->IsHandshakeRequired(sessionInfo))
        {
            o_tempTransports.push_back(auto_ptr<ITransport>(*it));
            // no early break, so that we delete all the unused transports
        }
        else
        {
            delete *it;
        }
    }
}