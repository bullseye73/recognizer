//﻿
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
	spdlog::stdout_color_mt("console");

	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	selvy::ocr::recognizer_factory::initialize();

	const auto image_directory = L"D:\\workspace\\OCR\\IBK\\images";

	const auto profile = L"trade";
	std::wstring dataset_directory = L"D:\\workspace\\OCR\\IBK\\images";// fmt::format(L"{}\\{}\\datasets", image_directory, profile);

	auto recognizer = selvy::ocr::recognizer_factory::create(profile);

	std::vector<std::wstring> categories = {
		L"INCOME",				//0
		L"PENSION",				//1
		L"SMALL",				//2
		L"MIDDLE",				//3
		L"COMMERCIAL INVOICE",	//4
								//
								L"BILL OF LADING",		//5
								L"PROFOMA INVOICE",		//6
								L"LETTER OF CREDIT",	//7

	};

	std::unordered_map<std::wstring, std::vector<std::wstring>> key_values = {
		{ categories[0],{ L"사업자번호", L"법인명", L"대표자", L"퇴직사유", L"퇴직급여", L"입사일", L"기산일",L"퇴사일",L"지급일",L"계좌번호",L"계좌입금금액" } },
		{ categories[1],{ L"사업자번호", L"근무처명", L"입사일", L"기산일",L"퇴사일",L"지급일",L"계좌번호",L"입금일", L"계좌입금금액" } },
		//{ categories[2],{ L"금융기관용", L"유효기간", L"발급번호", L"업체명",L"대표자",L"사업자등록번호",L"자금명",L"최대한도액", L"담보구분" } },
		{ categories[2],{ L"유효기간", L"발급번호", L"업체명",L"대표자",L"사업자등록번호",L"자금명" } },
		//{ categories[3],{ L"제목", L"업체명", L"대표자", L"대출취급기한",L"구분",L"금액",L"담보종류",L"취급기관", L"대출기간", L"금리" } },
		{ categories[3],{ L"제목", L"업체명", L"대표자",L"구분",L"금액",L"담보종류",L"취급기관", L"대출기간", L"금리", L"대출취급기한" } },
		{ categories[4],{ L"SELLER", L"L/C NO", L"AMOUNT", L"CURRENCY SIGN" } },
		//claude
		{ categories[5],{ L"CONSIGNEE", L"CARRIER/MASTER/OWNER", L"NOTIFY", L"PORT OF DISCHARGE", L"PORT OF LOADING", L"ON BOARD DATE", L"VESSEL NAME" } }, //BL
																																							//{ categories[6],{ L"SELLER", L"AMOUNT", L"L/C" } },
		{ categories[7],{ L"SWIFT MT SENDER", L"AMOUNT", L"CREDIT NUMBER" } },//취소불능화환신용장
																			  //{ categories[14],{ L"SELLER", L"BUYER", L"ORIGIN", L"CONSIGNEE", L"NOTIFY", L"PORT OF LOADING", L"EXPORTER", L"APPLICANT", L"MANUFACTURER", L"PORT OF DISCHARGE", L"PLACE OF DELIVERY", L"VESSEL NAME", L"L/C" } },
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

	std::wstring res;
	for (const auto& file : files) {
		//auto category = categories[5];
		auto category = L"BILL OF LADING";		//5
												//			L"PROFOMA INVOICE",		//6
												//			L"LETTER OF CREDIT",	//7

		const auto fields = recognizer->recognize(file, category);

		auto output_path = fmt::format(L"{}_result.txt", boost::filesystem::path(file).native());
		std::wstring result_text;

		if (fields.empty()) {
			continue;
		}

		auto& keys = key_values.at(category);
		int count = 1;
		//result_text += fmt::format(L"\"file name\",\"{}\"\n", fields.at(L"FILE NAME").front());
		res += fmt::format(L"\"set name\",\"{}\"\n", boost::filesystem::path(file).filename().native());
		res += fmt::format(L"\"file name\",\"{}\"\n", boost::filesystem::path(file).filename().native());
		res += fmt::format(L"\"category\",\"{}\"\n", category);
		result_text += fmt::format(L"\"category\",\"{}\"\n", category);
		count++;
		for (auto& key : keys) {
			auto& name = key;
			result_text += L"\"" + name + L"\",";
			res += L"\"" + name + L"\",";
			if (fields.find(name) == fields.end()) {
				result_text += L"\n";
				res += L"\n";
				continue;
			}
			auto& r = fields.at(name);

			for (auto& f : r) {
				result_text += L"\"" + f + L"\",";
				res += L"\"" + f + L"\",";
			}
			result_text += L"\n";
			res += L"\n";
		}
		result_text += L"\n";
		res += L"\n";
		res += L"\n";

		std::wofstream txt_file;
		txt_file.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
		txt_file.open(output_path, std::wofstream::out | std::wofstream::ate);
		txt_file << result_text << std::endl;
		txt_file.close();

	}
	auto cur = time(NULL);
	auto time = localtime(&cur);
	auto output_path = fmt::format(L"\\result_{:02d}{:02d}.txt", time->tm_mon + 1, time->tm_mday);

	std::wofstream res_all;
	res_all.imbue(std::locale(std::locale::empty(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::generate_header>));
	res_all.open(dataset_directory + output_path, std::wofstream::out | std::wofstream::ate);
	res_all << res << std::endl;
	res_all.close();

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

	const auto image_directory = L"D:\\work\\190710(kb_entrust)\\sample_kb";
	const auto profile = L"entrust";

	// std::wstring dataset_directory =L"D:\\ksjo\\images\\sample_report" ;// fmt::format(L"{}\\{}\\datasets", image_directory, profile);
	// std::wstring dataset_directory = L"D:\\test";
	std::wstring dataset_directory = L"D:\\work\\190710(kb_entrust)\\sample_kb";

	auto recognizer = selvy::ocr::recognizer_factory::create(profile);

	std::vector<std::wstring> categories = {
		L"document",
	};

	std::unordered_map<std::wstring, std::vector<std::wstring>> key_values = {
		{ categories[0],{ L"발행기관", L"계좌번호", L"상품명", L"매수일자", L"매수금액", L"투자기간", L"이율(연)",L"상환이자",L"상환금액" } },
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

		if (std::wcscmp(profile, L"entrust") == 0) {
			//auto keys = std::vector<std::wstring>{ L"ISSUING ENTITY", L"ACCOUNT NUMBER", L"PRODUCT NAME", L"PURCHASE DATE", L"PURCHASE AMOUNT", L"INVESTMENT PERIOD", L"INTEREST RATE", L"상환이자", L"상환금액" };
			auto keys = std::vector<std::wstring>{ L"발행기관", L"계좌번호", L"상품명", L"매수일자", L"매수금액", L"투자기간", L"이율(연)",L"상환이자",L"상환금액" };
			auto mames = std::vector<std::wstring>{ L"발행기관", L"계좌번호", L"상품명", L"매수일자", L"매수금액", L"투자기간", L"이율(연)",L"상환이자",L"상환금액" };
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
						result_text += fmt::format(L"\"{} \",\"{}\"\n", name, f);
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
