#include <Windows.h>
#include <Shlwapi.h>
#include <TlHelp32.h>

#include <string>
#include <chrono>
#include <random>

#include <boost/filesystem.hpp>
#include <yaml-cpp/yaml.h>
#include <fmt/format.h>

#include "types.hpp"
#include "encryption.h"

namespace selvy
{
    namespace ocr
    {
        std::wstring
        to_wstring(const std::string& str, int codepage)
        {
            const auto num_chars = MultiByteToWideChar(codepage, 0, str.data(), str.size(), nullptr, 0);
            std::wstring buffer(num_chars, L'\0');
            MultiByteToWideChar(codepage, 0, str.data(), str.size(), const_cast<wchar_t*>(buffer.data()), num_chars);

            return buffer;
        }

        std::string
        to_utf8(const std::wstring& str)
        {
            if (str.empty())
                return std::string();

            const auto num_chars = WideCharToMultiByte(CP_UTF8, 0, &str[0], static_cast<int>(str.size()), nullptr, 0, nullptr, nullptr);
            std::string buffer(num_chars, L'\0');
            WideCharToMultiByte(CP_UTF8, 0, str.data(), str.size(), const_cast<char*>(buffer.data()), num_chars, nullptr, nullptr);

            return buffer;
        }

        std::string
        to_cp949(const std::wstring& str)
        {
            if (str.empty())
                return std::string();

            const auto num_chars = WideCharToMultiByte(CP_ACP, 0, &str[0], static_cast<int>(str.size()), nullptr, 0, nullptr, nullptr);
            std::string buffer(num_chars, L'\0');
            WideCharToMultiByte(CP_ACP, 0, str.data(), str.size(), const_cast<char*>(buffer.data()), num_chars, nullptr, nullptr);

            return buffer;
        }

        std::wstring
        get_install_path()
        {
            WCHAR dest[MAX_PATH];
            wmemset(dest, 0x00, MAX_PATH);
            GetModuleFileNameW(NULL, dest, MAX_PATH);
            PathRemoveFileSpecW(dest);

            std::wstring path(dest);
            return boost::filesystem::path(path).parent_path().native();
        }

        /*std::wstring
        get_module_name()
        {
            WCHAR dest[MAX_PATH];
            wmemset(dest, 0x00, MAX_PATH);
            GetModuleFileNameW(NULL, dest, MAX_PATH);

            std::wstring path(dest);
            return boost::filesystem::change_extension(boost::filesystem::path(path).filename(), "").wstring();
        }*/

        std::string
        get_current_module_name()
        {
            WCHAR dest[MAX_PATH];
            wmemset(dest, 0x00, MAX_PATH);
            GetModuleFileNameW(NULL, dest, MAX_PATH);

            std::wstring path(dest);
            return boost::filesystem::change_extension(boost::filesystem::path(path).filename(), "").string();
        }

        std::wstring
        get_data_path(const configuration& configuration)
        {
            return boost::filesystem::absolute(configuration.at(L"service").at(L"data path")).native();
        }

        std::wstring
        get_storage_path(const configuration& configuration)
        {
			return boost::filesystem::absolute(configuration.at(L"service").at(L"storage path")).native();
        }

        std::wstring
        get_log_path(const configuration& configuration)
        {
			return boost::filesystem::absolute(configuration.at(L"service").at(L"log path")).native();
        }

		int
		get_log_level(const configuration& configuration)
		{
			return std::stoi(configuration.at(L"service").at(L"log level"));
		}

        std::wstring
        get_profile_directory(const configuration& configuration)
        {
            return boost::filesystem::path(fmt::format(L"{}\\{}", get_data_path(configuration),
				configuration.at(L"setting").at(L"profile"))).native();
        }

        std::wstring
        get_categories_directory(const configuration& configuration)
        {
            return boost::filesystem::path(fmt::format(L"{}\\categories", get_profile_directory(configuration))).native();
        }

        std::wstring
        get_classification_directory(const configuration& configuration)
        {
            return boost::filesystem::path(fmt::format(L"{}\\classification", get_profile_directory(configuration))).native();
        }

		std::wstring
		get_classification_data(const configuration& configuration)
		{
			return boost::filesystem::path(fmt::format(L"{}\\classification.data", get_classification_directory(configuration))).native();
		}

		int
		get_port_number(const configuration& configuration)
		{
			return std::stoi(configuration.at(L"service").at(L"port"));
		}

		std::wstring
			get_root_path(const configuration& configuration)
		{
			return configuration.at(L"service").at(L"path");
		}

        std::wstring
            get_web_path()
        {
            auto web_path = boost::filesystem::path(L"..\\web");
            if (!boost::filesystem::exists(web_path))
                web_path = boost::filesystem::path(fmt::format(L"{}\\web", get_install_path())).native();
            return web_path.native();
        }

        int
            get_check_timeout(const configuration& configuration)
        {
            return std::stoi(configuration.at(L"service").at(L"check timeout"));
        }

        std::wstring
        get_classification_model(const configuration& configuration)
        {
            return boost::filesystem::path(fmt::format(L"{}\\classification.model", get_classification_directory(configuration))).native();
        }

        configuration
        load_configuration()
        {
            const static std::wstring DEFAULT_CONCURRENCY{L"2"};
            const static std::wstring DEFAULT_TIMEOUT{
                std::to_wstring(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::minutes(10)).count())
            };
			const static std::wstring DEFAULT_CHECK_TIMEOUT{
				std::to_wstring(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::minutes(60)).count())
			};
            const static std::wstring DEFAULT_PORT{L"80"};
            const static std::wstring DEFAULT_PATH(L"/");
			const static std::wstring DEFAULT_BATCHMODE{ L"0" };
			const static std::wstring DEFAULT_LOGLEVEL{ L"2" }; // 0:trace, 1:debug, 2:info, 3:warn, 4:err, 5:critical, 6:off

            const static configuration DEFAULT_CONFIGURATIONS{
				std::make_pair(std::wstring(L"engine"),
				std::unordered_map<std::wstring, std::wstring>{
					std::make_pair(std::wstring(L"license"), std::wstring(L"")),
					std::make_pair(std::wstring(L"profile"), std::wstring(L"")),
					std::make_pair(std::wstring(L"concurrency"), DEFAULT_CONCURRENCY),
					std::make_pair(std::wstring(L"batchmode"), L"0"),
				}),
                std::make_pair(std::wstring(L"service"),
				std::unordered_map<std::wstring, std::wstring>{
						std::make_pair(std::wstring(L"port"), std::wstring(DEFAULT_PORT)),
						std::make_pair(std::wstring(L"data path"), std::wstring(L"")),
						std::make_pair(std::wstring(L"storage path"), std::wstring(L"")),
						std::make_pair(std::wstring(L"log path"), std::wstring(L"")),
						std::make_pair(std::wstring(L"log level"), std::wstring(L"2")),
						std::make_pair(std::wstring(L"timeout"), DEFAULT_TIMEOUT),
						std::make_pair(std::wstring(L"check timeout"), DEFAULT_CHECK_TIMEOUT),
						std::make_pair(std::wstring(L"path"), std::wstring(DEFAULT_PATH)),
				})
            };

            auto configurations = DEFAULT_CONFIGURATIONS;
            auto config_path = boost::filesystem::path(L"configuration.yaml");
            if (!boost::filesystem::exists(config_path))
                config_path = boost::filesystem::path(fmt::format(L"{}\\bin\\configuration.yaml", get_install_path()));

            if (boost::filesystem::exists(config_path) == false)
                return configurations;

            const auto config = YAML::LoadFile(to_utf8(config_path.native()));

            if (config["engine"]) {
                configurations[L"engine"][L"license"] = config["engine"]["license"].as<std::wstring>(L"");
                configurations[L"engine"][L"profile"] = config["engine"]["profile"].as<std::wstring>(L"");
                configurations[L"engine"][L"concurrency"] = config["engine"]["concurrency"].as<std::wstring>(DEFAULT_CONCURRENCY);
                configurations[L"engine"][L"batchmode"] = config["engine"]["batchmode"].as<std::wstring>(L"0");
            }

            if (config["service"]) {
                configurations[L"service"][L"port"] = config["service"]["port"].as<std::wstring>(DEFAULT_PORT);
                configurations[L"service"][L"path"] = config["service"]["path"].as<std::wstring>(DEFAULT_PATH);
                configurations[L"service"][L"data path"] = config["service"]["data path"].as<std::wstring>(L"");
                configurations[L"service"][L"storage path"] = config["service"]["storage path"].as<std::wstring>(L"");
                configurations[L"service"][L"log path"] = config["service"]["log path"].as<std::wstring>(L"");
                configurations[L"service"][L"log level"] = config["service"]["log level"].as<std::wstring>(L"");
                configurations[L"service"][L"timeout"] = config["service"]["timeout"].as<std::wstring>(DEFAULT_TIMEOUT);
				configurations[L"service"][L"check timeout"] = config["service"]["check timeout"].as<std::wstring>(DEFAULT_CHECK_TIMEOUT);
            }

            return configurations;
        }

        configuration
        load_configuration(const std::wstring& profile)
        {
            auto configurations = load_configuration();

            configurations[L"setting"] = [&]()
            {
                return std::unordered_map<std::wstring, std::wstring>{
                    std::make_pair(std::wstring(L"profile"), std::wstring(profile)),
                };
            }();

            return configurations;
        }

		void
		terminate_abbyy_process()
		{
			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
			if (snapshot == INVALID_HANDLE_VALUE) {
				return;
			}

			PROCESSENTRY32 entry;
			entry.dwSize = sizeof(PROCESSENTRY32);
			if (!Process32First(snapshot, &entry)) {
				CloseHandle(snapshot);
				return;
			}

			while (Process32Next(snapshot, &entry)) {
				std::string search_file(entry.szExeFile);
				if (search_file == "FREngine.exe") {
					HANDLE process = OpenProcess(PROCESS_TERMINATE, 0, (DWORD)entry.th32ProcessID);
					if (process != NULL) {
						TerminateProcess(process, 9);
						CloseHandle(process);
					}
				}
			}

			CloseHandle(snapshot);
		}

        /*std::string
        generate_secret_key(std::string::size_type length)
        {
            static auto& chars = "0123456789"
                "abcdefghijklmnopqrstuvwxyz"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "!@#$%^&*";

            static std::mt19937 random_device{std::random_device{}()};
            static std::uniform_int_distribution<std::string::size_type> pick(0, sizeof(chars) - 2);

            std::string s;

            s.reserve(length);

            while (length--)
                s += chars[pick(random_device)];

            return s;
        }*/
    }
}
