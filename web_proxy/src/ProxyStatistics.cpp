#include "../include/ProxyStatistics.h"
#include "../include/Logger.h"
#include <sstream>

void ProxyStatistics::Print() const
{
	std::ostringstream out;
	out << "STATISTICS requests=" << m_requests.load()
		<< " hits=" << m_hits.load()
		<< " misses=" << m_misses.load()
		<< " errors=" << m_errors.load()
		<< " bytes=" << m_bytes.load();
	Logger::Info(out.str());
}
