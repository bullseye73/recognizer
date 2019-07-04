
#define _WINSOCKAPI_

#include <Windows.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <zipper/zipper.h>
#include <zipper/unzipper.h>

#include <fmt/format.h>

#include <Poco/Runnable.h>
#include <Poco/NotificationQueue.h>

#include <Poco/CountingStream.h>
#include <Poco/FileStream.h>
#include <Poco/StreamCopier.h>
//#include <Poco/Net/NameValueCollection.h>
#include <Poco/Net/PartHandler.h>

#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/NetworkInterface.h>
#include <Poco/Util/ServerApplication.h>

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Base64Encoder.h>
#include <Poco/Base64Decoder.h>
#include <Poco/Timestamp.h>
#include <Poco/Timezone.h>
#include <Poco/URI.h>

#include <time.h>

#include <list>
#include <random>

#include "encryption.h"
#include "recognizer.hpp"
#include "utils.hpp"
#include "rrdtool.h"

namespace selvy
{
    namespace ocr
    {
        static unsigned long long req_total_bytes_ = 0L;
        static unsigned long long req_total_count_ = 0L;
        static unsigned long long req_fax_count_ = 0L;
        static unsigned long long req_trade_count_ = 0L;
        static unsigned long long req_doc_count_ = 0L;
        static bool force_stop_ = false;

        static std::string encode_password(const std::string& plain, const std::string& salt)
        {
            std::stringstream ss;
            Poco::Base64Encoder b64enc(ss);
            b64enc << encrypt_text(plain, salt);
            b64enc.close();

            return ss.str();
        }

        static std::string decode_password(const std::string& encoded, const std::string& salt)
        {
            std::stringstream ss(encoded);
            Poco::Base64Decoder b64dec(ss);
            std::string buffer;

            int ch = b64dec.get();
            while (ch != std::char_traits<char>::eof()) {
                buffer += (char)ch;
                ch = b64dec.get();
            }

            return decrypt_text(buffer, salt);
        }

        /*static boost::filesystem::path unzip(std::vector<unsigned char>& buffer, const boost::filesystem::path& tgt)
        {
            zipper::Unzipper unzipper(buffer);

            const auto entries = unzipper.entries();
            const boost::filesystem::path destination_folder = fmt::format(L"{}", tgt.native());
            if (!boost::filesystem::exists(destination_folder))
                boost::filesystem::create_directories(destination_folder);

            for (const auto& entry : entries) {
                std::vector<unsigned char> unzipped_entry;
                unzipper.extractEntryToMemory(entry.name, unzipped_entry);

                std::string image(std::begin(unzipped_entry), std::end(unzipped_entry));

                boost::filesystem::path image_path = fmt::format(L"{}\\{}", destination_folder.native(), to_wstring(entry.name, CP_ACP));
                std::ofstream image_file(image_path.native(), std::ios::binary);
                image_file.write(image.c_str(), image.size());
                image_file.close();
            }

            unzipper.close();

            return destination_folder;
        }*/

        static std::unordered_map<std::wstring, std::vector<std::wstring>>
        run_recognition(const std::wstring& type, const std::string& buffer, int languages, const std::string& secret)
        {
            SV_LOG("service", spdlog::level::info, "Starting recognition...");

            CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            std::unordered_map<std::wstring, std::vector<std::wstring>> fields;

            auto recognizer = selvy::ocr::recognizer_factory::create(type);
            if (type != L"document") {
                fields = recognizer->recognize(buffer, secret);
            } else {
                std::wstring contents;
                int confidence;
                std::tie(contents, confidence) = recognizer->recognize(buffer, languages, secret);
                fields.emplace(std::make_pair(L"contents", std::vector<std::wstring>{ contents }));
                fields.emplace(std::make_pair(L"confidence", std::vector<std::wstring>{ std::to_wstring(confidence) }));
            }

            CoUninitialize();
            SV_LOG("service", spdlog::level::info, "Finished recognition...");

            return fields;
        }

        void update_monitor_date()
        {
            Poco::Timestamp now;

            update_request_bytes(now.epochTime(), req_total_bytes_);
            update_request_count(now.epochTime(), req_total_count_);
            update_recognition_type(now.epochTime(), req_fax_count_, req_trade_count_, req_doc_count_);
            req_total_bytes_ = req_total_count_ = req_fax_count_ = req_trade_count_ = req_doc_count_ = 0;
        }

        void get_file_stream(const std::wstring &path, const bool &encoding, const bool &deleting, std::ostringstream &stream)
        {
            stream.str("");
            if (boost::filesystem::exists(path)) {
                std::wstring ext = path.substr(to_utf8(path).find_last_of(".") + 1);

                std::ifstream file;
                if (ext == L"log") {
                    file = std::ifstream(path);
                } else {
                    file = std::ifstream(path, std::ios_base::binary);
                }

                if (encoding) {
                    std::stringstream encoder_str;
                    Poco::Base64Encoder encoder(encoder_str);
                    encoder << file.rdbuf();
                    stream << encoder_str.str();
                } else {
                    stream << file.rdbuf();
                }
                file.close();

                if (deleting) {
                    boost::filesystem::remove(path);
                }
            }
        }

        class SPartHandler : public Poco::Net::PartHandler
        {
        public:
            SPartHandler()
            {
            }

            void handlePart(const Poco::Net::MessageHeader &header, std::istream &stream)
            {
                if (header.has("Content-Disposition")) {
                    std::string disp;
                    Poco::Net::NameValueCollection params;
                    Poco::Net::MessageHeader::splitParameters(header["Content-Disposition"], disp, params);
                    std::string file_name = params.get("filename", "unnamed.jpg");

                    if (file_name.empty() == false) {
                        file_name_ = to_cp949(boost::filesystem::path(file_name).filename().native());
                        Poco::StreamCopier::copyStream(stream, buffer_);
                    }
                }
            }
            const std::string buffer() const { return buffer_.str(); };
            const std::string& file_name() const { return file_name_; }

        private:
            std::stringstream buffer_;
            std::string file_name_;
        };

        class SOCRRequestHandler : public Poco::Net::HTTPRequestHandler
        {
        public:
            void handleRequest(Poco::Net::HTTPServerRequest &request, Poco::Net::HTTPServerResponse &response)
            {
                try {
                    std::string request_ip = request.clientAddress().toString();
                    SV_LOG("service", spdlog::level::info, "Request from {}. [URI:{}] [Request-Size:{}]", request_ip, request.getURI(), request.getContentLength());

                    // setting request
                    SPartHandler partHandler;
                    Poco::Net::HTMLForm form(request, request.stream(), partHandler);

                    std::wstring doc_type = L"";
                    int languages = 0;

                    if (form.empty() == false) {
                        Poco::Net::NameValueCollection::ConstIterator it = form.begin();
                        Poco::Net::NameValueCollection::ConstIterator end = form.end();
                        for (; it != end; ++it) {
                            std::string name = it->first;
                            std::string value = it->second;
                            if (name.compare("doctype") == 0) {
                                doc_type = selvy::ocr::to_wstring(value);
                            } else if (name.compare("languages") == 0) {
                                languages = std::stoi(value);
                            } else {
                                //SV_LOG("service", spdlog::level::info, "{}: {}", name, value);
                            }
                        }
                    }
                    SV_LOG("service", spdlog::level::info, "Request from {}. [Type:{}] [Languages:{}]", request_ip, to_utf8(doc_type), languageToString(languages));

                    req_total_count_++;
                    req_total_bytes_ += request.getContentLength();
                    if (doc_type == L"document") req_doc_count_++;
                    else if (doc_type == L"fax") req_fax_count_++;
                    else if (doc_type == L"trade") req_trade_count_++;

                    std::wstring path = L"";
                    if (partHandler.file_name().empty() == false) {
                        path = selvy::ocr::to_wstring(partHandler.file_name());
                    }

                    // recognition
                    int total_count = 0;
                    Poco::JSON::Array::Ptr array = new Poco::JSON::Array();

                    if (doc_type.empty() == false && path.empty() == false) {
                        cv::TickMeter recognition_ticks;
                        recognition_ticks.start();
                        std::unordered_map<std::wstring, std::vector<std::wstring>> results = run_recognition(doc_type, partHandler.buffer(), languages, generate_key());
                        recognition_ticks.stop();

                        SV_LOG("service", spdlog::level::info, "recognition time : {:.2f}Sec", recognition_ticks.getTimeSec());

                        for (auto field : results) {
                            int count = 0;
                            Poco::JSON::Array::Ptr values = new Poco::JSON::Array();
                            for (auto it : field.second) {
                                values->set(count++, selvy::ocr::to_utf8(it));
                            }

                            Poco::JSON::Object::Ptr obj = new Poco::JSON::Object();
                            obj->set(selvy::ocr::to_utf8(field.first), values);

                            array->set(total_count++, obj);
                        }
                    }

                    // response
                    response.setChunkedTransferEncoding(true);
                    response.setContentType("application/json");
                    std::stringstream ss;
                    array->stringify(ss);
                    response.setContentLength(ss.str().size());
                    response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);

                    std::ostream &responseStream = response.send();
                    array->stringify(responseStream);
                    responseStream.flush();

                    SV_LOG("service", spdlog::level::info, "Response to {}", request.clientAddress().toString());
                } catch (Poco::Exception &exception) {
                    SV_LOG("service", spdlog::level::err, "Exception: code={}, message={}", std::to_string(exception.code()), exception.displayText());
                }
            }
        private:
            std::string languageToString(const int languages)
            {
                std::string lan = "";
                switch (languages) {
                case 0:
                    lan = "all"; break;
                case 1:
                    lan = "english"; break;
                case 2:
                    lan = "hangul"; break;
                case 3:
                    lan = "digit"; break;
                default:
                    break;
                }
                return lan;
            }
        };

        class SMonitorRequestHandler : public Poco::Net::HTTPRequestHandler
        {
        public:
            SMonitorRequestHandler(const std::string &path) : file_name_(""), ext_name_(""), is_login_page_(false), login_step_(false), is_linked_file_(false)
            {
                if (path.find("/") != std::string::npos) {
                    int last_slash_index = path.find_last_of("/");
                    file_name_ = path.substr(last_slash_index + 1, path.length() - last_slash_index);
                } else {
                    file_name_ = path;
                }

                if (file_name_.find(".") != std::string::npos) {
                    int last_dot_index = file_name_.find_last_of(".");
                    ext_name_ = file_name_.substr(last_dot_index + 1, file_name_.length() - last_dot_index);
                    if (ext_name_ == "js" || ext_name_ == "css") {
                        is_linked_file_ = true;
                    }
                    if (file_name_ == "login.html") {
                        is_login_page_ = true;
                    }
                } else {
                    if (file_name_ == "login") {
                        login_step_ = true;
                        file_name_ = "";
                    }
                }
            }
            void handleRequest(Poco::Net::HTTPServerRequest &request, Poco::Net::HTTPServerResponse &response)
            {
                try {
                    SV_LOG("service", spdlog::level::info, "Request from {}. [URI:{]}", request.clientAddress().toString(), request.getURI());

                    // get login-form data
                    std::wstring username = L"";
                    std::wstring password = L"";
                    std::unordered_map<std::string, std::string> param_map;
                    Poco::Net::HTMLForm form(request, request.stream());
                    if (form.empty() == false) {
                        Poco::Net::NameValueCollection::ConstIterator it = form.begin();
                        Poco::Net::NameValueCollection::ConstIterator end = form.end();
                        for (; it != end; ++it) {
                            std::string name = it->first;
                            std::string value = it->second;
                            if (name.compare("username") == 0) {
                                username = to_wstring(value);
                            } else if (name.compare("password") == 0) {
                                std::stringstream ss(value);
                                Poco::Base64Decoder decoder(ss);
                                std::ostringstream ostr;
                                std::copy(std::istreambuf_iterator<char>(decoder), std::istreambuf_iterator<char>(), std::ostreambuf_iterator<char>(ostr));
                                password = to_wstring(ostr.str());
                            } else {
                                SV_LOG("service", spdlog::level::debug, "form [{}:{}]", name, value);
                                param_map[name] = value;
                            }
                        }
                    }

                    // check cookie
                    if (is_login_page_ == false && is_linked_file_ == false && username.empty()) {
                        Poco::Net::NameValueCollection cookies;
                        request.getCookies(cookies);
                        Poco::Net::NameValueCollection::ConstIterator it = cookies.find("omid");
                        if (it != cookies.end()) {
                            std::istringstream istr(it->second);
                            Poco::Base64Decoder decoder(istr);
                            std::ostringstream ostr;
                            std::copy(std::istreambuf_iterator<char>(decoder), std::istreambuf_iterator<char>(), std::ostreambuf_iterator<char>(ostr));
                            std::string value = decode_password(ostr.str(), generate_key());
                            username = to_wstring(value);
                        }
                    }

                    // validate username
                    Poco::Net::HTTPResponse::HTTPStatus status = Poco::Net::HTTPResponse::HTTP_OK;
                    std::stringstream stream;
                    if (is_login_page_ == false && is_linked_file_ == false) {
                        if (validateData("user", username) == true) {
                            if (login_step_ && password.empty() == false) {
                                std::ostringstream str;
                                Poco::Base64Encoder encoder(str);
                                encoder << encode_password(to_utf8(username), generate_key());
                                encoder.close();

                                Poco::Net::HTTPCookie cookie("omid", str.str());
                                response.addCookie(cookie);
                            }
                        } else {
                            status = Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED;
                            stream << "The user name is invalid.";
                        }
                    }

                    // set response data
                    if (status == Poco::Net::HTTPResponse::HTTP_OK) {
                        if (param_map.size() > 0) {  // query
                            std::string type = (param_map.find("type") != param_map.end()) ? param_map["type"] : "";

                            Poco::JSON::Object json;
                            if (type == "ip-list") {
                                std::list<std::wstring> ip_list;
                                getAccessDataList("server", ip_list);
                                int count = 0;
                                for (const std::wstring ip : ip_list) {
                                    json.set(std::to_string(count++), ip);
                                }
                            } else {
                                std::list<std::wstring> ip_list;
                                std::string days = "1";

                                if (type == "log") {
                                    std::string ip = (param_map.find("ip") != param_map.end()) ? param_map["ip"] : "";
                                    ip_list.push_back(to_wstring(ip));
                                } else if (type == "graph") {
                                    if (param_map.find("ip") == param_map.end()) {  // main-server
                                        getAccessDataList("server", ip_list);
                                    } else {  // remote-server
                                        ip_list.push_back(to_wstring(param_map["ip"]));
                                    }
                                    days = (param_map.find("days") != param_map.end()) ? param_map["days"] : "";
                                }
                                else {
                                    SV_LOG("service", spdlog::level::warn, "unknown query. [type:{}]", type);
                                }

                                for (const std::wstring ip : ip_list) {
                                    std::ostringstream info_stream;
                                    if (getInfo(type, to_utf8(ip), to_utf8(username), days, info_stream) == false) {
                                        SV_LOG("service", spdlog::level::err, "can't get monitoring information. [{}]", info_stream.str());
                                        status = Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR;
                                    }
                                    json.set(to_utf8(ip), info_stream.str());
                                }
                            }

                            json.stringify(stream);
                            response.setContentType("application/json");
                        } else if (file_name_.empty() == false) {  // get file info
                            if (ext_name_ == "css") {
                                response.setContentType("text/css");
                            } else if (ext_name_ == "png") {
                                response.setContentType("image/png");
                            } else if (ext_name_ == "html") {
                                response.setContentType("text/html");
                            }

                            auto path = boost::filesystem::path(fmt::format(L"{}\\{}", selvy::ocr::get_web_path(), to_wstring(file_name_))).native();
                            SV_LOG("service", spdlog::level::debug, to_utf8(path));
                            if (boost::filesystem::exists(path) == false) {
                                status = Poco::Net::HTTPResponse::HTTP_NOT_FOUND;
                            } else {
                                std::ifstream file(to_utf8(path));
                                stream << file.rdbuf();
                                file.close();
                            }
                        }
                    }

                    response.setStatus(status);
                    response.setChunkedTransferEncoding(true);
                    response.setContentLength(stream.str().length());

                    std::ostream &responseStream = response.send();
                    Poco::StreamCopier::copyStream(stream, responseStream);
                    responseStream.flush();
                    stream.clear();

                    SV_LOG("service", spdlog::level::info, "Response to {}", request.clientAddress().toString());
                } catch (Poco::Exception &exception) {
                    SV_LOG("service", spdlog::level::err, "Exception: {}", exception.displayText());
                }
            }
        private:
            Poco::JSON::Object::Ptr getJsonObject(const std::string &json)
            {
                Poco::JSON::Object::Ptr obj = nullptr;

                Poco::JSON::Parser parser;
                Poco::Dynamic::Var result = parser.parse(json);
                if (result.isEmpty() == false) {
                    obj = result.extract<Poco::JSON::Object::Ptr>();
                }
                return obj;
            }

            void getAccessDataList(const std::string type, std::list<std::wstring>& list)
            {
                list.clear();

                std::wstring config_path = boost::filesystem::path(fmt::format(L"{}\\monitor.yaml", selvy::ocr::get_web_path())).native();
                if (boost::filesystem::exists(config_path) == false) {
                    SV_LOG("service", spdlog::level::err, "no exist file. [{}]", to_utf8(config_path));
                    return;
                }

                const auto config = YAML::LoadFile(to_utf8(config_path));
                if (config[type]) {
                    const YAML::Node& nodes = config[type];
                    for (YAML::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
                        const YAML::Node& node = *it;
                        std::wstring read_data = node.as<std::wstring>(L"");
                        list.push_back(read_data);
                    }
                }
            }
            bool validateData(const std::string type, const std::wstring value)
            {
                SV_LOG("service", spdlog::level::debug, "[validateData] type:{}, value:{}", type, to_utf8(value));

                std::list<std::wstring> list;
                getAccessDataList(type, list);

                bool valid = false;

                std::list<std::wstring>::iterator it = std::find(list.begin(), list.end(), value);
                if (it != list.end()) {
                    valid = true;
                }
                return valid;
            }
            bool getInfo(const std::string type, const std::string ip, const std::string username, const std::string days, std::ostringstream &info)
            {
                if (ip.empty() || (type == "log" && validateData("server", to_wstring(ip)) == false)) {
                    info << "invalid ip address. [" + ip + "]";
                    return false;
                }

                std::vector<std::pair<std::string, std::string>> info_list;

                auto configuration = selvy::ocr::load_configuration();
                if (isLocalIP(ip) == true) {
                    SV_LOG("service", spdlog::level::info, "Local Monitor Info... [{}]", type);

                    if (type == "log") {
                        std::wstring log_path = boost::filesystem::path(fmt::format(L"{}\\service.log", selvy::ocr::get_log_path(configuration))).native();
                        std::ostringstream file_stream;
                        get_file_stream(log_path, false, false, file_stream);
                        info_list.push_back(std::make_pair("service", file_stream.str()));

                        log_path = boost::filesystem::path(fmt::format(L"{}\\recognizer.log", selvy::ocr::get_log_path(configuration))).native();
                        get_file_stream(log_path, false, false, file_stream);
                        info_list.push_back(std::make_pair("recognizer", file_stream.str()));
                    } else if (type == "graph") {
                        std::vector<std::pair<std::string, std::wstring>> graph_list;
                        export_graph(to_wstring(days), graph_list);

                        for (std::vector<std::pair<std::string, std::wstring>>::iterator iter = graph_list.begin(); iter != graph_list.end(); iter++) {
                            std::ostringstream file_stream;
                            get_file_stream(iter->second, true, true, file_stream);
                            info_list.push_back(std::make_pair(iter->first, file_stream.str()));
                        }
                    } else {
                        //return false;
                    }
                } else {
                    SV_LOG("service", spdlog::level::info, "Request Remote Info... [{}, {}]", ip, type);

                    int index = ip.find_last_of(":");
                    std::string ip_addr = ip.substr(0, index);
                    int port = atoi(ip.substr(index + 1).c_str());

                    Poco::URI uri;
                    uri.setScheme("http");
                    uri.setAuthority(ip_addr);
                    uri.setPort(port);

                    Poco::Net::HTTPClientSession session(uri.getHost(), uri.getPort());
                    session.setKeepAlive(true);

                    std::wstring path = selvy::ocr::get_root_path(configuration) + L"/monitor/info";
                    Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_POST, to_utf8(path), Poco::Net::HTTPMessage::HTTP_1_1);

                    Poco::Net::HTMLForm form;
                    form.set("username", username);
                    form.set("type", type);
                    form.set("ip", ip);
                    form.set("days", days);
                    form.prepareSubmit(request);

                    long timeout = std::stol(configuration[L"service"][L"timeout"]);
                    session.setTimeout(Poco::Timespan(timeout * 1000));
                    std::ostream &request_stream = session.sendRequest(request);
                    form.write(request_stream);

                    // response
                    Poco::Net::HTTPResponse response;
                    std::istream &response_stream = session.receiveResponse(response);

                    const std::string stream_str((std::istreambuf_iterator<char>(response_stream)), (std::istreambuf_iterator<char>()));
                    Poco::JSON::Object::Ptr json_ip_obj = getJsonObject(stream_str);
                    if (json_ip_obj.isNull() == false) {
                        Poco::JSON::Object::Ptr json_info_obj = getJsonObject(json_ip_obj->get(ip).toString());
                        if (json_info_obj.isNull() == false) {
                            for (Poco::JSON::Object::Iterator it = json_info_obj->begin(); it != json_info_obj->end(); ++it) {
                                info_list.push_back(std::make_pair(it->first, it->second.toString()));
                            }
                        }
                    }
                }

                Poco::JSON::Object json;
                for (std::vector<std::pair<std::string, std::string>>::iterator iter = info_list.begin(); iter != info_list.end(); iter++) {
                    json.set(iter->first, iter->second);
                }
                info_list.clear();

                json.stringify(info);

                return true;
            }
            bool isLocalIP(const std::string ip)
            {
                if (ip.rfind("localhost") == 0) {
                    return true;
                }

                bool valid = false;

                Poco::Net::NetworkInterface::Map map = Poco::Net::NetworkInterface::map(false, false);
                for (Poco::Net::NetworkInterface::Map::const_iterator it = map.begin(); it != map.end(); ++it) {
                    Poco::Net::NetworkInterface::MACAddress mac(it->second.macAddress());
                    if (mac.empty() || (it->second.type() == Poco::Net::NetworkInterface::NI_TYPE_SOFTWARE_LOOPBACK)) {
                        continue;
                    }

                    const Poco::Net::NetworkInterface::AddressList &list = it->second.addressList();
                    Poco::Net::NetworkInterface::AddressList::const_iterator ipit = list.begin();
                    for (int count = 0; ipit != list.end(); ++ipit, ++count) {
                        std::string addr = ipit->get<Poco::Net::NetworkInterface::IP_ADDRESS>().toString();

                        if (ip.rfind(addr) == 0 && ip.at(addr.length()) == ':') {
                            valid = true;
                            break;
                        }
                    }
                    if (valid) break;
                }
                return valid;
            }

            std::string file_name_;
            std::string ext_name_;
            bool is_login_page_;
            bool login_step_;
            bool is_linked_file_;
        };

        class SManagerRequestHandler : public Poco::Net::HTTPRequestHandler
        {
        public:
            SManagerRequestHandler(const std::string type)
            {
                type_ = type;
            }

            void handleRequest(Poco::Net::HTTPServerRequest &request, Poco::Net::HTTPServerResponse &response)
            {
                try {
                    SV_LOG("service", spdlog::level::debug, "Request from {}. [URI:{}] [TYPE:{}]", request.clientAddress().toString(), request.getURI(), type_);

                    // get data
                    SPartHandler partHandler;
                    Poco::Net::HTMLForm form(request, request.stream(), partHandler);

                    std::wstring username = L"";
                    std::wstring password = L"";
                    boost::filesystem::path path = "";
                    if (form.empty() == false) {
                        Poco::Net::NameValueCollection::ConstIterator it = form.begin();
                        Poco::Net::NameValueCollection::ConstIterator end = form.end();
                        for (; it != end; ++it) {
                            std::string name = it->first;
                            std::string value = it->second;
                            if (name.compare("username") == 0) {
                                username = to_wstring(value);
                            } else if (name.compare("password") == 0) {
                                std::istringstream istr(value);
                                Poco::Base64Decoder decoder(istr);
                                std::ostringstream ostr;
                                std::copy(std::istreambuf_iterator<char>(decoder), std::istreambuf_iterator<char>(), std::ostreambuf_iterator<char>(ostr));

                                password = to_wstring(decrypt_text(ostr.str(), generate_key()));
                            } else if (name.compare("dest") == 0 || name.compare("src") == 0) {
                                path = boost::filesystem::path(value);
                            }
                        }
                    }

                    Poco::Net::HTTPResponse::HTTPStatus status = Poco::Net::HTTPResponse::HTTP_OK;
                    std::stringstream stream;

                    // check user-authentication
                    if (validateData("user", username) == false) {
                        status = Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED;
                        stream << "The user name is invalid.";
                    }

                    // set response data
                    if (status == Poco::Net::HTTPResponse::HTTP_OK) {
                        auto configuration = load_configuration();
                        auto user_files_path = boost::filesystem::absolute(boost::filesystem::path(fmt::format(L"{}\\..\\user-files", selvy::ocr::get_storage_path(configuration))));

                        if (type_ == "upload") {
                            if (partHandler.buffer().size() > 0) {
                                auto decrypted = decrypt_image(partHandler.buffer(), generate_key());

                                auto dest_path = (path.empty()) ? user_files_path : path;
                                if (boost::filesystem::exists(dest_path) == false) {
                                    boost::filesystem::create_directories(dest_path);
                                }

                                auto file_path = boost::filesystem::path(fmt::format(L"{}\\{}", dest_path.native(), to_wstring(partHandler.file_name()))).native();
                                std::ofstream file(file_path, std::ios_base::binary);
                                file.write(decrypted.c_str(), decrypted.size());
                                file.close();

                                STARTUPINFOW si = {};
                                si.cb = sizeof(STARTUPINFOW);
                                GetStartupInfoW(&si);
                                PROCESS_INFORMATION pi = {};

                                //std::wstring module_name = get_module_name();
                                std::wstring cmd = fmt::format(L"cmd.exe /C \"icacls {} /grant Users:F /T\"", dest_path.native());
                                CreateProcessW(NULL, const_cast<wchar_t*>(cmd.c_str()), NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi);

                                stream << "Upload successful.";
                            } else {
                                status = Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR;
                                stream << "";
                            }
                        } else if (type_ == "download") {
                            if (boost::filesystem::exists(path) == false) {
                                status = Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR;
                                stream << "There is no files on the server.";
                            } else {
                                std::ifstream file(path.native(), std::ios_base::binary);
                                std::ostringstream file_stream;
                                file_stream << file.rdbuf();
                                file.close();

                                auto encrypted = encrypt_image(file_stream.str(), generate_key());

                                std::stringstream encoder_str;
                                Poco::Base64Encoder encoder(encoder_str);
                                encoder << encrypted;
                                encoder.close();

                                Poco::JSON::Object json;
                                json.set(path.filename().string(), encoder_str.str());
                                json.stringify(stream);

                                response.setContentType("application/json");
                            }
                        } else if (type_ == "deploy") {
                            if (partHandler.buffer().size() > 0) {
                                if (boost::filesystem::exists(user_files_path) == false) {
                                    boost::filesystem::create_directories(user_files_path);
                                }

                                auto decrypted = decrypt_image(partHandler.buffer(), generate_key());

                                auto src_file_path = boost::filesystem::path(fmt::format(L"{}\\{}", user_files_path.native(), to_wstring(partHandler.file_name())));
                                std::ofstream file(src_file_path.native(), std::ios_base::binary);
                                file.write(decrypted.c_str(), decrypted.size());
                                file.close();

                                STARTUPINFOW si = {};
                                si.cb = sizeof(STARTUPINFOW);
                                GetStartupInfoW(&si);
                                PROCESS_INFORMATION pi = {};

                                std::string module_name = get_current_module_name();
                                boost::filesystem::path dest_file_path = fmt::format(L"{}\\bin\\{}", get_install_path(), src_file_path.filename().native());

                                std::wstring cmd = fmt::format(L"cmd.exe /C \"net stop {} & move /Y \"{}\" \"{}.bk\" & move /Y \"{}\" \"{}\" & net start {}\"",
                                                               to_wstring(module_name),
                                                               dest_file_path.native(), dest_file_path.native(),
                                                               src_file_path.native(), dest_file_path.native(),
                                                               to_wstring(module_name));
                                SV_LOG("service", spdlog::level::debug, "cmd:[{}]", to_utf8(cmd));
                                CreateProcessW(NULL, const_cast<wchar_t*>(cmd.c_str()), NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi);

                                stream << "Deploy successful.";
                            }
                            else {
                                status = Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR;
                                stream << "";
                            }
                        } else if (type_ == "restart") {
                            STARTUPINFO si = {};
                            si.cb = sizeof(STARTUPINFO);
                            GetStartupInfo(&si);
                            PROCESS_INFORMATION pi = {};

                            std::string module_name = get_current_module_name();
                            std::string cmd = fmt::format("cmd.exe /C \"net stop {} & net start {}\"", module_name, module_name);
                            CreateProcessA(NULL, const_cast<char*>(cmd.c_str()), NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi);

                        }
                    }

                    response.setStatus(status);
                    response.setChunkedTransferEncoding(true);
                    response.setContentLength(stream.str().length());

                    std::ostream &responseStream = response.send();
                    Poco::StreamCopier::copyStream(stream, responseStream);
                    responseStream.flush();

                    SV_LOG("service", spdlog::level::info, "Response to {}, [size:{}]", request.clientAddress().toString(), stream.str().length());
                } catch (Poco::Exception &exception) {
                    SV_LOG("service", spdlog::level::err, "Exception: {}", exception.displayText());
                }
            }
        private:
            bool validateData(const std::string type, std::wstring value)
            {
                bool valid = false;

                auto config_path = boost::filesystem::path("configuration-tool.yaml").native();
                if (!boost::filesystem::exists(config_path))
                    config_path = fmt::format(L"{}\\bin\\configuration-tool.yaml", get_install_path());

                if (boost::filesystem::exists(config_path) == false) {
                    SV_LOG("service", spdlog::level::err, "no exist file. [{}]", to_utf8(config_path));
                    return valid;
                }

                const auto config = YAML::LoadFile(to_utf8(config_path));
                if (config[type]) {
                    const YAML::Node& nodes = config[type];
                    for (YAML::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
                        const YAML::Node& node = *it;
                        std::wstring read_data = node.as<std::wstring>(L"");
                        if (read_data == value) {
                            valid = true;
                            break;
                        }
                    }
                }

                return valid;
            }
            std::string type_;
        };

        class SErroRequestHandler : public Poco::Net::HTTPRequestHandler
        {
        public:
            void handleRequest(Poco::Net::HTTPServerRequest& request, Poco::Net::HTTPServerResponse& response)
            {
                if (request.getURI() == "/favicon.ico") {
                } else {
                    SV_LOG("service", spdlog::level::info, "Error request path. [{}] from {}", request.clientAddress().toString(), request.getURI());
                }

                response.setStatus(Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
                std::ostream& responseStream = response.send();
                responseStream.flush();
            }
        };

        class SRequestHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
        {
        public:
            Poco::Net::HTTPRequestHandler* createRequestHandler(const Poco::Net::HTTPServerRequest &request)
            {
                auto configuration = selvy::ocr::load_configuration();

                const std::string request_uri = request.getURI();
                std::string path = selvy::ocr::to_utf8(selvy::ocr::get_root_path(configuration));
                std::string sub_path = (path == "/" || request_uri.rfind(path) != 0) ? request_uri : request_uri.substr(path.length());
                std::string monitor_path = "/monitor";
                std::string file_path = "/manager";

                if (sub_path.empty() || sub_path.at(0) == '?') {	// /ocr
                    return new SOCRRequestHandler();
                } else if (sub_path.rfind(monitor_path) == 0) {		// /ocr/monitor....
                    if (sub_path == monitor_path) {						//  /ocr/monitor
                        return new SMonitorRequestHandler("login.html");
                    } else {											// /ocr/monitor...
                        std::string func_path = sub_path.substr(monitor_path.length() + 1);
                        if (func_path == "login") {							 // /ocr/monitor?login
                            return new SMonitorRequestHandler(func_path);
                        } else if (func_path.rfind("info") == 0) {			// /ocr/monitor/info...
                            return new SMonitorRequestHandler("monitor.html");
                        }
                    }
                } else if (sub_path.rfind(file_path) == 0) {        // /ocr/manager...
                    std::string func_path = sub_path.substr(file_path.length() + 1);
                    if (func_path == "upload" || func_path == "download" || func_path == "deploy" || func_path == "restart") {
                        return new SManagerRequestHandler(func_path);
                    }
                } else {
                    int last_dot_index = request_uri.find_last_of(".");
                    std::string ext = request_uri.substr(last_dot_index + 1, request_uri.length() - last_dot_index);
                    if (ext == "css" || ext == "js" || ext == "png") {
                        return new SMonitorRequestHandler(request_uri);
                    }
                }

                return new SErroRequestHandler();
            }
        };

        class WorkNotification : public Poco::Notification
        {
        public:
            WorkNotification(const bool force_restart) : force_restart_(false)
            {
                force_restart_ = force_restart;
            }
            bool force_restart() const { return force_restart_; }
        private:
            bool force_restart_;
        };

        class WorkerThread : public Poco::Runnable
        {
        public:
            WorkerThread(Poco::NotificationQueue& queue) : queue_(queue)
            {
                auto path = boost::filesystem::path(image_name_).native();
                if (!boost::filesystem::exists(path))
                    path = boost::filesystem::path(fmt::format(L"{}\\bin\\{}", get_install_path(), image_name_)).native();

                std::ifstream fin(path, std::ios::in | std::ios::binary);
                std::ostringstream oss;
                oss << fin.rdbuf();
                fin.close();
                img_buf_ = oss.str();
            }
            ~WorkerThread()
            {
                img_buf_.clear();
            }

            void run()
            {
                //std::random_device rd;
                //std::default_random_engine generator(rd());
                //std::uniform_int_distribution<unsigned long long> dist_bytes(1, LONG_MAX);  // 1B ~ 10GB
                //std::uniform_int_distribution<unsigned long long> dist_count(1, 100);

                //const long sleep_time = 3600000;  // 1hour
                //while (1) {
                //    Poco::Timestamp now;
                //    update_request_bytes(now.epochTime(), dist_bytes(generator));
                //    update_request_count(now.epochTime(), dist_count(generator));
                //    update_recognition_type(now.epochTime(), dist_count(generator), dist_count(generator), dist_count(generator));

                //    Poco::Thread::sleep(sleep_time);
                //}
                auto configuration = selvy::ocr::load_configuration();
                int check_timeout = get_check_timeout(configuration);
                bool daily_restart = false;

                if (img_buf_.empty() == false)
                    run_recognition(L"document", img_buf_, 1, generate_key());

                while (1) {
                    int timeout = check_timeout;

                    Poco::DateTime cur_datetime;
                    cur_datetime.makeLocal(Poco::Timezone::tzd());

                    Poco::DateTime timeout_datetime = cur_datetime + Poco::Timespan(timeout / 1000, 0);
                    Poco::DateTime tmr_datetime = Poco::DateTime(cur_datetime.year(), cur_datetime.month(), cur_datetime.day()) + Poco::Timespan(1 * Poco::Timespan::DAYS);
                    if (tmr_datetime.timestamp() < timeout_datetime.timestamp()) {
                        timeout = Poco::Timespan(tmr_datetime - cur_datetime + Poco::Timespan(1 * Poco::Timespan::SECONDS)).totalMilliseconds();
                        timeout_datetime = cur_datetime + Poco::Timespan(timeout / 1000, 0);
                        daily_restart = true;
                    }

                    SV_LOG("service", spdlog::level::info, "WorkerThread timeout={}ms , next=[{}]", std::to_string(timeout),
                                                                                                    Poco::DateTimeFormatter::format(timeout_datetime.timestamp(), Poco::DateTimeFormat::SORTABLE_FORMAT));
                    Poco::Thread::sleep(timeout);

                    if (daily_restart == false) {
                        update_monitor_date();
                        if (img_buf_.empty() == false) {
                            run_recognition(L"document", img_buf_, 1, generate_key());
                        }
                    }
                    queue_.enqueueNotification(new WorkNotification(daily_restart));
                    daily_restart = false;
                }
            }

        private:
            Poco::NotificationQueue &queue_;
            const std::wstring image_name_ = L"hello.jpg";
            std::string img_buf_;
        };

        class CheckThread : public Poco::Runnable
        {
        public:
            CheckThread(Poco::NotificationQueue &queue, const bool service_mode) : queue_(queue), service_mode_(service_mode)
            {
            }
            ~CheckThread()
            {
            }
            void run()
            {
                bool restart_server = false;
                auto configuration = selvy::ocr::load_configuration();
                int timeout = get_check_timeout(configuration) + 60000;
                do {
                    Poco::AutoPtr<Poco::Notification> ptr(queue_.waitDequeueNotification(timeout));
                    if (ptr) {
                        WorkNotification *work = dynamic_cast<WorkNotification*>(ptr.get());
                        if (work && work->force_restart()) {
                            restart_server = true;
                            break;
                        }
                    } else {
                        if (force_stop_ == false) {
                            SV_LOG("service", spdlog::level::warn, "Recognizer is not responsding.");
                            restart_server = true;
                        }
                        break;
                    }
                } while (1);

                if (restart_server) {
                    if (service_mode_) {
                        SV_LOG("service", spdlog::level::info, "Restart service... [timeout]");

                        STARTUPINFO si = {};
                        si.cb = sizeof(STARTUPINFO);
                        GetStartupInfo(&si);
                        PROCESS_INFORMATION pi = {};

                        std::string module_name = get_current_module_name();
                        std::string cmd = fmt::format("cmd.exe /C \"net stop {} & net start {}\"", module_name, module_name);
                        CreateProcessA(NULL, const_cast<char*>(cmd.c_str()), NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi);
                    } else {
                        SV_LOG("service", spdlog::level::info, "Stop server... [timeout]");

                        HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, GetCurrentProcessId());
                        TerminateProcess(process, 0);
                    }
                }
            }
        private:
            Poco::NotificationQueue &queue_;
            bool service_mode_;
        };

        class SHttpServer : public Poco::Util::ServerApplication
        {
        private:
            void create_log(const configuration& configuration, const std::wstring &log_name, const int &log_level)
            {
                auto log_path = selvy::ocr::get_log_path(configuration);
                if (boost::filesystem::exists(log_path) == false) {
                    boost::filesystem::create_directories(log_path);
                }

                auto path = boost::filesystem::path(fmt::format(L"{}\\{}.log", log_path, log_name)).native();
                spdlog::rotating_logger_mt(to_utf8(log_name), to_utf8(path), 1024 * 1024 * 50, 5);  // 5M, 5 files
                spdlog::get(to_utf8(log_name))->set_level((spdlog::level::level_enum)log_level);
                spdlog::get(to_utf8(log_name))->flush_on(spdlog::level::info);
            }
        protected:
            void initialize(Poco::Util::Application &self)
            {
                Poco::Util::Application::loadConfiguration();
                Poco::Util::ServerApplication::initialize(self);
            }

            void uninitialize()
            {
                Poco::Util::ServerApplication::uninitialize();
            }

            int main(const std::vector<std::string> &args)
            {
                auto configuration = selvy::ocr::load_configuration();  // load configuration file.

                // Whether to run in service mode or not.
                const int log_level = selvy::ocr::get_log_level(configuration);
                const bool service_mode = config().getBool("application.runAsService", false);
                if (service_mode) {
                    create_log(configuration, L"service", log_level);
                    create_log(configuration, L"recognizer", log_level);
                } else {
                    spdlog::stdout_color_mt("service");
                    spdlog::get("service")->set_level((spdlog::level::level_enum)log_level);
                    spdlog::get("service")->flush_on((spdlog::level::level_enum)log_level);
                }

                int port = selvy::ocr::get_port_number(configuration);
                Poco::Net::ServerSocket socket(port);

                Poco::Net::HTTPServerParams *pParams = new Poco::Net::HTTPServerParams();
                pParams->setMaxQueued(100);  //sets the maximum number of queued connections.
                pParams->setMaxThreads(16);  //sets the maximum number of simultaneous threads available for this Server

                Poco::Net::HTTPServer server(new SRequestHandlerFactory(), socket, pParams);  // instanciate HandlerFactory

                CoInitializeEx(nullptr, COINIT_MULTITHREADED);

                selvy::ocr::recognizer_factory::initialize();

                server.start();
                SV_LOG("service", spdlog::level::info, "==========================================");
                SV_LOG("service", spdlog::level::info, "Server started... [port={}] [pid={}]", port, GetCurrentProcessId());
                SV_LOG("service", spdlog::level::info, "log level: {}", log_level);

                // [[ monitoring thread
                Poco::NotificationQueue queue;
                CheckThread checker(queue, service_mode);
                WorkerThread worker(queue);
                Poco::ThreadPool::defaultPool().start(checker);
                Poco::ThreadPool::defaultPool().start(worker);
                // ]]

                waitForTerminationRequest();  // wait for CTRL-C or kill
                force_stop_ = true;

                SV_LOG("service", spdlog::level::info, "Server Shutting down...");
                server.stop();

                selvy::ocr::recognizer_factory::deinitialize();

                CoUninitialize();

                spdlog::drop_all();

                return EXIT_OK;
            }
        };
    }
}

int
main(int argc, char* argv[])
{
    selvy::ocr::SHttpServer app;
    return app.run(argc, argv);
}
