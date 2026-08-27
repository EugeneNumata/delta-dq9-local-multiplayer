/*
 * ENet LAN bridge for Delta's melonDS core.
 *
 * The protocol implementation is based on melonDS 1.1 LAN.cpp.
 * Copyright 2016-2025 melonDS team.
 * Modifications copyright 2026 Eugene Numata.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MPLANClient.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <queue>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "enet/enet.h"

namespace MPLAN
{
namespace
{
constexpr u32 DiscoveryMagic = 0x444E414C; // LAND
constexpr u32 LANMagic = 0x504E414C;       // LANP
constexpr u32 PacketMagic = 0x4946494E;    // NIFI
constexpr u32 ProtocolVersion = 1;
constexpr int DiscoveryPort = 7063;
constexpr int LANPort = 7064;
constexpr int RecvTimeout = 25;
constexpr int HandshakeRetryInterval = 80;
constexpr int HandshakeRetryLimit = 8;
constexpr int PacketTraceLimit = 24;

enum Channel { ChannelCommand = 0, ChannelMP = 1 };
enum Command
{
    ClientInit = 1,
    PlayerInfo,
    PlayerList,
    PlayerConnect,
    PlayerDisconnect,
};
enum PlayerStatus
{
    PlayerNone = 0,
    PlayerClient,
    PlayerHost,
    PlayerConnecting,
    PlayerDisconnected,
};

struct Player
{
    int ID;
    char Name[32];
    PlayerStatus Status;
    u32 Address;
    bool IsLocalPlayer;
    u32 Ping;
};

struct DiscoveryData
{
    u32 Magic;
    u32 Version;
    u32 Tick;
    char SessionName[64];
    u8 NumPlayers;
    u8 MaxPlayers;
    u8 Status;
};

struct PacketHeader
{
    u32 Magic;
    u32 SenderID;
    u32 Type;
    u32 Length;
    u64 Timestamp;
};

ENetHost* host = nullptr;
ENetPeer* hostPeer = nullptr;
ENetPeer* lastHostPeer = nullptr;
Player players[16] = {};
Player me = {};
u16 connectedMask = 0;
int lastHostID = -1;
u8 currentChannel = 1;
std::queue<ENetPacket*> rxQueue;
int traceTXCount = 0;
int traceRXCount = 0;
bool macMapped = false;
u8 localMAC[6] = {};
u8 wireMAC[6] = {};
u8 pendingHandshake[sizeof(PacketHeader) + 2048] = {};
size_t pendingHandshakeLength = 0;
u16 pendingResponseSubtype = 0xFFFF;
u32 pendingRetryAt = 0;
int pendingRetryCount = 0;

const char* PacketTypeName(u32 type)
{
    switch (type & 0xFFFF)
    {
    case 0: return "packet";
    case 1: return "cmd";
    case 2: return "reply";
    case 3: return "ack";
    default: return "unknown";
    }
}

u16 FrameControl(const u8* data, int len)
{
    if (!data || len < 14) return 0xFFFF;
    return data[12] | (data[13] << 8);
}

u16 ReadLE16(const u8* data)
{
    return data[0] | (data[1] << 8);
}

void TraceManagementFrame(const char* direction, const u8* data, int len)
{
    if (!data || len < 12 + 24) return;

    const u8* frame = data + 12;
    const u16 frameControl = ReadLE16(frame);
    const u16 subtype = frameControl & 0x00FC;
    if (subtype != 0x00B0 && subtype != 0x0000 && subtype != 0x0010 &&
        subtype != 0x00A0 && subtype != 0x00C0)
        return;

    const u8* destination = frame + 4;
    const u8* source = frame + 10;
    const u8* bssid = frame + 16;
    fprintf(stderr,
            "DQ9P2P: %s mgmt fc=%04X dst=%02X:%02X:%02X:%02X:%02X:%02X src=%02X:%02X:%02X:%02X:%02X:%02X bssid=%02X:%02X:%02X:%02X:%02X:%02X",
            direction, frameControl,
            destination[0], destination[1], destination[2], destination[3], destination[4], destination[5],
            source[0], source[1], source[2], source[3], source[4], source[5],
            bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);

    const u8* body = frame + 24;
    const int bodyLength = len - (12 + 24);
    if (subtype == 0x00B0 && bodyLength >= 6)
        fprintf(stderr, " auth_algorithm=%u auth_sequence=%u status=%u",
                ReadLE16(body), ReadLE16(body + 2), ReadLE16(body + 4));
    else if (subtype == 0x0000 && bodyLength >= 4)
        fprintf(stderr, " association_request capability=%04X listen_interval=%u",
                ReadLE16(body), ReadLE16(body + 2));
    else if (subtype == 0x0010 && bodyLength >= 6)
        fprintf(stderr, " association_response capability=%04X status=%u aid=%u",
                ReadLE16(body), ReadLE16(body + 2), ReadLE16(body + 4) & 0x3FFF);
    else if ((subtype == 0x00A0 || subtype == 0x00C0) && bodyLength >= 2)
        fprintf(stderr, " reason=%u", ReadLE16(body));
    fprintf(stderr, "\n");
}

u32 NowMS()
{
    using namespace std::chrono;
    return (u32)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void ClearPendingHandshake()
{
    pendingHandshakeLength = 0;
    pendingResponseSubtype = 0xFFFF;
    pendingRetryAt = 0;
    pendingRetryCount = 0;
}

void RememberHandshake(const ENetPacket* packet, u16 requestSubtype)
{
    if (!packet || packet->dataLength > sizeof(pendingHandshake)) return;
    memcpy(pendingHandshake, packet->data, packet->dataLength);
    pendingHandshakeLength = packet->dataLength;
    pendingResponseSubtype = requestSubtype == 0x00B0 ? 0x00B0 : 0x0010;
    pendingRetryAt = NowMS() + HandshakeRetryInterval;
    pendingRetryCount = 0;
}

void RetryPendingHandshake()
{
    if (!host || !hostPeer || !pendingHandshakeLength ||
        pendingRetryCount >= HandshakeRetryLimit)
        return;

    const u32 now = NowMS();
    if ((s32)(now - pendingRetryAt) < 0) return;

    ENetPacket* packet = enet_packet_create(pendingHandshake, pendingHandshakeLength,
                                             ENET_PACKET_FLAG_RELIABLE);
    if (!packet) return;
    const int result = enet_peer_send(hostPeer, ChannelMP, packet);
    enet_host_flush(host);
    pendingRetryCount++;
    pendingRetryAt = now + HandshakeRetryInterval;
    fprintf(stderr, "DQ9P2P: handshake retry %d/%d result=%d waiting_fc=%04X\n",
            pendingRetryCount, HandshakeRetryLimit, result, pendingResponseSubtype);
}

void DestroyQueue()
{
    while (!rxQueue.empty())
    {
        enet_packet_destroy(rxQueue.front());
        rxQueue.pop();
    }
}

void Disconnect()
{
    DestroyQueue();
    if (host)
    {
        if (hostPeer) enet_peer_disconnect_now(hostPeer, 0);
        enet_host_destroy(host);
    }
    host = nullptr;
    hostPeer = nullptr;
    lastHostPeer = nullptr;
    lastHostID = -1;
    currentChannel = 1;
    connectedMask = 0;
    traceTXCount = 0;
    traceRXCount = 0;
    macMapped = false;
    ClearPendingHandshake();
    memset(localMAC, 0, sizeof(localMAC));
    memset(wireMAC, 0, sizeof(wireMAC));
    memset(players, 0, sizeof(players));
    memset(&me, 0, sizeof(me));
}

bool DiscoverHost(char* address, size_t addressSize)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(DiscoveryPort);
    if (bind(sock, (sockaddr*)&local, sizeof(local)) < 0)
    {
        close(sock);
        return false;
    }

    const u32 deadline = NowMS() + 3500;
    bool found = false;
    while ((s32)(deadline - NowMS()) > 0)
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        timeval timeout = {0, 250000};
        if (select(sock + 1, &readfds, nullptr, nullptr, &timeout) <= 0) continue;

        DiscoveryData beacon = {};
        sockaddr_in remote = {};
        socklen_t remoteLen = sizeof(remote);
        const ssize_t len = recvfrom(sock, &beacon, sizeof(beacon), 0,
                                     (sockaddr*)&remote, &remoteLen);
        if (len < (ssize_t)sizeof(beacon)) continue;
        if (beacon.Magic != DiscoveryMagic || beacon.Version != ProtocolVersion) continue;
        if (beacon.NumPlayers >= beacon.MaxPlayers || beacon.MaxPlayers > 16) continue;

        if (inet_ntop(AF_INET, &remote.sin_addr, address, (socklen_t)addressSize))
        {
            fprintf(stderr, "DQ9P2P: discovered melonDS LAN host %s (%s)\n", address, beacon.SessionName);
            found = true;
            break;
        }
    }

    close(sock);
    return found;
}

bool Connect(const char* address)
{
    host = enet_host_create(nullptr, 16, 2, 0, 0);
    if (!host) return false;

    ENetAddress remote = {};
    if (enet_address_set_host(&remote, address) != 0)
    {
        Disconnect();
        return false;
    }
    remote.port = LANPort;
    hostPeer = enet_host_connect(host, &remote, 2, 0);
    if (!hostPeer)
    {
        Disconnect();
        return false;
    }

    memset(&me, 0, sizeof(me));
    strncpy(me.Name, "Delta iPhone", sizeof(me.Name) - 1);
    me.Status = PlayerConnecting;

    int state = 0;
    const u32 deadline = NowMS() + 5000;
    ENetEvent event = {};
    while ((s32)(deadline - NowMS()) > 0)
    {
        if (enet_host_service(host, &event, 250) <= 0) continue;
        if (event.type == ENET_EVENT_TYPE_CONNECT && state == 0)
        {
            state = 1;
            continue;
        }
        if (event.type == ENET_EVENT_TYPE_DISCONNECT) break;
        if (event.type != ENET_EVENT_TYPE_RECEIVE) continue;

        bool accepted = false;
        if (state == 1 && event.channelID == ChannelCommand && event.packet->dataLength == 11)
        {
            u8* data = event.packet->data;
            u32 magic = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
            u32 version = data[5] | (data[6] << 8) | (data[7] << 16) | (data[8] << 24);
            if (data[0] == ClientInit && magic == LANMagic && version == ProtocolVersion &&
                data[9] < 16 && data[10] <= 16)
            {
                me.ID = data[9];
                u8 command[9 + sizeof(Player)] = {};
                command[0] = PlayerInfo;
                memcpy(&command[1], &magic, sizeof(magic));
                memcpy(&command[5], &version, sizeof(version));
                memcpy(&command[9], &me, sizeof(me));
                ENetPacket* packet = enet_packet_create(command, sizeof(command), ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(event.peer, ChannelCommand, packet);
                enet_host_flush(host);
                event.peer->data = &players[0];
                accepted = true;
                state = 2;
            }
        }
        enet_packet_destroy(event.packet);
        if (accepted) break;
    }

    if (state != 2)
    {
        Disconnect();
        return false;
    }

    fprintf(stderr, "DQ9P2P: joined melonDS LAN as player %d\n", me.ID);
    return true;
}

void ProcessControl(ENetEvent& event)
{
    if (event.type == ENET_EVENT_TYPE_DISCONNECT)
    {
        fprintf(stderr, "DQ9P2P: melonDS LAN disconnected\n");
        hostPeer = nullptr;
        return;
    }
    if (event.type != ENET_EVENT_TYPE_RECEIVE) return;
    if (event.channelID != ChannelCommand || event.packet->dataLength < 1)
    {
        enet_packet_destroy(event.packet);
        return;
    }

    u8* data = event.packet->data;
    if (data[0] == PlayerList && event.packet->dataLength == 2 + sizeof(players) && data[1] <= 16)
    {
        memcpy(players, &data[2], sizeof(players));
        hostPeer->data = &players[0];
        fprintf(stderr, "DQ9P2P: player list count=%u host=%d status=%d\n",
                data[1], players[0].ID, players[0].Status);
    }
    else if (data[0] == PlayerConnect && event.packet->dataLength == 1)
    {
        Player* player = static_cast<Player*>(event.peer->data);
        if (player && player->ID >= 0 && player->ID < 16) connectedMask |= 1 << player->ID;
        fprintf(stderr, "DQ9P2P: player connected id=%d mask=%04X\n",
                player ? player->ID : -1, connectedMask);
    }
    else if (data[0] == PlayerDisconnect && event.packet->dataLength == 1)
    {
        Player* player = static_cast<Player*>(event.peer->data);
        if (player && player->ID >= 0 && player->ID < 16) connectedMask &= ~(1 << player->ID);
        fprintf(stderr, "DQ9P2P: player disconnected id=%d mask=%04X\n",
                player ? player->ID : -1, connectedMask);
    }
    enet_packet_destroy(event.packet);
}

// Matches melonDS 1.1 LAN::ProcessLAN behavior:
// 1 = look for a normal MP frame, 2 = wait for any MP frame.
void ProcessNetwork(int type)
{
    if (!host) return;

    RetryPendingHandshake();

    const u32 now = NowMS();
    while (!rxQueue.empty())
    {
        ENetPacket* packet = rxQueue.front();
        PacketHeader* header = reinterpret_cast<PacketHeader*>(packet->data);

        // Magic is replaced with the local receive time when a packet is queued.
        if (header->Magic > now || header->Magic < now - 16)
        {
            rxQueue.pop();
            enet_packet_destroy(packet);
            continue;
        }

        if (type == 2) return;
        if (type == 1)
        {
            if (header->Type == 0) return;
            rxQueue.pop();
            enet_packet_destroy(packet);
        }
        break;
    }

    ENetEvent event = {};
    int timeout = type == 2 ? RecvTimeout : 0;
    while (enet_host_service(host, &event, timeout) > 0)
    {
        timeout = 0;
        if (event.type == ENET_EVENT_TYPE_RECEIVE && event.channelID == ChannelMP)
        {
            if (event.packet->dataLength >= sizeof(PacketHeader))
            {
                PacketHeader* header = reinterpret_cast<PacketHeader*>(event.packet->data);
                if (header->Magic == PacketMagic && header->SenderID != (u32)me.ID &&
                    header->Length <= 2048 && sizeof(PacketHeader) + header->Length <= event.packet->dataLength)
                {
                    // A client can join after the host has already announced
                    // PlayerConnect. Seeing a valid MP packet proves that peer
                    // is active, so recover the missed ready bit here.
                    if (header->SenderID < 16 && !(connectedMask & (1 << header->SenderID)))
                    {
                        connectedMask |= 1 << header->SenderID;
                        fprintf(stderr, "DQ9P2P: inferred active player id=%u mask=%04X\n",
                                header->SenderID, connectedMask);
                    }
                    // melonDS 1.1 includes the active Wi-Fi channel in byte 9 of
                    // every frame. Delta's older Wi-Fi core predates that field,
                    // so learn it from the peer and add it to outgoing frames.
                    const u8* frame = event.packet->data + sizeof(PacketHeader);
                    if (header->Length > 9 && frame[9] >= 1 && frame[9] <= 14 && frame[9] != currentChannel)
                    {
                        currentChannel = frame[9];
                        fprintf(stderr, "DQ9P2P: learned Wi-Fi channel %u\n", currentChannel);
                    }
                    const u16 receivedSubtype = FrameControl(frame, header->Length) & 0x00FC;
                    bool completesHandshake = receivedSubtype == pendingResponseSubtype;
                    if (completesHandshake && receivedSubtype == 0x00B0)
                    {
                        // Authentication sequence 2 is the host's response;
                        // sequence 1 would merely be another request.
                        completesHandshake = header->Length >= 42 && ReadLE16(frame + 38) >= 2;
                    }
                    if (completesHandshake || receivedSubtype == 0x00A0 || receivedSubtype == 0x00C0)
                    {
                        if (pendingHandshakeLength)
                            fprintf(stderr, "DQ9P2P: handshake wait completed by fc=%04X after %d retries\n",
                                    FrameControl(frame, header->Length), pendingRetryCount);
                        ClearPendingHandshake();
                    }
                    if (traceRXCount++ < PacketTraceLimit)
                    {
                        fprintf(stderr,
                                "DQ9P2P: LAN RX %s raw=%08X sender=%u len=%u ts=%llu ch=%u mask=%04X fc=%04X\n",
                                PacketTypeName(header->Type), header->Type, header->SenderID,
                                header->Length, header->Timestamp,
                                header->Length > 9 ? frame[9] : 0, connectedMask,
                                FrameControl(frame, header->Length));
                        TraceManagementFrame("RX", frame, header->Length);
                    }
                    header->Magic = NowMS();
                    event.packet->userData = event.peer;
                    rxQueue.push(event.packet);
                    return;
                }
            }
            enet_packet_destroy(event.packet);
        }
        else
        {
            ProcessControl(event);
        }
    }
}

int Send(u32 type, u8* data, int len, u64 timestamp)
{
    if (!host || !hostPeer || len < 0 || len > 2048) return 0;
    const u16 frameControl = FrameControl(data, len);
    const u8 frameSubtype = frameControl == 0xFFFF ? 0xFF : (frameControl & 0x00FC);
    const bool managementHandshake = (type == 0) &&
        (frameSubtype == 0x00B0 || frameSubtype == 0x0000 || frameSubtype == 0x0010);
    const u32 flags = managementHandshake ? ENET_PACKET_FLAG_RELIABLE
                                          : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(nullptr, sizeof(PacketHeader) + len, flags);
    if (!packet) return 0;
    PacketHeader header = {PacketMagic, (u32)me.ID, type, (u32)len, timestamp};
    memcpy(packet->data, &header, sizeof(header));
    if (len) memcpy(packet->data + sizeof(header), data, len);
    if (len > 9)
        packet->data[sizeof(header) + 9] = currentChannel;

    // Both standalone melonDS and Delta commonly start from the same
    // generated firmware MAC. Give Delta a unique transmitter address only
    // on the LAN wire, then translate replies back on receive. This keeps the
    // guest firmware, saves and non-multiplayer configuration untouched.
    if (len >= 12 + 24)
    {
        u8* frame = packet->data + sizeof(header) + 12;
        u8* source = frame + 10;
        if (!macMapped)
        {
            memcpy(localMAC, source, sizeof(localMAC));
            memcpy(wireMAC, localMAC, sizeof(wireMAC));
            wireMAC[0] &= 0xFC;
            wireMAC[3] += me.ID;
            wireMAC[4] += me.ID * 0x44;
            wireMAC[5] += me.ID * 0x10;
            macMapped = true;
            fprintf(stderr,
                    "DQ9P2P: LAN MAC map %02X:%02X:%02X:%02X:%02X:%02X -> %02X:%02X:%02X:%02X:%02X:%02X\n",
                    localMAC[0], localMAC[1], localMAC[2], localMAC[3], localMAC[4], localMAC[5],
                    wireMAC[0], wireMAC[1], wireMAC[2], wireMAC[3], wireMAC[4], wireMAC[5]);
        }
        if (memcmp(source, localMAC, sizeof(localMAC)) == 0)
            memcpy(source, wireMAC, sizeof(wireMAC));
    }

    if (traceTXCount++ < PacketTraceLimit)
    {
        fprintf(stderr,
                "DQ9P2P: LAN TX %s raw=%08X sender=%d len=%d ts=%llu ch=%u mask=%04X aid=%u direct=%d fc=%04X\n",
                PacketTypeName(type), type, me.ID, len, timestamp,
                len > 9 ? currentChannel : 0, connectedMask, type >> 16,
                ((type & 0xFFFF) == 2 && lastHostPeer) ? 1 : 0,
                FrameControl(data, len));
        TraceManagementFrame("TX", packet->data + sizeof(header), len);
    }

    ENetPeer* destinationPeer = ((type & 0xFFFF) == 2 && lastHostPeer)
        ? lastHostPeer : hostPeer;
    if (managementHandshake)
        RememberHandshake(packet, frameSubtype);
    const int sendResult = enet_peer_send(destinationPeer, ChannelMP, packet);
    enet_host_flush(host);
    if (managementHandshake)
        fprintf(stderr, "DQ9P2P: reliable handshake send result=%d fc=%04X\n",
                sendResult, frameControl);
    return len;
}

int Receive(u8* data, bool block, u64* timestamp)
{
    if (!host) return 0;
    ProcessNetwork(block ? 2 : 1);
    if (rxQueue.empty()) return 0;

    ENetPacket* packet = rxQueue.front();
    rxQueue.pop();
    PacketHeader* header = reinterpret_cast<PacketHeader*>(packet->data);
    u32 len = header->Length > 2048 ? 2048 : header->Length;
    if (len)
    {
        u8* payload = packet->data + sizeof(PacketHeader);
        if (macMapped && len >= 12 + 24)
        {
            u8* destination = payload + 12 + 4;
            if (memcmp(destination, wireMAC, sizeof(wireMAC)) == 0)
                memcpy(destination, localMAC, sizeof(localMAC));
        }
        memcpy(data, payload, len);
    }
    if (header->Type == 1)
    {
        lastHostID = (int)header->SenderID;
        lastHostPeer = static_cast<ENetPeer*>(packet->userData);
    }
    if (timestamp) *timestamp = header->Timestamp;
    enet_packet_destroy(packet);
    return (int)len;
}
}

bool Init()
{
    if (enet_initialize() != 0)
    {
        fprintf(stderr, "DQ9P2P: ENet initialization failed\n");
        return false;
    }
    fprintf(stderr, "DQ9P2P: ENet initialized\n");
    return true;
}

void DeInit()
{
    Disconnect();
    enet_deinitialize();
}

void Begin()
{
    if (!host)
    {
        char address[INET_ADDRSTRLEN] = {};
        fprintf(stderr, "DQ9P2P: looking for melonDS LAN host\n");
        if (!DiscoverHost(address, sizeof(address)) || !Connect(address))
        {
            fprintf(stderr, "DQ9P2P: no compatible melonDS LAN host found\n");
            return;
        }
    }
    lastHostID = -1;
    lastHostPeer = nullptr;
    ClearPendingHandshake();
    connectedMask |= 1 << me.ID;
    traceTXCount = 0;
    traceRXCount = 0;
    u8 command = PlayerConnect;
    ENetPacket* packet = enet_packet_create(&command, 1, ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast(host, ChannelCommand, packet);
    enet_host_flush(host);
    fprintf(stderr, "DQ9P2P: announced ready id=%d mask=%04X\n", me.ID, connectedMask);
}

void End()
{
    if (!host) return;
    ClearPendingHandshake();
    connectedMask &= ~(1 << me.ID);
    u8 command = PlayerDisconnect;
    ENetPacket* packet = enet_packet_create(&command, 1, ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast(host, ChannelCommand, packet);
    enet_host_flush(host);
}

int SendPacket(u8* data, int len, u64 timestamp) { return Send(0, data, len, timestamp); }
int RecvPacket(u8* data, u64* timestamp) { return Receive(data, false, timestamp); }
int SendCmd(u8* data, int len, u64 timestamp) { return Send(1, data, len, timestamp); }
int SendReply(u8* data, int len, u64 timestamp, u16 aid) { return Send(2 | (aid << 16), data, len, timestamp); }
int SendAck(u8* data, int len, u64 timestamp) { return Send(3, data, len, timestamp); }

int RecvHostPacket(u8* data, u64* timestamp)
{
    if (lastHostID != -1 && !(connectedMask & (1 << lastHostID))) return -1;
    return Receive(data, true, timestamp);
}

u16 RecvReplies(u8* data, u64 timestamp, u16 aidmask)
{
    if (!host) return 0;
    u16 result = 0;
    u16 receivedPlayers = 1 << me.ID;
    if ((receivedPlayers & connectedMask) == connectedMask) return 0;
    for (;;)
    {
        ProcessNetwork(2);
        if (rxQueue.empty()) return result;
        ENetPacket* packet = rxQueue.front();
        rxQueue.pop();
        PacketHeader* header = reinterpret_cast<PacketHeader*>(packet->data);
        if ((header->Type & 0xFFFF) == 2 && header->Timestamp >= timestamp - 32)
        {
            u32 aid = header->Type >> 16;
            u32 len = header->Length > 1024 ? 1024 : header->Length;
            if (aid > 0 && aid < 16 && len)
            {
                memcpy(&data[(aid - 1) * 1024], packet->data + sizeof(PacketHeader), len);
                result |= 1 << aid;
            }
            receivedPlayers |= 1 << header->SenderID;
        }
        enet_packet_destroy(packet);
        if ((receivedPlayers & connectedMask) == connectedMask || (result & aidmask) == aidmask)
            return result;
    }
}
}
