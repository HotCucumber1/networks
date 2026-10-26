#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

struct Config
{
	std::string server;
	std::string port = "25";
	std::string from;
	std::string to;
	std::string subject = "SMTP test from C++20";
	std::string body = "Hello from the C++20 SMTP client.\n";
	std::string username;
	std::string password;
	std::string tls = "none";
	std::string auth = "none";
	std::vector<std::string> attachments;
	int timeoutSeconds = 30;
};

std::string requireValue(int& i, const int argc, char** argv, const std::string& option)
{
	if (i + 1 >= argc)
	{
		throw std::runtime_error("Missing value for " + option);
	}
	return argv[++i];
}

Config ParseArgs(const int argc, char** argv)
{
	constexpr int minTimeout = 1;
	constexpr int maxTimeout = 600;

	Config c;
	std::string passwordEnv;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--server")
			c.server = requireValue(i, argc, argv, a);
		else if (a == "--port")
			c.port = requireValue(i, argc, argv, a);
		else if (a == "--from")
			c.from = requireValue(i, argc, argv, a);
		else if (a == "--to")
			c.to = requireValue(i, argc, argv, a);
		else if (a == "--subject")
			c.subject = requireValue(i, argc, argv, a);
		else if (a == "--body")
			c.body = requireValue(i, argc, argv, a);
		else if (a == "--user")
			c.username = requireValue(i, argc, argv, a);
		else if (a == "--password-env")
			passwordEnv = requireValue(i, argc, argv, a);
		else if (a == "--tls")
			c.tls = requireValue(i, argc, argv, a);
		else if (a == "--auth")
			c.auth = requireValue(i, argc, argv, a);
		else if (a == "--attach")
			c.attachments.push_back(requireValue(i, argc, argv, a));
		else if (a == "--timeout")
		{
			const std::string v = requireValue(i, argc, argv, a);
			std::size_t used = 0;
			c.timeoutSeconds = std::stoi(v, &used);
			if (used != v.size() || c.timeoutSeconds < minTimeout || c.timeoutSeconds > maxTimeout)
			{
				throw std::runtime_error("--timeout must be between 1 and 600 seconds");
			}
		}
		else
		{
			throw std::runtime_error("Unknown option: " + a);
		}
	}

	if (c.server.empty() || c.from.empty() || c.to.empty())
	{
		throw std::runtime_error("--server, --from and --to are required");
	}
	if (c.tls != "starttls" && c.tls != "none")
	{
		throw std::runtime_error("--tls must be starttls or none");
	}
	if (c.auth != "plain" && c.auth != "login" && c.auth != "none")
	{
		throw std::runtime_error("--auth must be plain, login or none");
	}
	if (c.auth != "none" && c.username.empty())
	{
		throw std::runtime_error("--user is required unless --auth none is selected");
	}
	if (!passwordEnv.empty())
	{
		const char* p = std::getenv(passwordEnv.c_str());
		if (!p)
		{
			throw std::runtime_error("Password environment variable is not set: " + passwordEnv);
		}
		c.password = p;
	}
	else if (c.auth != "none")
	{
		throw std::runtime_error("Set the password with --password-env NAME (do not put it on the command line)");
	}
	if (c.auth != "none" && c.password.empty())
	{
		throw std::runtime_error("SMTP password is empty");
	}
	return c;
}

bool ContainsCRLF(const std::string_view s)
{
	return s.find('\r') != std::string_view::npos || s.find('\n') != std::string_view::npos;
}

void ValidateHeaderValue(const std::string& value, const std::string& name)
{
	if (ContainsCRLF(value))
	{
		throw std::runtime_error(name + " must not contain CR or LF");
	}
}

void ValidateMailbox(const std::string& address, const std::string& name)
{
	constexpr int maxSize = 254;
	ValidateHeaderValue(address, name);
	if (address.empty()
		|| address.size() > maxSize
		|| address.find('<') != std::string::npos
		|| address.find('>') != std::string::npos
		|| address.find(' ') != std::string::npos
		|| address.find('@') == std::string::npos)
	{
		throw std::runtime_error("Invalid " + name + " address");
	}
}

std::string Base64(const std::string_view input)
{
	if (input.empty())
	{
		return {};
	}
	std::string out(4 * ((input.size() + 2) / 3), '\0');
	const int n = EVP_EncodeBlock(
		reinterpret_cast<unsigned char*>(out.data()),
		reinterpret_cast<const unsigned char*>(input.data()),
		static_cast<int>(input.size()));
	if (n < 0)
	{
		throw std::runtime_error("Base64 encoding failed");
	}
	out.resize(static_cast<std::size_t>(n));
	return out;
}

std::string EncodedWordIfNeeded(const std::string& s)
{
	bool ascii = true;
	for (unsigned char c : s)
	{
		if (c < 32 || c > 126)
		{
			ascii = false;
			break;
		}
	}
	if (ascii)
	{
		return s;
	}
	return "=?UTF-8?B?" + Base64(s) + "?=";
}

std::string RandomBoundary()
{
	std::random_device rd;
	std::mt19937_64 gen(rd());
	std::ostringstream os;
	os << "----cpp20smtp-" << std::hex << gen();
	return os.str();
}

std::string FileName(const std::string& path)
{
	const auto slash = path.find_last_of("/\\");
	auto name = slash == std::string::npos
		? path
		: path.substr(slash + 1);
	if (name.empty())
	{
		name = "attachment.bin";
	}
	for (char& ch : name)
	{
		const auto u = static_cast<unsigned char>(ch);
		if (u < 32 || u == 127 || ch == '"' || ch == '\\' || ch == '\r' || ch == '\n')
		{
			ch = '_';
		}
	}
	return name;
}

std::string ContentType(const std::string& name)
{
	const auto dot = name.find_last_of('.');
	auto ext = dot == std::string::npos
		? ""
		: name.substr(dot + 1);
	std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	if (ext == "txt" || ext == "log" || ext == "csv")
		return "text/plain";
	if (ext == "html" || ext == "htm")
		return "text/html";
	if (ext == "pdf")
		return "application/pdf";
	if (ext == "png")
		return "image/png";
	if (ext == "jpg" || ext == "jpeg")
		return "image/jpeg";
	if (ext == "gif")
		return "image/gif";
	if (ext == "bmp")
		return "image/bmp";
	if (ext == "doc")
		return "application/msword";
	if (ext == "docx")
		return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
	if (ext == "xls")
		return "application/vnd.ms-excel";
	if (ext == "xlsx")
		return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
	if (ext == "ppt")
		return "application/vnd.ms-powerpoint";
	if (ext == "pptx")
		return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
	if (ext == "zip")
		return "application/zip";
	if (ext == "json")
		return "application/json";
	return "application/octet-stream";
}

class SmtpClient
{
public:
	explicit SmtpClient(const Config& config)
		: m_config(config)
	{
	}
	~SmtpClient() { Cleanup(); }
	SmtpClient(const SmtpClient&) = delete;
	SmtpClient& operator=(const SmtpClient&) = delete;

	void Send()
	{
		try
		{
			ConnectTCP();
			Expect(220);
			Command("EHLO localhost", 250);

			if (m_config.tls == "starttls")
			{
				Command("STARTTLS", 220);
				StartTls();
				Command("EHLO localhost", 250);
			}

			if (m_config.auth == "plain")
			{
				AuthenticatePlain();
			}
			else if (m_config.auth == "login")
			{
				AuthenticateLogin();
			}

			Command("MAIL FROM:<" + m_config.from + ">", 250);
			SendRecipients();
			Command("DATA", 354);
			SendMessageData();
			Expect(250);

			Command("QUIT", 221);
			Cleanup();
			std::cout << "SMTP server accepted the message.\n";
		}
		catch (...)
		{
			Cleanup();
			throw;
		}
	}

private:
	const Config& m_config;
	int m_fd = -1;
	SSL_CTX* m_ctx = nullptr;
	SSL* m_ssl = nullptr;

	void Cleanup() noexcept
	{
		if (m_ssl)
		{
			SSL_shutdown(m_ssl);
			SSL_free(m_ssl);
			m_ssl = nullptr;
		}
		if (m_ctx)
		{
			SSL_CTX_free(m_ctx);
			m_ctx = nullptr;
		}
		if (m_fd >= 0)
		{
			::close(m_fd);
			m_fd = -1;
		}
	}

	void ConnectTCP()
	{
		addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		addrinfo* result = nullptr;
		const int rc = getaddrinfo(m_config.server.c_str(), m_config.port.c_str(), &hints, &result);
		if (rc != 0)
		{
			throw std::runtime_error(std::string("DNS resolution failed: ") + gai_strerror(rc));
		}

		for (addrinfo* p = result; p; p = p->ai_next)
		{
			const int candidate = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
			if (candidate < 0)
			{
				continue;
			}

			timeval tv{};
			tv.tv_sec = m_config.timeoutSeconds;
			setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

			if (::connect(candidate, p->ai_addr, p->ai_addrlen) == 0)
			{
				m_fd = candidate;
				break;
			}
			::close(candidate);
		}
		freeaddrinfo(result);
		if (m_fd < 0)
		{
			throw std::runtime_error("TCP connection failed");
		}
		std::cout << "Connected to " << m_config.server << ':' << m_config.port << '\n';
	}

	void StartTls()
	{
		OPENSSL_init_ssl(0, nullptr);
		m_ctx = SSL_CTX_new(TLS_client_method());
		if (!m_ctx)
		{
			throw std::runtime_error("SSL_CTX_new failed");
		}
		SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);
		SSL_CTX_set_verify(m_ctx, SSL_VERIFY_PEER, nullptr);
		if (SSL_CTX_set_default_verify_paths(m_ctx) != 1)
		{
			throw std::runtime_error("Could not load system CA certificates");
		}

		m_ssl = SSL_new(m_ctx);
		if (!m_ssl)
			throw std::runtime_error("SSL_new failed");
		if (SSL_set_tlsext_host_name(m_ssl, m_config.server.c_str()) != 1)
			throw std::runtime_error("Could not set TLS SNI");
		if (SSL_set1_host(m_ssl, m_config.server.c_str()) != 1)
			throw std::runtime_error("Could not set TLS hostname verification");
		if (SSL_set_fd(m_ssl, m_fd) != 1)
			throw std::runtime_error("SSL_set_fd failed");
		if (SSL_connect(m_ssl) != 1)
			throw std::runtime_error("TLS handshake or certificate validation failed");
		std::cout << "TLS established: " << SSL_get_version(m_ssl) << '\n';
	}

	ssize_t TransportRead(void* data, const std::size_t size) const
	{
		if (!m_ssl)
		{
			return ::read(m_fd, data, size);
		}
		const int n = SSL_read(m_ssl, data, static_cast<int>(std::min<std::size_t>(size, 1024 * 1024)));
		if (n > 0)
		{
			return n;
		}
		const int e = SSL_get_error(m_ssl, n);
		if (e == SSL_ERROR_ZERO_RETURN)
		{
			return 0;
		}
		if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
		{
			errno = EINTR;
			return -1;
		}
		return -1;
	}

	ssize_t TransportWrite(const void* data, std::size_t size) const
	{
		if (!m_ssl)
		{
			return ::write(m_fd, data, size);
		}
		const int n = SSL_write(m_ssl, data, static_cast<int>(std::min<std::size_t>(size, 1024 * 1024)));
		if (n > 0)
		{
			return n;
		}
		const int e = SSL_get_error(m_ssl, n);
		if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
		{
			errno = EINTR;
			return -1;
		}
		return -1;
	}

	void WriteAll(const std::string_view data) const
	{
		std::size_t sent = 0;
		while (sent < data.size())
		{
			const ssize_t n = TransportWrite(data.data() + sent, data.size() - sent);
			if (n < 0 && errno == EINTR)
			{
				continue;
			}
			if (n <= 0)
				throw std::runtime_error("Socket/TLS write failed");
			sent += static_cast<std::size_t>(n);
		}
	}

	std::string ReadLine() const
	{
		std::string line;
		while (true)
		{
			char ch = '\0';
			const ssize_t n = TransportRead(&ch, 1);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				throw std::runtime_error(n == 0 ? "SMTP server closed connection" : "Socket/TLS read failed");
			line.push_back(ch);
			if (line.size() > 512)
				throw std::runtime_error("SMTP response line exceeds 512 octets");
			if (ch == '\n')
				break;
		}
		if (line.size() < 2 || line.substr(line.size() - 2) != "\r\n")
			throw std::runtime_error("SMTP response line does not end in CRLF");
		std::cout << "S: " << line;
		return line;
	}

	int ReadResponse() const
	{
		std::string line = ReadLine();
		auto validPrefix = [](const std::string& s) {
			return s.size() >= 4
				&& std::isdigit(static_cast<unsigned char>(s[0]))
				&& std::isdigit(static_cast<unsigned char>(s[1]))
				&& std::isdigit(static_cast<unsigned char>(s[2]))
				&& (s[3] == ' ' || s[3] == '-');
		};
		if (!validPrefix(line))
		{
			throw std::runtime_error("Malformed SMTP response");
		}
		const int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
		while (line[3] == '-')
		{
			line = ReadLine();
			if (!validPrefix(line) || line.compare(0, 3, std::to_string(code)) != 0)
			{
				throw std::runtime_error("Malformed multiline SMTP response");
			}
		}
		return code;
	}

	void Expect(const int expected) const
	{
		const int actual = ReadResponse();
		if (actual != expected)
		{
			throw std::runtime_error("Expected SMTP " + std::to_string(expected) + ", got " + std::to_string(actual));
		}
	}

	void Command(const std::string& text, const int expected) const
	{
		std::cout << "C: " << text << '\n';
		WriteAll(text + "\r\n");
		Expect(expected);
	}

	void AuthLine(const std::string& payload) const
	{
		WriteAll(payload + "\r\n");
		Expect(235);
	}

	void AuthenticatePlain() const
	{
		std::cout << "C: AUTH PLAIN [credentials hidden]\n";
		WriteAll("AUTH PLAIN " + Base64(std::string("\0", 1) + m_config.username + std::string("\0", 1) + m_config.password) + "\r\n");
		const int code = ReadResponse();
		if (code == 235)
		{
			return;
		}
		if (code == 334)
		{
			AuthLine(Base64(std::string("\0", 1) + m_config.username + std::string("\0", 1) + m_config.password));
			return;
		}
		throw std::runtime_error("AUTH PLAIN failed: " + std::to_string(code));
	}

	void AuthenticateLogin() const
	{
		Command("AUTH LOGIN", 334);
		std::cout << "C: [username hidden]\n";
		WriteAll(Base64(m_config.username) + "\r\n");
		Expect(334);
		std::cout << "C: [password hidden]\n";
		WriteAll(Base64(m_config.password) + "\r\n");
		Expect(235);
	}

	void SendRecipients() const
	{
		std::cout << "C: RCPT TO:<" << m_config.to << ">\n";
		WriteAll("RCPT TO:<" + m_config.to + ">\r\n");
		const int code = ReadResponse();
		if (code != 250 && code != 251)
		{
			throw std::runtime_error("RCPT TO failed: " + std::to_string(code));
		}
	}

	class DataWriter
	{
	public:
		explicit DataWriter(SmtpClient& owner)
			: m_owner(owner)
		{
		}

		void Raw(const std::string_view s)
		{
			std::string transformed;
			transformed.reserve(s.size() + 16);
			for (const char ch : s)
			{
				if (m_atLineStart && ch == '.')
				{
					transformed.push_back('.');
				}
				transformed.push_back(ch);
				m_atLineStart = (ch == '\n');
				m_lastWasCRLF = m_previousWasCR && ch == '\n';
				m_previousWasCR = ch == '\r';
			}
			if (!transformed.empty())
			{
				m_owner.WriteAll(transformed);
			}
		}

		void Line(const std::string_view s)
		{
			Raw(s);
			Raw("\r\n");
		}

		void Finish()
		{
			if (!m_lastWasCRLF)
			{
				Raw("\r\n");
			}
			m_owner.WriteAll(".\r\n");
		}

	private:
		SmtpClient& m_owner;
		bool m_atLineStart = true;
		bool m_previousWasCR = false;
		bool m_lastWasCRLF = false;
	};

	void SendMessageData()
	{
		ValidateHeaderValue(m_config.subject, "Subject");
		const std::string boundary = RandomBoundary();
		DataWriter out(*this);

		out.Line("From: <" + m_config.from + ">");
		out.Line("To: <" + m_config.to + ">");
		out.Line("Subject: " + EncodedWordIfNeeded(m_config.subject));
		out.Line("MIME-Version: 1.0");
		if (m_config.attachments.empty())
		{
			out.Line("Content-Type: text/plain; charset=UTF-8");
			out.Line("Content-Transfer-Encoding: 8bit");
			out.Line("");
			WriteBody(out, m_config.body);
		}
		else
		{
			out.Line("Content-Type: multipart/mixed; boundary=\"" + boundary + "\"");
			out.Line("");
			out.Line("--" + boundary);
			out.Line("Content-Type: text/plain; charset=UTF-8");
			out.Line("Content-Transfer-Encoding: 8bit");
			out.Line("");
			WriteBody(out, m_config.body);

			for (const auto& path : m_config.attachments)
			{
				const std::string name = FileName(path);
				std::ifstream file(path, std::ios::binary);
				if (!file)
					throw std::runtime_error("Cannot open attachment: " + path);
				out.Line("--" + boundary);
				out.Line("Content-Type: " + ContentType(name) + "; name=\"" + name + "\"");
				out.Line("Content-Transfer-Encoding: base64");
				out.Line("Content-Disposition: attachment; filename=\"" + name + "\"");
				out.Line("");

				std::array<unsigned char, 57> buffer{};
				while (file)
				{
					file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
					const std::streamsize count = file.gcount();
					if (count > 0)
					{
						const std::string encoded = Base64(std::string_view(
							reinterpret_cast<const char*>(buffer.data()),
							static_cast<std::size_t>(count)));
						out.Line(encoded);
					}
				}
				if (file.bad())
				{
					throw std::runtime_error("Error reading attachment: " + path);
				}
			}
			out.Line("--" + boundary + "--");
		}
		out.Finish();
	}

	static void WriteBody(DataWriter& out, const std::string& body)
	{
		std::string line;
		for (std::size_t i = 0; i < body.size(); ++i)
		{
			const char ch = body[i];
			if (ch == '\r' || ch == '\n')
			{
				if (ch == '\r' && i + 1 < body.size() && body[i + 1] == '\n')
					++i;
				out.Line(line);
				line.clear();
			}
			else
			{
				line.push_back(ch);
			}
		}
		if (!line.empty())
		{
			out.Line(line);
		}
	}
};

} // namespace

int main(const int argc, char** argv)
{
	std::signal(SIGPIPE, SIG_IGN);
	try
	{
		const Config config = ParseArgs(argc, argv);
		ValidateMailbox(config.from, "sender");
		ValidateMailbox(config.to, "recipient");
		ValidateHeaderValue(config.subject, "Subject");
		SmtpClient client(config);
		client.Send();
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "Error: " << e.what() << '\n';
		return 1;
	}
}
