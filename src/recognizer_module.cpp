#include <Windows.h>

#include <boost/algorithm/string.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "naturalorder.h"

#include "recognizer.hpp"
#include "utils.hpp"
#include <locale>
#include <codecvt>
// TEST
#include <boost/regex.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/algorithm/replace_if.hpp>
#include <boost/range/algorithm/remove_if.hpp>
#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/locale.hpp>
// TEST

std::string
to_cp949__(const std::wstring& str)
{
	if (str.empty())
		return std::string();

	const auto num_chars = WideCharToMultiByte(CP_ACP, 0, &str[0], static_cast<int>(str.size()), nullptr, 0, nullptr, nullptr);
	std::string buffer(num_chars, L'\0');
	WideCharToMultiByte(CP_ACP, 0, str.data(), str.size(), const_cast<char*>(buffer.data()), num_chars, nullptr, nullptr);

	return buffer;
}



int
main()
{	
	//auto lower_text = boost::regex_replace(L"", boost::wregex(L"[^/ 0-9a-z\\.-]"), L"");
	//cv::Mat image = cv::imread("E:/20190329104923-1.jpg");

	//cv::Mat matrix = cv::getRotationMatrix2D(cv::Point(image.cols / 2, image.rows / 2), 3, 1);
	//cv::Rect2f bbox = cv::RotatedRect(cv::Point2f(), image.size(), 3).boundingRect2f();
	//matrix.at<double>(0, 2) += bbox.width / 2.0 - image.cols / 2.0;
	//matrix.at<double>(1, 2) += bbox.height / 2.0 - image.rows / 2.0;	

	//cv::Mat dst;
	//cv::warpAffine(image, dst, matrix, bbox.size(), 1, cv::BORDER_REPLICATE);
	//cv::imwrite("E:/dst_03.jpg", dst);

    spdlog::stdout_color_mt("console");

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    selvy::ocr::recognizer_factory::initialize();

    const auto image_directory = L"C:\\workspace\\vs2015\\images";
    const auto profile = L"trade";
    // std::wstring dataset_directory =L"D:\\ksjo\\images\\sample_report" ;// fmt::format(L"{}\\{}\\datasets", image_directory, profile);
	// std::wstring dataset_directory = L"D:\\test";
	std::wstring dataset_directory = L"C:\\workspace\\vs2015\\images";
	
    auto recognizer = selvy::ocr::recognizer_factory::create(profile);

//    std::vector<std::wstring> categories = {
//        L"document",
//    };
	std::vector<std::wstring> categories = {
		L"AIR WAYBILL",              //0
		L"BILL OF EXCHANGE",         //1
		L"BILL OF EXCHANGE-KOR",     //2
		L"BILL OF LADING",           //3
		L"CARGO RECEIPT",            //4
		L"CERTIFICATE",              //5
		L"CERTIFICATE OF ORIGIN",    //6
		L"EXPORT PERMIT",            //7
		L"INSURANCE POLICY",         //8
		L"LETTER OF CREDIT",         //9
		L"PACKING LIST",             //10
		L"REMITTANCE LETTER",        //11
		L"SHIPMENT ADVICE",          //12
		L"WAYBILL",                  //13
		L"COMMERCIAL INVOICE",       //14
		L"LETTER OF GUARANTEE",      //15
		L"FIRM OFFER",               //16
		L"PROFORMA INVOICE",               //17
	};

//    std::unordered_map<std::wstring, std::vector<std::wstring>> key_values = {
//		{ categories[0], { L"COURT NAME", L"EVENT", L"CREDITOR", L"DEBTOR", L"DEBTOR_OTHER", L"PRICE", L"JUDGE" } },
//    };

	std::unordered_map<std::wstring, std::vector<std::wstring>> key_values = {
		{ categories[0],{ L"PORT OF LOADING", L"PORT OF DISCHARGE", L"CONSIGNEE", L"SHIPPER", L"NOTIFY", L"VESSEL NAME", L"AGENT" } },
		{ categories[1],{ L"NEGOTIATION BANK" } },
		{ categories[2],{ L"" } },
		{ categories[3],{ L"CONSIGNEE", L"CARRIER", L"NOTIFY", L"PORT OF DISCHARGE", L"PORT OF LOADING", L"SHIPMENT DATE", L"VESSEL NAME" } },
		//{ categories[3],{ L"PORT OF LOADING", L"PLACE OF RECEIPT", L"PORT OF DISCHARGE", L"PLACE OF DELIVERY", L"CONSIGNEE", L"SHIPPER", L"NOTIFY", L"VESSEL NAME", L"CARRIER", L"PLACE OF ISSUE", L"ORIGIN", L"SHIPPING LINE" } },
		{ categories[4],{ L"PORT OF LOADING", L"PLACE OF RECEIPT", L"PORT OF DISCHARGE", L"PLACE OF DELIVERY", L"CONSIGNEE", L"SHIPPER", L"NOTIFY", L"VESSEL NAME", L"PLACE OF ISSUE" } },
		{ categories[5],{ L"ORIGIN", L"GOODS DESCRIPTION", L"PORT OF LOADING", L"PLACE OF DELIVERY", L"PORT OF DISCHARGE", L"VESSEL NAME", } },
		{ categories[6],{ L"ORIGIN", L"IMPORTER" } },
		{ categories[7],{ L"GOODS DESCRIPTION" } },
		{ categories[8],{ L"INSURANCE COMPANY", L"INSURANCE SETTLING AGENT", L"INSURANCE SURVEY AGENT", L"PORT OF LOADING", L"PORT OF DISCHARGE", L"VESSEL NAME", L"ORIGIN", L"GOODS DESCRIPTION" } },
		//{ categories[9],{ L"ISSUING BANK", L"COLLECTING BANK", L"APPLICANT", L"BENEFICIARY", L"PORT OF LOADING", L"PORT OF DISCHARGE" } },
		{ categories[9],{ L"SWIFT MT SENDER", L"AMOUNT", L"CREDIT NUMBER" } },
		{ categories[10],{ L"SELLER", L"BUYER", L"ORIGIN", L"CONSIGNEE", L"NOTIFY", L"PORT OF LOADING", L"EXPORTER", L"APPLICANT", L"MANUFACTURER", L"PORT OF DISCHARGE", L"PLACE OF DELIVERTY", L"VESSEL NAME" } },
		{ categories[11],{ L"ISSUING BANK", L"DRAWER" } },
		{ categories[12],{ L"APPLICANT", L"BENEFICIARY", L"PORT OF LOADING", L"PORT OF DISCHARGE", L"VESSEL NAME", L"SHIPPING LINE", L"INSURANCE COMPANY" } },
		{ categories[13],{ L"", } },
		//{ categories[14],{ L"SELLER", L"BUYER", L"ORIGIN", L"CONSIGNEE", L"NOTIFY", L"PORT OF LOADING", L"EXPORTER", L"APPLICANT", L"MANUFACTURER", L"PORT OF DISCHARGE", L"PLACE OF DELIVERY", L"VESSEL NAME" } },
		{ categories[14],{ L"SELLER", L"AMOUNT", L"L/C" } },
		{ categories[15],{ L"CARRIER", L"SHOPPER", L"PORT OF LOADING", L"PORT OF DISCHARGE", L"VESSEL NAME", L"CONSIGNEE", L"GOODS DESCRIPTION", L"NOTIFY" } },
		{ categories[16],{ L"SELLER", L"BUYER", L"ORIGIN", L"APPLICANT", L"COLLECTING BANK" } },
		{ categories[17],{ L"BANK NAME", L"SWIFT CODE"} },
	};


	std::vector<std::wstring> files;

	for (auto& entry : boost::filesystem::directory_iterator(dataset_directory)) {
		const auto& file = entry.path();
		const auto extension = boost::algorithm::to_lower_copy(file.extension().native());

		if (extension != L".png" && extension != L".jpg" && extension != L".tif")
			continue;

		files.emplace_back(boost::filesystem::absolute(file).native());
	}

	std::sort(std::begin(files), std::end(files), compareNat);

	for (const auto& file : files) {
		auto category = L"BILL OF LADING";// categories[14];
		//auto category = L"LETTER OF CREDIT"; //
		//auto category = L"COMMERCIAL INVOICE";
		//auto category = L"PROFORMA INVOICE"; //
		const auto fields = recognizer->recognize(file, category);

		auto output_path = fmt::format(L"{}_result.txt", boost::filesystem::path(file).native());
		std::wstring result_text;

		if (fields.empty()) {
			continue;
		}

		auto& keys = key_values.at(category);
		int count = 1;
		result_text += fmt::format(L"\"file name\",\"{}\"\n", fields.at(L"FILE NAME").front());
		result_text += fmt::format(L"\"category\",\"{}\"\n", category);
		count++;
		for (auto& key : keys) {
			auto& name = key;
			result_text += L"\"" + name + L"\",";
			if (fields.find(name) == fields.end()) {
				result_text += L"\n";
				continue;
			}
			auto& r = fields.at(name);

			for (auto& f : r) {
				result_text += L"\"" + f + L"\",";
			}
			result_text += L"\n";
		}
		result_text += L"\n";
		std::wofstream txt_file;
		txt_file.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
		txt_file.open(output_path, std::wofstream::out | std::wofstream::ate);
		txt_file << result_text << std::endl;
		txt_file.close();

	}

	spdlog::drop_all();

	return 0;
}



int
main2()
{	
	//auto lower_text = boost::regex_replace(L"", boost::wregex(L"[^/ 0-9a-z\\.-]"), L"");
	//cv::Mat image = cv::imread("E:/20190329104923-1.jpg");

	//cv::Mat matrix = cv::getRotationMatrix2D(cv::Point(image.cols / 2, image.rows / 2), 3, 1);
	//cv::Rect2f bbox = cv::RotatedRect(cv::Point2f(), image.size(), 3).boundingRect2f();
	//matrix.at<double>(0, 2) += bbox.width / 2.0 - image.cols / 2.0;
	//matrix.at<double>(1, 2) += bbox.height / 2.0 - image.rows / 2.0;	

	//cv::Mat dst;
	//cv::warpAffine(image, dst, matrix, bbox.size(), 1, cv::BORDER_REPLICATE);
	//cv::imwrite("E:/dst_03.jpg", dst);

    spdlog::stdout_color_mt("console");

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    selvy::ocr::recognizer_factory::initialize();

    const auto image_directory = L"E:\\2019_PROJECT\\99.ETC\\추심\\문서이미지";
    const auto profile = L"idcard";
    // std::wstring dataset_directory =L"D:\\ksjo\\images\\sample_report" ;// fmt::format(L"{}\\{}\\datasets", image_directory, profile);
	// std::wstring dataset_directory = L"D:\\test";
	 std::wstring dataset_directory = L"E:\\awork\\sample3";

    auto recognizer = selvy::ocr::recognizer_factory::create(profile);

    std::vector<std::wstring> categories = {
        L"document",
    };

    std::unordered_map<std::wstring, std::vector<std::wstring>> key_values = {
		{ categories[0], { L"COURT NAME", L"EVENT", L"CREDITOR", L"DEBTOR", L"DEBTOR_OTHER", L"PRICE", L"JUDGE" } },
    };

    std::vector<std::wstring> files;

	for (auto& entry : boost::filesystem::recursive_directory_iterator(dataset_directory)) {
		const auto file = entry.path();
		if (!boost::filesystem::is_directory(file)) {
			const auto extension = boost::algorithm::to_lower_copy(file.extension().native());

			if (extension != L".png" && extension != L".jpg" && extension != L".tif" && extension != L".pdf")
				continue;

			files.emplace_back(boost::filesystem::absolute(file).native());
		}
	}
	
	std::sort(std::begin(files), std::end(files), compareNat);

    for (const auto& file : files) {
		cv::TickMeter processing_ticks;
		processing_ticks.start();

		const auto fields = recognizer->recognize(file, categories[0]);

		auto output_path = fmt::format(L"{}{}_result.csv", OUTPUT_FOLDER, boost::filesystem::path(file).filename().native());
        std::wstring result_text;

		if (fields.empty()) {
            continue;
        }

		if (profile == L"court") {
			auto keys = std::vector<std::wstring>{ L"COURT NAME", L"EVENT", L"CREDITOR", L"DEBTOR", L"DEBTOR_OTHER", L"PRICE", L"JUDGE" };
			auto mames = std::vector<std::wstring>{ L"법원명", L"사건", L"채권자", L"채무자", L"제3채무자", L"채권금액", L"판사" };
			//result_text += fmt::format(L"\"file name\",\"{}\"\n", fields.at(L"FILE NAME").front());
			//      result_text += fmt::format(L"\"category\",\"{}\"\n", category);
			int i = 0;
			for (auto& key : keys) {
				auto& name = mames[i];
				++i;
				if (fields.find(key) == fields.end()) {
					result_text += L"\"" + name + L"\",\n";
					continue;
				}
				auto& r = fields.at(key);
				if (r.size() == 0) {
					result_text += fmt::format(L"\"{}\"\n", name);
				}
				else {
					for (int i = 0; i < r.size(); ++i) {
						auto& f = r[i];
						result_text += fmt::format(L"\"{} {:03d}\",\"{}\"\n", name, i, f);
					}
				}
			}			
		}

        result_text += L"\n";
        std::wofstream txt_file;
        txt_file.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
        txt_file.open(output_path, std::wofstream::out | std::wofstream::ate);
        txt_file << result_text << std::endl;
        txt_file.close();

		processing_ticks.stop();
		spdlog::get("recognizer")->info("process total : {} ({:.2f}mSec)", to_cp949__(boost::filesystem::path(file).filename().native()),
			processing_ticks.getTimeMilli());
    }

    spdlog::drop_all();

    return 0;
}
