#pragma once

#include <memory>
#include <vector>
#include <unordered_map>
#include <string>

#include <boost/filesystem.hpp>
#include <opencv2/opencv.hpp>


#define ORIENTATION_VALUE VARIANT_FALSE
//#define ORIENTATION_VALUE VARIANT_TRUE

#define OUTPUT_FOLDER L"E:\\2019_PROJECT\\99.ETC\\Ãß½É\\output\\"

namespace selvy
{
    namespace ocr
    {
        struct recognizer
        {
            virtual ~recognizer() = default;
			virtual std::pair<std::wstring, int> recognize(const std::string& buffer, int languages, const std::string& secret="") = 0;
            virtual std::pair<std::wstring, int> recognize(const std::string& buffer, const std::wstring& type, const std::string& secret = "") = 0;
            virtual std::unordered_map<std::wstring, std::vector<std::wstring>> recognize(const boost::filesystem::path& path, const std::string& secret = "") = 0;
			virtual std::unordered_map<std::wstring, std::vector<std::wstring>> recognize(const boost::filesystem::path& path, const std::wstring& class_name =L"") = 0;
        };

        struct recognizer_factory
        {
			static void deinitialize();
            static void initialize();
            static std::unique_ptr<recognizer> create(const std::wstring& profile);
        };
    }
}
