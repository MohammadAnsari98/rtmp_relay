//
//  rtmp_relay
//

#include <iostream>
#include "RTMP.h"

namespace rtmp
{
    uint32_t parseInt(const std::vector<uint8_t>& data, uint32_t offset, uint32_t size, uint32_t& result)
    {
        if (data.size() - offset < size)
        {
            return 0;
        }
        
        result = 0;
        
        for (int i = 0; i < size; ++i)
        {
            result <<= 1;
            result += static_cast<uint32_t>(*(data.data() + offset));
            offset += 1;
        }
        
        return size;
    }
    
    uint32_t parseHeader(const std::vector<uint8_t>& data, uint32_t offset, Header& header)
    {
        uint32_t originalOffset = offset;
        
        if (data.size() - offset < 1)
        {
            return 0;
        }
        
        uint8_t headerData = *(data.data() + offset);
        offset += 1;
        
        if ((headerData & 0xFF) != 0x03)
        {
            fprintf(stderr, "Wrong header version\n");
            return 0;
        }
        
        header.type = static_cast<HeaderType>(headerData >> 6);
        
        if (header.type != HeaderType::ONE_BYTE)
        {
            uint32_t ret = parseInt(data, offset, 3, header.timestamp);
            
            if (!ret)
            {
                return 0;
            }
            
            offset += ret;
            
            std::cout << "Timestamp: " << header.timestamp << std::endl;
            
            if (header.type != HeaderType::FOUR_BYTE)
            {
                if (data.size() - offset < 4)
                {
                    return 0;
                }
                
                uint32_t ret = parseInt(data, offset, 3, header.length);
                
                if (!ret)
                {
                    return 0;
                }
                
                offset += ret;
                
                std::cout << "Length: " << header.length << std::endl;
                
                if (data.size() - offset < 1)
                {
                    return 0;
                }
                
                header.messageType = static_cast<MessageType>(*(data.data() + offset));
                offset += 1;
                
                std::cout << "Message type ID: " << static_cast<uint32_t>(header.messageType) << std::endl;
                
                if (header.type != HeaderType::EIGHT_BYTE)
                {
                    header.messageStreamId = *reinterpret_cast<const uint32_t*>(data.data() + offset);
                    offset += sizeof(header.messageStreamId);
                    
                    std::cout << "Message stream ID: " << header.messageStreamId << std::endl;
                }
            }
        }
        
        return offset - originalOffset;
    }
    
    uint32_t parsePacket(const std::vector<uint8_t>& data, uint32_t offset, Packet& packet)
    {
        uint32_t originalOffset = offset;
        
        Header header;
        
        uint32_t ret = parseHeader(data, offset, header);
        
        if (!ret)
        {
            return 0;
        }
        
        offset += ret;
        
        return offset - originalOffset;
    }
}