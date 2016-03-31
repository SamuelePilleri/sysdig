//
// mesos_http.cpp
//

#ifdef HAS_CAPTURE

#include "mesos_http.h"
#include "curl/curl.h"
#include "curl/easy.h"
#include "curl/curlbuild.h"
#include "sinsp.h"
#include "sinsp_int.h"
#include "mesos.h"
#define BUFFERSIZE 512 // b64 needs this macro
#include "b64/encode.h"
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>

mesos_http::mesos_http(mesos& m, const uri& url, bool discover, int timeout_ms):
	m_sync_curl(curl_easy_init()),
	m_select_curl(curl_easy_init()),
	m_mesos(m),
	m_url(url),
	m_connected(true),
	m_watch_socket(-1),
	m_timeout_ms(timeout_ms),
	m_callback_func(0),
	m_curl_version(curl_version_info(CURLVERSION_NOW)),
	m_request(make_request(url, m_curl_version)),
	m_is_mesos_state(url.to_string().find(mesos::default_state_api) != std::string::npos),
	m_discover_lead_master(discover)
{
	if(!m_sync_curl || !m_select_curl)
	{
		throw sinsp_exception("CURL initialization failed.");
	}

	ASSERT(m_curl_version);
	if((m_url.get_scheme() == "https") && (m_curl_version && !(m_curl_version->features | CURL_VERSION_SSL)))
	{
		throw sinsp_exception("HTTPS NOT supported");
	}

	check_error(curl_easy_setopt(m_sync_curl, CURLOPT_FORBID_REUSE, 1L));
	check_error(curl_easy_setopt(m_sync_curl, CURLOPT_CONNECTTIMEOUT_MS, m_timeout_ms));
	check_error(curl_easy_setopt(m_sync_curl, CURLOPT_TIMEOUT_MS, m_timeout_ms));

	check_error(curl_easy_setopt(m_select_curl, CURLOPT_CONNECTTIMEOUT_MS, m_timeout_ms));

	discover_mesos_leader();
}

mesos_http::~mesos_http()
{
	cleanup();
}

void mesos_http::cleanup()
{
	cleanup(&m_sync_curl);
	cleanup(&m_select_curl);
}

void mesos_http::cleanup(CURL** curl)
{
	if(curl && *curl)
	{
		curl_easy_cleanup(*curl);
		*curl = 0;
	}
	m_connected = false;
}

Json::Value mesos_http::get_state_frameworks()
{
	Json::Value frameworks;
	std::ostringstream os;
	CURLcode res = get_data(m_url.to_string(), os);
	if(res == CURLE_OK)
	{
		Json::Value root;
		Json::Reader reader;
		if(reader.parse(os.str(), root))
		{
			frameworks = root["frameworks"];
			if(frameworks.isNull() || !frameworks.isArray())
			{
				throw sinsp_exception("Unexpected condition while detecting Mesos master: frameworks entry not found.");
			}
		}
		else
		{
			throw sinsp_exception("Mesos master leader detection failed: Invalid JSON");
		}
	}
	else
	{
		throw sinsp_exception("Mesos master leader detection failed: " + m_url.to_string(false));
	}
	return frameworks;
}

void mesos_http::discover_mesos_leader()
{
	if(m_is_mesos_state)
	{
		g_logger.log("Inspecting Mesos leader [" + m_url.to_string(false) + ']', sinsp_logger::SEV_DEBUG);
		std::ostringstream os;
		CURLcode res = get_data(m_url.to_string(), os);
		if(res == CURLE_OK)
		{
			Json::Value root;
			Json::Reader reader;
			if(reader.parse(os.str(), root))
			{
				const Json::Value& frameworks = root["frameworks"];
				if(frameworks.isNull() || !frameworks.isArray())
				{
					throw sinsp_exception("Unexpected condition while detecting Mesos master: frameworks entry not found.");
				}
				g_logger.log("Found " + std::to_string(frameworks.size()) + " Mesos frameworks", sinsp_logger::SEV_DEBUG);
				if(frameworks.size()) // this is master leader
				{
					discover_framework_uris(frameworks);
					g_logger.log("Found Mesos master leader [" + m_url.to_string(false) + ']', sinsp_logger::SEV_INFO);
					return;
				}
				else  if(!m_discover_lead_master) // this is standby server and autodiscovery is disabled
				{
					throw sinsp_exception("Detected standby Mesos master: autodiscovery not enabled. Giving up (will retry).");
				}
				else // autodiscovery is enabled, find where is the master
				{
					const Json::Value& leader = root["leader"];
					if(!leader.isNull() && leader.isString())
					{
						std::string address = leader.asString();
						std::string::size_type pos = address.find('@');
						if(pos != std::string::npos && (pos + 1) < address.size())
						{
							address = std::string("http://").append(address.substr(pos + 1)).append(mesos::default_state_api);
							if(address != m_url.to_string())
							{
								g_logger.log("Detected Mesos master leader redirect: [" + address + ']', sinsp_logger::SEV_INFO);
								m_url = address;
								discover_mesos_leader();
							}
							else
							{
								throw sinsp_exception("Mesos master leader not discovered at [" + address + "] . "
													  "Giving up temporarily ...");
							}
						}
						else
						{
							throw sinsp_exception("Unexpected leader entry format while detecting Mesos master ("
												  + address + ").");
						}
					}
					else
					{
						throw sinsp_exception("Unexpected condition while detecting Mesos master: leader entry not found: ["
											  + m_url.to_string(false) + ']');
					}
				}
			}
			else
			{
				throw sinsp_exception("Mesos master leader detection failed: Invalid JSON ("
									  + m_url.to_string(false) + ']');
			}
		}
		else
		{
			throw sinsp_exception("Mesos master leader detection failed: "
								  + m_url.to_string(false));
		}
	}
}

std::string mesos_http::get_framework_url(const Json::Value& framework)
{
	Json::Value fw_url = framework["webui_url"];
	if(!fw_url.isNull() && fw_url.isString() && !fw_url.asString().empty())
	{
		return fw_url.asString();
	}
	else
	{
		fw_url = framework["hostname"];
		if(!fw_url.isNull() && fw_url.isString() && !fw_url.asString().empty())
		{
			return std::string("http://").append(fw_url.asString()).append(":8080");
		}
	}
	return "";
}

bool mesos_http::is_framework_active(const Json::Value& framework)
{
	Json::Value active = framework["active"];
	if(!active.isNull() && active.isBool() && active.asBool())
	{
		return true;
	}
	return false;
}

void mesos_http::discover_framework_uris(const Json::Value& frameworks)
{
	m_marathon_uris.clear();
	if(frameworks.isNull())
	{
		throw sinsp_exception("Unexpected condition while inspecting Marathon framework: frameworks entry not found.");
	}
	if(frameworks.isArray())
	{
		for(const auto& framework : frameworks)
		{
			Json::Value id = framework["id"];
			if(id.isNull() || !id.isString())
			{
				throw sinsp_exception("Unexpected condition while detecting Marathon framework: ID entry not found.");
			}
			else
			{
				Json::Value active = framework["active"];
				Json::Value fw_name = framework["name"];
				std::string name, id;
				if(!fw_name.isNull() && fw_name.isString() && !fw_name.asString().empty())
				{
					name = fw_name.asString();
				}
				Json::Value fw_id = framework["id"];
				if(!fw_id.isNull() && fw_id.isString() && !fw_id.asString().empty())
				{
					id = fw_id.asString();
				}
				if(!active.isNull() && active.isBool() && active.asBool())
				{
					std::string framework_url = get_framework_url(framework);
					if(!framework_url.empty())
					{
						if(mesos_framework::is_marathon(name))
						{
							g_logger.log(std::string("Found Marathon framework ").append(name).append(" (").append(id).append(") at [").append(framework_url).append(1, ']'),
										 sinsp_logger::SEV_INFO);
							m_marathon_uris.emplace_back(framework_url);
						}
						else
						{
							g_logger.log(std::string("Skipping non-Marathon framework URL detection ").append(name).append(" (").append(id).append(1, ')'), sinsp_logger::SEV_DEBUG);
						}
					}
					else
					{
						throw sinsp_exception(std::string("Can not obtain URL for framework ").append(name));
					}
				}
				else // framework exists, but is not active - remove it if we were watching it so far
				{
						g_logger.log(std::string("Mesos framework ").append(name).append(" (").append(id).append(") deactivated."), sinsp_logger::SEV_INFO);
						std::string framework_url = get_framework_url(framework);
						for(marathon_uri_t::iterator it = m_marathon_uris.begin(); it != m_marathon_uris.end();)
						{
							if(framework_url == *it)
							{
								it = m_marathon_uris.erase(it);
							}
							else { ++it; }
						}
					}
			}
		}
	}
	else
	{
		throw sinsp_exception("Mesos master leader detection failed: " + m_url.to_string(false));
	}
}

std::string mesos_http::make_request(uri url, curl_version_info_data* curl_version)
{
	std::ostringstream request;
	std::string host_and_port = url.get_host();
	int port = url.get_port();
	if(port)
	{
		host_and_port.append(1, ':').append(std::to_string(port));
	}
	request << "GET " << url.get_path();
	std::string query = url.get_query();
	if(!query.empty())
	{
		request << '?' << query;
	}
	request << " HTTP/1.1\r\nConnection: Keep-Alive\r\nUser-Agent: sysdig";
	if(curl_version && curl_version->version)
	{
		request << " (curl " << curl_version->version << ')';
	}
	request << "\r\nHost: " << host_and_port << "\r\nAccept: */*\r\n";
	std::string creds = url.get_credentials();
	if(!creds.empty())
	{
		std::istringstream is(creds);
		std::ostringstream os;
		base64::encoder().encode(is, os);
		request << "Authorization: Basic " << os.str() << "\r\n";
	}
	request << "\r\n";

	return request.str();
}

size_t mesos_http::write_data(void *ptr, size_t size, size_t nmemb, void *cb)
{
	std::string data(reinterpret_cast<const char*>(ptr), static_cast<size_t>(size * nmemb));
	*reinterpret_cast<std::ostream*>(cb) << data << std::flush;
	return size * nmemb;
}

CURLcode mesos_http::get_data(const std::string& url, std::ostream& os)
{
	g_logger.log(std::string("Retrieving data from ") + uri(url).to_string(false), sinsp_logger::SEV_DEBUG);
	curl_easy_setopt(m_sync_curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(m_sync_curl, CURLOPT_FOLLOWLOCATION, 1L);

	curl_easy_setopt(m_sync_curl, CURLOPT_NOSIGNAL, 1); //Prevent "longjmp causes uninitialized stack frame" bug
	curl_easy_setopt(m_sync_curl, CURLOPT_ACCEPT_ENCODING, "deflate");
	curl_easy_setopt(m_sync_curl, CURLOPT_WRITEFUNCTION, &mesos_http::write_data);

	curl_easy_setopt(m_sync_curl, CURLOPT_WRITEDATA, &os);
	return curl_easy_perform(m_sync_curl);
}

bool mesos_http::get_all_data(callback_func_t parse)
{
	std::ostringstream os;
	CURLcode res = get_data(m_url.to_string(), os);

	if(res != CURLE_OK)
	{
		g_logger.log(curl_easy_strerror(res), sinsp_logger::SEV_ERROR);
		m_connected = false;
	}
	else
	{
		(m_mesos.*parse)(os.str(), m_framework_id);
		m_connected = true;
	}

	return res == CURLE_OK;
}

int mesos_http::wait(int for_recv)
{
	struct timeval tv;
	fd_set infd, outfd, errfd;
	int res;

	tv.tv_sec = m_timeout_ms / 1000;
	tv.tv_usec = (m_timeout_ms % 1000) * 1000;

	FD_ZERO(&infd);
	FD_ZERO(&outfd);
	FD_ZERO(&errfd);
	FD_SET(m_watch_socket, &errfd);

	if(for_recv)
	{
		FD_SET(m_watch_socket, &infd);
	}
	else
	{
		FD_SET(m_watch_socket, &outfd);
	}

	res = select(m_watch_socket + 1, &infd, &outfd, &errfd, &tv);
	return res;
}

int mesos_http::get_socket(long timeout_ms)
{
	if(m_request.empty())
	{
		throw sinsp_exception("Cannot create watch socket (request empty).");
	}

	if(timeout_ms != -1)
	{
		m_timeout_ms = timeout_ms;
	}

	if(m_watch_socket < 0 || !m_connected)
	{
		long sockextr;
		std::string url = get_url().to_string();

		check_error(curl_easy_setopt(m_select_curl, CURLOPT_URL, url.c_str()));
		check_error(curl_easy_setopt(m_select_curl, CURLOPT_CONNECT_ONLY, 1L));

#if LIBCURL_VERSION_MAJOR >= 7 && LIBCURL_VERSION_MINOR >= 25
		// enable TCP keep-alive for this transfer
		check_error(curl_easy_setopt(m_select_curl, CURLOPT_TCP_KEEPALIVE, 1L));
		// keep-alive idle time
		check_error(curl_easy_setopt(m_select_curl, CURLOPT_TCP_KEEPIDLE, 300L));
		// interval time between keep-alive probes
		check_error(curl_easy_setopt(m_select_curl, CURLOPT_TCP_KEEPINTVL, 10L));
#endif // LIBCURL_VERSION_MAJOR >= 7 && LIBCURL_VERSION_MINOR >= 25

		check_error(curl_easy_perform(m_select_curl));

		check_error(curl_easy_getinfo(m_select_curl, CURLINFO_LASTSOCKET, &sockextr));
		m_watch_socket = sockextr;

		if(!wait(0))
		{
			throw sinsp_exception("Error obtaining socket: timeout.");
		}

		g_logger.log(std::string("Connected: collecting data from ") + uri(url).to_string(false), sinsp_logger::SEV_DEBUG);
	}

	if(m_watch_socket <= 0)
	{
		throw sinsp_exception("Error obtaining socket: " + std::to_string(m_watch_socket));
	}

	m_connected = true;
	return m_watch_socket;
}

void mesos_http::send_request()
{
	if(m_request.empty())
	{
		throw sinsp_exception("Mesos send: request (empty).");
	}

	if(m_watch_socket < 0)
	{
		throw sinsp_exception("Mesos send: invalid socket.");
	}

	size_t iolen = send(m_watch_socket, m_request.c_str(), m_request.size(), 0);
	if((iolen <= 0) || (m_request.size() != iolen))
	{
		throw sinsp_exception("Mesos send: socket connection error.");
	}
	else if(!wait(1))
	{
		throw sinsp_exception("Mesos send: timeout.");
	}
	g_logger.log(m_request, sinsp_logger::SEV_DEBUG);
}

bool purge_chunked_markers(std::string& data)
{
	std::string::size_type pos = data.find("}\r\n\0");
	if(pos != std::string::npos)
	{
		data = data.substr(0, pos);
	}

	const std::string nl = "\r\n";
	std::string::size_type begin, end;
	while((begin = data.find(nl)) != std::string::npos)
	{
		end = data.find(nl, begin + 2);
		if(end != std::string::npos)
		{
			data.erase(begin, end + 2 - begin);
		}
		else // newlines must come in pairs
		{
			return false;
		}
	}
	return true;
}

void mesos_http::handle_json(std::string::size_type end_pos, bool chunked)
{
	if(end_pos != std::string::npos)
	{
		if(m_data_buf.length() >= end_pos + 1)
		{
			m_data_buf = m_data_buf.substr(0, end_pos + 1);
			if(chunked && !purge_chunked_markers(m_data_buf))
			{
				g_logger.log("Invalid Mesos or Marathon JSON data detected (chunked transfer).", sinsp_logger::SEV_ERROR);
				(m_mesos.*m_callback_func)(std::string(), m_framework_id);
				m_data_buf.clear();
				m_content_length = string::npos;
				return;
			}
			if(try_parse(m_data_buf))
			{
				(m_mesos.*m_callback_func)(std::move(m_data_buf), m_framework_id);
			}
			else
			{
				g_logger.log("Invalid Mesos or Marathon JSON data detected (non-chunked transfer).", sinsp_logger::SEV_ERROR);
				(m_mesos.*m_callback_func)(std::string(), m_framework_id);
			}
			m_data_buf.clear();
			m_content_length = string::npos;
		}
	}
}

bool mesos_http::detect_chunked_transfer(const std::string& data)
{
	if(m_content_length == string::npos)
	{
		std::string::size_type cl_pos = data.find("Content-Length:");
		if(cl_pos != std::string::npos)
		{
			std::string::size_type nl_pos = data.find("\r\n", cl_pos);
			if(nl_pos != std::string::npos)
			{
				cl_pos += std::string("Content-Length:").length();
				std::string cl = data.substr(cl_pos, nl_pos - cl_pos);
				long len = strtol(cl.c_str(), NULL, 10);
				if(len == 0L || len == LONG_MAX || len == LONG_MIN || errno == ERANGE)
				{
					m_content_length = string::npos;
					(m_mesos.*m_callback_func)(std::string(), m_framework_id);
					m_data_buf.clear();
					return false;
				}
				else
				{
					m_content_length = static_cast<string::size_type>(len);
				}
			}
		}
	}
	return true;
}

void mesos_http::extract_data(std::string& data)
{
	if(!detect_chunked_transfer(data))
	{
		g_logger.log("An error occurred while detecting chunked transfer.", sinsp_logger::SEV_ERROR);
		return;
	}

	if(m_data_buf.empty())
	{
		m_data_buf = data;
		std::string::size_type pos = m_data_buf.find("\r\n{");
		if(pos != std::string::npos) // JSON begin
		{
			m_data_buf = m_data_buf.substr(pos + 2);
		}
	}
	else
	{
		m_data_buf.append(data);
	}
	bool chunked = (m_content_length == string::npos);
	if(chunked)
	{
		handle_json(m_data_buf.find("}\r\n0"), true);
	}
	else if (m_data_buf.length() >= m_content_length)
	{
		handle_json(m_data_buf.length() - 1, false);
	}
	return;
}

bool mesos_http::on_data()
{
	if(!m_callback_func)
	{
		throw sinsp_exception("Cannot parse data (parse function null).");
	}

	size_t iolen = 0;
	std::vector<char> buf;
	std::string data;

	try
	{
		int loop_counter = 0;
		do
		{
			size_t iolen = 0;
			int count = 0;
			int ioret = 0;
			ioret = ioctl(m_watch_socket, FIONREAD, &count);
			if(ioret >= 0 && count > 0)
			{
				if(count > static_cast<int>(buf.size()))
				{
					buf.resize(count);
				}
				iolen = recv(m_watch_socket, &buf[0], count, 0);
				if(iolen > 0)
				{
					data.append(&buf[0], iolen <= buf.size() ? iolen : buf.size());
				}
				else if(iolen == 0) { goto connection_closed; }
				else if(iolen < 0) { goto connection_error; }
			}
			else
			{
				if(ioret < 0) { goto connection_error; }
				else if(loop_counter == 0 && count == 0) { goto connection_closed; }
				break;
			}
			++loop_counter;
		} while(iolen && errno != CURLE_AGAIN);
		if(data.size())
		{
			extract_data(data);
		}
	}
	catch(sinsp_exception& ex)
	{
		g_logger.log(std::string("Data receive error [" + m_url.to_string() + "]: ").append(ex.what()), sinsp_logger::SEV_ERROR);
		return false;
	}
	return true;

connection_error:
{
	std::string err = strerror(errno);
	g_logger.log("Mesos or Marathon API connection [" + m_url.to_string() + "] error : " + err, sinsp_logger::SEV_ERROR);
	return false;
}

connection_closed:
	g_logger.log("Mesos or Marathon API connection [" + m_url.to_string() + "] closed.", sinsp_logger::SEV_ERROR);
	m_connected = false;
	return false;
}

void mesos_http::on_error(const std::string& /*err*/, bool /*disconnect*/)
{
	m_connected = false;
}

void mesos_http::check_error(CURLcode res)
{
	if(CURLE_OK != res && CURLE_AGAIN != res)
	{
		m_connected = false;
		std::ostringstream os;
		os << "Error: " << curl_easy_strerror(res);
		throw sinsp_exception(os.str());
	}
}

std::string mesos_http::make_uri(const std::string& path)
{
	uri url = get_url();
	std::string target_uri = url.get_scheme();
	target_uri.append("://");
	std::string user = url.get_user();
	if(!user.empty())
	{
		target_uri.append(user).append(1, ':').append(url.get_password()).append(1, '@');
	}
	target_uri.append(url.get_host());
	int port = url.get_port();
	if(port)
	{
		target_uri.append(1, ':').append(std::to_string(port));
	}
	target_uri.append(path);
	return target_uri;
}

Json::Value mesos_http::get_task_labels(const std::string& task_id)
{
	std::ostringstream os;
	CURLcode res = get_data(make_uri("/master/tasks"), os);

	Json::Value labels;
	if(res != CURLE_OK)
	{
		g_logger.log(curl_easy_strerror(res), sinsp_logger::SEV_ERROR);
		return labels;
	}

	try
	{
		Json::Value root;
		Json::Reader reader;
		if(reader.parse(os.str(), root, false))
		{
			Json::Value tasks = root["tasks"];
			if(!tasks.isNull())
			{
				for(const auto& task : tasks)
				{
					Json::Value id = task["id"];
					if(!id.isNull() && id.isString() && id.asString() == task_id)
					{
						Json::Value statuses = task["statuses"];
						if(!statuses.isNull())
						{
							double tstamp = 0.0;
							for(const auto& status : statuses)
							{
								// only task with most recent status
								// "TASK_RUNNING" considered
								Json::Value ts = status["timestamp"];
								if(!ts.isNull() && ts.isNumeric() && ts.asDouble() > tstamp)
								{
									Json::Value st = status["state"];
									if(!st.isNull() && st.isString())
									{
										if(st.asString() == "TASK_RUNNING")
										{
											labels = task["labels"];
											tstamp = ts.asDouble();
										}
										else
										{
											labels.clear();
										}
									}
								}
							}
							if(!labels.empty()) // currently running task found
							{
								return labels;
							}
						}
					}
				}
			}
		}
		else
		{
			g_logger.log("Error parsing tasks.\nJSON:\n---\n" + os.str() + "\n---", sinsp_logger::SEV_ERROR);
		}
	}
	catch(std::exception& ex)
	{
		g_logger.log(std::string("Error parsing tasks:") + ex.what(), sinsp_logger::SEV_ERROR);
	}

	return labels;
}

#endif // HAS_CAPTURE
