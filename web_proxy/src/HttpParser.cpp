#include "../include/HttpParser.h"
#include <algorithm>
#include <cctype>
#include <ranges>
#include <sstream>

std::string HttpParser::ToLower(const std::string& value)
{
	std::string result = value;
	std::ranges::transform(result, result.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return result;
}

std::string HttpParser::Trim(const std::string& value)
{
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
	{
		return {};
	}
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

bool HttpParser::ParseRequest(const std::string& data, HttpRequest& request, std::string& error)
{
	const auto headerEnd = data.find("\r\n\r\n");
	if (headerEnd == std::string::npos)
	{
		error = "Malformed HTTP headers";
		return false;
	}
	std::istringstream stream(data.substr(0, headerEnd));
	std::string line;
	if (!std::getline(stream, line))
	{
		error = "Missing request line";
		return false;
	}
	if (!line.empty() && line.back() == '\r')
	{
		line.pop_back();
	}
	std::istringstream firstLine(line);
	if (!(firstLine >> request.m_method >> request.m_target >> request.m_version))
	{
		error = "Invalid request line";
		return false;
	}
	while (std::getline(stream, line))
	{
		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
		const auto colon = line.find(':');
		if (colon == std::string::npos)
		{
			continue;
		}
		request.m_headers[ToLower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
	}
	const std::string targetLower = ToLower(request.m_target);
	if (targetLower.rfind("http://", 0) == 0)
	{
		const auto authorityStart = 7;
		const auto pathStart = request.m_target.find_first_of("/?", authorityStart);
		const std::string authority = request.m_target.substr(authorityStart, pathStart == std::string::npos ? std::string::npos : pathStart - authorityStart);
		request.m_path = pathStart == std::string::npos ? "/" : request.m_target.substr(pathStart);
		if (!request.m_path.empty() && request.m_path.front() == '?')
		{
			request.m_path.insert(request.m_path.begin(), '/');
		}
		const auto colon = authority.rfind(':');
		if (colon != std::string::npos)
		{
			request.m_host = authority.substr(0, colon);
			try
			{
				request.m_port = std::stoi(authority.substr(colon + 1));
			}
			catch (...)
			{
				error = "Invalid port";
				return false;
			}
		}
		else
		{
			request.m_host = authority;
		}
	}
	else if (targetLower.rfind("https://", 0) == 0)
	{
		error = "HTTPS proxying via CONNECT is not supported; this proxy handles plain HTTP";
		return false;
	}
	else
	{
		request.m_path = request.m_target;
		const auto hostIt = request.m_headers.find("host");
		if (hostIt != request.m_headers.end())
		{
			std::string authority = hostIt->second;
			const auto colon = authority.rfind(':');
			if (colon != std::string::npos)
			{
				request.m_host = authority.substr(0, colon);
				try
				{
					request.m_port = std::stoi(authority.substr(colon + 1));
				}
				catch (...)
				{
					error = "Invalid Host port";
					return false;
				}
			}
			else
			{
				request.m_host = authority;
			}
		}
	}
	if (request.m_host.empty() || request.m_path.empty() || request.m_port < 1 || request.m_port > 65535)
	{
		error = "Missing or invalid target host";
		return false;
	}
	if (request.m_path.find("\r") != std::string::npos || request.m_path.find("\n") != std::string::npos)
	{
		error = "Invalid request target";
		return false;
	}
	return true;
}

bool HttpParser::ParseResponse(const std::string& data, HttpResponse& response)
{
	const auto end = data.find("\r\n\r\n");
	if (end == std::string::npos)
	{
		return false;
	}
	response.m_raw = data;
	std::istringstream stream(data.substr(0, end));
	std::string line;
	if (!std::getline(stream, line))
	{
		return false;
	}
	if (!line.empty() && line.back() == '\r')
	{
		line.pop_back();
	}
	std::istringstream statusLine(line);
	std::string protocol;
	if (!(statusLine >> protocol >> response.m_statusCode))
	{
		return false;
	}
	while (std::getline(stream, line))
	{
		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
		const auto colon = line.find(':');
		if (colon == std::string::npos)
		{
			continue;
		}
		response.m_headers[ToLower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
	}
	return true;
}

std::string HttpParser::BuildOriginRequest(const HttpRequest& request, bool conditional, const std::string& etag, const std::string& lastModified)
{
	std::ostringstream out;
	out << request.m_method << ' ' << request.m_path << " HTTP/1.1\r\n";
	out << "Host: " << request.m_host;
	if (request.m_port != 80)
	{
		out << ':' << request.m_port;
	}
	out << "\r\nConnection: close\r\nVia: 1.1 cpp-web-proxy\r\n";
	for (const auto& header : request.m_headers)
	{
		const auto& name = header.first;
		if (name == "host"
			|| name == "connection"
			|| name == "proxy-connection"
			|| name == "keep-alive"
			|| name == "via"
			|| name == "transfer-encoding"
			|| name == "content-length")
		{
			continue;
		}
		if (conditional && name == "if-none-match")
		{
			continue;
		}
		if (conditional && name == "if-modified-since")
		{
			continue;
		}
		out << name << ": " << header.second << "\r\n";
	}
	if (conditional && !etag.empty())
	{
		out << "If-None-Match: " << etag << "\r\n";
	}
	if (conditional && !lastModified.empty())
	{
		out << "If-Modified-Since: " << lastModified << "\r\n";
	}
	out << "Content-Length: " << request.m_body.size() << "\r\n\r\n"
		<< request.m_body;
	return out.str();
}
