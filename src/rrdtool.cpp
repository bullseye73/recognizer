
#include "rrdtool.h"
#include <fmt/format.h>
#include <iostream>
#include "utils.hpp"

const static std::wstring rrdtool = L"rrdtool.exe";
const static std::wstring font = L"C:\\Windows\\Fonts\\arial.ttf";

const static std::wstring req_bytes_rrd_filename = L"req_bytes_3600.rrd";
const static std::wstring req_count_rrd_filename = L"req_count_3600.rrd";
const static std::wstring recog_type_rrd_filename = L"recog_type_3600.rrd";

std::wstring get_rrdtool_path()
{
    auto path = boost::filesystem::path(fmt::format(L"{}", rrdtool)).native();
    if (boost::filesystem::exists(selvy::ocr::to_utf8(path)) == false)
        path = boost::filesystem::path(fmt::format(L"{}\\bin\\{}", selvy::ocr::get_install_path(), rrdtool)).native();

    if (boost::filesystem::exists(selvy::ocr::to_utf8(path)) == false) {
        SV_LOG("service", spdlog::level::err, "no exist rrdtool file. [{}]", selvy::ocr::to_utf8(path));
        return L"";
    }

    return path;
}

void execute_command(const std::wstring cmd, const std::wstring data_path, std::wstring &output_path)
{
    if (data_path.empty() == false && boost::filesystem::exists(data_path) == false) {
        SV_LOG("service", spdlog::level::err, "can't find the rrdtool data file. [{}]", selvy::ocr::to_utf8(data_path));
        output_path = L"";
        return;
    }

    SV_LOG("service", spdlog::level::debug, "cmd=[{}]", selvy::ocr::to_utf8(cmd));
    std::system(selvy::ocr::to_utf8(cmd).c_str());

    if (boost::filesystem::exists(output_path) == false) {
        SV_LOG("service", spdlog::level::err, "can't find the rrdtool output file. [{}]", selvy::ocr::to_utf8(output_path));
        output_path = L"";
    }
}

std::wstring export_byte_graph(const std::wstring rrdtool_path, const std::wstring storage_path, const std::wstring days)
{
    // bytes graph
    //rrdtool.exe graph storage\graph.png --start 1545102540 --title "OCR Service Request status" --vertical-label "Bytes/h" --width 500 --height 200 --lower-limit 0 --alt-autoscale-max --font DEFAULT:12:"C:\Windows\Fonts\arial.ttf"
    //"DEF:Bytes=storage\bytes.rrd:Bytes:AVERAGE" "CDEF:kBytes=Bytes,1024,*" "GPRINT:Bytes:MAX:%.2lf %S" "LINE:Bytes#EA644A:Bytes/h"
    auto data_path = boost::filesystem::path(fmt::format(L"{}\\{}", storage_path, req_bytes_rrd_filename)).native();
    auto graph_path = boost::filesystem::path(fmt::format(L"{}\\req_bytes_{}d.png", storage_path, days)).native();
    std::wstring cmd = fmt::format(L"{} graph {} --start -{}d\
                                                 --title \"OCR Service Request Bytes\"\
                                                 --vertical-label \"Bytes\"\
                                                 --width 500\
                                                 --height 200\
                                                 --lower-limit 0\
                                                 --alt-autoscale-max\
                                                 --font TITLE:12:\"{}\"\
                                                 --font DEFAULT:10:\"{}\"\
                                                 \"DEF:Bytes={}:Bytes:AVERAGE\"\
                                                 \"CDEF:kBYtes=Bytes,1024,*\"\
                                                 \"LINE:Bytes#EA644A:Bytes/hour\"\
                                                 \"GPRINT:Bytes:MAX:Max %.2lf %S\"", rrdtool_path, graph_path, days, font, font, data_path);

    execute_command(cmd, data_path, graph_path);
    return graph_path;
}

std::wstring export_count_graph(const std::wstring rrdtool_path, const std::wstring storage_path, const std::wstring days)
{
    auto data_path = boost::filesystem::path(fmt::format(L"{}\\{}", storage_path, req_count_rrd_filename)).native();
    auto graph_path = boost::filesystem::path(fmt::format(L"{}\\req_count_{}d.png", storage_path, days)).native();
    std::wstring cmd = fmt::format(L"{} graph {} --start -{}d\
                                                 --title \"OCR Service Request Worker\"\
                                                 --vertical-label \"Worker\"\
                                                 --width 500\
                                                 --height 200\
                                                 --lower-limit 0\
                                                 --alt-autoscale-max\
                                                 --font TITLE:12:\"{}\"\
                                                 --font DEFAULT:10:\"{}\"\
                                                 \"DEF:Count={}:Count:AVERAGE\"\
                                                 \"LINE:Count#1598C3:Workers/hour\"\
                                                 \"GPRINT:Count:MAX:Max %.0lf\"", rrdtool_path, graph_path, days, font, font, data_path);

    execute_command(cmd, data_path, graph_path);
    return graph_path;
}

std::wstring export_recog_type_graph(const std::wstring rrdtool_path, const std::wstring storage_path, const std::wstring days)
{
    auto data_path = boost::filesystem::path(fmt::format(L"{}\\{}", storage_path, recog_type_rrd_filename)).native();
    auto graph_path = boost::filesystem::path(fmt::format(L"{}\\recog_type_{}d.png", storage_path, days)).native();
    std::wstring cmd = fmt::format(L"{} graph {} --start -{}d\
                                                 --title \"Recognition Type\"\
                                                 --vertical-label \"Count\"\
                                                 --width 500\
                                                 --height 200\
                                                 --lower-limit 0\
                                                 --alt-autoscale-max\
                                                 --font TITLE:12:\"{}\"\
                                                 --font DEFAULT:10:\"{}\"\
                                                 \"DEF:Fax={}:Fax:AVERAGE\"\
                                                 \"DEF:Trade={}:Trade:AVERAGE\"\
                                                 \"DEF:Document={}:Document:AVERAGE\"\
                                                 \"LINE:Fax#7648EC:Fax\"\
                                                 \"GPRINT:Fax:AVERAGE:%.1lf\"\
                                                 \"LINE:Trade#EA644A:Trade\"\
                                                 \"GPRINT:Trade:AVERAGE:%.1lf\"\
                                                 \"LINE:Document#54EC48:Document\"\
                                                 \"GPRINT:Document:AVERAGE:%.1lf\"", rrdtool_path, graph_path, days, font, font, data_path, data_path, data_path);

    execute_command(cmd, data_path, graph_path);
    return graph_path;
}

//===================================================================================================================================
//--------------------------------------------------------------------------------------- update data
void update_request_bytes(const std::time_t timestamp, const unsigned long long &size)
{
    std::wstring rrdtool_path = get_rrdtool_path();
    if (rrdtool_path.empty()) {
        return;
    }

    auto configuration = selvy::ocr::load_configuration();
    std::wstring storage_path = selvy::ocr::get_storage_path(configuration);
    auto rrd_path = boost::filesystem::path(fmt::format(L"{}\\{}", storage_path, req_bytes_rrd_filename)).native();
    if (boost::filesystem::exists(rrd_path) == false) {
        if (boost::filesystem::exists(storage_path) == false) {
            boost::filesystem::create_directories(storage_path);
        }

        SV_LOG("service", spdlog::level::debug, "new rrd data file: {}", selvy::ocr::to_utf8(rrd_path));
        /*std::wstring cmd = fmt::format(L"{} create {} --start {}\
                                                      --step 60\
                                                      \"DS:Bytes:GAUGE:600:0:U\"\
                                                      \"RRA:AVERAGE:0.5:1:60\"\
                                                      \"RRA:AVERAGE:0.5:1:1440\"", rrdtool_path, rrd_path, (timestamp - 1));*/
        std::wstring cmd = fmt::format(L"{} create {} --start {}\
                                                      --step 3600\
                                                      \"DS:Bytes:GAUGE:7200:0:U\"\
                                                      \"RRA:AVERAGE:0.5:1:24\"\
                                                      \"RRA:AVERAGE:0.5:6:28\"\
                                                      \"RRA:AVERAGE:0.5:24:15\"\
                                                      \"RRA:AVERAGE:0.5:24:30\"", rrdtool_path, rrd_path, (timestamp - 1));
        execute_command(cmd, L"", rrd_path);
    }
    if (rrd_path.empty()) {
        return;
    }

    SV_LOG("service", spdlog::level::info, "Updating bytes= [{}]:{}", timestamp, size);

    //rrdtool update path T:size
    /*boost::posix_time::ptime epoch(boost::gregorian::date(1970, 1, 1));
    boost::posix_time::ptime local = boost::posix_time::second_clock::universal_time();
    boost::posix_time::time_duration::sec_type secs = (local - epoch).total_seconds();
    std::wstring cmd = fmt::format(L"{} update {} {}:{}:{}", rrdtool, path, time_t(secs), std::to_wstring(1), std::to_wstring(size));*/
    std::wstring cmd = fmt::format(L"{} update {} {}:{}", rrdtool_path, rrd_path, std::to_wstring(timestamp), std::to_wstring(size));
    execute_command(cmd, rrd_path, rrd_path);
}

void update_request_count(const std::time_t timestamp, const unsigned long long &count)
{
    std::wstring rrdtool_path = get_rrdtool_path();
    if (rrdtool_path.empty()) {
        return;
    }

    auto configuration = selvy::ocr::load_configuration();
    std::wstring storage_path = selvy::ocr::get_storage_path(configuration);
    auto rrd_path = boost::filesystem::path(fmt::format(L"{}\\{}", storage_path, req_count_rrd_filename)).native();
    if (boost::filesystem::exists(rrd_path) == false) {
        if (boost::filesystem::exists(storage_path) == false) {
            boost::filesystem::create_directories(storage_path);
        }

        SV_LOG("service", spdlog::level::debug, "new rrd data file: {}", selvy::ocr::to_utf8(rrd_path));
        std::wstring cmd = fmt::format(L"{} create {} --start {}\
                                                      --step 3600\
                                                      \"DS:Count:GAUGE:7200:0:U\"\
                                                      \"RRA:AVERAGE:0.5:1:24\"\
                                                      \"RRA:AVERAGE:0.5:6:28\"\
                                                      \"RRA:AVERAGE:0.5:24:15\"\
                                                      \"RRA:AVERAGE:0.5:24:30\"", rrdtool_path, rrd_path, (timestamp - 1));
        execute_command(cmd, L"", rrd_path);
    }
    if (rrd_path.empty()) {
        return;
    }

    SV_LOG("service", spdlog::level::info, "Updating count= [{}]:{}", timestamp, count);

    std::wstring cmd = fmt::format(L"{} update {} {}:{}", rrdtool_path, rrd_path, std::to_wstring(timestamp), std::to_wstring(count));
    execute_command(cmd, rrd_path, rrd_path);
}

void update_recognition_type(const std::time_t timestamp, const unsigned long long &fax_count, const unsigned long long &trade_count, const unsigned long long &doc_count)
{
    std::wstring rrdtool_path = get_rrdtool_path();
    if (rrdtool_path.empty()) {
        return;
    }

    auto configuration = selvy::ocr::load_configuration();
    std::wstring storage_path = selvy::ocr::get_storage_path(configuration);
    auto rrd_path = boost::filesystem::path(fmt::format(L"{}\\{}", storage_path, recog_type_rrd_filename)).native();
    if (boost::filesystem::exists(rrd_path) == false) {
        if (boost::filesystem::exists(storage_path) == false) {
            boost::filesystem::create_directories(storage_path);
        }

        SV_LOG("service", spdlog::level::debug, "new rrd data file: {}", selvy::ocr::to_utf8(rrd_path));
        std::wstring cmd = fmt::format(L"{} create {} --start {}\
                                                      --step 3600\
                                                      \"DS:Fax:GAUGE:7200:0:U\"\
                                                      \"DS:Trade:GAUGE:7200:0:U\"\
                                                      \"DS:Document:GAUGE:7200:0:U\"\
                                                      \"RRA:AVERAGE:0.5:1:24\"\
                                                      \"RRA:AVERAGE:0.5:6:28\"\
                                                      \"RRA:AVERAGE:0.5:24:15\"\
                                                      \"RRA:AVERAGE:0.5:24:30\"", rrdtool_path, rrd_path, (timestamp - 1));
        execute_command(cmd, L"", rrd_path);
    }
    if (rrd_path.empty()) {
        return;
    }

    SV_LOG("service", spdlog::level::info, "Updating recognition= [{}]:f={},t={},d={}", timestamp, fax_count, trade_count, doc_count);

    std::wstring cmd = fmt::format(L"{} update {} {}:{}:{}:{}", rrdtool_path, rrd_path, std::to_wstring(timestamp), std::to_wstring(fax_count), std::to_wstring(trade_count), std::to_wstring(doc_count));
    execute_command(cmd, rrd_path, rrd_path);
}

//--------------------------------------------------------------------------------------- export graph
void export_graph(const std::wstring days, std::vector<std::pair<std::string, std::wstring>> &graph_list)
{
    graph_list.clear();

    std::wstring rrdtool_path = get_rrdtool_path();
    if (rrdtool_path.empty()) {
        return;
    }

    auto configuration = selvy::ocr::load_configuration();
    std::wstring storage_path = selvy::ocr::get_storage_path(configuration);

    auto bytes_graph = export_byte_graph(rrdtool_path, storage_path, days);
    graph_list.push_back(std::make_pair("req_bytes", bytes_graph));

    auto count_graph = export_count_graph(rrdtool_path, storage_path, days);
    graph_list.push_back(std::make_pair("req_count", count_graph));

    auto recog_type_graph = export_recog_type_graph(rrdtool_path, storage_path, days);
    graph_list.push_back(std::make_pair("recog_type", recog_type_graph));
}
