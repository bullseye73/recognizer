#pragma once

#include <string>
#include <iostream>
#include <unordered_map>

#include <string.h>
#include <spdlog/spdlog.h>

#include <yaml-cpp/yaml.h>
#include <opencv2/opencv.hpp>
#include <dlib/serialize.h>

#define __FILENAME__ (strrchr(__FILE__,'\\') + 1)
#define SV_LOG(type, level, fmt, ...) ((void)(selvy::ocr::write_log_info(type, level, fmt, __VA_ARGS__, __FILENAME__, __LINE__)))

namespace selvy
{
    namespace ocr
    {
        using configuration = std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::wstring>>;
        using character = std::pair<cv::Rect, wchar_t>;
        using word = std::vector<character>;
        using line = std::vector<word>;
        using block = std::vector<line>;

        std::wstring to_wstring(const std::string& str, int codepage = CP_UTF8);
        std::string to_utf8(const std::wstring& str);
		std::string to_cp949(const std::wstring& str);

		template<typename... Args>
		inline void
		write_log_info(const std::string &type, const spdlog::level::level_enum level, const std::string &fmt, const Args &... args)
		{
            auto configuration = load_configuration();
            if (level < get_log_level(configuration)) {
                return;
            }

			std::shared_ptr<spdlog::logger> logger = spdlog::get(type);
			if (logger != nullptr) {
				std::string format = std::string(fmt).append(" [{}:{}]");
				switch (level) {
				case spdlog::level::trace:
					logger->trace(format.c_str(), args...);
					break;
				case spdlog::level::debug:
					logger->debug(format.c_str(), args...);
					break;
				case spdlog::level::info:
					logger->info(format.c_str(), args...);
					break;
				case spdlog::level::warn:
					logger->warn(format.c_str(), args...);
					break;
				case spdlog::level::err:
					logger->error(format.c_str(), args...);
					break;
				case spdlog::level::critical:
					logger->critical(format.c_str(), args...);
					break;
				case spdlog::level::off:
				default:
					break;
				}
			}
		}
    }
}

namespace YAML
{
    template <>
    struct convert<std::wstring>
    {
        static Node
        encode(const std::wstring& rhs)
        {
            return Node(selvy::ocr::to_utf8(rhs));
        }

        static bool
        decode(const Node& node, std::wstring& rhs)
        {
            if (!node.IsScalar()) {
                return false;
            }

            rhs = selvy::ocr::to_wstring(node.as<std::string>());

            return true;
        }
    };
}

namespace dlib
{
    inline void
    serialize(const cv::Rect& item, std::ostream& out)
    {
        try {
            serialize(item.x, out);
            serialize(item.y, out);
            serialize(item.width, out);
            serialize(item.height, out);
        } catch (serialization_error& e) {
            throw serialization_error(fmt::format("{}\n while serializaing an ojbect of type cv::Rect", e.info));
        }
    }

    inline void
    deserialize(cv::Rect& item, std::istream& in)
    {
        try {
            deserialize(item.x, in);
            deserialize(item.y, in);
            deserialize(item.width, in);
            deserialize(item.height, in);
        } catch (serialization_error& e) {
            throw serialization_error(fmt::format("{}\n while deserializing on object of type cv::Rect", e.info));
        }
    }
}
