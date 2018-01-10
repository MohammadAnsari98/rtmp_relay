//
//  rtmp_relay
//

#pragma once

#include <string>
#include <vector>
#include "Amf.hpp"
#include "Socket.hpp"
#include "Status.hpp"
#include "Utils.hpp"

namespace relay
{
    class Relay;
    class Server;
    class Connection;

    class Stream
    {
    public:

        Stream(Server& aServer,
               const std::string& aApplicationName,
               const std::string& aStreamName);

        Stream(const Stream&) = delete;
        Stream(Stream&&) = delete;
        Stream& operator=(const Stream&) = delete;
        Stream& operator=(Stream&&) = delete;

        virtual ~Stream();

        Server& getServer() { return server; }
        const std::string& getApplicationName() const { return applicationName; }
        const std::string& getStreamName() const { return streamName; }

        void getStats(std::string& str, ReportType reportType) const;

        void start(Connection& connection);
        void stop(Connection& connection);

        Connection* getInputConnection() const { return inputConnection; }

        void sendAudioHeader(const std::vector<uint8_t>& headerData);
        void sendVideoHeader(const std::vector<uint8_t>& headerData);
        void sendAudioFrame(uint64_t timestamp, const std::vector<uint8_t>& audioData);
        void sendVideoFrame(uint64_t timestamp, const std::vector<uint8_t>& videoData, VideoFrameType frameType);
        void sendMetaData(const amf::Node& newMetaData);
        void sendTextData(uint64_t timestamp, const amf::Node& textData);

        bool hasDependableConnections();
        void close();
        bool isClosed() { return closed; }
        uint64_t getId() { return id; }
        void getConnections(std::map<Connection*, Stream*>& cons);

    private:
        const uint64_t id;
        bool closed = false;
        std::string idString;

        Server& server;

        std::string applicationName;
        std::string streamName;

        Connection* inputConnection = nullptr;
        bool inputConnectionCreated = false;
        std::vector<Connection*> outputConnections;

        bool streaming = false;
        bool dependableOutputsCreated = false;
        std::vector<uint8_t> audioHeader;
        std::vector<uint8_t> videoHeader;
        amf::Node metaData;

        std::vector<Connection*> connections;
    };
}
