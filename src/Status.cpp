//
//  rtmp_relay
//

#include "Status.hpp"
#include "Relay.hpp"
#include "StatusSender.hpp"

namespace relay
{
    Status::Status(Network& aNetwork, Relay& aRelay, const std::string& address):
        network(aNetwork), socket(aNetwork), relay(aRelay)
    {
        //socket.setConnectTimeout(connectionTimeout);
        socket.setAcceptCallback(std::bind(&Status::handleAccept, this, std::placeholders::_1, std::placeholders::_2));

        socket.startAccept(address);
    }

    void Status::update(float)
    {
        for (auto i = statusSenders.begin(); i != statusSenders.end();)
        {
            if ((*i)->isConnected())
            {
                ++i;
            }
            else
            {
                i = statusSenders.erase(i);
            }
        }
    }

    void Status::handleAccept(Socket&, Socket& clientSocket)
    {
        std::unique_ptr<StatusSender> statusSender(new StatusSender(network, clientSocket, relay));
        
        statusSenders.push_back(std::move(statusSender));
    }
}
