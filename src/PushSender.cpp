//
//  rtmp_relay
//

#include <iostream>
#include "PushSender.h"
#include "Constants.h"
#include "RTMP.h"
#include "Amf0.h"
#include "Log.h"
#include "Network.h"
#include "Utils.h"

using namespace cppsocket;

namespace relay
{
    PushSender::PushSender(Network& aNetwork,
                           const std::string& aApplication,
                           const std::string& aOverrideStreamName,
                           const std::vector<std::string>& aAddresses,
                           bool videoOutput,
                           bool audioOutput,
                           bool dataOutput,
                           const std::set<std::string>& aMetaDataBlacklist,
                           float aConnectionTimeout,
                           float aReconnectInterval,
                           uint32_t aReconnectCount):
        generator(rd()),
        network(aNetwork),
        socket(network),
        application(aApplication),
        overrideStreamName(aOverrideStreamName),
        addresses(aAddresses),
        videoStream(videoOutput),
        audioStream(audioOutput),
        dataStream(dataOutput),
        metaDataBlacklist(aMetaDataBlacklist),
        connectionTimeout(aConnectionTimeout),
        reconnectInterval(aReconnectInterval),
        reconnectCount(aReconnectCount)
    {
        if (!socket.setBlocking(false))
        {
            Log(Log::Level::ERR) << "[" << name << "] " << "Failed to set socket non-blocking";
        }

        socket.setConnectTimeout(connectionTimeout);
        socket.setConnectCallback(std::bind(&PushSender::handleConnect, this));
        socket.setConnectErrorCallback(std::bind(&PushSender::handleConnectError, this));
        socket.setReadCallback(std::bind(&PushSender::handleRead, this, std::placeholders::_1, std::placeholders::_2));
        socket.setCloseCallback(std::bind(&PushSender::handleClose, this, std::placeholders::_1));

        if (!videoOutput)
        {
            metaDataBlacklist.insert("width");
            metaDataBlacklist.insert("height");
            metaDataBlacklist.insert("videodatarate");
            metaDataBlacklist.insert("framerate");
            metaDataBlacklist.insert("videocodecid");
            metaDataBlacklist.insert("fps");
            metaDataBlacklist.insert("gopsize");
        }

        if (!audioOutput)
        {
            metaDataBlacklist.insert("audiodatarate");
            metaDataBlacklist.insert("audiosamplerate");
            metaDataBlacklist.insert("audiosamplesize");
            metaDataBlacklist.insert("stereo");
            metaDataBlacklist.insert("audiocodecid");
        }
    }

    void PushSender::reset()
    {
        socket.close();
        data.clear();

        state = rtmp::State::UNINITIALIZED;
        inChunkSize = 128;
        outChunkSize = 128;

        invokeId = 0;
        invokes.clear();

        streamId = 0;

        receivedPackets.clear();
        sentPackets.clear();

        active = false;
        connected = false;

        streaming = false;
        timeSinceConnect = 0.0f;
        timeSinceHandshake = 0.0f;

        audioHeaderSent = false;
        videoHeaderSent = false;
        videoFrameSent = false;

        metaDataSent = false;
    }

    bool PushSender::connect()
    {
        reset();
        active = true;

        if (addresses.empty())
        {
            Log(Log::Level::ERR) << "[" << name << "] " << "No addresses to connect to";
            return false;
        }

        if (connectCount >= reconnectCount)
        {
            connectCount = 0;
            ++addressIndex;
        }

        if (addressIndex >= addresses.size())
        {
            addressIndex = 0;
        }

        std::string address = addresses[addressIndex];

        Log(Log::Level::INFO) << "[" << name << "] " << "Connecting to " << address;

        if (!socket.connect(address, 1935))
        {
            return false;
        }

        ++connectCount;

        return true;
    }

    void PushSender::disconnect()
    {
        Log(Log::Level::ERR) << "[" << name << "] " << "Disconnecting from " << ipToString(socket.getIPAddress()) << ":" << socket.getPort();
        reset();
        active = false;
        addressIndex = 0;
        connectCount = 0;
    }

    void PushSender::update(float delta)
    {
        if (active)
        {
            timeSinceConnect += delta;
            timeSinceHandshake += delta;

            if ((!socket.isReady() && timeSinceConnect >= reconnectInterval) ||
                (socket.isReady() && state != rtmp::State::HANDSHAKE_DONE && timeSinceHandshake >= reconnectInterval))
            {
                connect();
            }
        }
    }

    void PushSender::handleConnect()
    {
        Log(Log::Level::INFO) << "[" << name << "] " << "Connected to " << ipToString(socket.getIPAddress()) << ":" << socket.getPort();

        std::vector<uint8_t> version;
        version.push_back(RTMP_VERSION);
        socket.send(version);

        rtmp::Challenge challenge;
        challenge.time = 0;
        std::copy(RTMP_SERVER_VERSION, RTMP_SERVER_VERSION + sizeof(RTMP_SERVER_VERSION), challenge.version);

        for (size_t i = 0; i < sizeof(challenge.randomBytes); ++i)
        {
            uint32_t randomValue = std::uniform_int_distribution<uint32_t>{ 0, 255 }(generator);

            challenge.randomBytes[i] = static_cast<uint8_t>(randomValue);
        }

        std::vector<uint8_t> challengeMessage;
        challengeMessage.insert(challengeMessage.begin(),
                                reinterpret_cast<uint8_t*>(&challenge),
                                reinterpret_cast<uint8_t*>(&challenge) + sizeof(challenge));
        socket.send(challengeMessage);

        state = rtmp::State::VERSION_SENT;
    }

    void PushSender::handleConnectError()
    {
        Log(Log::Level::INFO) << "[" << name << "] " << "Failed to connect to " << ipToString(socket.getIPAddress()) << ":" << socket.getPort();
    }

    bool PushSender::sendPacket(const std::vector<uint8_t>& packet)
    {
        socket.send(packet);

        return true;
    }

    void PushSender::handleRead(cppsocket::Socket&, const std::vector<uint8_t>& newData)
    {
        data.insert(data.end(), newData.begin(), newData.end());

        Log(Log::Level::ALL) << "[" << name << "] " << "Got " << std::to_string(newData.size()) << " bytes";

        uint32_t offset = 0;

        while (offset < data.size())
        {
            if (state == rtmp::State::VERSION_SENT)
            {
                if (data.size() - offset >= sizeof(uint8_t))
                {
                    // S0
                    uint8_t version = static_cast<uint8_t>(*data.data() + offset);
                    offset += sizeof(version);

                    Log(Log::Level::ALL) << "[" << name << "] " << "Got version " << static_cast<uint32_t>(version);

                    if (version != 0x03)
                    {
                        Log(Log::Level::ERR) << "[" << name << "] " << "Unsuported version, disconnecting";
                        socket.close();
                        break;
                    }

                    state = rtmp::State::VERSION_RECEIVED;

                    timeSinceHandshake = 0.0f;
                }
                else
                {
                    break;
                }
            }
            else if (state == rtmp::State::VERSION_RECEIVED)
            {
                if (data.size() - offset >= sizeof(rtmp::Challenge))
                {
                    // S1
                    rtmp::Challenge* challenge = reinterpret_cast<rtmp::Challenge*>(data.data() + offset);
                    offset += sizeof(*challenge);

                    Log(Log::Level::ALL) << "[" << name << "] " << "Got challenge message, time: " << challenge->time <<
                        ", version: " << static_cast<uint32_t>(challenge->version[0]) << "." <<
                        static_cast<uint32_t>(challenge->version[1]) << "." <<
                        static_cast<uint32_t>(challenge->version[2]) << "." <<
                        static_cast<uint32_t>(challenge->version[3]);

                    // C2
                    rtmp::Ack ack;
                    ack.time = challenge->time;
                    std::copy(challenge->version, challenge->version + sizeof(ack.version), ack.version);
                    std::copy(challenge->randomBytes, challenge->randomBytes + sizeof(ack.randomBytes), ack.randomBytes);

                    std::vector<uint8_t> ackData(reinterpret_cast<uint8_t*>(&ack),
                                                 reinterpret_cast<uint8_t*>(&ack) + sizeof(ack));
                    socket.send(ackData);

                    state = rtmp::State::ACK_SENT;

                    timeSinceHandshake = 0.0f;
                }
                else
                {
                    break;
                }
            }
            else if (state == rtmp::State::ACK_SENT)
            {
                if (data.size() - offset >= sizeof(rtmp::Ack))
                {
                    // S2
                    rtmp::Ack* ack = reinterpret_cast<rtmp::Ack*>(data.data() + offset);
                    offset += sizeof(*ack);

                    Log(Log::Level::ALL) << "[" << name << "] " << "Got Ack message, time: " << ack->time <<
                        ", version: " << static_cast<uint32_t>(ack->version[0]) << "." <<
                        static_cast<uint32_t>(ack->version[1]) << "." <<
                        static_cast<uint32_t>(ack->version[2]) << "." <<
                        static_cast<uint32_t>(ack->version[3]);
                    Log(Log::Level::ALL) << "[" << name << "] " << "Handshake done";

                    state = rtmp::State::HANDSHAKE_DONE;

                    sendConnect();

                    timeSinceHandshake = 0.0f;
                }
                else
                {
                    break;
                }
            }
            else if (state == rtmp::State::HANDSHAKE_DONE)
            {
                rtmp::Packet packet;

                uint32_t ret = rtmp::decodePacket(data, offset, inChunkSize, packet, receivedPackets);

                if (ret > 0)
                {
                    Log(Log::Level::ALL) << "[" << name << "] " << "Total packet size: " << ret;

                    offset += ret;

                    handlePacket(packet);
                }
                else
                {
                    break;
                }
            }
        }

        if (offset > data.size())
        {
            if (socket.isReady())
            {
                Log(Log::Level::ERR) << "[" << name << "] " << "Reading outside of the buffer, buffer size: " << static_cast<uint32_t>(data.size()) << ", data size: " << offset;
            }

            data.clear();
        }
        else
        {
            data.erase(data.begin(), data.begin() + offset);

            Log(Log::Level::ALL) << "[" << name << "] " << "Remaining data " << data.size();
        }
    }

    void PushSender::handleClose(cppsocket::Socket&)
    {
        if (active)
        {
            reset();
        }
    }

    bool PushSender::handlePacket(const rtmp::Packet& packet)
    {
        switch (packet.messageType)
        {
            case rtmp::MessageType::SET_CHUNK_SIZE:
            {
                uint32_t offset = 0;

                uint32_t ret = decodeInt(packet.data, offset, 4, inChunkSize);

                if (ret == 0)
                {
                    return false;
                }

                Log(Log::Level::ALL) << "[" << name << "] " << "Chunk size: " << inChunkSize;

                sendSetChunkSize();

                break;
            }

            case rtmp::MessageType::BYTES_READ:
            {
                uint32_t offset = 0;
                uint32_t bytesRead;

                uint32_t ret = decodeInt(packet.data, offset, 4, bytesRead);

                if (ret == 0)
                {
                    return false;
                }

                Log(Log::Level::ALL) << "[" << name << "] " << "Bytes read: " << bytesRead;

                break;
            }

            case rtmp::MessageType::PING:
            {
                uint32_t offset = 0;

                uint16_t pingType;
                uint32_t ret = decodeInt(packet.data, offset, 2, pingType);

                if (ret == 0)
                {
                    return false;
                }

                offset += ret;

                uint32_t param;
                ret = decodeInt(packet.data, offset, 4, param);

                if (ret == 0)
                {
                    return false;
                }

                offset += ret;

                Log(Log::Level::ALL) << "[" << name << "] " << "Ping type: " << pingType << ", param: " << param;

                break;
            }

            case rtmp::MessageType::SERVER_BANDWIDTH:
            {
                uint32_t offset = 0;

                uint32_t bandwidth;
                uint32_t ret = decodeInt(packet.data, offset, 4, bandwidth);

                if (ret == 0)
                {
                    return false;
                }

                offset += ret;

                Log(Log::Level::ALL) << "[" << name << "] " << "Server bandwidth: " << bandwidth;

                break;
            }

            case rtmp::MessageType::CLIENT_BANDWIDTH:
            {
                uint32_t offset = 0;

                uint32_t bandwidth;
                uint32_t ret = decodeInt(packet.data, offset, 4, bandwidth);

                if (ret == 0)
                {
                    return false;
                }

                offset += ret;

                uint8_t type;
                ret = decodeInt(packet.data, offset, 1, type);

                if (ret == 0)
                {
                    return false;
                }

                offset += ret;

                Log(Log::Level::ALL) << "[" << name << "] " << "Client bandwidth: " << bandwidth << ", type: " << static_cast<uint32_t>(type);

                break;
            }

            case rtmp::MessageType::NOTIFY:
            {
                break;
            }

            case rtmp::MessageType::AUDIO_PACKET:
            {
                // ignore this, audio data should not be received
                break;
            }

            case rtmp::MessageType::VIDEO_PACKET:
            {
                // ignore this, video data should not be received
                break;
            }

            case rtmp::MessageType::INVOKE:
            {
                uint32_t offset = 0;

                amf0::Node command;

                uint32_t ret = command.decode(packet.data, offset);

                if (ret == 0)
                {
                    return false;
                }

                offset += ret;

                Log(Log::Level::ALL) << "[" << name << "] " << "INVOKE command: ";

                {
                    Log log(Log::Level::ALL);
                    log << "[" << name << "] ";
                    command.dump(log);
                }

                amf0::Node transactionId;

                ret = transactionId.decode(packet.data, offset);

                if (ret == 0)
                {
                    return false;
                }

                offset += ret;

                Log(Log::Level::ALL) << "[" << name << "] " << "Transaction ID: ";

                {
                    Log log(Log::Level::ALL);
                    log << "[" << name << "] ";
                    transactionId.dump(log);
                }

                amf0::Node argument1;

                if ((ret = argument1.decode(packet.data, offset)) > 0)
                {
                    offset += ret;

                    Log(Log::Level::ALL) << "[" << name << "] " << "Argument 1: ";

                    Log log(Log::Level::ALL);
                    log << "[" << name << "] ";
                    argument1.dump(log);
                }

                amf0::Node argument2;

                if ((ret = argument2.decode(packet.data, offset)) > 0)
                {
                    offset += ret;

                    Log(Log::Level::ALL) << "[" << name << "] " << "Argument 2: ";

                    Log log(Log::Level::ALL);
                    log << "[" << name << "] ";
                    argument2.dump(log);
                }

                if (command.asString() == "onBWDone")
                {
                    sendCheckBW();
                }
                else if (command.asString() == "onFCPublish")
                {
                }
                else if (command.asString() == "_error")
                {
                    auto i = invokes.find(static_cast<uint32_t>(transactionId.asDouble()));

                    if (i != invokes.end())
                    {
                        Log(Log::Level::ALL) << "[" << name << "] " << i->second << " error";

                        invokes.erase(i);
                    }
                }
                else if (command.asString() == "_result")
                {
                    auto i = invokes.find(static_cast<uint32_t>(transactionId.asDouble()));

                    if (i != invokes.end())
                    {
                        Log(Log::Level::ALL) << "[" << name << "] " << i->second << " result";

                        if (i->second == "connect")
                        {
                            connected = true;
                            if (!streamName.empty())
                            {
                                sendReleaseStream();
                                sendFCPublish();
                                sendCreateStream();
                            }
                        }
                        else if (i->second == "releaseStream")
                        {
                        }
                        else if (i->second == "createStream")
                        {
                            streamId = static_cast<uint32_t>(argument2.asDouble());
                            sendPublish();

                            streaming = true;

                            sendMetaData();
                            sendAudioHeader();
                            sendVideoHeader();
                        }

                        invokes.erase(i);
                    }
                }
                break;
            }

            default:
            {
                Log(Log::Level::ERR) << "[" << name << "] " << "Unhandled message: " << static_cast<uint32_t>(packet.messageType);
                break;
            }
        }

        return true;
    }

    void PushSender::sendConnect()
    {
        rtmp::Packet packet;
        packet.channel = rtmp::Channel::SYSTEM;
        packet.timestamp = 0;
        packet.messageType = rtmp::MessageType::INVOKE;

        amf0::Node commandName = std::string("connect");
        commandName.encode(packet.data);

        amf0::Node transactionIdNode = static_cast<double>(++invokeId);
        transactionIdNode.encode(packet.data);

        amf0::Node argument1;
        argument1["app"] = application;
        argument1["flashVer"] = std::string("FMLE/3.0 (compatible; Lavf57.5.0)");
        argument1["tcUrl"] = std::string("rtmp://127.0.0.1:") + std::to_string(socket.getPort()) + "/" + application;
        argument1["type"] = std::string("nonprivate");
        argument1.encode(packet.data);

        std::vector<uint8_t> buffer;
        encodePacket(buffer, outChunkSize, packet, sentPackets);

        Log(Log::Level::ALL) << "[" << name << "] " << "Sending INVOKE " << commandName.asString() << ", transaction ID: " << invokeId;

        socket.send(buffer);

        invokes[invokeId] = commandName.asString();
    }

    void PushSender::sendSetChunkSize()
    {
        rtmp::Packet packet;
        packet.channel = rtmp::Channel::SYSTEM;
        packet.timestamp = 0;
        packet.messageType = rtmp::MessageType::SET_CHUNK_SIZE;

        encodeInt(packet.data, 4, outChunkSize);

        std::vector<uint8_t> buffer;
        encodePacket(buffer, outChunkSize, packet, sentPackets);

        Log(Log::Level::ALL) << "[" << name << "] " << "Sending SET_CHUNK_SIZE";

        socket.send(buffer);
    }

    void PushSender::sendCheckBW()
    {
        rtmp::Packet packet;
        packet.channel = rtmp::Channel::SYSTEM;
        packet.timestamp = 0;
        packet.messageType = rtmp::MessageType::INVOKE;

        amf0::Node commandName = std::string("_checkbw");
        commandName.encode(packet.data);

        amf0::Node transactionIdNode = static_cast<double>(++invokeId);
        transactionIdNode.encode(packet.data);

        amf0::Node argument1(amf0::Marker::Null);
        argument1.encode(packet.data);

        std::vector<uint8_t> buffer;
        encodePacket(buffer, outChunkSize, packet, sentPackets);

        Log(Log::Level::ALL) << "[" << name << "] " << "Sending INVOKE " << commandName.asString() << ", transaction ID: " << invokeId;

        socket.send(buffer);

        invokes[invokeId] = commandName.asString();
    }

    void PushSender::sendCreateStream()
    {
        rtmp::Packet packet;
        packet.channel = rtmp::Channel::SYSTEM;
        packet.timestamp = 0;
        packet.messageType = rtmp::MessageType::INVOKE;

        amf0::Node commandName = std::string("createStream");
        commandName.encode(packet.data);

        amf0::Node transactionIdNode = static_cast<double>(++invokeId);
        transactionIdNode.encode(packet.data);

        amf0::Node argument1(amf0::Marker::Null);
        argument1.encode(packet.data);

        std::vector<uint8_t> buffer;
        encodePacket(buffer, outChunkSize, packet, sentPackets);

        Log(Log::Level::ALL) << "[" << name << "] " << "Sending INVOKE " << commandName.asString() << ", transaction ID: " << invokeId;

        socket.send(buffer);

        invokes[invokeId] = commandName.asString();
    }

    void PushSender::sendReleaseStream()
    {
        rtmp::Packet packet;
        packet.channel = rtmp::Channel::SYSTEM;
        packet.timestamp = 0;
        packet.messageType = rtmp::MessageType::INVOKE;

        amf0::Node commandName = std::string("releaseStream");
        commandName.encode(packet.data);

        amf0::Node transactionIdNode = static_cast<double>(++invokeId);
        transactionIdNode.encode(packet.data);

        amf0::Node argument1(amf0::Marker::Null);
        argument1.encode(packet.data);

        amf0::Node argument2 = streamName;
        argument2.encode(packet.data);

        std::vector<uint8_t> buffer;
        encodePacket(buffer, outChunkSize, packet, sentPackets);

        Log(Log::Level::ALL) << "[" << name << "] " << "Sending INVOKE " << commandName.asString() << ", transaction ID: " << invokeId;

        socket.send(buffer);

        invokes[invokeId] = commandName.asString();
    }

    void PushSender::sendDeleteStream()
    {
        rtmp::Packet packet;
        packet.channel = rtmp::Channel::SYSTEM;
        packet.timestamp = 0;
        packet.messageType = rtmp::MessageType::INVOKE;

        amf0::Node commandName = std::string("deleteStream");
        commandName.encode(packet.data);

        amf0::Node transactionIdNode = static_cast<double>(++invokeId);
        transactionIdNode.encode(packet.data);

        amf0::Node argument1(amf0::Marker::Null);
        argument1.encode(packet.data);

        amf0::Node argument2 = 1.0;
        argument2.encode(packet.data);

        std::vector<uint8_t> buffer;
        encodePacket(buffer, outChunkSize, packet, sentPackets);

        Log(Log::Level::ALL) << "[" << name << "] " << "Sending INVOKE " << commandName.asString() << ", transaction ID: " << invokeId;

        socket.send(buffer);

        invokes[invokeId] = commandName.asString();
    }

    void PushSender::sendFCPublish()
    {
        rtmp::Packet packet;
        packet.channel = rtmp::Channel::SYSTEM;
        packet.timestamp = 0;
        packet.messageType = rtmp::MessageType::INVOKE;

        amf0::Node commandName = std::string("FCPublish");
        commandName.encode(packet.data);

        amf0::Node transactionIdNode = static_cast<double>(++invokeId);
        transactionIdNode.encode(packet.data);

        amf0::Node argument1(amf0::Marker::Null);
        argument1.encode(packet.data);

        amf0::Node argument2 = streamName;
        argument2.encode(packet.data);

        std::vector<uint8_t> buffer;
        encodePacket(buffer, outChunkSize, packet, sentPackets);

        Log(Log::Level::ALL) << "[" << name << "] " << "Sending INVOKE " << commandName.asString() << ", transaction ID: " << invokeId;

        socket.send(buffer);

        invokes[invokeId] = commandName.asString();
    }

    void PushSender::sendFCUnpublish()
    {
        rtmp::Packet packet;
        packet.channel = rtmp::Channel::SYSTEM;
        packet.timestamp = 0;
        packet.messageType = rtmp::MessageType::INVOKE;

        amf0::Node commandName = std::string("FCUnpublish");
        commandName.encode(packet.data);

        amf0::Node transactionIdNode = static_cast<double>(++invokeId);
        transactionIdNode.encode(packet.data);

        amf0::Node argument1(amf0::Marker::Null);
        argument1.encode(packet.data);

        amf0::Node argument2 = streamName;
        argument2.encode(packet.data);

        std::vector<uint8_t> buffer;
        encodePacket(buffer, outChunkSize, packet, sentPackets);

        Log(Log::Level::ALL) << "[" << name << "] " << "Sending INVOKE " << commandName.asString() << ", transaction ID: " << invokeId;

        socket.send(buffer);

        invokes[invokeId] = commandName.asString();
    }

    void PushSender::sendPublish()
    {
        rtmp::Packet packet;
        packet.channel = rtmp::Channel::SOURCE;
        packet.messageStreamId = streamId;
        packet.timestamp = 0;
        packet.messageType = rtmp::MessageType::INVOKE;

        amf0::Node commandName = std::string("publish");
        commandName.encode(packet.data);

        amf0::Node transactionIdNode = static_cast<double>(++invokeId);
        transactionIdNode.encode(packet.data);

        amf0::Node argument1(amf0::Marker::Null);
        argument1.encode(packet.data);

        amf0::Node argument2 = streamName;
        argument2.encode(packet.data);

        std::vector<uint8_t> buffer;
        encodePacket(buffer, outChunkSize, packet, sentPackets);

        Log(Log::Level::ALL) << "[" << name << "] " << "Sending INVOKE " << commandName.asString() << ", transaction ID: " << invokeId;

        socket.send(buffer);

        invokes[invokeId] = commandName.asString();

        Log(Log::Level::INFO) << "[" << name << "] " << "Published stream \"" << streamName << "\" (ID: " << streamId << ") to " << ipToString(socket.getIPAddress()) << ":" << socket.getPort();
    }

    void PushSender::createStream(const std::string& newStreamName)
    {
        if (overrideStreamName.empty())
        {
            streamName = newStreamName;
        }
        else
        {
            std::map<std::string, std::string> tokens = {
                { "streamName", newStreamName },
                { "applicationName", application },
                { "ipAddress", cppsocket::ipToString(socket.getIPAddress()) },
                { "port", std::to_string(socket.getPort()) }
            };

            streamName = overrideStreamName;
            replaceTokens(streamName, tokens);
        }

        if (connected && !streamName.empty())
        {
            sendReleaseStream();
            sendFCPublish();
            sendCreateStream();
        }
    }

    void PushSender::deleteStream()
    {
        if (connected && !streamName.empty())
        {
            sendDeleteStream();
        }

        streaming = false;
    }

    void PushSender::unpublishStream()
    {
        if (connected && !streamName.empty())
        {
            sendFCUnpublish();
        }

        streaming = false;
    }

    void PushSender::sendAudioHeader(const std::vector<uint8_t>& headerData)
    {
        audioHeader = headerData;
        audioHeaderSent = false;

        sendAudioHeader();
    }

    void PushSender::sendAudioHeader()
    {
        if (streaming && !audioHeader.empty() && audioStream)
        {
            sendAudioData(0, audioHeader);
            audioHeaderSent = true;
        }
    }

    void PushSender::sendVideoHeader(const std::vector<uint8_t>& headerData)
    {
        videoHeader = headerData;
        videoHeaderSent = false;

        sendVideoHeader();
    }

    void PushSender::sendVideoHeader()
    {
        if (streaming && !videoHeader.empty() && videoStream)
        {
            sendVideoData(0, videoHeader);
            videoHeaderSent = true;
        }
    }

    void PushSender::sendAudio(uint64_t timestamp, const std::vector<uint8_t>& audioData)
    {
        if (streaming && audioStream)
        {
            sendAudioData(timestamp, audioData);
        }
    }

    void PushSender::sendVideo(uint64_t timestamp, const std::vector<uint8_t>& videoData)
    {
        if (streaming && videoStream)
        {
            if (videoFrameSent || getVideoFrameType(videoData) == VideoFrameType::KEY)
            {
                sendVideoData(timestamp, videoData);
                videoFrameSent = true;
            }
        }
    }

    void PushSender::sendMetaData(const amf0::Node& newMetaData)
    {
        metaData = newMetaData;
        metaDataSent = false;

        sendMetaData();
    }

    void PushSender::sendAudioData(uint64_t timestamp, const std::vector<uint8_t>& audioData)
    {
        rtmp::Packet packet;
        packet.channel = rtmp::Channel::AUDIO;
        packet.messageStreamId = streamId;
        packet.timestamp = timestamp;
        packet.messageType = rtmp::MessageType::AUDIO_PACKET;

        packet.data = audioData;

        std::vector<uint8_t> buffer;
        encodePacket(buffer, outChunkSize, packet, sentPackets);

        Log(Log::Level::ALL) << "[" << name << "] " << "Sending audio packet";

        socket.send(buffer);
    }

    void PushSender::sendVideoData(uint64_t timestamp, const std::vector<uint8_t>& videoData)
    {
        rtmp::Packet packet;
        packet.channel = rtmp::Channel::VIDEO;
        packet.messageStreamId = streamId;
        packet.timestamp = timestamp;
        packet.messageType = rtmp::MessageType::VIDEO_PACKET;

        packet.data = videoData;

        std::vector<uint8_t> buffer;
        encodePacket(buffer, outChunkSize, packet, sentPackets);

        Log(Log::Level::ALL) << "[" << name << "] " << "Sending video packet";

        socket.send(buffer);
    }

    void PushSender::sendMetaData()
    {
        if (streaming && metaData.getMarker() != amf0::Marker::Unknown)
        {
            rtmp::Packet packet;
            packet.channel = rtmp::Channel::AUDIO;
            packet.messageStreamId = streamId;
            packet.timestamp = 0;
            packet.messageType = rtmp::MessageType::NOTIFY;

            amf0::Node commandName = std::string("@setDataFrame");
            commandName.encode(packet.data);

            amf0::Node argument1 = std::string("onMetaData");
            argument1.encode(packet.data);

            amf0::Node filteredMetaData = amf0::Marker::ECMAArray;

            for (auto value : metaData.asMap())
            {
                /// not in the blacklist
                if (metaDataBlacklist.find(value.first) == metaDataBlacklist.end())
                {
                    filteredMetaData[value.first] = value.second;
                }
            }

            amf0::Node argument2 = filteredMetaData;
            argument2.encode(packet.data);

            std::vector<uint8_t> buffer;
            encodePacket(buffer, outChunkSize, packet, sentPackets);

            Log(Log::Level::ALL) << "[" << name << "] " << "Sending meta data " << commandName.asString() << ":";

            Log log(Log::Level::ALL);
            log << "[" << name << "] ";
            argument2.dump(log);

            socket.send(buffer);

            metaDataSent = true;
        }
    }

    void PushSender::sendTextData(uint64_t timestamp, const amf0::Node& textData)
    {
        if (streaming && dataStream)
        {
            rtmp::Packet packet;
            packet.channel = rtmp::Channel::AUDIO;
            packet.messageStreamId = streamId;
            packet.timestamp = timestamp;
            packet.messageType = rtmp::MessageType::NOTIFY;

            amf0::Node commandName = std::string("onTextData");
            commandName.encode(packet.data);

            amf0::Node argument1 = textData;
            argument1.encode(packet.data);

            std::vector<uint8_t> buffer;
            encodePacket(buffer, outChunkSize, packet, sentPackets);

            Log(Log::Level::ALL) << "[" << name << "] " << "Sending text data";

            Log log(Log::Level::ALL);
            log << "[" << name << "] ";
            argument1.dump(log);

            socket.send(buffer);
        }
    }

    void PushSender::getInfo(std::string& str, ReportType reportType) const
    {
        switch (reportType)
        {
            case ReportType::TEXT:
            {
                str += "\t[" + name + "] " + (socket.isReady() ? "Connected" : "Not connected") + " to: " + ipToString(socket.getIPAddress()) + ":" + std::to_string(socket.getPort()) + ", state: ";

                switch (state)
                {
                    case rtmp::State::UNINITIALIZED: str += "UNINITIALIZED"; break;
                    case rtmp::State::VERSION_RECEIVED: str += "VERSION_RECEIVED"; break;
                    case rtmp::State::VERSION_SENT: str += "VERSION_SENT"; break;
                    case rtmp::State::ACK_SENT: str += "ACK_SENT"; break;
                    case rtmp::State::HANDSHAKE_DONE: str += "HANDSHAKE_DONE"; break;
                }
                str += ", name: " + streamName + "\n";
                break;
            }
            case ReportType::HTML:
            {
                str += "<tr><td>" + streamName + "</td><td>" + (socket.isReady() ? "Connected" : "Not connected") + "</td><td>" + ipToString(socket.getIPAddress()) + ":" + std::to_string(socket.getPort()) + "</td><td>";

                switch (state)
                {
                    case rtmp::State::UNINITIALIZED: str += "UNINITIALIZED"; break;
                    case rtmp::State::VERSION_RECEIVED: str += "VERSION_RECEIVED"; break;
                    case rtmp::State::VERSION_SENT: str += "VERSION_SENT"; break;
                    case rtmp::State::ACK_SENT: str += "ACK_SENT"; break;
                    case rtmp::State::HANDSHAKE_DONE: str += "HANDSHAKE_DONE"; break;
                }

                str += "</td></tr>";
                break;
            }
            case ReportType::JSON:
            {
                str += "{\"name\":\"" + streamName + "\"," +
                "\"connected\":" + (socket.isReady() ? "true" : "false") + "," +
                "\"address\":\"" + ipToString(socket.getIPAddress()) + ":" + std::to_string(socket.getPort()) + "\"," +
                "\"status\":";

                switch (state)
                {
                    case rtmp::State::UNINITIALIZED: str += "\"UNINITIALIZED\""; break;
                    case rtmp::State::VERSION_RECEIVED: str += "\"VERSION_RECEIVED\""; break;
                    case rtmp::State::VERSION_SENT: str += "\"VERSION_SENT\""; break;
                    case rtmp::State::ACK_SENT: str += "\"ACK_SENT\","; break;
                    case rtmp::State::HANDSHAKE_DONE: str += "\"HANDSHAKE_DONE\""; break;
                }

                str += "}";
                break;
            }
        }
    }
}