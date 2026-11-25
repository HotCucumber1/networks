#include "udp_protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

class UdpPingerServer
{
public:
	UdpPingerServer(int port, int dropPercent, int heartbeatTimeoutSeconds)
		: m_port(port)
		, m_dropPercent(dropPercent)
		, m_heartbeatTimeoutSeconds(heartbeatTimeoutSeconds)
		, m_socket(-1)
	{
	}

	~UdpPingerServer()
	{
		CloseSocket();
	}

	bool Run()
	{
		if (!OpenSocket())
		{
			return false;
		}

		std::cout << "UDP server started on port " << m_port << '\n';
		std::cout << "Simulated packet loss: " << m_dropPercent << "%\n";
		std::cout << "Heartbeat timeout: " << m_heartbeatTimeoutSeconds << " sec\n";

		while (true)
		{
			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(m_socket, &readSet);

			timeval waitTime{};
			waitTime.tv_sec = 0;
			waitTime.tv_usec = 200000;

			const int ready = select(m_socket + 1, &readSet, nullptr, nullptr, &waitTime);
			if (ready < 0)
			{
				if (errno == EINTR)
				{
					CheckHeartbeats();
					continue;
				}
				std::cerr << "select() failed: " << std::strerror(errno) << '\n';
				return false;
			}

			if (ready > 0 && FD_ISSET(m_socket, &readSet))
			{
				HandleDatagram();
			}
			CheckHeartbeats();
		}
	}

private:
	struct ClientState
	{
		std::chrono::steady_clock::time_point lastSeen;
		bool isAlive = true;
	};

	int m_port;
	int m_dropPercent;
	int m_heartbeatTimeoutSeconds;
	int m_socket;
	std::map<std::string, ClientState> m_clients;

	bool OpenSocket()
	{
		m_socket = socket(AF_INET, SOCK_DGRAM, 0);
		if (m_socket < 0)
		{
			std::cerr << "socket() failed: " << std::strerror(errno) << '\n';
			return false;
		}

		int reuseAddress = 1;
		if (setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress)) < 0)
		{
			std::cerr << "setsockopt() failed: " << std::strerror(errno) << '\n';
			CloseSocket();
			return false;
		}

		sockaddr_in serverAddress{};
		serverAddress.sin_family = AF_INET;
		serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
		serverAddress.sin_port = htons(static_cast<uint16_t>(m_port));

		if (bind(m_socket, reinterpret_cast<sockaddr*>(&serverAddress), sizeof(serverAddress)) < 0)
		{
			std::cerr << "bind() failed: " << std::strerror(errno) << '\n';
			CloseSocket();
			return false;
		}
		return true;
	}

	void HandleDatagram()
	{
		char buffer[512]{};
		sockaddr_in clientAddress{};
		socklen_t addressLength = sizeof(clientAddress);
		const ssize_t receivedBytes = recvfrom(
			m_socket,
			buffer,
			sizeof(buffer) - 1,
			0,
			reinterpret_cast<sockaddr*>(&clientAddress),
			&addressLength);

		if (receivedBytes < 0)
		{
			if (errno != EINTR)
			{
				std::cerr << "recvfrom() failed: " << std::strerror(errno) << '\n';
			}
			return;
		}

		const std::string data(buffer, static_cast<std::size_t>(receivedBytes));
		UdpProtocol::Message message;
		if (!UdpProtocol::Parse(data, message))
		{
			std::cerr << "Invalid packet received\n";
			return;
		}

		const std::string clientKey = GetClientKey(clientAddress);
		ClientState& client = m_clients[clientKey];
		const bool wasAlive = client.isAlive;
		client.lastSeen = std::chrono::steady_clock::now();
		client.isAlive = true;
		if (!wasAlive)
		{
			std::cout << "Client " << clientKey << " is alive again\n";
		}

		if (message.type == UdpProtocol::MessageType::Ping && ShouldDrop(message.sequenceNumber))
		{
			std::cout << "Simulated packet loss: " << data << " from " << clientKey << '\n';
			return;
		}

		const std::string response = UdpProtocol::Serialize(message);
		const ssize_t sentBytes = sendto(
			m_socket,
			response.data(),
			response.size(),
			0,
			reinterpret_cast<sockaddr*>(&clientAddress),
			addressLength);

		if (sentBytes < 0)
		{
			std::cerr << "sendto() failed: " << std::strerror(errno) << '\n';
			return;
		}

		std::cout << "Received: " << data << " from " << clientKey << '\n';
	}

	void CheckHeartbeats()
	{
		const auto now = std::chrono::steady_clock::now();
		for (auto& [clientKey, client] : m_clients)
		{
			const auto silence = std::chrono::duration_cast<std::chrono::seconds>(now - client.lastSeen).count();
			if (client.isAlive && silence >= m_heartbeatTimeoutSeconds)
			{
				client.isAlive = false;
				std::cout << "Client " << clientKey << " is DEAD (no heartbeat for " << silence << " sec)\n";
			}
		}

		std::erase_if(m_clients, [&](const auto& entry) {
			const auto& client = entry.second;
			const auto silence = std::chrono::duration_cast<std::chrono::seconds>(now - client.lastSeen).count();
			return !client.isAlive && silence >= 3 * m_heartbeatTimeoutSeconds;
		});
	}

	bool ShouldDrop(int sequenceNumber) const
	{
		if (m_dropPercent <= 0)
		{
			return false;
		}
		const int sequencePosition = (sequenceNumber - 1) % 10;
		return sequencePosition < (m_dropPercent / 10);
	}

	static std::string GetClientKey(const sockaddr_in& address)
	{
		char ipAddress[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &address.sin_addr, ipAddress, sizeof(ipAddress));
		return std::string(ipAddress) + ":" + std::to_string(ntohs(address.sin_port));
	}

	void CloseSocket()
	{
		if (m_socket >= 0)
		{
			close(m_socket);
			m_socket = -1;
		}
	}
};

std::optional<int> ParseInt(const char* text)
{
	try
	{
		std::size_t consumed = 0;
		const int value = std::stoi(text, &consumed);
		if (consumed != std::string(text).size())
		{
			return std::nullopt;
		}
		return value;
	}
	catch (const std::exception&)
	{
		return std::nullopt;
	}
}

int main(int argc, char* argv[])
{
	const std::optional<int> port = argc > 1 ? ParseInt(argv[1]) : 12000;
	const std::optional<int> dropPercent = argc > 2 ? ParseInt(argv[2]) : 0;
	const std::optional<int> heartbeatTimeout = argc > 3 ? ParseInt(argv[3]) : 5;

	if (!port || !dropPercent || !heartbeatTimeout || *port < 1 || *port > 65535 || *dropPercent < 0
		|| *dropPercent > 100 || *dropPercent % 10 != 0 || *heartbeatTimeout < 1)
	{
		std::cerr << "Usage: udp_pinger_server [port:1..65535] [drop_percent:0,10,...,100] [heartbeat_timeout_seconds]\n";
		return 1;
	}

	UdpPingerServer server(*port, *dropPercent, *heartbeatTimeout);
	return server.Run() ? 0 : 1;
}
