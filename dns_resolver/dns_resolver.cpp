#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

enum class RecordType : uint16_t
{
	A = 1,
	NS = 2,
	CNAME = 5,
	SOA = 6,
	MX = 15,
	TXT = 16,
	AAAA = 28,
	OPT = 41,
	RRSIG = 46,
	DNSKEY = 48
};

namespace NameUtils
{
std::string Normalize(const std::string& name)
{
	std::string result = name;
	while (result.ends_with('.'))
	{
		result.pop_back();
	}
	std::ranges::transform(result, result.begin(),
		[](const unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
	return result;
}

std::string ToDisplay(const std::string& name)
{
	return name + ".";
}

}; // namespace NameUtils

class Logger
{
public:
	explicit Logger(const bool debug)
		: m_debug(debug)
	{
	}

	void Debug(const std::string& message) const
	{
		if (m_debug)
		{
			std::cerr << "[debug] " << message << std::endl;
		}
	}

private:
	bool m_debug;
};

namespace RecordTypeConverter
{
const std::vector<std::pair<std::string, RecordType>>& Table()
{
	static const std::vector<std::pair<std::string, RecordType>> table = {
		{ "A", RecordType::A }, { "NS", RecordType::NS }, { "CNAME", RecordType::CNAME },
		{ "SOA", RecordType::SOA }, { "MX", RecordType::MX }, { "TXT", RecordType::TXT },
		{ "AAAA", RecordType::AAAA }, { "OPT", RecordType::OPT }, { "RRSIG", RecordType::RRSIG },
		{ "DNSKEY", RecordType::DNSKEY }
	};
	return table;
}

uint16_t ToCode(RecordType type)
{
	return static_cast<uint16_t>(type);
}

std::string ToString(const uint16_t code)
{
	for (const auto& entry : Table())
	{
		if (ToCode(entry.second) == code)
		{
			return entry.first;
		}
	}
	return "TYPE" + std::to_string(code);
}

RecordType FromString(const std::string& text)
{
	std::string upper = text;
	std::ranges::transform(upper, upper.begin(), [](unsigned char c) {
		return static_cast<char>(std::toupper(c));
	});
	for (const auto& entry : Table())
	{
		if (entry.first == upper)
		{
			return entry.second;
		}
	}
	throw std::runtime_error("Unsupported record type: " + text);
}
}; // namespace RecordTypeConverter

class ByteReader
{
public:
	explicit ByteReader(const std::vector<uint8_t>& data)
		: m_data(data)
		, m_position(0)
	{
	}

	uint8_t ReadU8()
	{
		const uint8_t value = ByteAt(m_position);
		++m_position;
		return value;
	}

	uint16_t ReadU16()
	{
		const auto high = ReadU8();
		const auto low = ReadU8();
		return static_cast<uint16_t>((high << 8) | low);
	}

	uint32_t ReadU32()
	{
		const auto high = ReadU16();
		const auto low = ReadU16();
		return (high << 16) | low;
	}

	std::vector<uint8_t> ReadBytes(const size_t count)
	{
		if (count > m_data.size() - m_position)
		{
			throw std::runtime_error("Malformed DNS message: unexpected end of data");
		}
		std::vector result(m_data.begin() + m_position, m_data.begin() + m_position + count);
		m_position += count;
		return result;
	}

	uint8_t ByteAt(const size_t position) const
	{
		if (position >= m_data.size())
		{
			throw std::runtime_error("Malformed DNS message: read out of bounds");
		}
		return m_data[position];
	}

	size_t Position() const
	{
		return m_position;
	}

	void Seek(size_t position)
	{
		if (position > m_data.size())
		{
			throw std::runtime_error("Malformed DNS message: invalid position");
		}
		m_position = position;
	}

private:
	const std::vector<uint8_t>& m_data;
	size_t m_position;
};

class ByteWriter
{
public:
	void WriteU8(uint8_t value)
	{
		m_buffer.push_back(value);
	}

	void WriteU16(uint16_t value)
	{
		WriteU8(static_cast<uint8_t>(value >> 8));
		WriteU8(static_cast<uint8_t>(value & 0xFF));
	}

	void WriteU32(uint32_t value)
	{
		WriteU16(static_cast<uint16_t>(value >> 16));
		WriteU16(static_cast<uint16_t>(value & 0xFFFF));
	}

	void WriteString(const std::string& text)
	{
		m_buffer.insert(m_buffer.end(), text.begin(), text.end());
	}

	const std::vector<uint8_t>& Data() const
	{
		return m_buffer;
	}

private:
	std::vector<uint8_t> m_buffer;
};

class NameCodec
{
public:
	static void Encode(const std::string& name, ByteWriter& writer)
	{
		if (name.size() > 253)
		{
			throw std::runtime_error("Domain name is too long: " + name);
		}
		size_t start = 0;
		while (start < name.size())
		{
			size_t dot = name.find('.', start);
			if (dot == std::string::npos)
			{
				dot = name.size();
			}
			WriteLabel(name.substr(start, dot - start), writer);
			start = dot + 1;
		}
		writer.WriteU8(0);
	}

	// Reads a name (following compression pointers) and leaves the reader right after it.
	static std::string Decode(ByteReader& reader)
	{
		std::string name;
		size_t position = reader.Position();
		size_t endPosition = 0;
		bool jumped = false;
		int jumps = 0;
		while (true)
		{
			uint8_t length = reader.ByteAt(position);
			if ((length & 0xC0) == 0xC0)
			{
				if (!jumped)
				{
					endPosition = position + 2;
					jumped = true;
				}
				if (++jumps > kMaxJumps)
				{
					throw std::runtime_error("Malformed DNS message: compression pointer loop");
				}
				position = static_cast<size_t>(((length & 0x3F) << 8) | reader.ByteAt(position + 1));
				continue;
			}
			if ((length & 0xC0) != 0)
			{
				throw std::runtime_error("Malformed DNS message: unsupported label type");
			}
			++position;
			if (length == 0)
			{
				break;
			}
			AppendLabel(name, reader, position, length);
			position += length;
		}
		reader.Seek(jumped ? endPosition : position);
		return NameUtils::Normalize(name);
	}

private:
	static constexpr int kMaxJumps = 64;

	static void WriteLabel(const std::string& label, ByteWriter& writer)
	{
		if (label.empty() || label.size() > 63)
		{
			throw std::runtime_error("Invalid label in domain name");
		}
		writer.WriteU8(static_cast<uint8_t>(label.size()));
		writer.WriteString(label);
	}

	static void AppendLabel(std::string& name, const ByteReader& reader, size_t position, uint8_t length)
	{
		if (!name.empty())
		{
			name += '.';
		}
		for (uint8_t i = 0; i < length; ++i)
		{
			name += static_cast<char>(reader.ByteAt(position + i));
		}
		if (name.size() > 255)
		{
			throw std::runtime_error("Malformed DNS message: domain name is too long");
		}
	}
};

class Encoding
{
public:
	static std::string Base64(const std::vector<uint8_t>& data)
	{
		static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out;
		size_t i = 0;
		for (; i + 2 < data.size(); i += 3)
		{
			uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (data[i + 1] << 8) | data[i + 2];
			out += { alphabet[(v >> 18) & 63], alphabet[(v >> 12) & 63], alphabet[(v >> 6) & 63], alphabet[v & 63] };
		}
		size_t rest = data.size() - i;
		if (rest == 1)
		{
			uint32_t v = static_cast<uint32_t>(data[i]) << 16;
			out += { alphabet[(v >> 18) & 63], alphabet[(v >> 12) & 63], '=', '=' };
		}
		else if (rest == 2)
		{
			uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (data[i + 1] << 8);
			out += { alphabet[(v >> 18) & 63], alphabet[(v >> 12) & 63], alphabet[(v >> 6) & 63], '=' };
		}
		return out;
	}

	static std::string Timestamp(uint32_t seconds)
	{
		time_t raw = seconds;
		tm utc;
		gmtime_r(&raw, &utc);
		char buffer[32];
		std::strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", &utc);
		return buffer;
	}
};

// ============================================================================
// DNS message model
// ============================================================================

struct DnsHeader
{
	uint16_t m_id = 0;
	uint16_t m_flags = 0;
	uint16_t m_questionCount = 0;
	uint16_t m_answerCount = 0;
	uint16_t m_authorityCount = 0;
	uint16_t m_additionalCount = 0;

	bool IsResponse() const { return (m_flags & 0x8000) != 0; }
	bool IsAuthoritative() const { return (m_flags & 0x0400) != 0; }
	bool IsTruncated() const { return (m_flags & 0x0200) != 0; }
	uint8_t ResponseCode() const { return static_cast<uint8_t>(m_flags & 0x000F); }
};

struct DnsQuestion
{
	std::string m_name;
	uint16_t m_type = 0;
	uint16_t m_class = 1;
};

struct DnsRecord
{
	std::string m_name;
	uint16_t m_type = 0;
	uint16_t m_class = 1;
	uint32_t m_ttl = 0;
	std::string m_text; // rdata as text
	std::string m_target; // target name for NS/CNAME/MX
};

struct DnsMessage
{
	DnsHeader m_header;
	std::vector<DnsQuestion> m_questions;
	std::vector<DnsRecord> m_answers;
	std::vector<DnsRecord> m_authority;
	std::vector<DnsRecord> m_additional;
};

// ============================================================================
// RDATA decoding
// ============================================================================

struct DecodedRdata
{
	std::string m_text;
	std::string m_target;
};

class RdataDecoder
{
public:
	// Reads RDATA of the given length and always leaves the reader at its end.
	DecodedRdata Decode(uint16_t type, ByteReader& reader, uint16_t length) const
	{
		size_t end = reader.Position() + length;
		DecodedRdata result = DecodeByType(type, reader, length, end);
		if (reader.Position() > end)
		{
			throw std::runtime_error("Malformed DNS message: RDATA exceeds its declared length");
		}
		reader.Seek(end);
		return result;
	}

private:
	DecodedRdata DecodeByType(uint16_t type, ByteReader& reader, uint16_t length, size_t end) const
	{
		switch (static_cast<RecordType>(type))
		{
		case RecordType::A:
			return DecodeAddress(AF_INET, 4, reader, length);
		case RecordType::AAAA:
			return DecodeAddress(AF_INET6, 16, reader, length);
		case RecordType::NS:
		case RecordType::CNAME:
			return DecodeName(reader);
		case RecordType::MX:
			return DecodeMx(reader);
		case RecordType::TXT:
			return DecodeTxt(reader, end);
		case RecordType::SOA:
			return DecodeSoa(reader);
		case RecordType::DNSKEY:
			return DecodeDnskey(reader, end);
		case RecordType::RRSIG:
			return DecodeRrsig(reader, end);
		default:
			return { "(" + std::to_string(length) + " bytes)", "" };
		}
	}

	static std::vector<uint8_t> ReadRest(ByteReader& reader, size_t end)
	{
		if (reader.Position() > end)
		{
			throw std::runtime_error("Malformed DNS message: RDATA is too short");
		}
		return reader.ReadBytes(end - reader.Position());
	}

	static DecodedRdata DecodeAddress(int family, size_t size, ByteReader& reader, uint16_t length)
	{
		if (length != size)
		{
			throw std::runtime_error("Malformed DNS message: wrong address length");
		}
		std::vector<uint8_t> bytes = reader.ReadBytes(size);
		char buffer[INET6_ADDRSTRLEN];
		if (inet_ntop(family, bytes.data(), buffer, sizeof(buffer)) == nullptr)
		{
			throw std::runtime_error("inet_ntop() failed to format the address");
		}
		return { buffer, "" };
	}

	static DecodedRdata DecodeName(ByteReader& reader)
	{
		std::string name = NameCodec::Decode(reader);
		return { NameUtils::ToDisplay(name), name };
	}

	static DecodedRdata DecodeMx(ByteReader& reader)
	{
		uint16_t preference = reader.ReadU16();
		std::string name = NameCodec::Decode(reader);
		return { std::to_string(preference) + " " + NameUtils::ToDisplay(name), name };
	}

	static DecodedRdata DecodeTxt(ByteReader& reader, size_t end)
	{
		std::string text;
		while (reader.Position() < end)
		{
			std::vector<uint8_t> chunk = reader.ReadBytes(reader.ReadU8());
			text += (text.empty() ? "\"" : " \"") + std::string(chunk.begin(), chunk.end()) + "\"";
		}
		return { text, "" };
	}

	static DecodedRdata DecodeSoa(ByteReader& reader)
	{
		std::string mname = NameCodec::Decode(reader);
		std::string rname = NameCodec::Decode(reader);
		std::string text = NameUtils::ToDisplay(mname) + " " + NameUtils::ToDisplay(rname);
		for (int i = 0; i < 5; ++i)
		{
			text += " " + std::to_string(reader.ReadU32());
		}
		return { text, "" };
	}

	static DecodedRdata DecodeDnskey(ByteReader& reader, size_t end)
	{
		uint16_t flags = reader.ReadU16();
		uint8_t protocol = reader.ReadU8();
		uint8_t algorithm = reader.ReadU8();
		std::string key = Encoding::Base64(ReadRest(reader, end));
		return { std::to_string(flags) + " " + std::to_string(protocol) + " " + std::to_string(algorithm) + " " + key, "" };
	}

	static DecodedRdata DecodeRrsig(ByteReader& reader, size_t end)
	{
		uint16_t covered = reader.ReadU16();
		uint8_t algorithm = reader.ReadU8();
		uint8_t labels = reader.ReadU8();
		uint32_t originalTtl = reader.ReadU32();
		uint32_t expiration = reader.ReadU32();
		uint32_t inception = reader.ReadU32();
		uint16_t keyTag = reader.ReadU16();
		std::string signer = NameCodec::Decode(reader);
		std::string signature = Encoding::Base64(ReadRest(reader, end));
		std::string text = RecordTypeConverter::ToString(covered) + " " + std::to_string(algorithm) + " " + std::to_string(labels) + " " + std::to_string(originalTtl) + " " + Encoding::Timestamp(expiration) + " " + Encoding::Timestamp(inception) + " " + std::to_string(keyTag) + " " + NameUtils::ToDisplay(signer) + " " + signature;
		return { text, "" };
	}
};

// ============================================================================
// Building and parsing DNS messages
// ============================================================================

struct QueryOptions
{
	uint16_t m_udpSize = 0; // 0 = no EDNS0 (classic 512-byte UDP limit)
	bool m_dnssecOk = false; // DO bit in EDNS0
};

class DnsMessageBuilder
{
public:
	std::vector<uint8_t> BuildQuery(uint16_t id, const std::string& name, uint16_t type,
		const QueryOptions& options) const
	{
		bool edns = options.m_udpSize != 0;
		ByteWriter writer;
		WriteHeader(writer, id, edns);
		WriteQuestion(writer, name, type);
		if (edns)
		{
			WriteOptRecord(writer, options);
		}
		return writer.Data();
	}

private:
	static void WriteHeader(ByteWriter& writer, uint16_t id, bool edns)
	{
		writer.WriteU16(id);
		writer.WriteU16(0x0000); // standard query, RD=0: we do the recursion ourselves
		writer.WriteU16(1);
		writer.WriteU16(0);
		writer.WriteU16(0);
		writer.WriteU16(edns ? 1 : 0);
	}

	static void WriteQuestion(ByteWriter& writer, const std::string& name, uint16_t type)
	{
		NameCodec::Encode(name, writer);
		writer.WriteU16(type);
		writer.WriteU16(1); // class IN
	}

	static void WriteOptRecord(ByteWriter& writer, const QueryOptions& options)
	{
		writer.WriteU8(0); // root name
		writer.WriteU16(static_cast<uint16_t>(RecordType::OPT));
		writer.WriteU16(options.m_udpSize); // CLASS field carries the UDP payload size
		writer.WriteU32(options.m_dnssecOk ? 0x00008000u : 0u);
		writer.WriteU16(0);
	}
};

class DnsMessageParser
{
public:
	DnsHeader ParseHeader(const std::vector<uint8_t>& data) const
	{
		ByteReader reader(data);
		return ReadHeader(reader);
	}

	DnsMessage Parse(const std::vector<uint8_t>& data) const
	{
		ByteReader reader(data);
		DnsMessage message;
		message.m_header = ReadHeader(reader);
		for (uint16_t i = 0; i < message.m_header.m_questionCount; ++i)
		{
			message.m_questions.push_back(ReadQuestion(reader));
		}
		message.m_answers = ReadSection(reader, message.m_header.m_answerCount);
		message.m_authority = ReadSection(reader, message.m_header.m_authorityCount);
		message.m_additional = ReadSection(reader, message.m_header.m_additionalCount);
		return message;
	}

private:
	RdataDecoder m_rdataDecoder;

	static DnsHeader ReadHeader(ByteReader& reader)
	{
		DnsHeader header;
		header.m_id = reader.ReadU16();
		header.m_flags = reader.ReadU16();
		header.m_questionCount = reader.ReadU16();
		header.m_answerCount = reader.ReadU16();
		header.m_authorityCount = reader.ReadU16();
		header.m_additionalCount = reader.ReadU16();
		return header;
	}

	static DnsQuestion ReadQuestion(ByteReader& reader)
	{
		DnsQuestion question;
		question.m_name = NameCodec::Decode(reader);
		question.m_type = reader.ReadU16();
		question.m_class = reader.ReadU16();
		return question;
	}

	std::vector<DnsRecord> ReadSection(ByteReader& reader, uint16_t count) const
	{
		std::vector<DnsRecord> records;
		for (uint16_t i = 0; i < count; ++i)
		{
			records.push_back(ReadRecord(reader));
		}
		return records;
	}

	DnsRecord ReadRecord(ByteReader& reader) const
	{
		DnsRecord record;
		record.m_name = NameCodec::Decode(reader);
		record.m_type = reader.ReadU16();
		record.m_class = reader.ReadU16();
		record.m_ttl = reader.ReadU32();
		uint16_t length = reader.ReadU16();
		DecodedRdata rdata = m_rdataDecoder.Decode(record.m_type, reader, length);
		record.m_text = rdata.m_text;
		record.m_target = rdata.m_target;
		return record;
	}
};

// ============================================================================
// Sockets: RAII wrapper and I/O helpers
// ============================================================================

class Socket
{
public:
	Socket()
		: m_fd(-1)
	{
	}
	explicit Socket(int fd)
		: m_fd(fd)
	{
	}
	~Socket() { Close(); }

	Socket(const Socket&) = delete;
	Socket& operator=(const Socket&) = delete;

	Socket(Socket&& other) noexcept
		: m_fd(other.m_fd)
	{
		other.m_fd = -1;
	}

	Socket& operator=(Socket&& other) noexcept
	{
		if (this != &other)
		{
			Close();
			m_fd = other.m_fd;
			other.m_fd = -1;
		}
		return *this;
	}

	static Socket Create(int type)
	{
		int fd = ::socket(AF_INET, type, 0);
		if (fd < 0)
		{
			throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
		}
		return Socket(fd);
	}

	int Fd() const { return m_fd; }

	void Close()
	{
		if (m_fd >= 0)
		{
			::close(m_fd);
			m_fd = -1;
		}
	}

private:
	int m_fd;
};

class Deadline
{
public:
	explicit Deadline(int timeoutMs)
		: m_end(std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs))
	{
	}

	int RemainingMs() const
	{
		auto left = std::chrono::duration_cast<std::chrono::milliseconds>(m_end - std::chrono::steady_clock::now());
		return left.count() > 0 ? static_cast<int>(left.count()) : 0;
	}

private:
	std::chrono::steady_clock::time_point m_end;
};

class SocketIo
{
public:
	static sockaddr_in MakeAddress(const std::string& ip, uint16_t port)
	{
		sockaddr_in address;
		std::memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1)
		{
			throw std::runtime_error("Invalid server IPv4 address: " + ip);
		}
		return address;
	}

	static void ConnectUdp(const Socket& socket, const sockaddr_in& address)
	{
		if (::connect(socket.Fd(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0)
		{
			throw std::runtime_error(std::string("connect() failed: ") + std::strerror(errno));
		}
	}

	static void ConnectTcp(const Socket& socket, const sockaddr_in& address, const Deadline& deadline)
	{
		SetBlocking(socket.Fd(), false);
		int result = ::connect(socket.Fd(), reinterpret_cast<const sockaddr*>(&address), sizeof(address));
		if (result < 0 && errno != EINPROGRESS)
		{
			throw std::runtime_error(std::string("connect() failed: ") + std::strerror(errno));
		}
		if (result < 0)
		{
			WaitFor(socket.Fd(), POLLOUT, deadline, "TCP connection");
			CheckConnectResult(socket.Fd());
		}
		SetBlocking(socket.Fd(), true);
	}

	// Waits until the descriptor is ready; throws when the deadline expires.
	static void WaitFor(int fd, short events, const Deadline& deadline, const std::string& what)
	{
		pollfd descriptor{ fd, events, 0 };
		int result;
		do
		{
			result = ::poll(&descriptor, 1, deadline.RemainingMs());
		} while (result < 0 && errno == EINTR);
		if (result == 0)
		{
			throw std::runtime_error("Timeout while waiting for: " + what);
		}
		if (result < 0)
		{
			throw std::runtime_error(std::string("poll() failed: ") + std::strerror(errno));
		}
	}

private:
	static void SetBlocking(const int fd, bool blocking)
	{
		int flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0)
		{
			throw std::runtime_error(std::string("fcntl() failed: ") + std::strerror(errno));
		}
		flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
		if (::fcntl(fd, F_SETFL, flags) < 0)
		{
			throw std::runtime_error(std::string("fcntl() failed: ") + std::strerror(errno));
		}
	}

	static void CheckConnectResult(int fd)
	{
		int error = 0;
		socklen_t length = sizeof(error);
		if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
		{
			throw std::runtime_error(std::string("getsockopt() failed: ") + std::strerror(errno));
		}
		if (error != 0)
		{
			throw std::runtime_error(std::string("connect() failed: ") + std::strerror(error));
		}
	}
};

// ============================================================================
// Transports: UDP and TCP (separate logic)
// ============================================================================

constexpr uint16_t kDnsPort = 53;

class Transport
{
public:
	virtual ~Transport() = default;
	virtual std::vector<uint8_t> Exchange(const std::string& serverIp, const std::vector<uint8_t>& query) = 0;
};

class UdpTransport final : public Transport
{
public:
	explicit UdpTransport(int timeoutMs)
		: m_timeoutMs(timeoutMs)
	{
	}

	std::vector<uint8_t> Exchange(const std::string& serverIp, const std::vector<uint8_t>& query) override
	{
		const Socket socket = Socket::Create(SOCK_DGRAM);
		SocketIo::ConnectUdp(socket, SocketIo::MakeAddress(serverIp, kDnsPort));
		SendDatagram(socket, query);
		return ReceiveMatching(socket, query);
	}

private:
	int m_timeoutMs;

	static void SendDatagram(const Socket& socket, const std::vector<uint8_t>& query)
	{
		ssize_t sent = ::write(socket.Fd(), query.data(), query.size());
		if (sent != static_cast<ssize_t>(query.size()))
		{
			throw std::runtime_error(std::string("write() failed: ") + std::strerror(errno));
		}
	}

	// Reads datagrams until one carries our query ID (others are ignored).
	std::vector<uint8_t> ReceiveMatching(const Socket& socket, const std::vector<uint8_t>& query) const
	{
		Deadline deadline(m_timeoutMs);
		std::vector<uint8_t> buffer(65535);
		while (true)
		{
			SocketIo::WaitFor(socket.Fd(), POLLIN, deadline, "UDP response");
			ssize_t received = ::read(socket.Fd(), buffer.data(), buffer.size());
			if (received < 0)
			{
				throw std::runtime_error(std::string("read() failed: ") + std::strerror(errno));
			}
			if (IsMatching(buffer, static_cast<size_t>(received), query))
			{
				buffer.resize(static_cast<size_t>(received));
				return buffer;
			}
		}
	}

	static bool IsMatching(const std::vector<uint8_t>& buffer, size_t size, const std::vector<uint8_t>& query)
	{
		return size >= 12 && buffer[0] == query[0] && buffer[1] == query[1];
	}
};

class TcpTransport : public Transport
{
public:
	explicit TcpTransport(int timeoutMs)
		: m_timeoutMs(timeoutMs)
	{
	}

	std::vector<uint8_t> Exchange(const std::string& serverIp, const std::vector<uint8_t>& query) override
	{
		Deadline deadline(m_timeoutMs);
		Socket socket = Socket::Create(SOCK_STREAM);
		SocketIo::ConnectTcp(socket, SocketIo::MakeAddress(serverIp, kDnsPort), deadline);
		WriteAll(socket, FrameQuery(query), deadline);
		return ReadFramedResponse(socket, deadline);
	}

private:
	int m_timeoutMs;

	// In TCP every message is preceded by a 2-byte big-endian length.
	static std::vector<uint8_t> FrameQuery(const std::vector<uint8_t>& query)
	{
		if (query.size() > 0xFFFF)
		{
			throw std::runtime_error("Query is too large for a TCP frame");
		}
		ByteWriter writer;
		writer.WriteU16(static_cast<uint16_t>(query.size()));
		for (uint8_t byte : query)
		{
			writer.WriteU8(byte);
		}
		return writer.Data();
	}

	static void WriteAll(const Socket& socket, const std::vector<uint8_t>& data, const Deadline& deadline)
	{
		size_t sent = 0;
		while (sent < data.size())
		{
			SocketIo::WaitFor(socket.Fd(), POLLOUT, deadline, "sending over TCP");
			const ssize_t written = ::write(socket.Fd(), data.data() + sent, data.size() - sent);
			if (written < 0 && errno == EINTR)
			{
				continue;
			}
			if (written < 0)
			{
				throw std::runtime_error(std::string("write() failed: ") + std::strerror(errno));
			}
			sent += static_cast<size_t>(written);
		}
	}

	static std::vector<uint8_t> ReadExact(const Socket& socket, size_t count, const Deadline& deadline)
	{
		std::vector<uint8_t> buffer(count);
		size_t received = 0;
		while (received < count)
		{
			SocketIo::WaitFor(socket.Fd(), POLLIN, deadline, "TCP response");
			ssize_t got = ::read(socket.Fd(), buffer.data() + received, count - received);
			if (got < 0 && errno == EINTR)
			{
				continue;
			}
			if (got < 0)
			{
				throw std::runtime_error(std::string("read() failed: ") + std::strerror(errno));
			}
			if (got == 0)
			{
				throw std::runtime_error("Server closed the TCP connection before the response was complete");
			}
			received += static_cast<size_t>(got);
		}
		return buffer;
	}

	static std::vector<uint8_t> ReadFramedResponse(const Socket& socket, const Deadline& deadline)
	{
		std::vector<uint8_t> prefix = ReadExact(socket, 2, deadline);
		size_t length = (static_cast<size_t>(prefix[0]) << 8) | prefix[1];
		if (length < 12)
		{
			throw std::runtime_error("TCP frame is shorter than a DNS header");
		}
		return ReadExact(socket, length, deadline);
	}
};

// ============================================================================
// Query client: UDP -> (TC flag?) -> TCP
// ============================================================================

class DnsQueryClient
{
public:
	DnsQueryClient(const Logger& logger, const QueryOptions& options, Transport& udp, Transport& tcp)
		: m_logger(logger)
		, m_options(options)
		, m_udp(udp)
		, m_tcp(tcp)
		, m_random(std::random_device{}())
	{
	}

	DnsMessage Query(const std::string& server, const std::string& name, RecordType type)
	{
		uint16_t id = NextId();
		std::vector<uint8_t> query = m_builder.BuildQuery(id, name, RecordTypeConverter::ToCode(type), m_options);
		m_logger.Debug("  -> UDP query to " + server + " (" + std::to_string(query.size()) + " bytes, id=" + std::to_string(id) + ")");
		std::vector<uint8_t> response = m_udp.Exchange(server, query);
		if (m_parser.ParseHeader(response).IsTruncated())
		{
			response = SwitchToTcp(server, query);
		}
		DnsMessage message = m_parser.Parse(response);
		ValidateResponse(message, id, name, type);
		return message;
	}

private:
	const Logger& m_logger;
	QueryOptions m_options;
	Transport& m_udp;
	Transport& m_tcp;
	DnsMessageBuilder m_builder;
	DnsMessageParser m_parser;
	std::mt19937 m_random;

	uint16_t NextId()
	{
		std::uniform_int_distribution<int> distribution(0, 65535);
		return static_cast<uint16_t>(distribution(m_random));
	}

	std::vector<uint8_t> SwitchToTcp(const std::string& server, const std::vector<uint8_t>& query)
	{
		m_logger.Debug("  !! TC flag set in response: UDP could not fit the response. Switching to TCP.");
		auto started = std::chrono::steady_clock::now();
		std::vector<uint8_t> response = m_tcp.Exchange(server, query);
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
		m_logger.Debug("  <- TCP response in " + std::to_string(elapsed.count()) + " ms, " + std::to_string(response.size()) + " bytes (UDP failed, TCP succeeded)");
		if (m_parser.ParseHeader(response).IsTruncated())
		{
			throw std::runtime_error("Server set the TC flag even in a TCP response");
		}
		return response;
	}

	static void ValidateResponse(const DnsMessage& message, uint16_t id, const std::string& name, RecordType type)
	{
		if (!message.m_header.IsResponse() || message.m_header.m_id != id)
		{
			throw std::runtime_error("Unexpected DNS message: QR flag or ID mismatch");
		}
		if (message.m_questions.size() != 1 || message.m_questions[0].m_name != name || message.m_questions[0].m_type != RecordTypeConverter::ToCode(type))
		{
			throw std::runtime_error("Question section of the response does not match the query");
		}
	}
};

// ============================================================================
// Resolution result and the TTL cache
// ============================================================================

struct ResultRecord
{
	std::string m_owner;
	std::string m_type;
	uint32_t m_ttl;
	std::string m_data;

	std::string ToLine() const
	{
		return NameUtils::ToDisplay(m_owner) + "\t" + std::to_string(m_ttl) + "\tIN\t" + m_type + "\t" + m_data;
	}
};

struct CacheEntry
{
	std::string m_name;
	std::string m_type;
	time_t m_expiresAt = 0;
	std::vector<ResultRecord> m_records;
};

class DnsCache
{
public:
	DnsCache(std::string path, const Logger& logger)
		: m_path(std::move(path))
		, m_logger(logger)
	{
	}

	void Load()
	{
		std::ifstream file(m_path);
		std::string line;
		while (file && std::getline(file, line))
		{
			ParseLine(line);
		}
		PurgeExpired();
	}

	void Save() const
	{
		const std::string temporary = m_path + ".tmp";
		if (!WriteFile(temporary))
		{
			m_logger.Debug("Cache: cannot write " + temporary);
			return;
		}
		if (std::rename(temporary.c_str(), m_path.c_str()) != 0)
		{
			m_logger.Debug("Cache: cannot replace " + m_path);
		}
	}

	std::optional<std::vector<ResultRecord>> Lookup(const std::string& name, const std::string& type)
	{
		const auto found = m_entries.find(Key(name, type));
		if (found == m_entries.end())
		{
			return std::nullopt;
		}
		if (found->second.m_expiresAt <= std::time(nullptr))
		{
			m_entries.erase(found);
			return std::nullopt;
		}
		return WithRemainingTtl(found->second);
	}

	void Store(const std::string& name, const std::string& type, const std::vector<ResultRecord>& records)
	{
		if (records.empty())
		{
			return;
		}
		uint32_t minTtl = records.front().m_ttl;
		for (const ResultRecord& record : records)
		{
			minTtl = std::min(minTtl, record.m_ttl);
		}
		if (minTtl == 0)
		{
			return;
		}
		CacheEntry entry;
		entry.m_name = name;
		entry.m_type = type;
		entry.m_expiresAt = std::time(nullptr) + minTtl;
		entry.m_records = records;
		m_entries[Key(name, type)] = entry;
	}

	void Clear()
	{
		m_entries.clear();
		std::remove(m_path.c_str());
	}

	void Show(std::ostream& out) const
	{
		if (m_entries.empty())
		{
			out << "Cache is empty" << std::endl;
			return;
		}
		for (const auto& item : m_entries)
		{
			ShowEntry(item.second, out);
		}
	}

private:
	std::string m_path;
	const Logger& m_logger;
	std::map<std::string, CacheEntry> m_entries;

	static std::string Key(const std::string& name, const std::string& type)
	{
		return name + "|" + type;
	}

	static uint32_t Remaining(const CacheEntry& entry)
	{
		const time_t left = entry.m_expiresAt - std::time(nullptr);
		return left > 0
			? static_cast<uint32_t>(left)
			: 0;
	}

	static std::vector<ResultRecord> WithRemainingTtl(const CacheEntry& entry)
	{
		std::vector<ResultRecord> records = entry.m_records;
		for (ResultRecord& record : records)
		{
			record.m_ttl = Remaining(entry);
		}
		return records;
	}

	static void ShowEntry(const CacheEntry& entry, std::ostream& out)
	{
		out << NameUtils::ToDisplay(entry.m_name) << " " << entry.m_type << "  (" << Remaining(entry) << " s remaining)" << std::endl;
		for (const ResultRecord& record : WithRemainingTtl(entry))
		{
			out << "    " << record.ToLine() << std::endl;
		}
	}

	void PurgeExpired()
	{
		time_t now = std::time(nullptr);
		std::erase_if(m_entries, [now](const auto& item) {
			return item.second.m_expiresAt <= now;
		});
	}

	void ParseLine(const std::string& line)
	{
		const std::vector<std::string> fields = SplitFields(line, 6);
		if (fields.size() != 6)
		{
			return;
		}
		auto& [m_name, m_type, m_expiresAt, m_records] = m_entries[Key(fields[1], fields[2])];
		m_name = fields[1];
		m_type = fields[2];
		m_expiresAt = static_cast<time_t>(std::strtoll(fields[0].c_str(), nullptr, 10));
		m_records.push_back({ fields[3], fields[4], 0, fields[5] });
	}

	static std::vector<std::string> SplitFields(const std::string& line, size_t count)
	{
		std::vector<std::string> fields;
		size_t start = 0;
		while (fields.size() + 1 < count)
		{
			const size_t tab = line.find('\t', start);
			if (tab == std::string::npos)
			{
				return {};
			}
			fields.push_back(line.substr(start, tab - start));
			start = tab + 1;
		}
		fields.push_back(line.substr(start));
		return fields;
	}

	static std::string Sanitize(std::string text)
	{
		std::ranges::replace(text, '\t', ' ');
		std::ranges::replace(text, '\n', ' ');
		return text;
	}

	bool WriteFile(const std::string& path) const
	{
		std::ofstream file(path, std::ios::trunc);
		if (!file)
		{
			return false;
		}
		const auto now = std::time(nullptr);
		for (const auto& val : m_entries | std::views::values)
		{
			if (val.m_expiresAt > now)
			{
				WriteEntry(val, file);
			}
		}
		file.flush();
		return static_cast<bool>(file);
	}

	static void WriteEntry(const CacheEntry& entry, std::ostream& out)
	{
		for (const ResultRecord& record : entry.m_records)
		{
			out << entry.m_expiresAt << '\t'
				<< entry.m_name << '\t'
				<< entry.m_type << '\t'
				<< record.m_owner << '\t'
				<< record.m_type << '\t'
				<< Sanitize(record.m_data) << '\n';
		}
	}
};

enum class OutcomeKind
{
	Answer,
	Cname,
	Referral,
	NoData,
	NameError,
	ServerError
};

struct ResponseOutcome
{
	OutcomeKind m_kind = OutcomeKind::ServerError;
	std::vector<DnsRecord> m_records; // Answer/Cname: CNAME chain + data
	std::string m_cnameTarget; // Cname: the name that is still to be resolved
	std::string m_zone; // Referral: zone the NS records delegate
	std::vector<std::string> m_nsNames; // Referral: name server names
	std::vector<std::string> m_glue; // Referral: IPv4 addresses from the Additional section
	std::string m_reason; // ServerError: description
};

class ResponseAnalyzer
{
public:
	static ResponseOutcome Analyze(const DnsMessage& message, const std::string& name, RecordType type)
	{
		uint8_t code = message.m_header.ResponseCode();
		if (code == kNxDomain)
		{
			return MakeOutcome(OutcomeKind::NameError);
		}
		if (code != 0)
		{
			return MakeError("server returned RCODE " + std::to_string(code));
		}
		const auto answer = FollowAnswers(message, name, type);
		if (answer)
		{
			return *answer;
		}
		return AnalyzeAuthority(message);
	}

private:
	static constexpr uint8_t kNxDomain = 3;
	static constexpr int kMaxChain = 16;

	static ResponseOutcome MakeOutcome(const OutcomeKind& kind)
	{
		ResponseOutcome outcome;
		outcome.m_kind = kind;
		return outcome;
	}

	static ResponseOutcome MakeError(const std::string& reason)
	{
		ResponseOutcome outcome = MakeOutcome(OutcomeKind::ServerError);
		outcome.m_reason = reason;
		return outcome;
	}

	static bool IsType(const DnsRecord& record, RecordType type)
	{
		return record.m_type == RecordTypeConverter::ToCode(type);
	}

	// Walk the CNAME chain inside the Answer section until the requested data is found.
	static std::optional<ResponseOutcome> FollowAnswers(
		const DnsMessage& message,
		const std::string& name,
		const RecordType type)
	{
		std::vector<DnsRecord> chain;
		std::string current = name;
		for (int i = 0; i < kMaxChain; ++i)
		{
			std::vector<DnsRecord> found = CollectOwned(message.m_answers, current, type);
			if (ContainsType(found, type))
			{
				chain.insert(chain.end(), found.begin(), found.end());
				return MakeAnswer(OutcomeKind::Answer, chain, "");
			}
			const DnsRecord* alias = FindCname(message.m_answers, current);
			if (alias == nullptr)
			{
				break;
			}
			chain.push_back(*alias);
			current = alias->m_target;
		}
		if (chain.empty())
		{
			return std::nullopt;
		}
		return MakeAnswer(OutcomeKind::Cname, chain, current);
	}

	static ResponseOutcome MakeAnswer(
		const OutcomeKind kind,
		const std::vector<DnsRecord>& records,
		const std::string& target)
	{
		ResponseOutcome outcome = MakeOutcome(kind);
		outcome.m_records = records;
		outcome.m_cnameTarget = target;
		return outcome;
	}

	static std::vector<DnsRecord> CollectOwned(const std::vector<DnsRecord>& section, const std::string& owner, RecordType type)
	{
		std::vector<DnsRecord> result;
		for (const DnsRecord& record : section)
		{
			if (record.m_name == owner && (IsType(record, type) || IsType(record, RecordType::RRSIG)))
			{
				result.push_back(record);
			}
		}
		return result;
	}

	static bool ContainsType(const std::vector<DnsRecord>& records, RecordType type)
	{
		return std::ranges::any_of(records, [&](const DnsRecord& r) {
			return IsType(r, type);
		});
	}

	static const DnsRecord* FindCname(const std::vector<DnsRecord>& section, const std::string& owner)
	{
		for (const DnsRecord& record : section)
		{
			if (record.m_name == owner && IsType(record, RecordType::CNAME))
			{
				return &record;
			}
		}
		return nullptr;
	}

	static ResponseOutcome AnalyzeAuthority(const DnsMessage& message)
	{
		const std::vector<DnsRecord>& authority = message.m_authority;
		const bool hasSoa = std::ranges::any_of(authority, [](const DnsRecord& r) {
			return IsType(r, RecordType::SOA);
		});
		if (hasSoa)
		{
			return MakeOutcome(OutcomeKind::NoData);
		}
		ResponseOutcome referral = MakeOutcome(OutcomeKind::Referral);
		for (const DnsRecord& record : authority)
		{
			if (IsType(record, RecordType::NS))
			{
				referral.m_zone = record.m_name;
				referral.m_nsNames.push_back(record.m_target);
			}
		}
		if (referral.m_nsNames.empty())
		{
			return MakeError("response has no answer, no delegation and no SOA");
		}
		referral.m_glue = ExtractGlue(message.m_additional, referral.m_nsNames);
		return referral;
	}

	static std::vector<std::string> ExtractGlue(const std::vector<DnsRecord>& additional,
		const std::vector<std::string>& nsNames)
	{
		std::vector<std::string> glue;
		for (const DnsRecord& record : additional)
		{
			bool isNs = std::ranges::find(nsNames, record.m_name) != nsNames.end();
			if (isNs && IsType(record, RecordType::A))
			{
				glue.push_back(record.m_text);
			}
		}
		return glue;
	}
};

class IterativeResolver
{
public:
	IterativeResolver(DnsQueryClient& client, DnsCache& cache, const Logger& logger, bool dnssecOk)
		: m_client(client)
		, m_cache(cache)
		, m_logger(logger)
		, m_dnssecOk(dnssecOk)
	{
	}

	std::vector<ResultRecord> Resolve(const std::string& name, RecordType type)
	{
		return ResolveName(NameUtils::Normalize(name), type, ResolveDepth{});
	}

private:
	struct ResolveDepth
	{
		int m_cname = 0;
		int m_ns = 0;
	};

	static constexpr int kMaxSteps = 24;
	static constexpr int kMaxCnameDepth = 8;
	static constexpr int kMaxNsDepth = 4;
	static constexpr int kMaxNsLookups = 3;

	DnsQueryClient& m_client;
	DnsCache& m_cache;
	const Logger& m_logger;
	bool m_dnssecOk;
	ResponseAnalyzer m_analyzer;

	std::vector<ResultRecord> ResolveName(const std::string& name, const RecordType type, const ResolveDepth& depth)
	{
		const auto cached = LookupCache(name, type);
		if (cached)
		{
			return *cached;
		}
		std::vector<ResultRecord> records = WalkFromRoot(name, type, depth);
		m_cache.Store(name, CacheType(type), records);
		return records;
	}

	std::vector<ResultRecord> WalkFromRoot(const std::string& name, RecordType type, const ResolveDepth& depth)
	{
		std::vector<std::string> servers = RootServers();
		std::string zone;
		for (int step = 1; step <= kMaxSteps; ++step)
		{
			m_logger.Debug("Step " + std::to_string(step) + ": zone '" + NameUtils::ToDisplay(zone) + "'");
			ResponseOutcome outcome = AskServers(servers, name, type);
			if (outcome.m_kind != OutcomeKind::Referral)
			{
				return FinishWalk(outcome, name, type, depth);
			}
			servers = ServersFromReferral(outcome, depth);
			zone = outcome.m_zone;
		}
		throw std::runtime_error("Too many resolution steps for " + name);
	}

	std::vector<ResultRecord> FinishWalk(const ResponseOutcome& outcome, const std::string& name, RecordType type,
		const ResolveDepth& depth)
	{
		switch (outcome.m_kind)
		{
		case OutcomeKind::Answer:
			m_logger.Debug("Answer found: " + std::to_string(outcome.m_records.size()) + " records.");
			return ToResultRecords(outcome.m_records);
		case OutcomeKind::Cname:
			return FollowCname(outcome, type, depth);
		case OutcomeKind::NoData:
			m_logger.Debug("Name exists but has no records of this type (NODATA).");
			throw std::runtime_error("No " + TypeLabel(type) + " records for " + name);
		case OutcomeKind::NameError:
			m_logger.Debug("Authoritative server says 'no such name' (NXDOMAIN).");
			throw std::runtime_error("Host not found (NXDOMAIN): " + name);
		default:
			throw std::runtime_error("Unexpected response outcome");
		}
	}

	std::vector<ResultRecord> FollowCname(const ResponseOutcome& outcome, RecordType type, const ResolveDepth& depth)
	{
		if (depth.m_cname >= kMaxCnameDepth)
		{
			throw std::runtime_error("CNAME chain is too long");
		}
		m_logger.Debug("This is an alias! Now looking up '" + outcome.m_cnameTarget + "'.");
		ResolveDepth next = depth;
		++next.m_cname;
		std::vector<ResultRecord> records = ToResultRecords(outcome.m_records);
		std::vector<ResultRecord> tail = ResolveName(outcome.m_cnameTarget, type, next);
		records.insert(records.end(), tail.begin(), tail.end());
		return records;
	}

	// Ask the servers one by one until somebody gives a usable response.
	ResponseOutcome AskServers(const std::vector<std::string>& servers, const std::string& name, RecordType type)
	{
		std::string lastError = "no servers to ask";
		for (const std::string& server : servers)
		{
			m_logger.Debug("Asking " + server + ": " + name + " " + TypeLabel(type) + "?");
			try
			{
				DnsMessage message = m_client.Query(server, name, type);
				LogSummary(message);
				ResponseOutcome outcome = m_analyzer.Analyze(message, name, type);
				if (outcome.m_kind != OutcomeKind::ServerError)
				{
					return outcome;
				}
				lastError = outcome.m_reason;
			}
			catch (const std::runtime_error& error)
			{
				lastError = error.what();
			}
			m_logger.Debug("  This server is unhelpful (" + lastError + "), trying next.");
		}
		throw std::runtime_error("No server could answer: " + lastError);
	}

	void LogSummary(const DnsMessage& message) const
	{
		const DnsHeader& h = message.m_header;
		m_logger.Debug(std::string("  <- answer: RCODE=") + std::to_string(h.ResponseCode()) + (h.IsAuthoritative() ? " AA" : "") + " answers=" + std::to_string(message.m_answers.size()) + " authority=" + std::to_string(message.m_authority.size()) + " additional=" + std::to_string(message.m_additional.size()));
	}

	std::vector<std::string> ServersFromReferral(const ResponseOutcome& outcome, const ResolveDepth& depth)
	{
		m_logger.Debug("Server does not know the answer, but refers to zone '" + NameUtils::ToDisplay(outcome.m_zone) + "' (NS: " + Join(outcome.m_nsNames) + ")");
		std::vector<std::string> servers = outcome.m_glue;
		if (servers.empty())
		{
			m_logger.Debug("No glue records - will have to resolve name server addresses myself.");
			servers = ResolveNameServers(outcome.m_nsNames, depth);
		}
		else
		{
			m_logger.Debug("NS addresses came in glue (Additional): " + Join(servers));
		}
		if (servers.empty())
		{
			throw std::runtime_error("Cannot obtain addresses of the name servers for zone " + outcome.m_zone);
		}
		return servers;
	}

	std::vector<std::string> ResolveNameServers(const std::vector<std::string>& nsNames, const ResolveDepth& depth)
	{
		if (depth.m_ns >= kMaxNsDepth)
		{
			throw std::runtime_error("Name server lookups are nested too deeply");
		}
		ResolveDepth next = depth;
		++next.m_ns;
		std::vector<std::string> addresses;
		int lookups = 0;
		for (const std::string& nsName : nsNames)
		{
			if (!addresses.empty() || ++lookups > kMaxNsLookups)
			{
				break;
			}
			addresses = TryResolveAddresses(nsName, next);
		}
		return addresses;
	}

	std::vector<std::string> TryResolveAddresses(const std::string& nsName, const ResolveDepth& depth)
	{
		std::vector<std::string> addresses;
		try
		{
			for (const ResultRecord& record : ResolveName(nsName, RecordType::A, depth))
			{
				if (record.m_type == "A")
				{
					addresses.push_back(record.m_data);
				}
			}
		}
		catch (const std::runtime_error& error)
		{
			m_logger.Debug("  Failed to resolve address for " + nsName + ": " + error.what());
		}
		return addresses;
	}

	static std::vector<std::string> RootServers()
	{
		return { "198.41.0.4", "170.247.170.2", "192.33.4.12", "199.7.91.13", "192.203.230.10",
			"192.5.5.241", "192.112.36.4", "198.97.190.53", "192.36.148.17", "192.58.128.30",
			"193.0.14.129", "199.7.83.42", "202.12.27.33" };
	}

	static std::vector<ResultRecord> ToResultRecords(const std::vector<DnsRecord>& records)
	{
		std::vector<ResultRecord> result;
		for (const DnsRecord& record : records)
		{
			result.push_back({ record.m_name, RecordTypeConverter::ToString(record.m_type), record.m_ttl, record.m_text });
		}
		return result;
	}

	static std::string TypeLabel(RecordType type)
	{
		return RecordTypeConverter::ToString(RecordTypeConverter::ToCode(type));
	}

	std::string CacheType(RecordType type) const
	{
		return TypeLabel(type) + (m_dnssecOk ? "+DO" : "");
	}

	std::optional<std::vector<ResultRecord>> LookupCache(const std::string& name, RecordType type) const
	{
		std::optional<std::vector<ResultRecord>> hit = m_cache.Lookup(name, CacheType(type));
		if (hit)
		{
			m_logger.Debug("Already know this! '" + name + "' " + TypeLabel(type) + " taken from cache (TTL not expired).");
		}
		return hit;
	}

	static std::string Join(const std::vector<std::string>& items)
	{
		std::string result;
		for (const std::string& item : items)
		{
			result += (result.empty() ? "" : ", ") + item;
		}
		return result;
	}
};

struct Options
{
	std::string m_domain;
	RecordType m_type = RecordType::A;
	bool m_dnssec = false;
	bool m_debug = false;
	bool m_showCache = false;
	bool m_clearCache = false;
};

class CommandLineParser
{
public:
	static Options Parse(const int argc, char** argv)
	{
		Options options;
		std::vector<std::string> positional;
		for (int i = 1; i < argc; ++i)
		{
			std::string arg = argv[i];
			if (arg.starts_with('-'))
			{
				ApplyFlag(options, arg);
			}
			else
			{
				positional.push_back(arg);
			}
		}
		ApplyPositional(options, positional);
		return options;
	}

	static std::string Usage()
	{
		return "Usage:\n"
			   "  dns_resolver <domain> <A|AAAA|NS|CNAME|MX|TXT|SOA|DNSKEY|DNSSEC> [-d]\n"
			   "  dns_resolver --cache-show     show the cache\n"
			   "  dns_resolver --cache-clear    clear the cache\n"
			   "  -d    debug mode\n";
	}

private:
	static void ApplyFlag(Options& options, const std::string& arg)
	{
		if (arg == "-d")
		{
			options.m_debug = true;
		}
		else if (arg == "--cache-show")
		{
			options.m_showCache = true;
		}
		else if (arg == "--cache-clear")
		{
			options.m_clearCache = true;
		}
		else
		{
			throw std::runtime_error("Unknown option: " + arg);
		}
	}

	static void ApplyPositional(Options& options, const std::vector<std::string>& positional)
	{
		if (options.m_showCache || options.m_clearCache)
		{
			return;
		}
		if (positional.size() != 2)
		{
			throw std::runtime_error("Expected a domain name and a record type");
		}
		options.m_domain = positional[0];
		ApplyType(options, positional[1]);
	}

	static void ApplyType(Options& options, const std::string& text)
	{
		std::string upper = text;
		std::ranges::transform(upper, upper.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		if (upper == "DNSSEC")
		{
			options.m_type = RecordType::DNSKEY; // DNSKEY + RRSIG, requested with the DO bit
			options.m_dnssec = true;
		}
		else
		{
			options.m_type = RecordTypeConverter::FromString(upper);
		}
	}
};

void Print(const std::vector<ResultRecord>& records, std::ostream& out)
{
	for (const ResultRecord& record : records)
	{
		out << record.ToLine() << std::endl;
	}
}

class Application
{
public:
	static int Run(const int argc, char** argv)
	{
		Options options;
		try
		{
			options = CommandLineParser::Parse(argc, argv);
		}
		catch (const std::runtime_error& error)
		{
			std::cerr << "Error: " << error.what() << "\n\n"
					  << CommandLineParser::Usage();
			return 1;
		}
		try
		{
			const Logger logger(options.m_debug);
			DnsCache cache(kCachePath, logger);
			cache.Load();
			if (options.m_showCache)
			{
				cache.Show(std::cout);
				return 0;
			}
			if (options.m_clearCache)
			{
				cache.Clear();
				std::cout << "Cache clear" << std::endl;
				return 0;
			}
			return ResolveAndPrint(options, logger, cache);
		}
		catch (const std::runtime_error& error)
		{
			std::cerr << "Error: " << error.what() << std::endl;
			return 1;
		}
	}

private:
	static constexpr auto kCachePath = ".dns_resolver_cache";
	static constexpr int kUdpTimeoutMs = 2000;
	static constexpr int kTcpTimeoutMs = 5000;
	static constexpr uint16_t kDnssecUdpSize = 512;

	static int ResolveAndPrint(const Options& options, const Logger& logger, DnsCache& cache)
	{
		UdpTransport udp(kUdpTimeoutMs);
		TcpTransport tcp(kTcpTimeoutMs);
		const QueryOptions queryOptions{ options.m_dnssec
				? kDnssecUdpSize
				: uint16_t{ 0 },
			options.m_dnssec };
		DnsQueryClient client(logger, queryOptions, udp, tcp);
		IterativeResolver resolver(client, cache, logger, options.m_dnssec);
		try
		{
			Print(resolver.Resolve(options.m_domain, options.m_type), std::cout);
		}
		catch (...)
		{
			cache.Save();
			throw;
		}
		cache.Save();
		return 0;
	}
};

int main(const int argc, char** argv)
{
	std::signal(SIGPIPE, SIG_IGN);
	return Application::Run(argc, argv);
}
