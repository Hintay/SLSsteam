#include "utils.hpp"

#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <openssl/sha.h>

std::vector<std::string> Utils::strsplit(char *str, const char *delimeter)
{
	auto splits = std::vector<std::string>();

	char* split = strtok(str, delimeter);
	splits.emplace(splits.end(), std::string(split));

	while(split)
	{
		split = strtok(nullptr, delimeter);
		if (!split)
		{
			break;
		}

		splits.emplace(splits.end(), std::string(split));
	}

	return splits;
}

std::string Utils::getFileSHA256(const char *filePath)
{
	std::ifstream fs(filePath, std::ios::binary);
	if (!fs.is_open())
	{
		//TODO: Read more about error types in C++ :)
		throw std::runtime_error("Unable to read file!");
	}

	SHA256_CTX ctx;
	if (SHA256_Init(&ctx) != 1)
	{
		throw std::runtime_error("Unable to initialize SHA256!");
	}

	char buffer[64 * 1024];
	while (fs.good())
	{
		fs.read(buffer, sizeof(buffer));
		const std::streamsize read = fs.gcount();
		if (read > 0 && SHA256_Update(&ctx, buffer, static_cast<size_t>(read)) != 1)
		{
			throw std::runtime_error("Unable to update SHA256!");
		}
	}
	if (fs.bad())
	{
		throw std::runtime_error("Unable to read complete file!");
	}

	unsigned char sha256Bytes[SHA256_DIGEST_LENGTH];
	if (SHA256_Final(sha256Bytes, &ctx) != 1)
	{
		throw std::runtime_error("Unable to finalize SHA256!");
	}

	std::stringstream sha256;
	for(int i = 0; i < SHA256_DIGEST_LENGTH; i++)
	{
		sha256 << std::hex << std::setw(2) << std::setfill('0') << (int)sha256Bytes[i];
	}

	fs.close();
	return sha256.str();
}
