//
//  rtmp_relay
//

#pragma once

#include <string>
#include <set>
#include <vector>
#include <map>
#include <memory>
#include "Network.h"
#include "Connector.h"
#include "RTMP.h"
#include "PushSender.h"
#include "PullServer.h"
#include "Status.h"

namespace relay
{
    struct ApplicationDescriptor
    {
        std::string name;
        std::vector<PushDescriptor> pushDescriptors;
        std::vector<PullDescriptor> pullDescriptors;
    };

    class Application
    {
    public:
        Application(cppsocket::Network& aNetwork,
                    const ApplicationDescriptor& applicationDescriptor,
                    const std::string& aName);

        const std::string& getName() const { return name; }

        void update(float delta);

        void createStream(const std::string& newStreamName);
        void deleteStream();
        void unpublishStream();

        void sendAudioHeader(const std::vector<uint8_t>& headerData);
        void sendVideoHeader(const std::vector<uint8_t>& headerData);
        void sendAudio(uint64_t timestamp, const std::vector<uint8_t>& audioData);
        void sendVideo(uint64_t timestamp, const std::vector<uint8_t>& videoData);
        void sendMetaData(const amf0::Node& metaData);
        void sendTextData(uint64_t timestamp, const amf0::Node& textData);

        void getInfo(std::string& str, ReportType reportType) const;

    private:
        std::string name;

        std::vector<std::unique_ptr<PushSender>> pushSenders;
        std::vector<std::unique_ptr<PullServer>> pullServers;
    };
}
