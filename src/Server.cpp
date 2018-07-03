//
//  rtmp_relay
//

#include "Server.hpp"
#include "Relay.hpp"

namespace relay
{
    Server::Server(Relay& aRelay,
                   Network& aNetwork):
        relay(aRelay),
        id(Relay::nextId()),
        network(aNetwork)
    {
    }

    void Server::stop()
    {
        for (auto& s : streams)
        {
            s->close();
        }

        for (auto& c : connections)
        {
            c->close(true);
        }
    }

    Stream* Server::findStream(const std::string& applicationName,
                               const std::string& streamName) const
    {
        for (auto i = streams.begin(); i != streams.end(); ++i)
        {
            if ((*i)->getApplicationName() == applicationName &&
                (*i)->getStreamName() == streamName &&
                !(*i)->isClosed())
            {
                return i->get();
            }
        }

        return nullptr;
    }

    Connection* Server::createConnection(Stream& stream,
                                         const Endpoint& endpoint)
    {
        std::unique_ptr<Connection> connection(new Connection(relay, stream, endpoint));
        Connection* connectionPtr = connection.get();
        connections.push_back(std::move(connection));

        return connectionPtr;
    }

    void Server::deleteConnection(Connection* connection)
    {
        for (auto i = connections.begin(); i != connections.end();)
        {
            if (i->get() == connection)
            {
                i = connections.erase(i);
            }
            else
            {
                ++i;
            }
        }
    }

    Stream* Server::createStream(const std::string& applicationName,
                                 const std::string& streamName)
    {
        std::unique_ptr<Stream> stream(new Stream(*this, applicationName, streamName));
        Stream* streamPtr = stream.get();
        streams.push_back(std::move(stream));

        return streamPtr;
    }

    void Server::deleteStream(Stream* stream)
    {
        for (auto i = streams.begin(); i != streams.end();)
        {
            if (i->get() == stream)
            {
                i = streams.erase(i);
            }
            else
            {
                ++i;
            }
        }
    }

    void Server::start(const std::vector<Endpoint>& aEndpoints)
    {
        endpoints = aEndpoints;

        for (const Endpoint& endpoint : endpoints)
        {
            if (endpoint.connectionType == Connection::Type::CLIENT &&
                endpoint.direction == Connection::Direction::INPUT &&
                endpoint.isNameKnown())
            {
                Socket socket(network);

                Stream* stream = createStream(endpoint.applicationName,
                                              endpoint.streamName);

                std::unique_ptr<Connection> connection(new Connection(relay,
                                                                      *stream,
                                                                      endpoint));

                connection->setStream(stream);

                connection->connect();

                connections.push_back(std::move(connection));
            }
        }
    }

    void Server::update(float delta)
    {
        for (auto i = connections.begin(); i != connections.end();)
        {
            i = ((*i)->isClosed() ? connections.erase(i) : i + 1);
        }

        for (auto si = streams.begin(); si != streams.end();)
        {
            si = ((*si)->isClosed() ? streams.erase(si) : si + 1);
        }

        // update connections
        for (auto i = connections.begin(); i != connections.end();)
        {
            const std::unique_ptr<Connection>& connection = *i;

            if (connection->isClosed())
            {
                i = connections.erase(i);
                continue;
            }
            else
            {
                ++i;
            }

            connection->update(delta);
        }
    }

    void Server::getConnections(std::map<Connection*, Stream*>& cons)
    {
        for (auto& c : connections)
        {
            cons[c.get()] = c->getStream();
        }

        for (auto& s : streams)
        {
            s->getConnections(cons);
        }
    }

    void Server::getStats(std::string& str, ReportType reportType) const
    {
        switch (reportType)
        {
            case ReportType::TEXT:
            {
                for (const auto& c : connections)
                {
                    c->getStats(str, reportType);
                }

                for (const auto& s : streams)
                {
                    s->getStats(str, reportType);
                }
                break;
            }
            case ReportType::HTML:
            {
                for (const auto& c : connections)
                {
                    c->getStats(str, reportType);
                }
                break;
            }
            case ReportType::JSON:
            {
                bool first = true;
                for (const auto& c : connections)
                {
                    if (!first) str += ",";
                    first = false;
                    c->getStats(str, reportType);
                }
                break;
            }
        }
    }
}
