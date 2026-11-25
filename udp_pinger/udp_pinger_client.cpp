#include "udp_protocol.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

class UdpPingerClient
{
public:
	UdpPingerClient(std::string host, int port, int timeoutMilliseconds)
		: m_host(std::move(host))
		, m_port(port)
		, m_timeoutMilliseconds(timeoutMilliseconds)
		, m_socket(-1)
		, m_sentCount(0)
		, m_receivedCount(0)
	{
	}

	~UdpPingerClient()
	{
		CloseSocket();
	}

	bool RunPing(int packetCount)
	{
		if (!OpenSocket())
		{
			return false;
		}

		for (int sequenceNumber = 1; sequenceNumber <= packetCount; ++sequenceNumber)
		{
			SendAndReceive(UdpProtocol::MessageType::Ping, sequenceNumber);
		}

		PrintStatistics();
		return true;
	}

	bool RunHeartbeat(int durationSeconds, int intervalMilliseconds)
	{
		if (!OpenSocket())
		{
			return false;
		}

		std::cout << "Heartbeat started for " << durationSeconds << " seconds\n";
		const auto finishTime = std::chrono::steady_clock::now() + std::chrono::seconds(durationSeconds);
		int sequenceNumber = 1;

		while (std::chrono::steady_clock::now() < finishTime)
		{
			SendAndReceive(UdpProtocol::MessageType::Heartbeat, sequenceNumber++);
			const auto nextSendTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(intervalMilliseconds);
			while (std::chrono::steady_clock::now() < nextSendTime && std::chrono::steady_clock::now() < finishTime)
			{
				usleep(10000);
			}
		}

		PrintStatistics();
		return true;
	}

private:
	struct Statistics
	{
		double minimumRtt = 0.0;
		double maximumRtt = 0.0;
		double totalRtt = 0.0;
		int receivedCount = 0;
	};

	std::string m_host;
	int m_port;
	int m_timeoutMilliseconds;
	int m_socket;
	sockaddr_in m_serverAddress{};
	int m_sentCount;
	int m_receivedCount;
	Statistics m_statistics;

	bool OpenSocket()
	{
		addrinfo hints{};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;

		addrinfo* addressInfo = nullptr;
		const std::string portString = std::to_string(m_port);
		const int resolveResult = getaddrinfo(m_host.c_str(), portString.c_str(), &hints, &addressInfo);
		if (resolveResult != 0)
		{
			std::cerr << "Host resolution failed: " << gai_strerror(resolveResult) << '\n';
			return false;
		}

		m_serverAddress = *reinterpret_cast<sockaddr_in*>(addressInfo->ai_addr);
		freeaddrinfo(addressInfo);

		m_socket = socket(AF_INET, SOCK_DGRAM, 0);
		if (m_socket < 0)
		{
			std::cerr << "socket() failed: " << std::strerror(errno) << '\n';
			return false;
		}

		timeval socketTimeout{};
		socketTimeout.tv_sec = m_timeoutMilliseconds / 1000;
		socketTimeout.tv_usec = (m_timeoutMilliseconds % 1000) * 1000;
		if (setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &socketTimeout, sizeof(socketTimeout)) < 0)
		{
			std::cerr << "setsockopt(SO_RCVTIMEO) failed: " << std::strerror(errno) << '\n';
			CloseSocket();
			return false;
		}
		return true;
	}

	void SendAndReceive(UdpProtocol::MessageType type, int sequenceNumber)
	{
		const std::int64_t timestamp = UdpProtocol::GetTimestampMilliseconds();
		const UdpProtocol::Message message{ type, sequenceNumber, timestamp };
		const std::string request = UdpProtocol::Serialize(message);
		const auto startTime = std::chrono::steady_clock::now();

		const ssize_t sentBytes = sendto(
			m_socket,
			request.data(),
			request.size(),
			0,
			reinterpret_cast<sockaddr*>(&m_serverAddress),
			sizeof(m_serverAddress));

		if (sentBytes < 0)
		{
			std::cerr << "sendto() failed for packet " << sequenceNumber << ": " << std::strerror(errno) << '\n';
			return;
		}
		++m_sentCount;

		const auto deadline = startTime + std::chrono::milliseconds(m_timeoutMilliseconds);
		while (true)
		{
			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline)
			{
				PrintTimeout(type, sequenceNumber);
				return;
			}

			const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
			timeval waitTime{};
			waitTime.tv_sec = static_cast<time_t>(remaining.count() / 1000000);
			waitTime.tv_usec = static_cast<suseconds_t>(remaining.count() % 1000000);

			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(m_socket, &readSet);
			const int ready = select(m_socket + 1, &readSet, nullptr, nullptr, &waitTime);
			if (ready < 0)
			{
				if (errno == EINTR)
				{
					continue;
				}
				std::cerr << "select() failed: " << std::strerror(errno) << '\n';
				return;
			}
			if (ready == 0)
			{
				PrintTimeout(type, sequenceNumber);
				return;
			}

			char buffer[512]{};
			sockaddr_in sourceAddress{};
			socklen_t sourceLength = sizeof(sourceAddress);
			const ssize_t receivedBytes = recvfrom(
				m_socket,
				buffer,
				sizeof(buffer) - 1,
				0,
				reinterpret_cast<sockaddr*>(&sourceAddress),
				&sourceLength);
			if (receivedBytes < 0)
			{
				if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				{
					continue;
				}
				std::cerr << "recvfrom() failed: " << std::strerror(errno) << '\n';
				return;
			}

			if (sourceAddress.sin_addr.s_addr != m_serverAddress.sin_addr.s_addr || sourceAddress.sin_port != m_serverAddress.sin_port)
			{
				continue;
			}

			UdpProtocol::Message response;
			if (!UdpProtocol::Parse(std::string(buffer, static_cast<std::size_t>(receivedBytes)), response))
			{
				continue;
			}
			if (response.type != type || response.sequenceNumber != sequenceNumber || response.timestamp != timestamp)
			{
				continue;
			}

			const double rttMilliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - startTime)
											   .count();
			++m_receivedCount;
			UpdateStatistics(rttMilliseconds);
			PrintResponse(response, rttMilliseconds);
			return;
		}
	}

	void UpdateStatistics(double rttMilliseconds)
	{
		if (m_statistics.receivedCount == 0)
		{
			m_statistics.minimumRtt = rttMilliseconds;
			m_statistics.maximumRtt = rttMilliseconds;
		}
		else
		{
			m_statistics.minimumRtt = std::min(m_statistics.minimumRtt, rttMilliseconds);
			m_statistics.maximumRtt = std::max(m_statistics.maximumRtt, rttMilliseconds);
		}
		m_statistics.totalRtt += rttMilliseconds;
		++m_statistics.receivedCount;
	}

	static void PrintResponse(const UdpProtocol::Message& message, double rttMilliseconds)
	{
		std::cout << "Ответ от сервера: " << UdpProtocol::Serialize(message)
				  << ", RTT = " << std::fixed << std::setprecision(6) << rttMilliseconds / 1000.0
				  << " сек (" << std::setprecision(3) << rttMilliseconds << " мс)\n";
	}

	static void PrintTimeout(UdpProtocol::MessageType type, int sequenceNumber)
	{
		std::cout << UdpProtocol::GetMessageName(type) << " " << sequenceNumber << ": Request timed out\n";
	}

	void PrintStatistics() const
	{
		const int lostCount = m_sentCount - m_receivedCount;
		const double lossPercent = m_sentCount == 0 ? 0.0 : 100.0 * lostCount / m_sentCount;

		std::cout << "\n--- Статистика ping ---\n";
		std::cout << "Отправлено: " << m_sentCount
				  << ", Получено: " << m_receivedCount
				  << ", Потеряно: " << lostCount
				  << " (" << std::fixed << std::setprecision(1) << lossPercent << "%)\n";

		if (m_statistics.receivedCount > 0)
		{
			const double averageRtt = m_statistics.totalRtt / m_statistics.receivedCount;
			std::cout << "RTT: мин = " << std::setprecision(6) << m_statistics.minimumRtt / 1000.0
					  << "с, макс = " << m_statistics.maximumRtt / 1000.0
					  << "с, средн = " << averageRtt / 1000.0 << "с\n";
		}
		else
		{
			std::cout << "RTT: нет полученных ответов\n";
		}
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

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		std::cerr << "Usage:\n"
				  << "  udp_pinger_client <host> [port] [count] [timeout_ms]\n"
				  << "  udp_pinger_client <host> <port> --heartbeat [duration_seconds] [interval_ms] [timeout_ms]\n";
		return 1;
	}

	try
	{
		const std::string host = argv[1];
		const int port = argc > 2 ? std::stoi(argv[2]) : 12000;
		if (port < 1 || port > 65535)
		{
			std::cerr << "Port must be between 1 and 65535\n";
			return 1;
		}

		if (argc > 3 && std::string(argv[3]) == "--heartbeat")
		{
			const int durationSeconds = argc > 4 ? std::stoi(argv[4]) : 20;
			const int intervalMilliseconds = argc > 5 ? std::stoi(argv[5]) : 1000;
			const int timeoutMilliseconds = argc > 6 ? std::stoi(argv[6]) : 1000;
			if (durationSeconds < 1 || intervalMilliseconds < 1 || timeoutMilliseconds < 1)
			{
				std::cerr << "Heartbeat duration, interval and timeout must be positive\n";
				return 1;
			}
			UdpPingerClient client(host, port, timeoutMilliseconds);
			return client.RunHeartbeat(durationSeconds, intervalMilliseconds) ? 0 : 1;
		}

		const int packetCount = argc > 3 ? std::stoi(argv[3]) : 10;
		const int timeoutMilliseconds = argc > 4 ? std::stoi(argv[4]) : 1000;
		if (packetCount < 1 || timeoutMilliseconds < 1)
		{
			std::cerr << "Packet count and timeout must be positive\n";
			return 1;
		}

		UdpPingerClient client(host, port, timeoutMilliseconds);
		return client.RunPing(packetCount) ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Client error: " << error.what() << '\n';
		return 1;
	}
}
