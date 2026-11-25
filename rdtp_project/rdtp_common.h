#pragma once
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <random>
#include <ranges>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <vector>

namespace rdtp
{

constexpr uint32_t RDTP_NAME = 0x52445450;
constexpr uint16_t VERSION = 1;
constexpr size_t PAYLOAD_SIZE = 1024;
constexpr int MAX_DATAGRAM = 1200;

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

enum class PacketType : uint8_t
{
	Data = 1,
	Ack = 2,
	Fin = 3,
	FinAck = 4
};

enum class Algorithm
{
	GBN,
	SR
};

#pragma pack(push, 1)
struct PacketHeader
{
	uint32_t rdtpName;
	uint16_t version;
	uint8_t type;
	uint8_t reserved;
	uint32_t transferId;
	uint32_t sequence;
	uint32_t totalPackets;
	uint16_t payloadLength;
	uint16_t reserved2;
	uint32_t checksum;
};
#pragma pack(pop)

struct Packet
{
	PacketHeader header{};
	std::vector<uint8_t> payload;
};

inline uint32_t Checksum(const uint8_t* data, const size_t size)
{
	uint32_t hash = 2166136261u;
	for (size_t i = 0; i < size; ++i)
	{
		hash ^= data[i];
		hash *= 16777619u;
	}
	return hash;
}

inline std::vector<uint8_t> Serialize(Packet packet)
{
	packet.header.rdtpName = htonl(RDTP_NAME);
	packet.header.version = htons(VERSION);
	packet.header.payloadLength = htons(static_cast<uint16_t>(packet.payload.size()));
	packet.header.checksum = 0;

	std::vector<uint8_t> bytes(sizeof(PacketHeader) + packet.payload.size());
	std::memcpy(bytes.data(), &packet.header, sizeof(PacketHeader));
	if (!packet.payload.empty())
	{
		std::ranges::copy(packet.payload, bytes.begin() + sizeof(PacketHeader));
	}
	const uint32_t sum = htonl(Checksum(bytes.data(), bytes.size()));
	std::memcpy(bytes.data() + offsetof(PacketHeader, checksum), &sum, sizeof(sum));
	return bytes;
}

inline bool Deserialize(const uint8_t* data, const size_t size, Packet& packet)
{
	if (size < sizeof(PacketHeader))
	{
		return false;
	}
	PacketHeader raw{};
	std::memcpy(&raw, data, sizeof(raw));
	if (ntohl(raw.rdtpName) != RDTP_NAME || ntohs(raw.version) != VERSION)
	{
		return false;
	}

	uint16_t length = ntohs(raw.payloadLength);
	if (length > PAYLOAD_SIZE || size != sizeof(PacketHeader) + length)
	{
		return false;
	}
	std::vector copy(data, data + size);
	uint32_t received = 0;
	std::memcpy(&received, copy.data() + offsetof(PacketHeader, checksum), sizeof(received));
	std::memset(copy.data() + offsetof(PacketHeader, checksum), 0, sizeof(uint32_t));
	if (ntohl(received) != Checksum(copy.data(), copy.size()))
	{
		return false;
	}
	packet.header = raw;
	packet.header.transferId = ntohl(raw.transferId);
	packet.header.sequence = ntohl(raw.sequence);
	packet.header.totalPackets = ntohl(raw.totalPackets);
	packet.header.payloadLength = length;
	packet.header.checksum = ntohl(received);
	packet.payload.assign(data + sizeof(PacketHeader), data + size);
	return true;
}

inline Packet MakePacket(
	PacketType type,
	const uint32_t transferId,
	const uint32_t sequence,
	const uint32_t total,
	const std::vector<uint8_t>& payload = {})
{
	Packet packet;
	packet.header.type = static_cast<uint8_t>(type);
	packet.header.transferId = htonl(transferId);
	packet.header.sequence = htonl(sequence);
	packet.header.totalPackets = htonl(total);
	packet.payload = payload;
	return packet;
}

class Logger
{
public:
	explicit Logger(bool debug = false)
		: m_debug(debug)
	{
	}
	void Info(const std::string& message)
	{
		std::cout << "[INFO] " << message << '\n';
	}
	void Debug(const std::string& message) const
	{
		if (m_debug)
		{
			std::cout << "[DEBUG] " << message << '\n';
		}
	}
	void Error(const std::string& message) const
	{
		std::cerr << "[ERROR] " << message << '\n';
	}

private:
	bool m_debug;
};

struct NetworkConfig
{
	double loss = 0.0;
	int delayMs = 0;
	int jitterMs = 0;
};

class UnreliableChannel
{
public:
	explicit UnreliableChannel(const NetworkConfig config)
		: m_config(config)
		, m_rng(std::random_device{}())
		, m_loss(0.0, 1.0)
		, m_delay(-config.jitterMs, config.jitterMs)
	{
	}
	bool ShouldDrop()
	{
		return m_loss(m_rng) < m_config.loss;
	}
	int Delay()
	{
		return std::max(0, m_config.delayMs + (m_config.jitterMs ? m_delay(m_rng) : 0));
	}

private:
	NetworkConfig m_config;
	std::mt19937 m_rng;
	std::uniform_real_distribution<double> m_loss;
	std::uniform_int_distribution<int> m_delay;
};

inline bool SendPacket(int socketFd, const sockaddr_in& address, const Packet& packet, UnreliableChannel* channel, const Logger& logger, const std::string& label)
{
	if (channel && channel->ShouldDrop())
	{
		logger.Debug("Lose simulation: " + label);
		return true;
	}
	if (channel)
	{
		int delay = channel->Delay();
		if (delay > 0)
		{
			std::this_thread::sleep_for(Milliseconds(delay));
		}
	}
	const auto bytes = Serialize(packet);
	ssize_t sent = sendto(socketFd, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
	return sent == static_cast<ssize_t>(bytes.size());
}

inline bool ReceivePacket(const int socketFd, Packet& packet, sockaddr_in& address, int timeoutMs)
{
	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET(socketFd, &readSet);
	timeval timeout{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
	int ready = select(socketFd + 1, &readSet, nullptr, nullptr, &timeout);
	if (ready <= 0)
	{
		return false;
	}
	uint8_t buffer[MAX_DATAGRAM];
	socklen_t addressLength = sizeof(address);
	const ssize_t received = recvfrom(socketFd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&address), &addressLength);
	return received > 0
		&& Deserialize(buffer, static_cast<size_t>(received), packet);
}

inline std::string FormatBytes(uint64_t bytes)
{
	std::ostringstream stream;
	stream << std::fixed << std::setprecision(2);
	if (bytes >= 1024ull * 1024ull)
	{
		stream << bytes / 1048576.0 << " MiB";
	}
	else if (bytes >= 1024ull)
	{
		stream << bytes / 1024.0 << " KiB";
	}
	else
	{
		stream << bytes << " B";
	}
	return stream.str();
}

} // namespace rdtp
