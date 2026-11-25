#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <sys/time.h>

namespace UdpProtocol
{

enum class MessageType
{
	Ping,
	Heartbeat,
	Invalid
};

struct Message
{
	MessageType type = MessageType::Invalid;
	int sequenceNumber = 0;
	std::int64_t timestamp = 0;
};

inline std::int64_t GetTimestampMilliseconds()
{
	timeval currentTime{};
	gettimeofday(&currentTime, nullptr);
	return static_cast<std::int64_t>(currentTime.tv_sec) * 1000 + currentTime.tv_usec / 1000;
}

inline std::string GetMessageName(MessageType type)
{
	if (type == MessageType::Ping)
	{
		return "PING";
	}
	if (type == MessageType::Heartbeat)
	{
		return "HEARTBEAT";
	}
	return "INVALID";
}

inline std::string Serialize(const Message& message)
{
	return GetMessageName(message.type) + " " + std::to_string(message.sequenceNumber) + " " + std::to_string(message.timestamp);
}

inline bool Parse(const std::string& data, Message& message)
{
	std::istringstream input(data);
	std::string type;
	std::string extra;
	if (!(input >> type >> message.sequenceNumber >> message.timestamp) || (input >> extra))
	{
		return false;
	}
	if (type == "PING")
	{
		message.type = MessageType::Ping;
	}
	else if (type == "HEARTBEAT")
	{
		message.type = MessageType::Heartbeat;
	}
	else
	{
		return false;
	}
	return message.sequenceNumber > 0 && message.timestamp >= 0;
}

} // namespace UdpProtocol
