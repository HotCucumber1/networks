#include "rdtp_common.h"
#include <map>
#include <optional>

using namespace rdtp;

struct ReceiverOptions
{
	int port = 0;
	std::string output;
	Algorithm algorithm = Algorithm::SR;
	bool debug = false;
	double loss = 0.0;
	int delay = 0;
	int jitter = 0;
};

static void ShowUsage()
{
	std::cout << "Usage: ./rdt_receiver <port> <received_file> [-alg GBN|SR] [-d] [-loss 0.25] [-delay 20] [-jitter 5]\n";
}

static bool ParseArgumentsImpl(int argc, char** argv, ReceiverOptions& options)
{
	if (argc < 3)
	{
		return false;
	}
	options.port = std::stoi(argv[1]);
	options.output = argv[2];
	for (int i = 3; i < argc; ++i)
	{
		std::string arg = argv[i];
		if (arg == "-d")
		{
			options.debug = true;
		}
		else if (i + 1 < argc)
		{
			std::string value = argv[++i];
			if (arg == "-alg")
			{
				if (value == "GBN")
				{
					options.algorithm = Algorithm::GBN;
				}
				else if (value == "SR")
				{
					options.algorithm = Algorithm::SR;
				}
				else
				{
					return false;
				}
			}
			else if (arg == "-loss")
			{
				options.loss = std::stod(value);
			}
			else if (arg == "-delay")
			{
				options.delay = std::stoi(value);
			}
			else if (arg == "-jitter")
			{
				options.jitter = std::stoi(value);
			}
			else
			{
				return false;
			}
		}
		else
		{
			return false;
		}
	}
	return options.port > 0 && options.port <= 65535 && options.loss >= 0.0 && options.loss < 1.0;
}

class ReceiverApplication
{
public:
	~ReceiverApplication()
	{
		CloseResources();
	}

	int Run(int argc, char** argv)
	{
		if (!ParseArguments(argc, argv))
		{
			ShowUsage();
			return 1;
		}
		m_logger = Logger(m_options.debug);
		if (!InitializeSocket())
		{
			return 1;
		}
		if (!BindSocket())
		{
			return 1;
		}
		if (!OpenOutputFile())
		{
			return 1;
		}
		InitializeChannel();
		m_logger.Info("Receiver started on UDP port " + std::to_string(m_options.port) + ", algorithm: " + (m_options.algorithm == Algorithm::GBN ? "GBN" : "SR"));
		ReceiveLoop();
		return 0;
	}

private:
	bool ParseArguments(int argc, char** argv)
	{
		return ParseArgumentsImpl(argc, argv, m_options);
	}

	bool InitializeSocket()
	{
		m_socketFd = socket(AF_INET, SOCK_DGRAM, 0);
		if (m_socketFd < 0)
		{
			m_logger.Error("socket() failed");
			return false;
		}
		return true;
	}

	bool BindSocket()
	{
		m_local = {};
		m_local.sin_family = AF_INET;
		m_local.sin_addr.s_addr = htonl(INADDR_ANY);
		m_local.sin_port = htons(static_cast<uint16_t>(m_options.port));
		if (bind(m_socketFd, reinterpret_cast<sockaddr*>(&m_local), sizeof(m_local)) < 0)
		{
			m_logger.Error("bind() failed: " + std::string(std::strerror(errno)));
			return false;
		}
		return true;
	}

	bool OpenOutputFile()
	{
		m_output.open(m_options.output, std::ios::binary | std::ios::trunc);
		if (!m_output)
		{
			m_logger.Error("Failed to create output file");
			return false;
		}
		return true;
	}

	void InitializeChannel()
	{
		m_channel.emplace(NetworkConfig{ m_options.loss, m_options.delay, m_options.jitter });
	}

	UnreliableChannel* GetChannel()
	{
		return m_channel ? &*m_channel : nullptr;
	}

	void ReceiveLoop()
	{
		while (true)
		{
			Packet packet;
			sockaddr_in sender{};
			if (!ReceivePacket(m_socketFd, packet, sender, 1000))
				continue;
			if (packet.header.type == static_cast<uint8_t>(PacketType::Data))
			{
				HandleData(sender, packet);
			}
			else if (packet.header.type == static_cast<uint8_t>(PacketType::Fin))
			{
				if (HandleFin(sender, packet))
					break;
			}
		}
	}

	void HandleData(const sockaddr_in& sender, const Packet& packet)
	{
		if (GetChannel() && GetChannel()->ShouldDrop())
		{
			m_logger.Debug("Simulated loss of incoming DATA");
			return;
		}
		StartTransferIfNeeded(packet);
		if (packet.header.transferId != m_transferId || packet.header.totalPackets != m_totalPackets)
		{
			++m_invalidPackets;
			return;
		}
		const uint32_t sequence = packet.header.sequence;
		const bool accepted = m_options.algorithm == Algorithm::GBN
			? AcceptSegmentGbn(packet, sequence)
			: AcceptSegmentSr(packet, sequence);
		if (accepted)
			m_logger.Debug("Accepted DATA #" + std::to_string(sequence) + ", next expected=" + std::to_string(m_expectedSequence));
		else
			m_logger.Debug("Rejected/duplicated DATA #" + std::to_string(sequence));
		SendAck(sender, sequence);
	}

	void StartTransferIfNeeded(const Packet& packet)
	{
		if (m_transferStarted)
			return;
		m_transferStarted = true;
		m_transferId = packet.header.transferId;
		m_totalPackets = packet.header.totalPackets;
		m_logger.Info("Transfer started, total packets: " + std::to_string(m_totalPackets));
	}

	bool AcceptSegmentGbn(const Packet& packet, uint32_t sequence)
	{
		if (sequence == m_expectedSequence)
		{
			WritePayload(packet.payload);
			++m_expectedSequence;
			++m_acceptedPackets;
			return true;
		}
		if (sequence < m_expectedSequence)
			++m_duplicatePackets;
		return false;
	}

	bool AcceptSegmentSr(const Packet& packet, uint32_t sequence)
	{
		if (sequence < m_expectedSequence)
		{
			++m_duplicatePackets;
			return false;
		}
		if (sequence >= m_expectedSequence + 4096)
			return false;
		if (sequence == m_expectedSequence)
		{
			WritePayload(packet.payload);
			++m_expectedSequence;
			++m_acceptedPackets;
			FlushBufferedSegments();
			return true;
		}
		if (!m_buffered.count(sequence))
		{
			m_buffered.emplace(sequence, packet.payload);
			++m_acceptedPackets;
			return true;
		}
		++m_duplicatePackets;
		return false;
	}

	void FlushBufferedSegments()
	{
		while (m_buffered.count(m_expectedSequence))
		{
			const auto& data = m_buffered[m_expectedSequence];
			WritePayload(data);
			m_buffered.erase(m_expectedSequence);
			++m_expectedSequence;
			++m_acceptedPackets;
		}
	}

	void WritePayload(const std::vector<uint8_t>& payload)
	{
		m_output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
	}

	void SendAck(const sockaddr_in& sender, uint32_t sequence)
	{
		const uint32_t ackNumber = m_options.algorithm == Algorithm::GBN
			? (m_expectedSequence == 0 ? UINT32_MAX : m_expectedSequence - 1)
			: sequence;
		Packet ack = MakePacket(PacketType::Ack, m_transferId, ackNumber, m_totalPackets);
		SendPacket(m_socketFd, sender, ack, GetChannel(), m_logger, "ACK #" + std::to_string(ackNumber));
	}

	bool HandleFin(const sockaddr_in& sender, const Packet& packet)
	{
		if (!m_transferStarted && packet.header.totalPackets == 0)
		{
			m_transferStarted = true;
			m_transferId = packet.header.transferId;
			m_totalPackets = 0;
		}
		if (!m_transferStarted || packet.header.transferId != m_transferId)
			return false;
		if (m_expectedSequence != m_totalPackets)
		{
			m_logger.Debug("Premature FIN received; expecting packet #" + std::to_string(m_expectedSequence));
			return false;
		}
		m_output.flush();
		Packet response = MakePacket(PacketType::FinAck, m_transferId, m_totalPackets, m_totalPackets);
		SendPacket(m_socketFd, sender, response, GetChannel(), m_logger, "FIN-ACK");
		m_logger.Info("File assembled successfully: " + m_options.output);
		m_logger.Info("Unique packets accepted: " + std::to_string(m_acceptedPackets) + ", duplicates: " + std::to_string(m_duplicatePackets) + ", invalid: " + std::to_string(m_invalidPackets));
		return true;
	}

	void CloseResources()
	{
		if (m_output.is_open())
		{
			m_output.close();
		}
		if (m_socketFd >= 0)
		{
			close(m_socketFd);
			m_socketFd = -1;
		}
	}

	ReceiverOptions m_options;
	Logger m_logger;
	int m_socketFd = -1;
	sockaddr_in m_local{};
	std::ofstream m_output;
	std::optional<UnreliableChannel> m_channel;
	std::map<uint32_t, std::vector<uint8_t>> m_buffered;
	uint32_t m_expectedSequence = 0;
	uint32_t m_transferId = 0;
	uint32_t m_totalPackets = 0;
	uint64_t m_acceptedPackets = 0;
	uint64_t m_duplicatePackets = 0;
	uint64_t m_invalidPackets = 0;
	bool m_transferStarted = false;
};

int main(int argc, char** argv)
{
	ReceiverApplication application;
	return application.Run(argc, argv);
}
