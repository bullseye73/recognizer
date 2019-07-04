
#define _WINSOCKAPI_
#if 0
#define _DUMMY_ENGINE_
#endif

#include "Selvy_Recognizer.h"
#include <Windows.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <random>
#include <unordered_map>
#include "encryption.h"
#include "recognizer.hpp"
#include "utils.hpp"

static std::shared_ptr<spdlog::logger> logger_ = nullptr;

//==============================================================================================================
void create_log()
{
    if (logger_ != nullptr) {
        return;
    }

    const static std::string log_name_ = "recognizer";

    auto configuration = selvy::ocr::load_configuration();  // load configuration file.

    auto log_path = selvy::ocr::get_log_path(configuration);
    if (boost::filesystem::exists(log_path) == false) {
        boost::filesystem::create_directories(log_path);
    }

    auto path = boost::filesystem::path(fmt::format(L"{}\\{}.log", log_path, selvy::ocr::to_wstring(log_name_))).native();
    logger_ = spdlog::rotating_logger_mt(log_name_, selvy::ocr::to_utf8(path), 1024 * 1024 * 50, 5);  // 5M, 5 files
    logger_->set_level((spdlog::level::level_enum)selvy::ocr::get_log_level(configuration));
    logger_->flush_on(spdlog::level::info);
}

std::wstring jstr2wstr(JNIEnv *env, jstring jstr)
{
    std::wstring wstr;

    if (jstr != NULL) {
        const jchar *raw = env->GetStringChars(jstr, NULL);
        if (raw != NULL) {
            int len = env->GetStringLength(jstr);
            wchar_t *tmp = new wchar_t[len];
            memcpy(tmp, raw, len * sizeof(wchar_t));
            wstr.assign(tmp, len);
            delete[] tmp;
            env->ReleaseStringChars(jstr, raw);
        }
    }

    return wstr;
}

jstring wstr2jstr(JNIEnv *env, std::wstring wstr)
{
    int len = wstr.size();
    jchar *raw = new jchar[len];
    memcpy(raw, wstr.c_str(), len * sizeof(wchar_t));

    jstring jstr = env->NewString(raw, len);
    delete[] raw;

    return jstr;
}

//==============================================================================================================
JNIEXPORT void JNICALL Java_Selvy_Recognizer_initialize(JNIEnv *env, jobject obj)
{
    create_log();

#ifndef _DUMMY_ENGINE_
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    selvy::ocr::recognizer_factory::initialize();
#endif  // _DUMMY_ENGINE_
    logger_->info("The recognizer engine has been initialized.");
}

JNIEXPORT void JNICALL Java_Selvy_Recognizer_deinitialize(JNIEnv *env, jobject obj)
{
    create_log();

#ifndef _DUMMY_ENGINE_
    selvy::ocr::recognizer_factory::deinitialize();
    CoUninitialize();
#endif  / /_DUMMY_ENGINE_
    logger_->info("The recognizer engine has been deinitialized.");
}

JNIEXPORT jstring JNICALL Java_Selvy_Recognizer_recognizer(JNIEnv *env, jobject obj, jstring jtype, jstring jimagepath)
{
    const char *type = env->GetStringUTFChars(jtype, 0);
    const char *image_path = env->GetStringUTFChars(jimagepath, 0);

    std::string recog_type(type);
    std::string recog_path(image_path);
    logger_->info(fmt::format("Type:[{}], Path:[{}]", recog_type, recog_path));

#ifdef _DUMMY_ENGINE_
    std::vector<std::wstring> field_value_list{ L"필드 값 1", L"필드 값 2", L"필드 값 3" };
    std::random_device rd;
    std::default_random_engine generator(rd());
    std::uniform_int_distribution<int> dist_num(0, field_value_list.size() - 1);

    boost::property_tree::wptree tree;
    for (int i = 0; i < 1; i++) {
        boost::property_tree::wptree fields_node;

        for (int j = 0; j < 2; j++) {
            boost::property_tree::wptree value_node;
            value_node.put(L"value", field_value_list.at(dist_num(generator)));
            value_node.put(L"x", 0);
            value_node.put(L"y", 0);
            value_node.put(L"width", 0);
            value_node.put(L"height", 0);

            boost::property_tree::wptree values_node;
            values_node.push_back(std::make_pair(L"", value_node));

            std::wstring field_name = fmt::format(L"field name {}", j);
            //fields_node.add_child(field_name, values_node);
            boost::property_tree::wptree field_node;
            field_node.add_child(field_name, values_node);
            fields_node.push_back(std::make_pair(L"", field_node));
        }

        boost::property_tree::wptree category_node;
        category_node.add_child(L"document", fields_node);

        std::wstring filename = fmt::format(L"file{}", i);
        tree.add_child(filename, category_node);
    }

    std::wostringstream buf;
    boost::property_tree::write_json(buf, tree);
    jstring json = wstr2jstr(env, buf.str());

#else
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::wstring class_name = L"document";  // 문서 분류 카테고리명. 현재 코드는 구현되어진 court recognizer를 쓰기위해 document로 박아놓은 상태임.
    auto recognizer = selvy::ocr::recognizer_factory::create(selvy::ocr::to_wstring(recog_type));  // 개발될 recognizer class 이름.

    // ==> recognize 후, 리턴되어야할 구조체는 아래와 같아야함.
    //std::unordered_map(file, pair(category, vector(pair(fieldname, vector(pair(fieldvalue, rect))))))
    boost::filesystem::path path(recog_path);
    std::unordered_map<std::wstring, std::vector<std::wstring>> fields = recognizer->recognize(path, class_name);  // 구현될 class로 변경.
    logger_->info(fmt::format("result size:{}", fields.size()));

    /* 서버로 전송하기 위한 json 구조
    "category":[
        {"fieldname1":[
                        {value: "", x : "", y : "", width : "", height : ""},
                        {value: "", x : "", y : "", width : "", height : ""}
                        ]
        },
        {"fieldname2" : []}
    ]*/
    boost::property_tree::wptree fields_node;
    for (auto field : fields) {  // field
        boost::property_tree::wptree values_node;

        for (auto value : field.second) {  // field values
            boost::property_tree::wptree value_node;
            value_node.put(L"value", value);
            value_node.put(L"x", 0);
            value_node.put(L"y", 0);
            value_node.put(L"width", 0);
            value_node.put(L"height", 0);

            values_node.push_back(std::make_pair(L"", value_node));  // vector(field-value)
        }

        //fields_node.add_child(field.first, values_node);  // pair(field-name, vector(field-value))
        boost::property_tree::wptree field_node;
        field_node.add_child(field.first, values_node);
        fields_node.push_back(std::make_pair(L"", field_node));  // pair(field-name, vector(field-value))
    }

    boost::property_tree::wptree category_node;
    category_node.add_child(class_name, fields_node);  // pair(category, vector(fields))

    //boost::property_tree::wptree file_node;
    //file_node.add_child(path.filename().native(), category_node);  //  file, category

    std::wostringstream buf;
    //boost::property_tree::write_json(buf, file_node);
    boost::property_tree::write_json(buf, category_node);
    jstring json = wstr2jstr(env, buf.str());

    CoUninitialize();
#endif  // _DUMMY_ENGINE_

    env->ReleaseStringUTFChars(jimagepath, image_path);
    env->ReleaseStringUTFChars(jtype, type);

    return json;
}
