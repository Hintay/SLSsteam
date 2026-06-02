#include "curl.hpp"

#include "log.hpp"

#include <curl/curl.h>
#include <curl/easy.h>
#include <strings.h>  // strcasecmp (POSIX)

// Accumulate response bytes into a std::string.
static size_t writeCallback(const char* content, size_t size, size_t memberSize, std::string* data)
{
	data->append(content, size * memberSize);
	return size * memberSize;
}

int Curl::request(const char* method,
                  const char* url,
                  const std::vector<std::pair<std::string, std::string>>& headers,
                  const std::string& body,
                  std::string& out,
                  long& statusOut)
{
	statusOut = 0;

	CURL* handle = curl_easy_init();
	if (!handle)
		return CURLE_FAILED_INIT;

	curl_easy_setopt(handle, CURLOPT_URL, url);
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeCallback);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, &out);

	// Follow redirects (same behaviour OST WinHttp naturally follows).
	curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);

	// Method / body setup.
	const bool isPost = (method != nullptr && strcasecmp(method, "POST") == 0);
	if (isPost) {
		curl_easy_setopt(handle, CURLOPT_POST, 1L);
		curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.c_str());
		curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
	} else if (method != nullptr && strcasecmp(method, "GET") != 0) {
		// Support HEAD, PUT, DELETE, … if ever needed.
		curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method);
	}

	// Build curl_slist for extra headers.
	struct curl_slist* slist = nullptr;
	for (const auto& [k, v] : headers) {
		std::string header = k + ": " + v;
		slist = curl_slist_append(slist, header.c_str());
	}
	if (slist)
		curl_easy_setopt(handle, CURLOPT_HTTPHEADER, slist);

	CURLcode res = curl_easy_perform(handle);

	if (res == CURLE_OK)
		curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &statusOut);

	if (slist)
		curl_slist_free_all(slist);

	curl_easy_cleanup(handle);
	return static_cast<int>(res);
}

int Curl::getString(const char* url, std::string& out)
{
	long status = 0;
	static const std::vector<std::pair<std::string, std::string>> noHeaders;
	static const std::string noBody;
	return Curl::request("GET", url, noHeaders, noBody, out, status);
}
