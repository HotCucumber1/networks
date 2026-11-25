#pragma once
#include "HttpMessage.h"
#include <string>

class HttpParser
{
public:
	static bool ParseRequest(const std::string& data, HttpRequest& request, std::string& error);
	static bool ParseResponse(const std::string& data, HttpResponse& response);
	static std::string BuildOriginRequest(const HttpRequest& request, bool conditional, const std::string& etag, const std::string& lastModified);
	static std::string ToLower(const std::string& value);
	static std::string Trim(const std::string& value);
};
