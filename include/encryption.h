#pragma once

#include <string>

std::string encrypt_image(const std::string& image, const std::string& keyStr);
std::string decrypt_image(const std::string& image, const std::string& keyStr);
std::string encrypt_image_internal_key(const std::string& image);
std::string decrypt_image_internal_key(const std::string& image);
std::string encrypt_text(const std::string& textStr, const std::string& keyStr);
std::string decrypt_text(const std::string& textStr, const std::string& keyStr);

std::string generate_key();