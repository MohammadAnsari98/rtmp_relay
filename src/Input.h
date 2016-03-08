//
//  rtmp_relay
//

#pragma once

#include <vector>
#include "Socket.h"

class Input
{
public:
    Input() = default;
    Input(Network& network);
    ~Input();
    
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    
    Input(Input&& other);
    Input& operator=(Input&& other);
    
    bool init(int serverSocket);
    void update();
    
    bool getPacket(std::vector<char>& packet);
    
private:
    Network& _network;
    Socket _socket;
    
    std::vector<char> _data;
};
