#pragma once

#include <string>

#include <boost/filesystem.hpp>

#include "types.hpp"

namespace selvy
{
    namespace ocr
    {
		std::wstring get_install_path();
        //std::wstring get_module_name();
        std::string get_current_module_name();
        std::wstring get_data_path(const configuration& configuration);
        std::wstring get_storage_path(const configuration& configuration);
		std::wstring get_log_path(const configuration& configuration);
        int get_log_level(const configuration& configuration);
        std::wstring get_profile_root_directory(const configuration& configuration);
        std::wstring get_profile_directory(const configuration& configuration);
        std::wstring get_categories_directory(const configuration& configuration);
        std::wstring get_classification_directory(const configuration& configuration);
		std::wstring get_classification_data(const configuration& configuration);
        std::wstring get_classification_model(const configuration& configuration);
		int get_port_number(const configuration& configuration);
		std::wstring get_root_path(const configuration& configuration);
		std::wstring get_web_path();
        int get_check_timeout(const configuration& configuration);

        configuration load_configuration();
        configuration load_configuration(const std::wstring& profile);

        std::wstring to_wstring(const std::string& str, int codepage);
        std::string to_utf8(const std::wstring& str);
        std::string to_cp949(const std::wstring& str);

        std::string generate_secret_key(std::string::size_type length);
    }
}