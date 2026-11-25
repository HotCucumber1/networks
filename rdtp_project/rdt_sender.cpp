#include "rdtp_common.h"
#include <limits>
#include <map>
#include <optional>

using namespace rdtp;

struct SenderOptions
{
	std::string host;
	int port = 0;
	std::string file;
	Algorithm algorithm = Algorithm::SR;
	bool debug = false;
	bool adaptive = true;
	double loss = 0.0;
	int delay = 0;
	int jitter = 0;
	int window = 32;
	int timeout = 500;
};

static void ShowUsage()
{
	std::cout << "Usage: ./rdt_sender <receiver_host> <receiver_port> <file> [-alg GBN|SR] [-d] [-loss 0.25] [-delay 20] [-jitter 5] [-window 32] [-timeout 500] [-no-adaptive]\n";
}

static bool ParseArgumentsImpl(int argc, char** argv, SenderOptions& options)
{
	if (argc < 4)
	{
		return false;
	}
	options.host = argv[1];
	options.port = std::stoi(argv[2]);
	options.file = argv[3];

	for (int i = 4; i < argc; ++i)
	{
		std::string arg = argv[i];
		if (arg == "-d")
		{
			options.debug = true;
		}
		else if (arg == "-no-adaptive")
		{
			options.adaptive = false;
		}
		else if (arg == "-alg" && i + 1 < argc)
		{
			std::string value = argv[++i];
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
		else if (i + 1 < argc)
		{
			std::string value = argv[++i];
			if (arg == "-loss")
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
			else if (arg == "-window")
			{
				options.window = std::stoi(value);
			}
			else if (arg == "-timeout")
			{
				options.timeout = std::stoi(value);
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
	return options.port > 0
		&& options.port <= 65535
		&& options.loss >= 0.0
		&& options.loss < 1.0
		&& options.window > 0
		&& options.timeout > 0;
}

struct SegmentState
{
	Clock::time_point lastSent{};
	bool acknowledged = false;
	bool retransmitted = false;
};

class SenderApplication
{
public:
	~SenderApplication()
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
		if (!OpenInputFile())
		{
			return 1;
		}
		SplitIntoPayloads();
		if (!InitializeSocket())
		{
			return 1;
		}
		if (!ResolveReceiverAddress())
		{
			return 1;
		}
		InitializeChannel();
		InitializeMetrics();
		if (!TransferLoop())
		{
			return 1;
		}
		if (!FinishTransfer())
		{
			m_logger.Error("Receiver did not confirm the end of the transfer");
			return 2;
		}
		PrintSummary();
		return 0;
	}

private:
	bool ParseArguments(const int argc, char** argv)
	{
		return ParseArgumentsImpl(argc, argv, m_options);
	}

	bool OpenInputFile()
	{
		m_input.open(m_options.file, std::ios::binary);
		if (!m_input)
		{
			m_logger.Error("Failed to open input file: " + m_options.file);
			return false;
		}
		m_fileData.assign(std::istreambuf_iterator<char>(m_input), std::istreambuf_iterator<char>());
		return true;
	}

	void SplitIntoPayloads()
	{
		m_totalPackets = static_cast<uint32_t>((m_fileData.size() + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE);
		m_payloads.resize(m_totalPackets);
		for (uint32_t i = 0; i < m_totalPackets; ++i)
		{
			const size_t begin = static_cast<size_t>(i) * PAYLOAD_SIZE;
			const size_t count = std::min(PAYLOAD_SIZE, m_fileData.size() - begin);
			m_payloads[i].assign(m_fileData.begin() + begin, m_fileData.begin() + begin + count);
		}
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

	bool ResolveReceiverAddress()
	{
		m_receiver = {};
		m_receiver.sin_family = AF_INET;
		m_receiver.sin_port = htons(static_cast<uint16_t>(m_options.port));
		if (inet_pton(AF_INET, m_options.host.c_str(), &m_receiver.sin_addr) == 1)
		{
			return true;
		}
		hostent* host = gethostbyname(m_options.host.c_str());
		if (!host)
		{
			m_logger.Error("Failed to resolve receiver address");
			return false;
		}
		std::memcpy(&m_receiver.sin_addr, host->h_addr_list[0], host->h_length);
		return true;
	}

	void InitializeChannel()
	{
		m_channel.emplace(NetworkConfig{ m_options.loss, m_options.delay, m_options.jitter });
		m_transferId = static_cast<uint32_t>(Clock::now().time_since_epoch().count());
		m_states.resize(m_totalPackets);
		m_maxWindow = static_cast<double>(m_options.window);
		m_slowStartThreshold = m_maxWindow;
	}

	void InitializeMetrics()
	{
		m_base = 0;
		m_nextSequence = 0;
		m_congestionWindow = 1.0;
		m_transmissions = 0;
		m_retransmissions = 0;
		m_ackReceived = 0;
		m_start = Clock::now();
		m_lastProgress = m_start;
		m_smoothedRtt = 0.0;
		m_rttVariance = 0.0;
		m_currentTimeout = m_options.timeout;
		m_csv.open("rdtp_metrics.csv");
		m_csv << "elapsed_ms,cwnd,throughput_kbps,rtt_ms,base,next_sequence,retransmissions\n";
		m_logger.Info("Transfer started: " + FormatBytes(m_fileData.size()) + ", packets: " + std::to_string(m_totalPackets) + ", algorithm: " + (m_options.algorithm == Algorithm::GBN ? "GBN" : "SR"));
	}

	bool TransferLoop()
	{
		while (m_base < m_totalPackets)
		{
			if (!SendNewPackets())
			{
				return false;
			}
			ProcessIncomingAcks();
			const auto now = Clock::now();
			HandleTimeouts(now);
			ReportProgress(now);
		}
		return true;
	}

	bool SendNewPackets()
	{
		const uint32_t allowedWindow = static_cast<uint32_t>(std::max(1.0, std::min(m_maxWindow, std::floor(m_congestionWindow))));
		while (m_nextSequence < m_totalPackets && m_nextSequence < m_base + allowedWindow)
		{
			if (!SendDataPacket(m_nextSequence, "DATA #" + std::to_string(m_nextSequence)))
			{
				m_logger.Error("sendto() failed while sending DATA");
				return false;
			}
			m_states[m_nextSequence].lastSent = Clock::now();
			++m_transmissions;
			m_logger.Debug("Sent DATA #" + std::to_string(m_nextSequence) + ", window [" + std::to_string(m_base) + ", " + std::to_string(m_base + allowedWindow - 1) + "]");
			++m_nextSequence;
		}
		return true;
	}

	bool SendDataPacket(uint32_t sequence, const std::string& label)
	{
		Packet packet = MakePacket(PacketType::Data, m_transferId, sequence, m_totalPackets, m_payloads[sequence]);
		return SendPacket(m_socketFd, m_receiver, packet, GetChannel(), m_logger, label);
	}

	UnreliableChannel* GetChannel()
	{
		return m_channel
			? &*m_channel
			: nullptr;
	}

	void ProcessIncomingAcks()
	{
		Packet incoming;
		sockaddr_in from{};
		if (!ReceivePacket(m_socketFd, incoming, from, 20))
		{
			return;
		}
		if (incoming.header.transferId != m_transferId || incoming.header.type != static_cast<uint8_t>(PacketType::Ack))
		{
			return;
		}
		++m_ackReceived;
		const uint32_t ack = incoming.header.sequence;
		if (m_options.algorithm == Algorithm::GBN)
		{
			HandleCumulativeAck(ack);
		}
		else
		{
			HandleSelectiveAck(ack);
		}
	}

	void HandleCumulativeAck(uint32_t ack)
	{
		if (ack >= m_totalPackets || ack < m_base)
		{
			return;
		}
		for (uint32_t i = m_base; i <= ack; ++i)
		{
			if (!m_states[i].acknowledged && !m_states[i].retransmitted)
			{
				UpdateRttEstimate(m_states[i]);
			}
			m_states[i].acknowledged = true;
		}
		m_base = ack + 1;
		m_logger.Debug("Received cumulative ACK #" + std::to_string(ack) + ", new base=" + std::to_string(m_base));
		GrowCongestionWindowOnAck();
	}

	void HandleSelectiveAck(uint32_t ack)
	{
		if (ack >= m_totalPackets || ack < m_base)
		{
			return;
		}
		if (m_states[ack].acknowledged)
		{
			return;
		}
		if (!m_states[ack].retransmitted)
		{
			UpdateRttEstimate(m_states[ack]);
		}
		m_states[ack].acknowledged = true;
		m_logger.Debug("Received selective ACK #" + std::to_string(ack));
		GrowCongestionWindowOnAck();
		while (m_base < m_totalPackets && m_states[m_base].acknowledged)
		{
			++m_base;
		}
	}

	void UpdateRttEstimate(SegmentState& state)
	{
		const double sample = std::chrono::duration<double, std::milli>(Clock::now() - state.lastSent).count();
		if (sample <= 0)
		{
			return;
		}
		if (m_smoothedRtt == 0)
		{
			m_smoothedRtt = sample;
			m_rttVariance = sample / 2.0;
		}
		else
		{
			m_rttVariance = 0.75 * m_rttVariance + 0.25 * std::abs(m_smoothedRtt - sample);
			m_smoothedRtt = 0.875 * m_smoothedRtt + 0.125 * sample;
		}
		m_currentTimeout = std::clamp(static_cast<int>(m_smoothedRtt + 4.0 * m_rttVariance), 100, 5000);
	}

	void GrowCongestionWindowOnAck()
	{
		if (!m_options.adaptive)
		{
			return;
		}
		if (m_options.algorithm == Algorithm::GBN)
		{
			if (m_congestionWindow < m_slowStartThreshold)
			{
				m_congestionWindow = std::min(m_maxWindow, m_congestionWindow * 2.0);
			}
			else
			{
				m_congestionWindow = std::min(m_maxWindow, m_congestionWindow + 1.0 / m_congestionWindow);
			}
		}
		else
		{
			if (m_congestionWindow < m_slowStartThreshold)
			{
				m_congestionWindow = std::min(m_maxWindow, m_congestionWindow + 1.0);
			}
			else
			{
				m_congestionWindow = std::min(m_maxWindow, m_congestionWindow + 1.0 / m_congestionWindow);
			}
		}
	}

	void HandleTimeouts(Clock::time_point now)
	{
		bool timeoutOccurred = false;
		if (m_options.algorithm == Algorithm::GBN)
		{
			timeoutOccurred = RetransmitWindow(now);
		}
		else
		{
			timeoutOccurred = RetransmitTimedOutSegments(now);
		}
		if (timeoutOccurred && m_options.adaptive)
		{
			m_slowStartThreshold = std::max(2.0, m_congestionWindow / 2.0);
			m_congestionWindow = 1.0;
			m_currentTimeout = std::min(5000, m_currentTimeout * 2);
			m_logger.Debug("Congestion window reduced to 1, ssthresh=" + std::to_string(m_slowStartThreshold));
		}
	}

	bool RetransmitWindow(Clock::time_point now)
	{
		if (m_base >= m_nextSequence || m_states[m_base].acknowledged || now - m_states[m_base].lastSent < Milliseconds(m_currentTimeout))
		{
			return false;
		}
		m_logger.Debug("Timeout on packet #" + std::to_string(m_base) + ", retransmitting window");
		for (uint32_t i = m_base; i < m_nextSequence; ++i)
		{
			if (m_states[i].acknowledged)
			{
				continue;
			}
			RetransmitSegment(i);
		}
		return true;
	}

	bool RetransmitTimedOutSegments(Clock::time_point now)
	{
		bool timeoutOccurred = false;
		for (uint32_t i = m_base; i < m_nextSequence; ++i)
		{
			if (m_states[i].acknowledged || now - m_states[i].lastSent < Milliseconds(m_currentTimeout))
			{
				continue;
			}
			m_logger.Debug("Timeout on packet #" + std::to_string(i) + ", selective retransmission");
			RetransmitSegment(i);
			timeoutOccurred = true;
		}
		return timeoutOccurred;
	}

	void RetransmitSegment(uint32_t sequence)
	{
		SendDataPacket(sequence, "RETX DATA #" + std::to_string(sequence));
		m_states[sequence].lastSent = Clock::now();
		m_states[sequence].retransmitted = true;
		++m_transmissions;
		++m_retransmissions;
	}

	void ReportProgress(Clock::time_point now)
	{
		if (now - m_lastProgress < Milliseconds(250))
		{
			return;
		}
		const auto elapsed = std::chrono::duration_cast<Milliseconds>(now - m_start).count();
		const double seconds = std::max(0.001, elapsed / 1000.0);
		const double throughput = (static_cast<double>(m_base) * PAYLOAD_SIZE * 8.0 / 1000.0) / seconds;
		m_csv << elapsed << ',' << m_congestionWindow << ',' << throughput << ',' << m_smoothedRtt << ',' << m_base << ',' << m_nextSequence << ',' << m_retransmissions << '\n';
		std::cout << "\rProgress: " << (m_totalPackets ? (100.0 * m_base / m_totalPackets) : 100.0) << "% | window: " << std::fixed << std::setprecision(1) << m_congestionWindow << " | retransmissions: " << m_retransmissions << std::flush;
		m_lastProgress = now;
	}

	bool FinishTransfer()
	{
		for (int attempt = 0; attempt < 30; ++attempt)
		{
			Packet fin = MakePacket(PacketType::Fin, m_transferId, m_totalPackets, m_totalPackets);
			SendPacket(m_socketFd, m_receiver, fin, GetChannel(), m_logger, "FIN");
			Packet response;
			sockaddr_in from{};
			if (ReceivePacket(m_socketFd, response, from, m_currentTimeout) && response.header.transferId == m_transferId && response.header.type == static_cast<uint8_t>(PacketType::FinAck))
			{
				return true;
			}
		}
		return false;
	}

	void PrintSummary()
	{
		const double duration = std::chrono::duration<double>(Clock::now() - m_start).count();
		std::cout << '\n';
		const double speed = duration > 0 ? m_fileData.size() / duration : 0;
		m_logger.Info("Transfer completed successfully");
		m_logger.Info("Time: " + std::to_string(duration) + " sec., speed: " + FormatBytes(static_cast<uint64_t>(speed)) + "/s");
		m_logger.Info("DATA sent: " + std::to_string(m_transmissions) + ", retransmissions: " + std::to_string(m_retransmissions) + ", ACKs received: " + std::to_string(m_ackReceived));
		m_logger.Info("RTT estimate: " + std::to_string(m_smoothedRtt) + " ms, final RTO: " + std::to_string(m_currentTimeout) + " ms");
		m_logger.Info("Metrics saved to rdtp_metrics.csv");
	}

	void CloseResources()
	{
		if (m_csv.is_open())
		{
			m_csv.close();
		}
		if (m_input.is_open())
		{
			m_input.close();
		}
		if (m_socketFd >= 0)
		{
			close(m_socketFd);
			m_socketFd = -1;
		}
	}

	SenderOptions m_options;
	Logger m_logger;
	std::ifstream m_input;
	std::vector<uint8_t> m_fileData;
	std::vector<std::vector<uint8_t>> m_payloads;
	std::vector<SegmentState> m_states;
	int m_socketFd = -1;
	sockaddr_in m_receiver{};
	std::optional<UnreliableChannel> m_channel;
	uint32_t m_transferId = 0;
	uint32_t m_totalPackets = 0;
	uint32_t m_base = 0;
	uint32_t m_nextSequence = 0;
	double m_congestionWindow = 1.0;
	double m_slowStartThreshold = 0.0;
	double m_maxWindow = 0.0;
	uint64_t m_transmissions = 0;
	uint64_t m_retransmissions = 0;
	uint64_t m_ackReceived = 0;
	Clock::time_point m_start{};
	Clock::time_point m_lastProgress{};
	std::ofstream m_csv;
	double m_smoothedRtt = 0.0;
	double m_rttVariance = 0.0;
	int m_currentTimeout = 500;
};

int main(int argc, char** argv)
{
	SenderApplication application;
	return application.Run(argc, argv);
}
