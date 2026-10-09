// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/ENet.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <picojson.h>
#include <SFML/Network/IpAddress.hpp>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace Common::ENet
{
namespace
{
constexpr std::array<u8, 4> ROLLBACK_MAGIC{'G', 'K', 'R', 'B'};
constexpr u8 ROLLBACK_VERSION = 1;
constexpr size_t ROLLBACK_HEADER_SIZE = 12;

std::mutex s_rollback_mutex;
ENetHost* s_rollback_host = nullptr;
bool s_rollback_host_is_server = false;
bool s_rollback_active = false;
u32 s_rollback_session_id = 0;
std::deque<RollbackDatagram> s_rollback_datagrams;
RollbackDatagramStats s_rollback_stats;

bool s_rendezvous_active = false;
std::string s_rendezvous_match_id;
std::string s_rendezvous_player_id;
std::optional<RendezvousManifest> s_rendezvous_manifest;
std::unordered_map<std::string, ENetAddress> s_rendezvous_peers;
std::unordered_set<std::string> s_rendezvous_acked_peers;

void WriteSessionId(u8* header, u32 session_id)
{
  header[8] = static_cast<u8>(session_id >> 24);
  header[9] = static_cast<u8>(session_id >> 16);
  header[10] = static_cast<u8>(session_id >> 8);
  header[11] = static_cast<u8>(session_id);
}

u32 ReadSessionId(const u8* header)
{
  return static_cast<u32>(header[8]) << 24 | static_cast<u32>(header[9]) << 16 |
         static_cast<u32>(header[10]) << 8 | static_cast<u32>(header[11]);
}

bool ParseHostPort(const std::string& value, ENetAddress* address)
{
  const size_t separator = value.rfind(':');
  if (separator == std::string::npos || separator == 0 || separator + 1 == value.size())
    return false;

  const std::string host = value.substr(0, separator);
  const std::string port = value.substr(separator + 1);
  char* end = nullptr;
  const unsigned long parsed_port = std::strtoul(port.c_str(), &end, 10);
  if (!end || *end != '\0' || parsed_port == 0 || parsed_port > 65535)
    return false;

  address->port = static_cast<enet_uint16>(parsed_port);
  return enet_address_set_host(address, host.c_str()) == 0;
}

bool InterceptRendezvousDatagram(ENetHost* host, ENetEvent* event)
{
  std::string match_id;
  std::string player_id;
  {
    std::lock_guard lk(s_rollback_mutex);
    if (!s_rendezvous_active || host != s_rollback_host)
      return false;
    match_id = s_rendezvous_match_id;
    player_id = s_rendezvous_player_id;
  }

  const auto* bytes = reinterpret_cast<const char*>(host->receivedData);
  const std::string payload(bytes, bytes + host->receivedDataLength);
  picojson::value root;
  if (!picojson::parse(root, payload).empty() || !root.is<picojson::object>())
    return false;

  const auto& object = root.get<picojson::object>();
  const auto type_it = object.find("type");
  const auto match_it = object.find("match_id");
  if (type_it == object.end() || !type_it->second.is<std::string>() || match_it == object.end() ||
      !match_it->second.is<std::string>() || match_it->second.get<std::string>() != match_id)
  {
    return false;
  }

  const std::string& type = type_it->second.get<std::string>();
  if (type == "mesh_probe" || type == "mesh_ack")
  {
    const auto from_it = object.find("player_id");
    if (from_it == object.end() || !from_it->second.is<std::string>())
      return false;
    const std::string& from = from_it->second.get<std::string>();

    bool valid_peer = false;
    {
      std::lock_guard lk(s_rollback_mutex);
      const auto peer = s_rendezvous_peers.find(from);
      valid_peer = peer != s_rendezvous_peers.end() &&
                   peer->second.host == host->receivedAddress.host &&
                   peer->second.port == host->receivedAddress.port;
      if (valid_peer && type == "mesh_ack")
        s_rendezvous_acked_peers.emplace(from);
    }
    if (!valid_peer)
      return false;

    if (type == "mesh_probe")
    {
      picojson::object ack;
      ack["type"] = picojson::value("mesh_ack");
      ack["match_id"] = picojson::value(match_id);
      ack["player_id"] = picojson::value(player_id);
      const std::string ack_payload = picojson::value(ack).serialize();
      ENetBuffer buffer{};
      buffer.data = const_cast<char*>(ack_payload.data());
      buffer.dataLength = ack_payload.size();
      enet_socket_send(host->socket, &host->receivedAddress, &buffer, 1);
    }
    event->type = static_cast<ENetEventType>(SKIPPABLE_EVENT);
    return true;
  }

  const auto players_it = object.find("players");
  if (type != "manifest" || players_it == object.end() ||
      !players_it->second.is<picojson::array>())
  {
    return false;
  }

  RendezvousManifest manifest;
  manifest.match_id = match_id;
  for (const picojson::value& entry : players_it->second.get<picojson::array>())
  {
    if (!entry.is<picojson::object>())
      return false;
    const auto& player = entry.get<picojson::object>();
    const auto id = player.find("player_id");
    const auto seat = player.find("seat");
    const auto endpoint = player.find("endpoint");
    if (id == player.end() || !id->second.is<std::string>() || seat == player.end() ||
        !seat->second.is<double>() || endpoint == player.end() ||
        !endpoint->second.is<std::string>())
    {
      return false;
    }

    RendezvousPlayer parsed;
    parsed.player_id = id->second.get<std::string>();
    parsed.seat = static_cast<int>(seat->second.get<double>());
    if (!ParseHostPort(endpoint->second.get<std::string>(), &parsed.endpoint))
      return false;
    manifest.players.emplace_back(std::move(parsed));
  }

  {
    std::lock_guard lk(s_rollback_mutex);
    if (s_rendezvous_active && s_rendezvous_match_id == match_id)
      s_rendezvous_manifest = std::move(manifest);
  }
  event->type = static_cast<ENetEventType>(SKIPPABLE_EVENT);
  return true;
}
}  // namespace

void WakeupThread(ENetHost* host)
{
  // Send ourselves a spurious message.  This is hackier than it should be.
  // comex reported this as https://github.com/lsalzman/enet/issues/23, so
  // hopefully there will be a better way to do it in the future.
  ENetAddress address;
  if (host->address.port != 0)
    address.port = host->address.port;
  else
    enet_socket_get_address(host->socket, &address);
  address.host = 0x0100007f;  // localhost
  u8 byte = 0;
  ENetBuffer buf;
  buf.data = &byte;
  buf.dataLength = 1;
  enet_socket_send(host->socket, &address, &buf, 1);
}

int ENET_CALLBACK InterceptCallback(ENetHost* host, ENetEvent* event)
{
  if (InterceptRendezvousDatagram(host, event))
    return 1;
  if (InterceptRollbackDatagram(host, event))
    return 1;

  // wakeup packet received
  if (host->receivedDataLength == 1 && host->receivedData[0] == 0)
  {
    event->type = static_cast<ENetEventType>(SKIPPABLE_EVENT);
    return 1;
  }
  return 0;
}

std::optional<RendezvousManifest>
RunRendezvous(const std::string& server, const std::string& match_id, const std::string& player_id,
              const std::string& token, int expected_players, bool service_socket,
              std::chrono::milliseconds timeout, std::string* error)
{
  ENetAddress server_address{};
  if (!ParseHostPort(server, &server_address))
  {
    if (error)
      *error = "Invalid rendezvous server address";
    return std::nullopt;
  }

  ENetHost* host = nullptr;
  {
    std::lock_guard lk(s_rollback_mutex);
    host = s_rollback_host;
    if (!host)
    {
      if (error)
        *error = "No NetPlay UDP socket is registered";
      return std::nullopt;
    }
    s_rendezvous_active = true;
    s_rendezvous_match_id = match_id;
    s_rendezvous_player_id = player_id;
    s_rendezvous_manifest.reset();
    s_rendezvous_peers.clear();
    s_rendezvous_acked_peers.clear();
  }

  picojson::object registration;
  registration["type"] = picojson::value("register");
  registration["match_id"] = picojson::value(match_id);
  registration["player_id"] = picojson::value(player_id);
  registration["token"] = picojson::value(token);
  ENetAddress local_address{};
  if (enet_socket_get_address(host->socket, &local_address) != 0 || local_address.port == 0)
  {
    ENetAddress bind_address{};
    bind_address.host = ENET_HOST_ANY;
    bind_address.port = 0;
    enet_socket_bind(host->socket, &bind_address);
  }
  const std::optional<sf::IpAddress> local_ip = sf::IpAddress::getLocalAddress();
  if (local_ip && enet_socket_get_address(host->socket, &local_address) == 0 &&
      local_address.port != 0)
  {
    registration["local_endpoint"] = picojson::value(
        local_ip->toString() + ":" + std::to_string(local_address.port));
  }
  const std::string payload = picojson::value(registration).serialize();
  ENetBuffer buffer{};
  buffer.data = const_cast<char*>(payload.data());
  buffer.dataLength = payload.size();

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto next_send = std::chrono::steady_clock::time_point{};
  std::optional<RendezvousManifest> result;
  bool manifest_received = false;
  while (std::chrono::steady_clock::now() < deadline)
  {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_send && !manifest_received)
    {
      enet_socket_send(host->socket, &server_address, &buffer, 1);
      next_send = now + std::chrono::milliseconds(250);
    }

    if (service_socket)
    {
      ENetEvent event{};
      while (enet_host_service(host, &event, 5) > 0)
      {
        if (event.type == ENET_EVENT_TYPE_RECEIVE)
          enet_packet_destroy(event.packet);
      }
    }
    else
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    {
      std::lock_guard lk(s_rollback_mutex);
      if (s_rendezvous_manifest &&
          static_cast<int>(s_rendezvous_manifest->players.size()) == expected_players)
      {
        result = std::move(s_rendezvous_manifest);
        for (const RendezvousPlayer& player : result->players)
        {
          if (player.player_id != player_id)
            s_rendezvous_peers.emplace(player.player_id, player.endpoint);
        }
        manifest_received = true;
        break;
      }
    }
  }

  // Punch every pair directly through the exact socket that will carry rollback traffic. A player
  // is admitted only after every other roster member has acknowledged a packet from this endpoint.
  next_send = {};
  const auto mesh_deadline = std::chrono::steady_clock::now() + timeout;
  std::optional<std::chrono::steady_clock::time_point> mesh_ready_since;
  bool mesh_complete = false;
  while (result && std::chrono::steady_clock::now() < mesh_deadline)
  {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_send)
    {
      picojson::object probe;
      probe["type"] = picojson::value("mesh_probe");
      probe["match_id"] = picojson::value(match_id);
      probe["player_id"] = picojson::value(player_id);
      const std::string probe_payload = picojson::value(probe).serialize();
      ENetBuffer probe_buffer{};
      probe_buffer.data = const_cast<char*>(probe_payload.data());
      probe_buffer.dataLength = probe_payload.size();
      std::lock_guard lk(s_rollback_mutex);
      for (const auto& [peer_id, endpoint] : s_rendezvous_peers)
      {
        if (!s_rendezvous_acked_peers.contains(peer_id))
          enet_socket_send(host->socket, &endpoint, &probe_buffer, 1);
      }
      next_send = now + std::chrono::milliseconds(100);
    }

    if (service_socket)
    {
      ENetEvent event{};
      while (enet_host_service(host, &event, 5) > 0)
      {
        if (event.type == ENET_EVENT_TYPE_RECEIVE)
          enet_packet_destroy(event.packet);
      }
    }
    else
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    {
      std::lock_guard lk(s_rollback_mutex);
      if (s_rendezvous_acked_peers.size() == s_rendezvous_peers.size())
      {
        if (s_rendezvous_peers.empty())
        {
          mesh_complete = true;
          break;
        }
        if (!mesh_ready_since)
          mesh_ready_since = std::chrono::steady_clock::now();
        // Keep answering probes briefly after local success so slower peers can finish too.
        if (std::chrono::steady_clock::now() - *mesh_ready_since >=
            std::chrono::milliseconds(750))
        {
          mesh_complete = true;
          break;
        }
      }
      else
      {
        mesh_ready_since.reset();
      }
    }
  }

  if (result && !mesh_complete)
  {
    result.reset();
    if (error)
      *error = "Direct connectivity to every matched player could not be established";
  }

  {
    std::lock_guard lk(s_rollback_mutex);
    s_rendezvous_active = false;
    s_rendezvous_match_id.clear();
    s_rendezvous_player_id.clear();
    s_rendezvous_manifest.reset();
    s_rendezvous_peers.clear();
    s_rendezvous_acked_peers.clear();
  }
  if (!result && error)
  {
    if (error->empty())
      *error = manifest_received ? "Direct peer mesh timed out" :
                                   "Timed out waiting for every matched player to open Dolphin";
  }
  return result;
}

void RegisterRollbackSocket(ENetHost* host, bool server_socket)
{
  if (!host)
    return;

  std::lock_guard lk(s_rollback_mutex);
  if (!s_rollback_host || server_socket || !s_rollback_host_is_server)
  {
    s_rollback_host = host;
    s_rollback_host_is_server = server_socket;
  }
}

void UnregisterRollbackSocket(ENetHost* host)
{
  std::lock_guard lk(s_rollback_mutex);
  if (s_rollback_host == host)
  {
    s_rollback_host = nullptr;
    s_rollback_host_is_server = false;
    s_rollback_active = false;
    s_rollback_datagrams.clear();
  }
}

bool StartRollbackDatagrams(u32 session_id)
{
  std::lock_guard lk(s_rollback_mutex);
  if (!s_rollback_host)
    return false;

  s_rollback_session_id = session_id;
  s_rollback_datagrams.clear();
  s_rollback_stats = {};
  s_rollback_active = true;
  ENetAddress local_address{};
  enet_socket_get_address(s_rollback_host->socket, &local_address);
  INFO_LOG_FMT(NETPLAY, "GekkoNet shared UDP transport active on local port {} (session {})",
               local_address.port, session_id);
  return true;
}

void StopRollbackDatagrams()
{
  std::lock_guard lk(s_rollback_mutex);
  s_rollback_active = false;
  s_rollback_datagrams.clear();
}

bool SendRollbackDatagram(const ENetAddress& address, const void* payload, size_t size)
{
  std::array<u8, ROLLBACK_HEADER_SIZE> header{};
  ENetSocket socket = ENET_SOCKET_NULL;
  {
    std::lock_guard lk(s_rollback_mutex);
    if (!s_rollback_active || !s_rollback_host)
      return false;
    socket = s_rollback_host->socket;
    std::copy(ROLLBACK_MAGIC.begin(), ROLLBACK_MAGIC.end(), header.begin());
    header[4] = ROLLBACK_VERSION;
    WriteSessionId(header.data(), s_rollback_session_id);
  }

  ENetBuffer buffers[2]{};
  buffers[0].data = header.data();
  buffers[0].dataLength = header.size();
  buffers[1].data = const_cast<void*>(payload);
  buffers[1].dataLength = size;
  const bool sent = enet_socket_send(socket, &address, buffers, 2) >= 0;
  if (sent)
  {
    std::lock_guard lk(s_rollback_mutex);
    ++s_rollback_stats.sent;
  }
  return sent;
}

std::vector<RollbackDatagram> DrainRollbackDatagrams()
{
  std::lock_guard lk(s_rollback_mutex);
  std::vector<RollbackDatagram> result;
  result.reserve(s_rollback_datagrams.size());
  while (!s_rollback_datagrams.empty())
  {
    result.emplace_back(std::move(s_rollback_datagrams.front()));
    s_rollback_datagrams.pop_front();
  }
  return result;
}

RollbackDatagramStats GetRollbackDatagramStats()
{
  std::lock_guard lk(s_rollback_mutex);
  return s_rollback_stats;
}

bool InterceptRollbackDatagram(ENetHost* host, ENetEvent* event)
{
  if (host->receivedDataLength < ROLLBACK_HEADER_SIZE ||
      std::memcmp(host->receivedData, ROLLBACK_MAGIC.data(), ROLLBACK_MAGIC.size()) != 0)
  {
    return false;
  }

  const auto* data = static_cast<const u8*>(host->receivedData);
  {
    std::lock_guard lk(s_rollback_mutex);
    if (host == s_rollback_host && s_rollback_active && data[4] == ROLLBACK_VERSION &&
        ReadSessionId(data) == s_rollback_session_id)
    {
      RollbackDatagram datagram;
      datagram.address = host->receivedAddress;
      datagram.payload.assign(data + ROLLBACK_HEADER_SIZE, data + host->receivedDataLength);
      s_rollback_datagrams.emplace_back(std::move(datagram));
      ++s_rollback_stats.received;
    }
    else
      ++s_rollback_stats.rejected;
  }

  // Always consume our envelope, including stale sessions, so ENet never parses it as ENet data.
  event->type = static_cast<ENetEventType>(SKIPPABLE_EVENT);
  return true;
}

bool SendPacket(ENetPeer* socket, const sf::Packet& packet, u8 channel_id)
{
  if (!socket)
  {
    ERROR_LOG_FMT(NETPLAY, "Target socket is null.");
    return false;
  }

  ENetPacket* epac =
      enet_packet_create(packet.getData(), packet.getDataSize(), ENET_PACKET_FLAG_RELIABLE);
  if (!epac)
  {
    ERROR_LOG_FMT(NETPLAY, "Failed to create ENetPacket ({} bytes).", packet.getDataSize());
    return false;
  }

  const int result = enet_peer_send(socket, channel_id, epac);
  if (result != 0)
  {
    ERROR_LOG_FMT(NETPLAY, "Failed to send ENetPacket (error code {}).", result);
    return false;
  }

  return true;
}
}  // namespace Common::ENet
